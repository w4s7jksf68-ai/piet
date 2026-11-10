// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// display_command.cpp — the `display` statement: accepted formats, `_request()`,
// item grammar, directives, verbatim fallback and handleDisplay.
// Shared helpers are declared in display_internal.h.
#include "piet/display_hardening.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: %tbNAME rendering

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "piet/cclass.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "piet/eval/pyvalue.h"
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/missing.h"  // BUG-341 subscript: the extended-missing kind of a cell

#include "display_internal.h"

namespace piet {

using namespace display_detail;

namespace {

// BUG-1345: whether a `%` word of a display line is a format StataBE 19.5 takes;
// display refuses any other with r(120) `invalid %format`. A width is one to four
// digits (a leading 0 counts: `%0010s`, `%0010.2f` and `%0021x` are formats,
// `%00010s` is not) worth 1 to 2045. The families:
//   %[-]w{.,}d{f,g}[c] and %[-]w{.,}de with d below w (`%9.2fc`, `%9,2f`; not
//     `%9.2ec`, `%3.3f`, `%9.f`, `%.2f`, `%9.2F`, `%+9.2f`);
//   %[-~]ws (`%-~10s`, `%~-10s`, `%10S`, `%10ss`, `%0s` are not);
//   %[-]21x, %[-]16H, %[-]16L, %[-]8H, %[-]8L (`%10x`, `%21X`, `%16h` are not);
//   %[-]t{c,C,d,w,m,q,h,y,g}[mask] and %tbNAME[:mask] (`%t`, `%tx`, `%10td`,
//     `%-tbNAME` are not). A mask is not checked: `%tdx` and `%td!` are r(120)
//     in StataBE and printed here.
// The legacy `%d` family is not decided here (the caller's own path).
bool isStataDisplayFormat(const std::string& fmt) {
  static const std::regex reNum(R"(^%-?(\d{1,4})[.,](\d+)(?:[fg]c?|e)$)");
  static const std::regex reWidthed(R"(^%(?:-|~(?=\d+s$))?(\d{1,4})([sxHL])$)");
  static const std::regex reTime(R"(^%-?t[cCdwmqhyg].*$)");
  static const std::regex reBcal(R"(^%tb([A-Za-z_][A-Za-z0-9_]*)(?::.*)?$)");
  std::smatch m;
  if (std::regex_match(fmt, m, reNum)) {
    const int w = std::stoi(m[1].str());
    const std::string d = m[2].str();
    const size_t lead = d.find_first_not_of('0');
    return w >= 1 && w <= 2045 &&
           (lead == std::string::npos || (d.size() - lead <= 4 && std::stoi(d.substr(lead)) < w));
  }
  if (std::regex_match(fmt, m, reWidthed)) {
    const int w = std::stoi(m[1].str());
    switch (m[2].str()[0]) {
      case 's': return w >= 1 && w <= 2045;
      case 'x': return w == 21;
      default: return w == 16 || w == 8;  // H, L
    }
  }
  if (std::regex_match(fmt, m, reBcal)) return bcal::isValidCalName(m[1].str());
  return std::regex_match(fmt, reTime);
}

// _RE_REQUEST_CALL — BUG-087/D9 (BETA Phase F, D9 locked decision):
// `_request(macroName)` call-syntax token. Deliberately NOT part of
// piet::eval::tryExtendedMacroFn()'s colon-form dispatch
// (`local NAME : func ...`, macros.cpp) — real Stata's `_request()` is
// call-syntax invoked from inside `display`, exactly the shape BUG-088
// traced (this file's resolveToken() falling through -> eval::evalArith()
// -> pyeval's NameError). Intercepted in flushUnquoted() below (NOT inside
// resolveToken()) for two reasons: (a) resolveToken() has no channel to
// report a genuine error back to its caller — BUG-088's fix added that
// channel to evalArith(), one layer up, and duplicating it into
// resolveToken() too would be a second, parallel error path; (b) the
// modal's "prompt" text is the PRECEDING display text on this same
// statement (BUG-087's own repro: `display as text "Include additional
// controls? ..." _request(controls_input)` — the string literal before the
// call IS the prompt a real Stata user sees) — only flushUnquoted() has
// that (via `parts`); resolveToken() only ever sees one bare token in
// isolation.
const std::regex& reRequestCall() {
  static const std::regex r(std::string(R"(^_request\(\s*()") + eval::kRxName + R"()\s*\)$)");
  return r;
}

// PIET_REQUEST_<NAME> env var name for the headless fallback (macroName
// uppercased, any non-alnum char -> '_') — exact spec BUG-087's brief gives.
std::string requestEnvVarName(const std::string& macroName) {
  std::string out = "PIET_REQUEST_";
  for (char c : macroName) {
    unsigned char uc = static_cast<unsigned char>(c);
    out += std::isalnum(uc) ? static_cast<char>(std::toupper(uc)) : '_';
  }
  return out;
}

// _request(macroName) — BUG-087/D9: blocks on a real modal round-trip
// (windowed, on the do-executor worker thread) or reads
// PIET_REQUEST_<NAME>/PIET_REQUEST_DEFAULT (headless: --do/--selftest/
// --run-block/--evalcase never construct a Shell at all, so
// session.hasRequestPostEvent() is false there), sets the GLOBAL macro
// $macroName, and returns "" — `_request()` itself contributes no further
// display text (matches real Stata: the visible prompt is whatever was
// already displayed BEFORE the call blocks, not a "return value" from the
// call). On failure (cancelled run, wrong thread, or headless with no
// matching env var) sets *error and returns nullopt; the caller
// (flushUnquoted) turns that into an honest engine_error — same BUG-088
// shape, never a hang, never a silent literal-text fallback.
std::optional<std::string> resolveRequestCall(const std::string& macroName,
                                               const std::string& promptSoFar,
                                               SessionState& session, std::string* error) {
  if (session.hasRequestPostEvent()) {
    // A single-line `display _request(x)` submitted via #cmd-input goes
    // through the SYNCHRONOUS "command" bindRoute, which runs ON the UI
    // thread (shell.h's threading note) — blocking there would deadlock the
    // whole app (postEvent()'s queued JS injection, and the user's eventual
    // click, both need that same thread's run loop pumping). Refuse
    // honestly instead; see session_state.h's isCurrentThreadADoWorker()
    // doc comment for the full rationale.
    if (!piet::isCurrentThreadADoWorker()) {
      if (error) {
        *error =
            "_request(" + macroName +
            "): interactive input is only supported while running a full .do file (Run .do / "
            "the do-file editor) — it cannot be used from the single command line.";
      }
      return std::nullopt;
    }
    auto answer = session.requestUserInput(macroName, promptSoFar);
    if (!answer.has_value()) {
      if (error) {
        *error = "_request(" + macroName +
                 ") was cancelled — the run stopped before an answer arrived. Global macro $" +
                 macroName + " was not set.";
      }
      return std::nullopt;
    }
    session.eval.globals.set(macroName, *answer);
    return std::string();
  }

  // Headless: NEVER block waiting for a frontend that doesn't exist.
  std::string envName = requestEnvVarName(macroName);
  const char* v = std::getenv(envName.c_str());
  if (!v) v = std::getenv("PIET_REQUEST_DEFAULT");
  if (v != nullptr) {
    session.eval.globals.set(macroName, std::string(v));
    return std::string();
  }
  if (error) {
    *error = "_request(" + macroName +
             "): interactive input is unsupported in a headless run. Set the environment "
             "variable " +
             envName +
             " (or PIET_REQUEST_DEFAULT) before running, or set the macro yourself first: "
             "global " +
             macroName + " <value>.";
  }
  return std::nullopt;
}

// A display item that is a parenthesised group `(...)` or a function call
// `name(...)`, its opening parenthesis closed by the item's last character.
bool isGroupItem(const std::string& item) {
  size_t open = 0;
  if (!item.empty() && item[0] != '(') {
    if (!cecon::ident::isIdentStartByte(item[0])) return false;
    while (open < item.size() && cecon::ident::isIdentByte(item[open])) ++open;
  }
  if (open >= item.size() || item[open] != '(') return false;
  return matchingClose(item, open) == item.size() - 1;
}

// display's own DIRECTIVES (`_n`, `_col(#)`, `_dup(#)`, ...): display's
// language, not expressions, and a named gap of this layer rather than an
// error to classify (BUG-293(b)).
bool isDisplayDirective(const std::string& item) {
  static const std::regex reDisplayDirective(
      R"(^_(n|newline|continue|asis|request|skip|col|column|dup|char)\b.*)");
  return std::regex_match(item, reDisplayDirective);
}

// FU-142: the items of an unquoted display segment when it is two or more
// ADJACENT parenthesised groups or function calls -- `("a"=="b") ("a"=="a")`,
// `(1)(2)`, `missing(x[1]) missing(x[2])`. Stata evaluates each as its own
// expression and prints them back to back (w2f_residuals.log:76 lt_01 ends
// `|001|`); joined into one string they are no expression at all, and display
// echoed the source. Tokens are already cut at top-level whitespace; a token is
// cut again where a group closes at depth 0 and the next one opens at once.
// nullopt for anything else (one item, or any item that is a bare word, an
// operator or a subscript), so prose and ordinary expressions keep their route.
std::optional<std::vector<std::string>> splitAdjacentGroups(
    const std::vector<std::string>& tokens) {
  std::vector<std::string> items;
  for (const auto& tok : tokens) {
    size_t start = 0;
    size_t i = 0;
    int depth = 0;
    while (i < tok.size()) {
      const size_t q = skipQuotedSpan(tok, i);
      if (q != i) {
        i = q;
        continue;
      }
      const char c = tok[i];
      if (c == '(' || c == '[') {
        ++depth;
      } else if (c == ')' || c == ']') {
        --depth;
        if (depth == 0 && c == ')' && i + 1 < tok.size() && tok[i + 1] == '(') {
          items.push_back(tok.substr(start, i + 1 - start));
          start = i + 1;
        }
      }
      ++i;
    }
    items.push_back(tok.substr(start));
  }
  if (items.size() < 2) return std::nullopt;
  // A subscript `NAME[...]` is an item of its own as well: `display t[1] t[2]
  // t[3]` prints `123` (w2_smalls.log:148-149 sort_stable).
  // `_b[...]`/`_se[...]` keep their verbatim (%.9g) route as one segment.
  const auto isSubscriptItem = [](const std::string& item) {
    size_t j = 0;
    if (item.empty() || !cecon::ident::isIdentStartByte(item[0])) return false;
    if (item.rfind("_b[", 0) == 0 || item.rfind("_se[", 0) == 0 || item.rfind("_coef[", 0) == 0)
      return false;
    while (j < item.size() && cecon::ident::isIdentByte(item[j])) ++j;
    if (j >= item.size() || item[j] != '[' || item.back() != ']') return false;
    int depth = 0;
    for (size_t k = j; k < item.size(); ++k) {
      if (item[k] == '[') ++depth;
      else if (item[k] == ']' && --depth == 0 && k + 1 != item.size()) return false;
    }
    return depth == 0;
  };
  for (const auto& item : items)
    if ((!isGroupItem(item) && !isSubscriptItem(item)) || isDisplayDirective(item))
      return std::nullopt;
  return items;
}

// BUG-1333: the expressions of an unquoted display segment. Stata's display
// reads the longest expression it can and starts the next item where one
// operand is followed, across a blank, by another: `display %9.2f price xd`
// formats price and prints xd at the default (`    10.00.33333334`), `1 2` is
// `12`, `1 -2` is one expression (-1) and `abs (-1)` is `abs not found`
// (StataBE 19.5). The segment's tokens were joined into one expression, which
// failed and echoed the format and the source (`%9.2f10.0 0.3333333432674408`
// at rc 0). Tokens are already cut at top-level blanks; a token ending an
// operand followed by one opening an operand (not a binary operator) starts a
// new expression. A `!` or `~` does not open one: Stata keeps `1 !2` in one
// expression and refuses it (`1!2 invalid name` r(198), `1 !(2)` r(133)).
std::vector<std::vector<std::string>> splitExpressions(const std::vector<std::string>& tokens) {
  const auto opensOperand = [](const std::string& t) {
    const unsigned char c = static_cast<unsigned char>(t[0]);
    return std::isalnum(c) || c == '_' || c == '.' || c == '(' || c == '"' || c == '`' || c >= 0x80;
  };
  const auto endsOperand = [](const std::string& t) {
    const unsigned char c = static_cast<unsigned char>(t.back());
    return std::isalnum(c) || c == '_' || c == '.' || c == ')' || c == ']' || c == '"' ||
           c == '\'' || c >= 0x80;
  };
  std::vector<std::vector<std::string>> out;
  for (const auto& tok : tokens) {
    if (tok.empty()) continue;
    if (out.empty() || (endsOperand(out.back().back()) && opensOperand(tok)))
      out.emplace_back();
    out.back().push_back(tok);
  }
  return out;
}

// BUG-1333: an expression where `!` or `~` follows an operand across a blank
// (`1 !2`, `x !x`, `3 !abs(2)`). Stata reads the operand's last word, the
// operator and the next word as one name and refuses it: `1!2 invalid name`
// r(198), `unknown function 3!abs()` r(133) (StataBE 19.5; `1+1 !2` is `1!2`
// too). The message with its r(#) prefix, or nothing when the expression has no
// such juxtaposition or the operand does not end in a word (`(1) !2`, `x[1] !2`
// take other paths in Stata).
std::optional<std::string> juxtaposedNotError(const std::vector<std::string>& expr) {
  const auto isWordByte = [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '.' || u >= 0x80;
  };
  for (size_t j = 1; j < expr.size(); ++j) {
    const std::string& tok = expr[j];
    if (tok.empty() || (tok[0] != '!' && tok[0] != '~')) continue;
    if (tok.size() > 1 && tok[1] == '=') continue;
    const std::string& prev = expr[j - 1];
    size_t w = prev.size();
    while (w > 0 && isWordByte(prev[w - 1])) --w;
    if (w == prev.size()) continue;
    std::string tail = tok;
    for (size_t k = j + 1; k < expr.size() && tail.find_first_not_of("!~") == std::string::npos; ++k)
      tail += expr[k];
    size_t e = tail.find_first_not_of("!~");
    if (e == std::string::npos) e = tail.size();
    while (e < tail.size() && isWordByte(tail[e])) ++e;
    const std::string name = prev.substr(w) + tail.substr(0, e);
    if (e < tail.size() && tail[e] == '(') return "r(133): unknown function " + name + "()";
    return "r(198): " + name + " invalid name";
  }
  return std::nullopt;
}

// FU-855 / FU-201: true when a lone display item is an expression by Stata's
// grammar -- a parenthesised group, a function call, a subscript, or operators
// over an operand that starts like a number (`1/0`, `-x*2`). A bare word
// (`display "text" foo`) is not, nor is a word run into punctuation (`a-b`,
// `/usr/bin`, the `one>|` a mangled quote leaves): that is the prose echo this
// layer keeps on purpose.
bool isExpressionItem(const std::string& item) {
  if (item.empty()) return false;
  if (isGroupItem(item)) return true;
  bool op = false;
  size_t i = 0;
  while (i < item.size()) {
    const size_t q = skipQuotedSpan(item, i);
    if (q != i) {
      i = q;
      continue;
    }
    if (item[i] == '[') return true;
    if (std::string("+-*/^<>=!&|").find(item[i]) != std::string::npos) op = true;
    ++i;
  }
  const unsigned char c0 = static_cast<unsigned char>(item[0]);
  const bool numberLike =
      std::isdigit(c0) || c0 == '-' || c0 == '+' || c0 == '!' ||
      (c0 == '.' && item.size() > 1 && std::isdigit(static_cast<unsigned char>(item[1])));
  return op && numberLike;
}

// The refusal display gives an expression that failed: the BUG-246
// classifier's code when it claims the shape; otherwise an unbound name is
// Stata's r(111) `NAME not found` (display already tried it as a variable and
// a scalar) and anything else r(198) `invalid syntax`. Never the evaluator's
// raw NameError text, which carries no code and answered `_rc` 1 (`display
// nosuchfn(1)`; StataBE: r(133) unknown function, exprfns_contract_oracle.log H).
std::string namedRefusal(const std::string& expr, const std::string& raised) {
  eval::MacroFnError me;
  if (eval::classifyFailedExpr(expr, raised, &me) && me.failed())
    return "r(" + me.rc + "): " + me.message;
  static const std::string kNameErr = "NameError: name '";
  if (raised.rfind(kNameErr, 0) == 0) {
    const size_t end = raised.find('\'', kNameErr.size());
    if (end != std::string::npos && end > kNameErr.size())
      return "r(111): " + raised.substr(kNameErr.size(), end - kNameErr.size()) + " not found";
  }
  return "r(198): invalid syntax";
}

// name-binding-display-literals: a string value in a `%#s` / `%-#s` / `%~#s`
// field (reStringFmtSpec): blanks to the left, to the right, or split with the
// odd one on the right. A value as wide as the field or wider prints whole.
std::string padStringField(const std::string& fmt, const std::string& text) {
  size_t k = 1;
  const char align = (k < fmt.size() && (fmt[k] == '-' || fmt[k] == '~')) ? fmt[k++] : '\0';
  size_t width = 0;
  while (k < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[k])))
    width = width * 10 + static_cast<size_t>(fmt[k++] - '0');
  size_t len = 0;
  for (char c : text)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++len;
  if (len >= width) return text;
  const size_t fill = width - len;
  if (align == '-') return text + std::string(fill, ' ');
  if (align == '~') return std::string(fill / 2, ' ') + text + std::string(fill - fill / 2, ' ');
  return std::string(fill, ' ') + text;
}

// A resolved _b[]/_se[]/e() token as display's VERBATIM fallback prints it (the
// line did not evaluate as one expression: `display _col(3) e(N)`, `display "E|"
// _b[x1] _b[2.c]`). The resolvers hand back full-precision text (BUG-554) because
// an expression or a %fmt re-renders it; the verbatim fallback renders nothing, so
// there the value prints as it did before BUG-554, `%.9g`: `60`, not `60.0`, and
// nine significant digits, not seventeen. Non-numeric text is returned as is.
std::string verbatimCoefText(const std::string& token, const std::string& resolved) {
  if (!(std::regex_match(token, reBRef()) || std::regex_match(token, reSeRef()) ||
        std::regex_match(token, reERef()) || eScalarKey().count(token) > 0))
    return resolved;
  char* end = nullptr;
  const double v = std::strtod(resolved.c_str(), &end);
  if (resolved.empty() || end == nullptr || *end != '\0' || !std::isfinite(v)) return resolved;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry point — handle_display

ojson handleDisplay(const std::string& rawArgsIn, SessionState& session) {
  std::string rawArgs = rawArgsIn;

  // U7/S10 (RISK-2): strip a leading `as text|txt|result|res|error|err`
  // modifier (optionally `_continue`/`_n`), case-insensitive.
  {
    static const std::regex reAsMod(
        R"(^as\s+(?:text|txt|result|res|error|err)(?:_(?:continue|n))?\s+)",
        std::regex::icase);
    std::string stripped0 = strip(rawArgs);
    std::smatch m0;
    if (std::regex_search(stripped0, m0, reAsMod) && m0.position(0) == 0) {
      rawArgs = stripped0.substr(m0.length(0));
    }
  }

  std::string s = strip(rawArgs);

  std::vector<std::string> parts;
  std::vector<std::string> unquotedTokens;
  std::optional<std::string> pendingFmt;
  // BUG-088: set when an unresolved call-syntax token (an extended macro
  // function PIET doesn't support) raised a genuine error — surfaced as an
  // honest engine_error instead of echoing the literal source text.
  std::optional<std::string> displayError;
  // How many parts were printed before the piece that raised displayError:
  // Stata writes each piece as it evaluates it and stops at the failing one,
  // so these stay on the line ahead of the message (the dispatcher's
  // displayPartialBlock reads them off the error block as `partial_text`).
  size_t partsBeforeError = 0;

  auto flushUnquotedPiece = [&]() {
    if (unquotedTokens.empty()) return;

    // BUG-087/D9 (BETA Phase F): a LONE `_request(macroName)` unquoted token
    // is handled as a dedicated case, not through the generic per-token
    // resolveToken() loop below — see reRequestCall()'s doc comment for why.
    // Scoped deliberately narrow (exactly one token, this exact shape) —
    // real Stata usage is always `_request(name)` standalone at the end of
    // a display statement (BUG-087's own repro); a token combined with
    // other unquoted text (e.g. `_request(x) foo`) falls through to the
    // generic path unchanged, where it hits the SAME pre-existing BUG-088
    // syntax-error-is-silent-literal-fallthrough behavior any other
    // malformed call-syntax expression gets — not a new gap introduced here.
    if (unquotedTokens.size() == 1) {
      std::smatch rm;
      if (std::regex_match(unquotedTokens[0], rm, reRequestCall())) {
        // Extract the macro name BEFORE clearing unquotedTokens: `rm` holds
        // iterators into unquotedTokens[0]'s string storage, so clearing the
        // vector first (destroying that string) would leave rm[1].str()
        // reading freed memory (caught live: a dangling-iterator bug that
        // produced garbage macro names during manual --do verification of
        // this exact code path before this fix).
        std::string macroName = rm[1].str();
        unquotedTokens.clear();
        std::string promptSoFar;
        for (const auto& p : parts) promptSoFar += p;
        std::string err;
        auto result = resolveRequestCall(macroName, promptSoFar, session, &err);
        if (result.has_value()) {
          // _request() contributes no further display text. A dangling
          // pending %fmt (e.g. `display %9.2f _request(x)`, not a real
          // usage anywhere in this codebase's corpus) has nothing numeric
          // left to format against — drop it rather than inventing a
          // formatting rule for a non-existent case.
          pendingFmt.reset();
          return;
        }
        if (!displayError) displayError = err;
        return;
      }
    }

    // FU-640: a lone string-variable subscript prints its text verbatim. It
    // used to be substituted unquoted and then evaluated as arithmetic, so
    // `" 12 "` printed `12`, `"1e3"` printed `1000` and `""` printed `.`
    // (w2x_fn_string_math fx_fix_s).
    if (unquotedTokens.size() == 1 && !pendingFmt.has_value()) {
      if (auto sv = stringVarSubscript(unquotedTokens[0], session)) {
        parts.push_back(*sv);
        unquotedTokens.clear();
        return;
      }
    }

    std::vector<std::string> resolved;
    resolved.reserve(unquotedTokens.size());
    std::vector<bool> resolvedString;  // a string variable's cell (resolveToken's strValue)
    std::optional<double> loneStored;  // BUG-554: the stored double of a lone item
    for (const auto& tok : unquotedTokens) {
      // BUG-162: an unresolvable coefficient name is an honest error, not a
      // silent `.`. First error wins, matching the displayError convention
      // the _request path above already uses.
      std::string tokErr;
      bool isStr = false;
      resolved.push_back(resolveToken(tok, session, &tokErr, &loneStored, &isStr));
      resolvedString.push_back(isStr);
      if (!tokErr.empty() && !displayError) displayError = tokErr;
    }
    // BUG-554 / FU-45 (W2D-T03): a bare `_b[x]`, `_se[x]`, `e(name)` or
    // `e(name)[i,j]` under an explicit format renders the STORED double, the
    // same text the same value held in a scalar gets. Its resolved text is the
    // nine-digit %.9g form, which is what `display %18.0g _b[x]` printed
    // (.333333333) and why `%21x _b[x]` printed a decimal. Only the lone,
    // formatted item takes this route: an unformatted item and any expression
    // built on one keep the text exactly as before.
    if (pendingFmt.has_value() && unquotedTokens.size() == 1 && loneStored.has_value() &&
        std::isfinite(*loneStored)) {
      unquotedTokens.clear();
      std::string fmtErr;
      // The text is the one `scalar s = <value>` stores (evalArith's number
      // form, eval/macros.cpp: an integer-valued double as its exact integer,
      // anything else as repr), so a spec this layer does not claim (`%20x`,
      // `%9s`, `%21X`) echoes `12`, never the repr `12.0` -- the same text the
      // scalar prints. Both forms parse back to the same double, so the
      // claimed specs are unaffected.
      const std::string storedText = eval::pyIsInteger(*loneStored)
                                         ? eval::pythonIntStrFromDouble(*loneStored)
                                         : eval::pythonFloatRepr(*loneStored);
      parts.push_back(applyStataFmtLocal(*pendingFmt, storedText, &fmtErr));
      if (!fmtErr.empty() && !displayError) displayError = fmtErr;
      pendingFmt.reset();
      return;
    }
    // BUG-293(b) needs to know whether ANY resolution actually happened: a
    // token that resolved to real data (`c(current_date)` -> a date string) is
    // a value to print, not an expression that failed to parse.
    std::string origJoined;
    for (size_t i = 0; i < unquotedTokens.size(); ++i) {
      if (i > 0) origJoined += " ";
      origJoined += unquotedTokens[i];
    }
    std::vector<std::string> srcTokens;  // for the verbatim fallback below
    srcTokens.swap(unquotedTokens);

    // BUG-710 / FU-1324: a STRING scalar's value is a string literal here, not
    // expression text: `scalar s2 = "d13[1]*3"` then `display s2` printed 1 (the
    // cell read FU-207's binder found inside the value) and `scalar s = "1+1"`
    // then `display s` printed 2, where StataBE prints the text. The macro
    // layer types the store (EvalSession::stringScalars); a value holding a
    // `"` keeps the old form.
    // name-binding-display-literals: a string variable's cell is the same kind of
    // value (w2av_name_binding bd_dy_strvar `[ab1]`, bd_dy_fstr `%9s s`).
    std::string joined;
    for (size_t i = 0; i < resolved.size(); ++i) {
      if (i > 0) joined += " ";
      if (resolvedString[i] && resolved[i].find('"') == std::string::npos) {
        joined += "\"" + resolved[i] + "\"";
        continue;
      }
      const std::string* stored = session.eval.scalars.find(srcTokens[i]);
      if (stored != nullptr && *stored == resolved[i] && session.eval.isStringScalar(srcTokens[i]) &&
          resolved[i].find('"') == std::string::npos) {
        joined += "\"" + resolved[i] + "\"";
        continue;
      }
      joined += resolved[i];
    }

    // BS-091/BS-101 (BETA A4): resolveToken's step 8 (above) only catches a
    // BARE whole-token `c(name)` (its regex is a full-string match) — a
    // c(name) reference COMBINED with real arithmetic (`c(N)*2`) never
    // matches that whole-token pattern and would otherwise reach evalArith
    // as literal, unresolved text. substituteCRefs is a general find-
    // anywhere pass (piet::eval::resolveEstScalarRefs's own established
    // shape), so it also correctly handles the plain bare-token case
    // resolveToken already resolved (a no-op there — no more literal
    // "c(...)" text remains to find).
    joined = substituteCRefs(joined, session);
    // The text before FU-207's binding: what the refusal gate below compares
    // with the source, what a refusal message names and what an echo prints
    // (never the bound `missing((5.0))`).
    const std::string sourceJoined = joined;
    if (auto bound = bindVarSubscripts(joined, session)) joined = *bound;  // FU-207

    std::string arithRaised;
    bool arithIsString = false;
    auto arithResult = eval::evalArith(joined, &session.eval, &arithRaised, &arithIsString);
    // FU-853 / FU-226 / FU-349: the subscripts FU-207's literal-index pass
    // leaves (an index that is an expression, `x[_n+1]`; a matrix index that
    // is one, `c[1,colsof(c)-1]`; a string cell, `length(s[1])`) are bound here
    // and the expression evaluated again (bindSubscripts). Only on failure, so
    // every expression that already evaluated keeps its answer, and only an
    // answer is taken from the bound form: when it fails too the original error
    // stands (`display s[2]+"c"` stays r(111), selftest_ereturn_matrix_functions (9b)). Only
    // SOURCE tokens are bound here: a token that resolved is a value (a
    // scalar's stored text), not an expression of this line.
    std::string evalText = joined;
    std::string unknownSub;
    if (!arithResult.has_value()) {
      std::optional<std::string> bound;
      {
        std::string rebuilt;
        bool changed = false;
        for (size_t i = 0; i < resolved.size(); ++i) {
          std::string piece = resolved[i];
          if (piece == srcTokens[i]) {
            if (auto b = bindSubscripts(piece, session, &unknownSub)) {
              piece = *b;
              changed = true;
            }
          }
          if (i > 0) rebuilt += " ";
          rebuilt += piece;
        }
        if (changed) bound = substituteCRefs(rebuilt, session);
      }
      if (bound) {
        std::string raised2;
        bool isStr2 = false;
        auto r2 = eval::evalArith(*bound, &session.eval, &raised2, &isStr2);
        evalText = *bound;
        if (r2.has_value()) {
          arithResult = r2;
          arithIsString = isStr2;
          arithRaised.clear();
        }
      }
    }
    // FU-640: a bare variable name in a display expression reads observation 1,
    // as in Stata (`display "Growth: " string(v, "%9.1f")` prints v[1]; w2x
    // fx_string2_disp). Only on the NameError path, so a scalar or any name the
    // macro layer already binds keeps its meaning.
    if (!arithResult.has_value() && arithRaised.rfind("NameError", 0) == 0) {
      if (auto bound = bindBareVarsToObs1(evalText, session)) {
        std::string raised2;
        bool isStr2 = false;
        auto r2 = eval::evalArith(*bound, &session.eval, &raised2, &isStr2);
        if (r2.has_value()) {
          arithResult = r2;
          arithIsString = isStr2;
          arithRaised.clear();
        }
      }
    }

    if (arithResult.has_value()) {
      if (pendingFmt.has_value() && arithIsString && std::regex_match(*pendingFmt, reStringFmtSpec())) {
        // name-binding-display-literals: a string value under `%#s` fills the
        // field, right-justified (`%-#s` left, `%~#s` centred); it printed bare
        // (w2av_name_binding bd_dy_fstr `%9s s` -> `      ab1`).
        parts.push_back(padStringField(*pendingFmt, *arithResult));
        pendingFmt.reset();
      } else if (pendingFmt.has_value()) {
        // BUG-185: a malformed '-'-prefixed spec reports itself via fmtErr
        // instead of silently dropping the format or echoing the spec text.
        std::string fmtErr;
        parts.push_back(applyStataFmtLocal(*pendingFmt, *arithResult, &fmtErr));
        if (!fmtErr.empty() && !displayError) displayError = fmtErr;
        pendingFmt.reset();
      } else {
        // W:P5: no explicit %fmt -> Stata's default `%9.0g` for a numeric
        // result. Non-numeric results (string concatenation, an unresolved
        // literal) pass through untouched — only a value that round-trips
        // through strtod as the WHOLE string is reformatted.
        // BUG-293: a STRING-valued expression is printed verbatim, never
        // re-formatted as a number. `display string(42.5,"%9.2f")` is `42.50`
        // in Stata; routing it through the default %9.0g gave `42.5` — the
        // format the user asked string() for, undone one layer later.
        const std::string& ar = *arithResult;
        char* endp = nullptr;
        const double av = std::strtod(ar.c_str(), &endp);
        const bool wholeNumber = !arithIsString && !ar.empty() && endp != nullptr &&
                                 *endp == '\0' && std::isfinite(av);
        parts.push_back(wholeNumber ? fmtDisplayDefaultG(av) : ar);  // BUG-1016
      }
    } else {
      // BUG-1420: a c(NAME) that is no c() key at all is Stata's r(133)
      // "c(NAME) undefined" wherever it stands (`display c(bogus_setting)`,
      // `display 1 + c(bogus_setting)`); it was the evaluator's NameError at
      // rc 1 (lane expr-sem x1b/a25.do).
      if (!displayError) {
        if (auto undef = eval::undefinedCRef(sourceJoined)) displayError = "r(133): " + *undef;
      }
      // BUG-088: a genuine NameError on a single call-syntax token `name(args)`
      // (an unsupported extended macro function) surfaces honestly rather than
      // being echoed as literal text. Narrowed to a lone call-syntax token so
      // ordinary literal display (`foo bar`, bare words) stays byte-identical.
      // An evaluator error that names Stata's exact code (`r(NNN): ...`) is
      // claimed in every shape below: prose never raises one. So is scalar() of
      // a scalar that does not exist, which is r(111) "NAME not found" in
      // StataBE (w2ac_expr_valueref scalar_not_neg: the failed `scalar s = !-1`
      // leaves s unset), not the evaluator's NameError text at rc 1.
      const bool exactCode = arithRaised.rfind("r(", 0) == 0 ||
                             arithRaised.rfind("NameError: name 'scalar' ", 0) == 0;
      // FU-418: a CALL to a function this evaluator does not know is Stata's
      // r(133) "unknown function NAME()" -- the code `local x = f(1)` already
      // answers through the same classifier -- in every shape, never the
      // evaluator's raw `NameError: name 'f' is not defined` at rc 1 (`display
      // "x" nosuchfn_x(1)`; also a real Stata function this build lacks, such as
      // reldif(), until it is implemented). A bare-name NameError is
      // not a call and keeps its paths below. A `c(...)` left unresolved is no
      // call either: the classifier words it as StataBE does, r(133) `c(name)
      // undefined`, or refuses a real c() name this build does not serve
      // (BUG-1330; it was the evaluator's NameError at rc 1). Nor is a display
      // DIRECTIVE (`_col(10)`, `_dup(3)`, ...): that is display's language, not
      // a function; its gap (FU-984) keeps bin/main's path, as the directive
      // guard below does.
      static const std::regex reDirectiveName(
          R"(^NameError: name '_(n|newline|continue|asis|request|skip|col|column|dup|char)' )");
      if (!displayError && !exactCode && arithRaised.rfind("NameError", 0) == 0 &&
          !std::regex_search(arithRaised, reDirectiveName)) {
        eval::MacroFnError me;
        if (eval::classifyFailedExpr(joined, arithRaised, &me) && me.rc == "133")
          displayError = "r(133): " + me.message;
      }
      if (!arithRaised.empty() && !displayError && !exactCode) {
        static const std::regex reCallSyntax(
            std::string(R"(^\s*)") + eval::kRxName + R"(\s*\(.*\)\s*$)");
        // A display directive and a c()/r()/e()/s() reference are no function
        // calls: they keep the evaluator's text (BUG-088) rather than be called
        // an unknown function.
        static const std::regex reClassRef(R"(^\s*[cres]\s*\(.*)");
        if (std::regex_match(sourceJoined, reCallSyntax))
          displayError = isDisplayDirective(sourceJoined) || std::regex_match(sourceJoined, reClassRef)
                             ? arithRaised
                             : namedRefusal(sourceJoined, arithRaised);
      }
      // BUG-293(b): an unevaluable expression stops being echoed back as its
      // own source text at rc 0. Reuses the BUG-246 classifier, so display and
      // `local NAME = <expr>` answer ONE mapping — including its two
      // deliberately-unclaimed shapes (string-literal RHS, bare-identifier
      // NameError = the dataset-variable read), which keep the legacy echo.
      //
      // Gated on there being exactly ONE unquoted segment with no quoted parts
      // beside it, and on the segment not being one of display's own
      // DIRECTIVES (`_n`, `_col(#)`, `_dup(#)`, …) — those are display's
      // language, not expressions, and are a named gap rather than an error.
      // `display "text" foo` and multi-token prose keep echoing: this claims
      // the shape the oracle measured (`display 1%2` -> r(198)), not every
      // line that ever reached this fallback.
      //
      // `joined == origJoined` is the load-bearing half, found by the selftest:
      // when resolveToken/substituteCRefs DID resolve the token, what reaches
      // evalArith is the resolved VALUE, and a value that is not arithmetic
      // (`display c(current_date)` -> `12 Aug 2026`) must print, not error.
      // Only a token that survived resolution untouched is a candidate.
      //
      // exactCode (above) is claimed in every shape: `display ! -1` splits into
      // `!` and `-1` here, and `display "x|" (!-1>0)` has a part before it; both
      // echoed the source at rc 0 where StataBE gives r(198) (w2ac_expr_valueref
      // disp_not_neg_sp, w2ac_expr_relational disp_not_gt).
      //
      // FU-141 / FU-855 / FU-201: the same claim for a lone unresolved item
      // under a pending %fmt (`display %21x d13[1]*3` printed `d13[1]*3`) and for
      // one with parts before it when the item is an expression by grammar
      // (isExpressionItem: `display "a|" (1+) "|"` printed `a|(1+)|`, `display
      // "d10|" 1/0` printed `d10|1/0`). The rule that keeps the prose echo: a
      // bare word, or several tokens that are not all groups, is text; a
      // parenthesised group, a call, or text with an operator or subscript is
      // an expression, and an expression that fails is an error. A group the
      // classifier leaves unclaimed (`(foo)`, an unbound bare name) is refused
      // too (namedRefusal), since a parenthesis is never prose.
      const bool loneUnresolved = resolved.size() == 1 && sourceJoined == origJoined;
      if (!displayError &&
          (exactCode || (loneUnresolved && (parts.empty() || pendingFmt.has_value() ||
                                            isExpressionItem(sourceJoined))))) {
        if (!isDisplayDirective(sourceJoined)) {
          eval::MacroFnError me;
          if (!exactCode && !unknownSub.empty()) {
            // `missing(nosuchvar[1])`: the name, not the call, is what failed.
            displayError = "r(111): " + unknownSub + " not found";
          } else if (eval::classifyFailedExpr(sourceJoined, arithRaised, &me) && me.failed()) {
            // A shape this gate newly claims (parts before it, or a pending
            // %fmt) says what StataBE says, `invalid syntax`, and does not
            // quote the item back (w2ac_expr_relational.log:2602-2606
            // disp_not_gt); the lone item keeps the classifier's wording.
            const bool newShape = !parts.empty() || pendingFmt.has_value();
            displayError = "r(" + me.rc + "): " +
                           (newShape && !exactCode && me.rc == "198" ? std::string("invalid syntax")
                                                                     : me.message);
          } else if (loneUnresolved && (isGroupItem(sourceJoined) || pendingFmt.has_value())) {
            // Under a pending %fmt a bare item is an expression too, never
            // prose: `display %9.2f foo` is StataBE's `foo not found` r(111),
            // after the parts before it (`a|foo not found`, measured 19.5).
            displayError = namedRefusal(sourceJoined, arithRaised);
          }
        }
      }
      // name-binding-display-literals: a source token that resolved to nothing
      // and holds a name that binds to nothing is Stata's r(111), never its own
      // text at rc 0 (unboundNameRefusal). A token that resolved to a value
      // (`c(current_date)`, a string e()) is that value and keeps printing.
      for (size_t t = 0; !displayError && t < resolved.size(); ++t) {
        if (resolved[t] != srcTokens[t] || isDisplayDirective(srcTokens[t])) continue;
        if (auto refusal = unboundNameRefusal(srcTokens[t], session)) displayError = *refusal;
      }
      // FU-141: a scalar the macro layer could not evaluate when it was set
      // holds its source text (`scalar b_04 = xd[_N-1]` stores `xd[_N-1]`), and
      // a pending numeric %fmt printed that text at rc 0 where StataBE prints
      // the number (w2i_varsub b_04; w2e_float_default fn_expr_reads_double
      // `%21x se_d`). The store keeps no type, and a string scalar under a
      // numeric format prints its text in StataBE (`scalar ss = "abc"`, `display
      // %9.2f ss` -> `abc`, measured 19.5), so only stored text that is an
      // expression by grammar (isExpressionItem) is refused, and never under a
      // string format. Until the assignment evaluates it
      // (varsub-assign-from-variable-bug710) this is a named gap, not the number.
      // The cost is one-sided and loud: a string scalar that holds such text
      // (`scalar sx = "xd[_N-1]"`, `display %21x sx`) is refused where StataBE
      // prints the text.
      if (!displayError && pendingFmt.has_value() && resolved.size() == 1 &&
          !std::regex_match(*pendingFmt, reStringFmtSpec()) && isExpressionItem(resolved[0])) {
        const std::string* stored = session.eval.scalars.find(srcTokens[0]);
        if (stored != nullptr && *stored == resolved[0])
          displayError = "r(198): scalar " + srcTokens[0] + " holds the unevaluated expression " +
                         resolved[0] + "; PIET cannot format it as a number";
      }
      if (pendingFmt.has_value() && resolved.size() == 1) {
        std::string fmtErr;
        parts.push_back(applyStataFmtLocal(*pendingFmt, resolved[0], &fmtErr));
        if (!fmtErr.empty() && !displayError) displayError = fmtErr;
        pendingFmt.reset();
      } else {
        if (pendingFmt.has_value()) {
          parts.push_back(*pendingFmt);
          pendingFmt.reset();
        }
        // Verbatim: coefficient and e() values print at %.9g (verbatimCoefText).
        std::string verbatim;
        bool legacy = false;
        for (size_t i = 0; i < resolved.size(); ++i) {
          const std::string t = verbatimCoefText(srcTokens[i], resolved[i]);
          legacy = legacy || t != resolved[i];
          if (i > 0) verbatim += " ";
          verbatim += t;
        }
        parts.push_back(legacy ? substituteCRefs(verbatim, session) : sourceJoined);
      }
    }
  };
  auto flushUnquoted = [&]() {
    // BUG-1333 / FU-142: each expression of the segment (splitExpressions), and
    // each of several adjacent groups inside one (splitAdjacentGroups), is
    // evaluated one by one, exactly as a lone item is. A pending %fmt formats
    // the first; the first that fails stops the line with the items before it
    // printed.
    std::vector<std::vector<std::string>> items;
    std::optional<std::string> juxtaposed;  // stops the line after `items`
    for (auto& expr : splitExpressions(unquotedTokens)) {
      if ((juxtaposed = juxtaposedNotError(expr))) break;
      if (auto split = splitAdjacentGroups(expr)) {
        for (auto& item : *split) items.push_back({std::move(item)});
      } else {
        items.push_back(std::move(expr));
      }
    }
    unquotedTokens.clear();
    for (const auto& item : items) {
      unquotedTokens = item;
      const size_t before = parts.size();
      const bool hadError = displayError.has_value();
      flushUnquotedPiece();
      if (!hadError && displayError.has_value()) {
        partsBeforeError = before;
        return;
      }
    }
    if (juxtaposed && !displayError) {
      if (pendingFmt.has_value()) pendingFmt.reset();
      displayError = *juxtaposed;
      partsBeforeError = parts.size();
    }
  };

  // name-binding-display-literals: display's own DIRECTIVES ([P] display:
  // `as style`, `in smcl`, `_asis`, `_skip(#)`, `_column(#)`, `_newline[(#)]`,
  // `_continue`, `_dup(#)`, `_char(#)`, `,` and `,,`), each in the spelling
  // Stata accepts (`_n`, `_col`, `_c`). They used to reach the expression path
  // as words: `as text`, `in red` and `_continue` printed as text, `_n` read as
  // the observation number 1, `_col(30)`/`_skip(3)` were refused at rc 1
  // (w2av_name_binding bd_dy_astext, bd_dy_inred, bd_dy_cont, bd_dy_nl,
  // bd_dy_col, bd_dy_skip). A directive ends the expression before it.
  size_t i = 0;  // the scan position in `s` (takeStyleWord reads past it)
  std::optional<long long> dupCount;  // `_dup(#)`: repeat the next directive
  size_t dupMark = 0;
  bool continueLine = false;  // `_continue`: the display block says so (doexec joins)
  auto applyDup = [&]() {
    if (!dupCount.has_value() || parts.size() <= dupMark) return;
    const std::vector<std::string> unit(parts.begin() + static_cast<std::ptrdiff_t>(dupMark),
                                        parts.end());
    parts.resize(dupMark);
    for (long long r = 0; r < *dupCount; ++r) parts.insert(parts.end(), unit.begin(), unit.end());
    dupCount.reset();
  };
  auto flushDirective = [&]() {
    flushUnquoted();
    applyDup();
  };
  // The output column the next character lands in (0-based), for `_column(#)`.
  auto currentColumn = [&]() {
    size_t col = 0;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      const size_t nl = it->rfind('\n');
      for (size_t k = nl == std::string::npos ? 0 : nl + 1; k < it->size(); ++k)
        if ((static_cast<unsigned char>((*it)[k]) & 0xC0) != 0x80) ++col;
      if (nl != std::string::npos) break;
    }
    return col;
  };
  auto directiveArg = [&](const std::string& inner) -> std::optional<long long> {
    std::string raised;
    bool isStr = false;
    auto r = eval::evalArith(strip(inner), &session.eval, &raised, &isStr);
    if (!r.has_value() || isStr) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(r->c_str(), &end);
    if (r->empty() || end == nullptr || *end != '\0' || !std::isfinite(v) || std::fabs(v) > 1e6)
      return std::nullopt;
    return static_cast<long long>(v);
  };
  // The style word after `as` / `in`, consumed from `s` at `i` when it is one.
  auto takeStyleWord = [&](bool afterAs) {
    size_t a = i;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = a;
    while (b < s.size() && std::isalpha(static_cast<unsigned char>(s[b]))) ++b;
    if (b == a || (b < s.size() && !std::isspace(static_cast<unsigned char>(s[b])) &&
                   s[b] != '"' && s[b] != '`' && s[b] != '_' && s[b] != '%' && s[b] != ','))
      return false;
    const std::string w = s.substr(a, b - a);
    const auto abbrevOf = [&w](const std::string& full, size_t minLen) {
      return w.size() >= minLen && w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
    };
    const bool ok = afterAs ? (w == "text" || w == "txt" || abbrevOf("result", 3) ||
                               abbrevOf("error", 3) || abbrevOf("input", 3))
                            : true;  // `in smcl`, and the old `in red`/`in gr`... colours
    if (ok) i = b;
    return ok;
  };
  // BUG-1345: every `%` word is a format to StataBE 19.5, and one it does not
  // take (isStataDisplayFormat) is r(120) `invalid %format` after the parts
  // already printed. `%10S`, `%-~10s`, `%10ss`, a bare `%-`, `%9.2F` and `%.2f`
  // printed their item at rc 0, `%abc` was r(111) and `%-~10s 3` rc 1. The
  // legacy `%d` family keeps its old paths: `%d` reads as a name and `%-d` as
  // a format applyStataFmtLocal refuses (BUG-185).
  auto isFmtToken = [](const std::string& token) {
    return !token.empty() && token[0] == '%' && token.rfind("%d", 0) != 0;
  };
  auto refuseFormat = [&]() {
    flushDirective();
    if (displayError.has_value()) return;
    partsBeforeError = parts.size();
    displayError = "r(120): invalid %format";
  };
  // A comma at depth 0 outside quotes (npos when none).
  auto topLevelComma = [](const std::string& token) {
    int depth = 0;
    size_t k = 0;
    while (k < token.size()) {
      const size_t q = skipQuotedSpan(token, k);
      if (q != k) {
        k = q;
        continue;
      }
      if (token[k] == '(' || token[k] == '[') ++depth;
      if (token[k] == ')' || token[k] == ']') --depth;
      if (token[k] == ',' && depth == 0) return k;
      ++k;
    }
    return std::string::npos;
  };
  // A directive token: true when consumed (its output is in `parts`).
  auto takeDirective = [&](const std::string& token) {
    static const std::regex reDir(R"(^(_[A-Za-z]+)(?:\(([^()]*)\))?$)");
    std::smatch dm;
    if (!std::regex_match(token, dm, reDir)) return false;
    const std::string name = dm[1].str();
    const bool hasArg = dm[2].matched;
    const auto abbrevOf = [&name](const std::string& full, size_t minLen) {
      return name.size() >= minLen && name.size() <= full.size() &&
             full.compare(0, name.size(), name) == 0;
    };
    const bool known = abbrevOf("_newline", 2) || (abbrevOf("_column", 4) && hasArg) ||
                       (name == "_char" && hasArg) || (abbrevOf("_continue", 2) && !hasArg) ||
                       (abbrevOf("_skip", 2) && hasArg) || (abbrevOf("_dup", 2) && hasArg) ||
                       (abbrevOf("_asis", 2) && !hasArg);
    if (!known) return false;  // `_request(...)` and any other name keep their paths
    std::optional<long long> arg;
    if (hasArg) {
      arg = directiveArg(dm[2].str());
      if (!arg.has_value()) return false;
    }
    if (abbrevOf("_newline", 2)) {
      flushDirective();
      parts.push_back(std::string(static_cast<size_t>(std::max<long long>(hasArg ? *arg : 1, 0)), '\n'));
      return true;
    }
    if (abbrevOf("_column", 4) && hasArg) {
      flushDirective();
      const size_t col = currentColumn();
      if (*arg > 1 && static_cast<size_t>(*arg - 1) > col)
        parts.push_back(std::string(static_cast<size_t>(*arg - 1) - col, ' '));
      return true;
    }
    if (name == "_char" && hasArg) {
      flushDirective();
      if (*arg > 0 && *arg < 128) parts.push_back(std::string(1, static_cast<char>(*arg)));
      applyDup();
      return true;
    }
    if (abbrevOf("_continue", 2) && !hasArg) {
      flushDirective();
      continueLine = true;
      return true;
    }
    if (abbrevOf("_skip", 2) && hasArg) {
      flushDirective();
      parts.push_back(std::string(static_cast<size_t>(std::max<long long>(*arg, 0)), ' '));
      return true;
    }
    if (abbrevOf("_dup", 2) && hasArg) {
      flushDirective();
      dupCount = std::max<long long>(*arg, 0);
      dupMark = parts.size();
      return true;
    }
    if (abbrevOf("_asis", 2) && !hasArg) {
      flushDirective();
      return true;
    }
    return false;
  };
  std::function<void(const std::string&)> takeToken = [&](const std::string& token) {
    if (token.empty() || displayError.has_value()) return;
    if (isFmtToken(token)) {
      if (token.rfind("%-d", 0) != 0 && !isStataDisplayFormat(token)) {
        refuseFormat();
        return;
      }
      flushDirective();
      pendingFmt = token;
      return;
    }
    // `,` displays one blank between two directives, `,,` none.
    const size_t comma = topLevelComma(token);
    if (comma != std::string::npos) {
      takeToken(token.substr(0, comma));
      if (displayError.has_value()) return;
      size_t k = comma;
      while (k < token.size() && token[k] == ',') ++k;
      flushDirective();
      if (k - comma == 1) parts.push_back(" ");
      takeToken(token.substr(k));
      return;
    }
    if ((token == "as" && takeStyleWord(true)) || (token == "in" && takeStyleWord(false))) {
      flushDirective();
      return;
    }
    if (takeDirective(token)) return;
    unquotedTokens.push_back(token);
  };

  // BUG-1345: a quoted literal is the item a pending format applies to, and it
  // uses it up: `%-20s "abc"` fills a 20-wide field, while a numeric format
  // leaves the text as it is (`%9.2f "abc" 3.14159` prints abc3.14159, StataBE
  // 19.5). The format used to stay pending and land on the next item, or vanish.
  auto formatLiteral = [&](std::string text) {
    if (!pendingFmt.has_value()) return text;
    if (std::regex_match(*pendingFmt, reStringFmtSpec())) text = padStringField(*pendingFmt, text);
    pendingFmt.reset();
    return text;
  };

  // A quoted literal right after a binary operator is that operator's operand,
  // not an item of its own: `display s[1+1] + "z"` is dez and `display c(os) ==
  // "MacOSX"` is 1 (StataBE 19.5). The expression before it was cut off at the
  // quote and printed as its own text (`"de" +z`, `"MacOSX" ==MacOSX`). An
  // operator with no operand before it (`display "a" + "b"`) keeps its path.
  auto takesQuotedOperand = [&]() {
    std::string pending;
    for (const auto& t : unquotedTokens) pending += t;
    const size_t opEnd = pending.find_last_not_of(" \t");
    if (opEnd == std::string::npos || std::strchr("+-*/^<>=&|", pending[opEnd]) == nullptr)
      return false;
    return pending.find_last_not_of("+-*/^<>=&|!~ \t", opEnd) != std::string::npos;
  };

  // Evaluation stops at the first failing piece, as Stata's does: a later
  // piece's runiform() draw or _request() prompt never runs.
  while (i < s.size() && !displayError.has_value()) {
    // BUG-1419: a string literal right after a binary operator is that
    // operator's operand, not a new display item: `display s1 + "x"` (string
    // scalar s1 "-3") prints -3x, and `t + "x" + t`, `e(cmd) + "z"`, `s[1] +
    // "z"`, `s[1] == "ab"`, `2 * "ab"` are one expression each (StataBE 19.5,
    // lane expr-sem x1b/a26.do). It printed `"-3" +x`. A pending format applies
    // to the whole expression (`display %9s s1 + "x"` is `      -3x`), and a
    // compound-quoted literal is an operand the same way (`display s1 +
    // `"q"a"'` is -3q"a). A literal that STARTS an item stays a display string,
    // as in Stata (`display "a" -5` is a-5), and so does one after an operator
    // with nothing on its left: `display "a" + "b"` is Stata's refusal (`+"b"
    // invalid name`, r(198)), never `a+ "b"`.
    if ((s[i] == '"' || (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"')) &&
        !unquotedTokens.empty()) {
      const std::string& prev = unquotedTokens.back();
      size_t j = std::string::npos;  // one past the literal
      if (s[i] == '"') {
        const size_t close = s.find('"', i + 1);
        if (close != std::string::npos) j = close + 1;
      } else {
        const size_t q = skipQuotedSpan(s, i);
        if (q >= i + 4 && s.compare(q - 2, 2, "\"'") == 0) j = q;
      }
      const bool leftOperand =
          unquotedTokens.size() > 1 || prev.find_first_not_of("+-*/^=<>&|!~") != std::string::npos;
      if (j != std::string::npos && !prev.empty() && leftOperand &&
          std::string("+-*/^=<>&|").find(prev.back()) != std::string::npos) {
        unquotedTokens.push_back(s.substr(i, j - i));
        i = j;
        continue;
      }
    }
    // EM4: a compound-quoted `"..."' piece prints its body verbatim, inner quotes
    // and nested spans included (w2af_macro_text_file.log:955 `<impute must be
    // 'all', "none">`, :761 `<x `"in "q""' y>`).
    if (i + 1 < s.size() && s[i] == '`' && s[i + 1] == '"') {
      const size_t j = skipQuotedSpan(s, i);
      if (j >= i + 4 && s.compare(j - 2, 2, "\"'") == 0 && takesQuotedOperand()) {
        unquotedTokens.push_back(s.substr(i, j - i));
        i = j;
        continue;
      }
      if (j >= i + 4 && s.compare(j - 2, 2, "\"'") == 0) {
        flushDirective();
        if (displayError.has_value()) break;
        parts.push_back(formatLiteral(s.substr(i + 2, j - i - 4)));
        applyDup();
        i = j;
        continue;
      }
      // BUG-1523: an unterminated compound quote runs to the end of the line
      // (StataBE 19.5: `display `"cq` prints `cq` at rc 0). The token scanner
      // below stops at the opening tick without taking a byte, so this piece
      // used to loop forever.
      flushDirective();
      if (displayError.has_value()) break;
      parts.push_back(formatLiteral(s.substr(i + 2)));
      break;
    }
    if (s[i] == '"' && takesQuotedOperand() && s.find('"', i + 1) != std::string::npos) {
      const size_t j = s.find('"', i + 1);
      unquotedTokens.push_back(s.substr(i, j + 1 - i));
      i = j + 1;
      continue;
    }
    if (s[i] == '"') {
      flushDirective();
      if (displayError.has_value()) break;
      size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        parts.push_back(formatLiteral(s.substr(i + 1)));
        break;
      }
      parts.push_back(formatLiteral(s.substr(i + 1, j - (i + 1))));
      applyDup();
      i = j + 1;
    } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    } else {
      // BUG-293(c): inside a PARENTHESIZED argument list a `"` is part of the
      // expression, not a segment delimiter. The scanner used to break on every
      // quote at any depth, so `display string(42.5,"%9.2f")` was shredded into
      // `string(42.5,` + a string part `%9.2f` + `)`, evalArith never saw a
      // callable expression, and display echoed the fragments back joined at rc
      // 0 (`string(42.5,%9.2f)`) where Stata prints `42.50`. Whitespace inside
      // the parens is swallowed for the same reason (`string(42.5, "%9.2f")`).
      // Deliberately one-sided: if the parens never close, the token is re-cut
      // under the ORIGINAL rule, so every unbalanced/degenerate line this
      // function ever handled stays byte-identical.
      // BUG-1352: a subscript's brackets count the same way, so the quoted row
      // name of `delta["d2",1]` is part of the token (StataBE prints 2); the
      // cut at its quote left `delta[` to fail as `delta not found`. They hold
      // their blanks too (BUG-1402): `NoSuchM[1, 1]` is one piece, r(111)
      // `NoSuchM not found` as without the blank, and `A[ 1, 2]` reads the
      // element, where the cut pieces echoed their own text at rc 0.
      size_t j = i;
      {
        int depth = 0;
        bool closed = true;
        while (j < s.size()) {
          const char c = s[j];
          if (c == '(' || c == '[') {
            ++depth;
          } else if (c == ')' || c == ']') {
            if (depth > 0) --depth;
          } else if (c == '`' && j + 1 < s.size() && s[j + 1] == '"') {
            // EM4: a compound literal inside an expression piece is one operand
            // (`strpos(`"a "b" c"', "b")`, w2af :1065 mt_fn_strpos_disp `<4>`).
            if (depth == 0) break;
            const size_t q = skipQuotedSpan(s, j);
            if (q < j + 4 || s.compare(q - 2, 2, "\"'") != 0) {  // unterminated
              closed = false;
              break;
            }
            j = q - 1;
          } else if (c == '"') {
            if (depth == 0) break;
            const size_t close = s.find('"', j + 1);
            if (close == std::string::npos) {  // unterminated — old rule wins
              closed = false;
              break;
            }
            j = close;
          } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (depth == 0) break;
          }
          ++j;
        }
        if (!closed || depth != 0) {
          j = i;
          while (j < s.size() && s[j] != '"' && !std::isspace(static_cast<unsigned char>(s[j]))) {
            ++j;
          }
        }
      }
      std::string token = s.substr(i, j - i);
      i = j;
      // BUG-1345: a format word runs to a blank, so one with a quote right after
      // it (`%9.2f"a"`) is no format either (StataBE r(120)).
      if (isFmtToken(token) && j < s.size() && (s[j] == '"' || s[j] == '`')) {
        refuseFormat();
        break;
      }
      takeToken(token);
    }
  }
  if (!displayError.has_value()) flushDirective();

  // BUG-088: an unsupported extended macro function was hit — fail loudly at the
  // point of failure instead of emitting confidently-wrong literal text.
  if (displayError.has_value()) {
    // BUG-293: an expression-layer error that already NAMES its Stata code
    // ("r(109): type mismatch" — evalArith's BUG-246/249 channel) must carry
    // that code into the block's `reason`, which is the only field
    // eval::coarseRc() reads as exact. Without it `capture noisily display
    // string("abc","%9.0g")` printed the right message and then answered
    // `_rc == 1` where Stata answers 109, so a do-file branching on the code
    // took the wrong branch off an error whose own text named it.
    //
    // Review finding NB1 (2026-09-09): this used to READ the prefix and leave
    // it in the message, so display was the one surface that printed
    // `r(109): type mismatch: ...` where the count/local/gen/if twins print
    // `type mismatch: ...` with the code carried in `reason` (Stata prints the
    // bare sentence). Measured before: `display strlen(e(nosuch))` ->
    // "r(109): type mismatch: `strlen(.)` ...", `display e(cmd)+1` ->
    // "r(109): type mismatch"; rc was 109 in both, so this was cosmetic
    // double-printing, not a wrong answer. The splitter is now the SHARED
    // eval::splitErrCode, so the prefix cannot be consumed two ways -- but its
    // r(111) DEFAULT belongs to the resolver's unprefixed channel, not to
    // display, so it is only consulted when a prefix is really there.
    std::optional<std::string> reason;
    std::string de = *displayError;
    if (de.rfind("r(", 0) == 0) {
      std::string stripped = de;
      const std::string code = eval::splitErrCode(&stripped);
      if (stripped != de) {  // splitErrCode only rewrites when the prefix parsed
        reason = code;
        de = stripped;
      }
    }
    ojson errBlock(eval::errorBlock("engine_error", de, std::nullopt, std::nullopt, std::nullopt,
                                    reason));
    std::string partial;
    for (size_t k = 0; k < partsBeforeError && k < parts.size(); ++k) partial += parts[k];
    if (!partial.empty()) errBlock["partial_text"] = partial;
    return errBlock;
  }

  std::string text;
  for (const auto& p : parts) text += p;

  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // `_continue`: the next display output joins this line (doexec's pushBlock).
  if (continueLine) out["continue"] = true;
  return out;
}

}  // namespace piet
