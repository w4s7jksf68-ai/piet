// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "expression_support.h"
#include "text_support.h"
#include "options.h"
#include "handler_declarations.h"

namespace piet::dispatch_detail {


// BUG-176 (P2-PREP): the `tempfile`/`tempname`/`tempvar` intercept body,
// lifted verbatim out of dispatchImpl's step (c)/(d) so the SAME matcher can
// run twice — once on the raw line (the literal path, unchanged) and once on
// the macro-EXPANDED line at step 0b-bis. Returns true when it consumed the
// line, writing the reply into `out`; false leaves `out` untouched and the
// caller falls through exactly as before.
//
// Duplicating the body instead would have been the third instance of this
// codebase's recurring one-sided-edit failure (BUG-158/BUG-161): two copies
// of one matcher drift, and the macro-indirected path silently stops
// agreeing with the literal one. One body, two call sites.
bool tryTempDeclIntercept(const std::string& line, SessionState& session,
                          const nlohmann::ordered_json& echo, nlohmann::ordered_json& out) {
  std::string u4Stripped = trimWs(line);
  size_t u4Sp = u4Stripped.find_first_of(" \t");
  std::string u4First = lower(u4Sp == std::string::npos ? u4Stripped : u4Stripped.substr(0, u4Sp));
  if (u4First != "tempfile" && u4First != "tempname" && u4First != "tempvar") return false;
  std::vector<std::string> names;
  if (u4Sp != std::string::npos) {
    std::istringstream namesIss(u4Stripped.substr(u4Sp));
    std::string nm;
    while (namesIss >> nm) names.push_back(nm);
  }
  nlohmann::ordered_json r = (u4First == "tempfile")   ? handlers::runTempfile(names, session)
                             : (u4First == "tempname") ? handlers::runTempname(names, session)
                                                       : handlers::runTempvar(names, session);
  if (r.is_array() && r.empty()) {
    out = nlohmann::ordered_json::array();
    return true;
  }
  out = nlohmann::ordered_json::array({echo, r});
  return true;
}


// BUG-513: `local NAME : display <display-args>` -- the extended macro function
// that stores what `display` would have printed (`local m : display _N` -> 12,
// tests/eval/golden/aer_w2/w2_smalls.log arm ext_display_N). It was the generic
// BUG-216 refusal, "display not allowed" r(101): honest, but it is how a do-file
// formats a number into a macro (`local b : display %5.3f _b[x]`), so every such
// line stopped its file.
//
// Answered HERE and not in eval::tryExtendedMacroFn because the answer IS
// handleDisplay's -- token resolution, %fmt, _b[]/e()/c()/scalar reads -- and
// that needs the full SessionState the eval layer deliberately does not see
// (same reason substituteCRefs runs at this call site). A second, thinner
// display inside the eval layer is the two-layer drift this file keeps paying
// for. One body, two call sites, like tryTempDeclIntercept above.
//
// The line is macro-expanded first (step 0a sees the RAW line), exactly what
// step 0b would have done to an ordinary `display` line. A display ERROR is
// returned as the block it is, reason and all, and the macro stays UNSET --
// BUG-216/217's rule for a failed colon form.
//
// The abbreviations are the display command's own (`di` .. `display`); only
// the full word is in the oracle.
bool tryExtDisplayCapture(const std::string& lineIn, SessionState& session,
                          nlohmann::ordered_json& out) {
  if (lineIn.find(':') == std::string::npos) return false;  // memchr-class gate
  {
    // Second gate, still no regex: only a `local`/`global` line can be this
    // form, and `by g:` / `bysort g:` prefixes are the colon lines that matter.
    const size_t h = lineIn.find_first_not_of(" \t");
    if (h == std::string::npos) return false;
    const std::string head = lower(lineIn.substr(h, 6));
    if (head.compare(0, 5, "local") != 0 && head != "global") return false;
  }
  static const std::regex reLoose(
      R"(^\s*(?:local|global)\s+\S+?\s*:\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_]|[^\x00-\x7F]))",
      std::regex::icase);
  if (!std::regex_search(lineIn, reLoose)) return false;
  std::string line = lineIn;
  if (eval::hasMacroRefs(line)) {
    eval::ExpandResult er = eval::expandMacros(line, session.eval);
    if (er.isError) {
      out = nlohmann::ordered_json::array({er.error});
      return true;
    }
    line = er.line;
  }
  // The macro name is eval::kRxName (a Unicode name is one name; StataBE takes
  // `local قيم : display 1` and even `local ١ : display 1`, rc 0 both,
  // scratch-r150/uni-d/badnames.log).
  static const std::regex reStrict(
      std::string(R"(^\s*(local|global)\s+()") + eval::kRxName +
          R"()\s*:\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_]|[^\x00-\x7F])(.*)$)",
      std::regex::icase);
  std::smatch m;
  if (!std::regex_match(line, m, reStrict)) return false;  // not this form after all
  // reStrict has THREE capture groups (kind, name, display args); the display
  // arguments are group 3. Group 4 does not exist and std::smatch answers ""
  // for it, which is how the first build stored an EMPTY macro at rc 0.
  nlohmann::ordered_json shown = handleDisplay(trimWs(m.str(3)), session);
  if (!shown.is_object() || shown.value("type", std::string()) != "display" ||
      !shown.contains("text") || !shown.at("text").is_string()) {
    if (shown.is_object()) shown.erase("partial_text");
    out = nlohmann::ordered_json::array({shown});
    return true;
  }
  const std::string text = shown.at("text").get<std::string>();
  if (lower(m.str(1)) == "local") {
    session.eval.locals.set(m.str(2), text);
  } else {
    session.eval.globals.set(m.str(2), text);
  }
  out = nlohmann::ordered_json::array();
  return true;
}


// The INLINE form of BUG-513's function: `` `: display <args>' `` anywhere in a
// line, `local m = strltrim("`: display %9.3f 0.25'")` (w2x_fn_string_math
// fx_strltrim_macro: "0.250"). Each span is replaced by what handleDisplay prints
// for its arguments, field padding included, innermost (rightmost) first, after
// the span's own macros are expanded. A display error is returned as its block.
// Every other inline `: fn' is left for the macro layer (eval/macros.cpp).
std::string substituteInlineDisplay(const std::string& lineIn, SessionState& session,
                                    std::optional<nlohmann::ordered_json>& err) {
  std::string line = lineIn;
  static const std::regex reHead(R"(^\s*(?:display|displa|displ|disp|dis|di)(?:\s+|$))",
                                 std::regex::icase);
  size_t searchEnd = line.size();
  for (int guard = 0; guard < 64; ++guard) {
    const size_t p = searchEnd == 0 ? std::string::npos : line.rfind("`:", searchEnd - 1);
    if (p == std::string::npos) break;
    size_t end = std::string::npos;
    int depth = 0;
    for (size_t i = p; i < line.size(); ++i) {
      if (line[i] == '`') ++depth;
      else if (line[i] == '\'' && --depth == 0) { end = i; break; }
    }
    std::smatch hm;
    const std::string body = end == std::string::npos ? std::string() : line.substr(p + 2, end - p - 2);
    if (end == std::string::npos || !std::regex_search(body, hm, reHead)) {
      searchEnd = p;
      continue;
    }
    std::string args = body.substr(static_cast<size_t>(hm.length(0)));
    if (eval::hasMacroRefs(args)) {
      eval::ExpandResult er = eval::expandMacros(args, session.eval);
      if (er.isError) {
        err = er.error;
        return lineIn;
      }
      args = er.line;
    }
    nlohmann::ordered_json shown = handleDisplay(trimWs(args), session);
    if (!shown.is_object() || shown.value("type", std::string()) != "display" ||
        !shown.contains("text") || !shown.at("text").is_string()) {
      // handleDisplay's internal partial_text (the pieces printed before the
      // failing one) is for a display statement's own caller; the macro the
      // inline display was to expand into shows nothing, as the `local q : display`
      // path above and the failed-display echo below erase it too.
      if (shown.is_object()) shown.erase("partial_text");
      err = shown;
      return lineIn;
    }
    line.replace(p, end - p + 1, shown.at("text").get<std::string>());
    searchEnd = p;
  }
  return line;
}


// The text a failed-expansion display line prints before its first piece
// whose macros do not expand (displayPartialBlock): the pieces are the segments
// handleDisplay itself evaluates one by one (a quoted string, a %fmt with the
// expression after it, a run of unquoted tokens). Prefixes are only EXPANDED
// to find the failing piece; the longest that expands is displayed once.
std::optional<std::string> displayExpandablePrefix(const std::string& args, SessionState& session) {
  if (args.find("`++") != std::string::npos || args.find("`--") != std::string::npos ||
      args.find("++'") != std::string::npos || args.find("--'") != std::string::npos ||
      args.find("_request") != std::string::npos || args.find("`\"") != std::string::npos ||
      args.find("\"'") != std::string::npos || args.find("`=") != std::string::npos)
    return std::nullopt;
  static const std::regex reAsMod(
      R"(^\s*as\s+(?:text|txt|result|res|error|err)(?:_(?:continue|n))?\s+)", std::regex::icase);
  size_t start = 0;
  {
    std::smatch m0;
    if (std::regex_search(args, m0, reAsMod) && m0.position(0) == 0)
      start = static_cast<size_t>(m0.length(0));
  }
  // Piece ends (offsets into args), cut where handleDisplay flushes its segments.
  std::vector<size_t> ends;
  bool fmtPending = false, inRun = false;
  size_t i = start;
  while (i < args.size()) {
    const char c = args[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '"') {
      if (inRun) {
        ends.push_back(i);
        inRun = false;
      }
      if (fmtPending) return std::nullopt;  // a format before a string literal
      const size_t close = args.find('"', i + 1);
      if (close == std::string::npos) return std::nullopt;
      i = close + 1;
      ends.push_back(i);
      continue;
    }
    size_t j = i;
    int depth = 0;
    while (j < args.size()) {
      const char d = args[j];
      if (d == '(') {
        ++depth;
      } else if (d == ')') {
        if (depth > 0) --depth;
      } else if (d == '"') {
        if (depth == 0) break;
        const size_t close = args.find('"', j + 1);
        if (close == std::string::npos) return std::nullopt;
        j = close;
      } else if (std::isspace(static_cast<unsigned char>(d)) && depth == 0) {
        break;
      }
      ++j;
    }
    if (depth != 0) return std::nullopt;
    const std::string tok = args.substr(i, j - i);
    static const std::regex reDirective(R"(^_(n|newline|continue|asis|skip|col|column|dup|char)\b.*)");
    if (tok == "as" || (tok[0] == '_' && std::regex_match(tok, reDirective))) return std::nullopt;
    const bool isFmt = tok.size() >= 2 && tok[0] == '%' &&
                       (std::isdigit(static_cast<unsigned char>(tok[1])) || tok[1] == 't' ||
                        tok[1] == '-');
    if (isFmt) {
      if (inRun) {
        ends.push_back(i);
        inRun = false;
      }
      if (fmtPending) return std::nullopt;
      fmtPending = true;
    } else {
      inRun = true;
      fmtPending = false;
    }
    i = j;
  }
  if (inRun) ends.push_back(args.size());
  if (ends.size() < 2) return std::nullopt;
  for (size_t k = ends.size() - 1; k-- > 0;) {
    std::string line = "display " + args.substr(start, ends[k] - start);
    if (eval::hasMacroRefs(line)) {
      eval::ExpandResult er = eval::expandMacros(line, session.eval);
      if (er.isError) continue;
      line = er.line;
    }
    const nlohmann::ordered_json shown = handleDisplay(rawArgsAfterCmd(line), session);
    if (!shown.is_object() || shown.value("type", std::string()) != "display" ||
        !shown.contains("text") || !shown.at("text").is_string())
      return std::nullopt;
    return shown.at("text").get<std::string>();
  }
  return std::nullopt;
}


// A `display` that fails part-way. Stata writes each piece as it evaluates it,
// so the pieces before the failing one stay on the line and the error's message
// follows them on the same line (StataBE 19.5, w2v_margins_corpus:
// `PREC|F7_dydx_minmax|121|N=.|k=type mismatch` for a colsof(r(b)) with r(b)
// absent; `PREC|F7nb_dcat_dydx_post_bname1|0|1.tt#0.dcat|[1.tt#0.dcat] not found`
// for an unknown _b[] name). PIET answered the error block alone, so the line
// was lost. `err` is the error block the line produced; the result is the
// display block to show BEFORE it, or nothing. No piece is ever evaluated twice
// (a runiform() draw or an inline `=exp' would run again):
//  - handleDisplay failed (`evaluated`): it stopped at the failing piece and
//    left the text printed before it on the block as `partial_text`.
//  - macro expansion failed (PIET resolves r() there; Stata reads it in
//    display's own evaluation): display never ran. The longest piece prefix
//    that expands is displayed once. Only lines whose expansion has no side
//    effect are taken (no `++`/`--` macro, no inline `=exp', no _request(), no
//    display directive or mid-line `as`), and a prefix that itself fails gives
//    nothing (the error block already carries the message).
// The message is Stata's sentence: PIET's text up to its first ": " detail.
std::optional<nlohmann::ordered_json> displayPartialBlock(const std::string& args,
                                                          const nlohmann::ordered_json& err,
                                                          SessionState& session, bool evaluated) {
  if (!err.is_object() || err.value("type", std::string()) != "error" || !err.contains("message") ||
      !err.at("message").is_string())
    return std::nullopt;
  std::optional<std::string> printed;
  if (evaluated) {
    if (err.contains("partial_text") && err.at("partial_text").is_string())
      printed = err.at("partial_text").get<std::string>();
  } else {
    printed = displayExpandablePrefix(args, session);
  }
  if (!printed.has_value() || printed->empty()) return std::nullopt;
  std::string msg = err.at("message").get<std::string>();
  if (msg.rfind("r(", 0) == 0) {
    const size_t close = msg.find("): ");
    if (close != std::string::npos) msg = msg.substr(close + 3);
  }
  const size_t detail = msg.find(": ");
  if (detail != std::string::npos) msg = msg.substr(0, detail);
  nlohmann::ordered_json out;
  out["type"] = "display";
  out["text"] = *printed + msg;
  return out;
}


bool isDisplayLine(const std::string& raw) {
  static const std::regex reDisplay(R"(^\s*(?:display|displa|displ|disp|dis|di)(?![A-Za-z0-9_]|[^\x00-\x7F]))");
  return std::regex_search(raw, reDisplay);
}


// FU-602: `c_local NAME [= exp | : fn | text]` sets NAME in the CALLER's local
// scope ([P] macro: "c_local ... sets a local macro in the caller's space";
// oracle w2_esample_copies.do:39-40, the helper program pcnt returns its count
// through it). The definition is evaluated exactly as `local` would evaluate it
// in the CURRENT scope -- the same tryCaptureDefinition call, so `_rc`, r() and
// the macros on the right-hand side read what this program sees -- and the
// value is then moved to the caller's frame (session.scopeStack.back(), the
// frame program.cpp's ScopeGuard and doexec's PrivateLocalScope push), leaving
// this scope's own NAME as it was. With no caller frame (the top level of the
// session) there is no caller to write to: a named r(198). Returns false when
// the line is not a c_local line.
bool tryCLocal(const std::string& rawLine, SessionState& session, nlohmann::ordered_json& out) {
  const std::string t = strutil::trim(rawLine);
  if (t.size() < 7 || lower(t.substr(0, 7)) != "c_local") return false;
  if (t.size() > 7 && std::isspace(static_cast<unsigned char>(t[7])) == 0) return false;
  auto refuse = [&](const std::string& msg) {
    out = nlohmann::ordered_json::array({ojson(eval::errorBlock(
        "syntax_error", msg, std::string("c_local"), std::nullopt, t, "r(198)"))});
    return true;
  };
  if (session.scopeStack.empty()) {
    return refuse("c_local sets a local in the caller's scope; it is valid only inside a "
                  "program or a do-file run by another one");
  }
  std::string rest = strutil::trim(t.substr(7));
  auto identLen = [](const std::string& r) {  // bytes; a Unicode name stays whole
    size_t j = 0;
    if (j < r.size() && cecon::ident::isIdentStartByte(r[j])) {
      while (j < r.size() && cecon::ident::isIdentByte(r[j])) ++j;
    }
    return j;
  };
  // The name may arrive through a macro (`c_local `name' = ...`): expand the
  // line in this scope first, as step 0b-bis does for a macro-named `local`.
  if (identLen(rest) == 0 && eval::hasMacroRefs(rest)) {
    eval::ExpandResult er = eval::expandMacros(rest, session.eval);
    if (er.isError) {
      out = nlohmann::ordered_json::array({er.error});
      return true;
    }
    rest = strutil::trim(er.line);
  }
  const size_t nl = identLen(rest);
  if (nl == 0 || cecon::ident::codePointCount(std::string_view(rest).substr(0, nl)) > 31 ||
      (nl < rest.size() && std::isspace(static_cast<unsigned char>(rest[nl])) == 0 &&
       rest[nl] != '=' && rest[nl] != ':')) {
    return refuse("c_local: " + (rest.empty() ? std::string("a macro name is required")
                                              : "'" + rest.substr(0, rest.find(' ')) +
                                                    "' is not a valid macro name"));
  }
  const std::string name = rest.substr(0, nl);
  std::optional<std::string> prev;
  if (const std::string* p = session.eval.locals.find(name)) prev = *p;
  // NAME is not cleared first: the right-hand side may read it (`c_local x `x'`).
  const std::string defLine = "local " + name + (nl < rest.size() ? " " + rest.substr(nl) : "");
  eval::MacroFnError mfe;
  // `c_local NAME : display ...` is the `local` form's own display (BUG-513);
  // StataBE 19.5: `c_local c2 : display %5.2f 3.14159` gives the caller " 3.14".
  nlohmann::ordered_json shown;
  const bool viaDisplay = tryExtDisplayCapture(defLine, session, shown);
  const bool consumed =
      viaDisplay ||
      eval::tryCaptureDefinition(substituteCRefs(defLine, session), session.eval, &mfe);
  std::optional<std::string> val;
  if (const std::string* v = session.eval.locals.find(name)) val = *v;
  if (prev.has_value()) session.eval.locals.set(name, *prev);
  else session.eval.locals.erase(name);
  if (viaDisplay && !shown.empty()) {  // display's own error; the caller's NAME is untouched
    out = shown;
    return true;
  }
  if (!consumed) return refuse("c_local: cannot read the definition of " + name);
  if (mfe.failed()) {
    out = nlohmann::ordered_json::array({ojson(eval::errorBlock(
        "engine_error", mfe.message, std::nullopt, std::nullopt, t, "r(" + mfe.rc + ")"))});
    return true;
  }
  if (val.has_value()) session.scopeStack.back().set(name, *val);
  else session.scopeStack.back().erase(name);
  out = nlohmann::ordered_json::array();
  return true;
}


// M2 scope (ROADMAP "M2 — glue + vertical slice") ... M3/M4/BETA extension
// point comment continues to apply unchanged (see this file's top-of-file
// doc comment) — this function is exactly what used to be named
// `dispatch()` before BS-006 (BUG-028's real log write) split it in two.
// The public `dispatch()` (defined at the very bottom of this file, after
// this function's closing brace) is now a thin log-teeing wrapper; this is
// still the ~2470-line M2/M3/M4/BETA routing body, completely unchanged
// otherwise. Every recursive self-call inside this function's own body
// still literally says `dispatch(...)` (not `dispatchImpl(...)`) — quite
// deliberately, so nested calls (capture/quietly/noisily/by/bysort prefix-
// stripping; the eststo/version bare-command forwards; and, via the
// `dispatch` function pointer handed to doexec::executeDoSource/executeDo
// and program::runProgram below, every line of a replayed do-file/loop/
// program body) all flow through the SAME wrapper as any other call —
// see dispatch()'s own doc comment for why that matters.
// ---------------------------------------------------------------------------
// BUG-428 / BUG-432 — THE ROW-PREDICATE SUBSTITUTION POINT.
//
// `local a = M[1,1]` reads a matrix element and `display s` reads a stored
// scalar, because the macro/expression layer substitutes both before evaluating.
// The ROW layer — `assert`, `count if`, `keep if`, the `if` of a `gen`, and
// gen's own expression — never got that pass, so `assert Q[1,1] == 1` came back
// "unknown VARIABLE Q" for a matrix defined on the line above, and
// `gen x = y * scalar(s)` was rc 1 while `display s` printed 3. Two ids, one
// missing pass ([[two-evaluator-layers-predicate-split]]).
//
// Both halves are no-ops on text that names no defined matrix and no defined
// scalar, which keeps the blast radius to expressions that fail today: nothing
// that already works can start substituting.
//
// A VARIABLE always wins over a same-named scalar — Stata's rule, and the reason
// this cannot simply loop `substituteBareName` over the scalar store the way
// evalArith does: evalArith has no frame in hand, and here we do.
// BUG-456 (2026-09-08) — the row plane's CONSTANT STRING COMPARE fold.
//
// The engine's data plane has no string type: its `--keepif` accepts
// `factorvar ==/!= "literal"` and nothing else (src/data/frame.cpp), and
// CiParser bails on any string operand. Stata's row plane takes a constant
// string compare everywhere -- `assert e(cmd)=="regress"` rc 0, `gen z =
// (e(cmd)=="regress")` mean 1, `count if e(cmd)=="nope"` 0 (capture.log
// A1/G1/G10) -- and after the resolver above has turned e(cmd) into `"regress"`
// the claim is a constant. So the constant is decided HERE, before the engine
// sees it: `"lit" op "lit"` folds to 1/0 for ==, !=, ~=, <, <=, >, >= (Stata
// orders strings bytewise, refute3 Q1-Q3), and `"lit" op <number|.>` (either
// order) is r(109) "type mismatch" (A9/A10/D10/N1/M1-M7), never a silent 0.
// Anything with a variable, a function call or a non-constant operand on
// either side is left exactly as it was -- CiParser / the engine own it.
//
// Quote-aware by construction: the operator is found with eval::searchUnquoted
// (tokenizer.h), never a quote-blind regex ([[piet-quote-aware-qualifier]]),
// and each operand is classified from the characters beside the operator.
// NAMED GAP (2026-09-09, review finding 11): the operand scanner below bounds a
// literal with a bare rfind('"')/find('"') and does NOT understand the `\"`
// escaping resolveERef (macros.cpp) and resolveEStringLiteral
// (display_hardening.cpp) emit. Unreachable today -- measured: PIET refuses
// compound double quotes, so no e-string can carry a `"` -- so this is a pin
// owed whenever that gap closes, not a live defect.
// String FUNCTIONS on a constant (`inlist(e(cmd),"a")`, `strpos(e(cmdline),
// "x")`) are NOT folded here; they take the strmutate detour when it can
// evaluate them and are refused honestly by it when it cannot -- named scope,
// not claimed by this pass.


bool rowConstNumberToken(const std::string& t) {
  static const std::regex reNum(R"(^([0-9]+\.?[0-9]*|\.[0-9]+|\.)([eE][+-]?[0-9]+)?$)");
  return !t.empty() && std::regex_match(t, reNum);
}


// A byte >= 0x80 is a name byte (core/ident.h): the `2` of `ق2 == "x"` is the
// tail of the string variable ق2, not a number compared with a string (r(109)).
bool rowOperandBoundaryLeft(char c) {  // char just BEFORE a constant operand
  return !(cecon::ident::isIdentByte(c) || c == '"' || c == ')' || c == ']' || c == '.');
}

bool rowOperandBoundaryRight(char c) {  // char just AFTER a constant operand
  return !(cecon::ident::isIdentByte(c) || c == '"' || c == '(' || c == '[' || c == '.');
}


RowConstOperand rowLeftOperand(const std::string& e, size_t opPos) {
  RowConstOperand o;
  size_t l = opPos;
  while (l > 0 && std::isspace(static_cast<unsigned char>(e[l - 1]))) --l;
  if (l == 0) return o;
  if (e[l - 1] == '"') {
    if (l < 2) return o;
    const size_t q = e.rfind('"', l - 2);
    if (q == std::string::npos) return o;
    o.kind = 1;
    o.begin = q;
    o.end = l;
    o.text = e.substr(q + 1, l - 2 - q);
  } else {
    size_t k = l;
    auto isTokChar = [](char c) { return cecon::ident::isIdentByte(c) || c == '.'; };
    while (k > 0 && isTokChar(e[k - 1])) --k;
    // exponent sign: `1e+307` scanned back to `307`; extend over the `e+`.
    if (k >= 2 && (e[k - 1] == '+' || e[k - 1] == '-') && (e[k - 2] == 'e' || e[k - 2] == 'E')) {
      k -= 2;
      while (k > 0 && isTokChar(e[k - 1])) --k;
    }
    const std::string tok = e.substr(k, l - k);
    if (!rowConstNumberToken(tok)) return o;
    o.kind = 2;
    o.begin = k;
    o.end = l;
    o.text = tok;
  }
  if (o.begin > 0 && !rowOperandBoundaryLeft(e[o.begin - 1])) o.kind = 0;
  return o;
}


RowConstOperand rowRightOperand(const std::string& e, size_t opEnd) {
  RowConstOperand o;
  size_t r = opEnd;
  while (r < e.size() && std::isspace(static_cast<unsigned char>(e[r]))) ++r;
  if (r >= e.size()) return o;
  if (e[r] == '"') {
    const size_t q = e.find('"', r + 1);
    if (q == std::string::npos) return o;
    o.kind = 1;
    o.begin = r;
    o.end = q + 1;
    o.text = e.substr(r + 1, q - r - 1);
  } else {
    static const std::regex reNumHead(R"(^([0-9]+\.?[0-9]*|\.[0-9]+|\.)([eE][+-]?[0-9]+)?)");
    std::smatch m;
    const std::string tail = e.substr(r);
    if (!std::regex_search(tail, m, reNumHead)) return o;
    o.kind = 2;
    o.begin = r;
    o.end = r + static_cast<size_t>(m.length(0));
    o.text = m.str(0);
  }
  if (o.end < e.size() && !rowOperandBoundaryRight(e[o.end])) o.kind = 0;
  return o;
}


// Folds in place. Returns false (and fills *mismatch) on a string-vs-number
// compare -- the caller turns that into the r(109) block.
bool foldRowConstStringCompares(std::string& e, std::string* mismatch) {
  if (e.find('"') == std::string::npos) return true;  // nothing constant-string to fold
  static const std::regex reCmp(R"(==|!=|~=|<=|>=|<|>)");
  size_t from = 0;
  while (from < e.size()) {
    std::smatch m;
    const std::string tail = e.substr(from);
    if (!eval::searchUnquoted(tail, reCmp, m)) break;
    const size_t opPos = from + static_cast<size_t>(m.position(0));
    const std::string op = m.str(0);
    const size_t opEnd = opPos + op.size();
    const RowConstOperand L = rowLeftOperand(e, opPos);
    const RowConstOperand R = rowRightOperand(e, opEnd);
    if (L.kind == 0 || R.kind == 0) {  // a variable / call / non-constant: not ours
      from = opEnd;
      continue;
    }
    if (L.kind != R.kind) {
      *mismatch = e.substr(L.begin, R.end - L.begin);
      return false;
    }
    // BUG-1419: an arithmetic operator binds tighter than a comparison, so a
    // literal beside one is only part of its operand: in `s1 + "x" == "-3x"` the
    // left side is s1 + "x", not "x" (StataBE 19.5 counts every row; the fold
    // made it s1 + 0, a type mismatch). The operand's type is still the
    // literal's, so the mismatch above stands (`1 + 2 == "3"` is r(109)).
    const auto isArith = [](char c) { return std::string("+-*/^").find(c) != std::string::npos; };
    size_t lb = L.begin, ra = R.end;
    while (lb > 0 && std::isspace(static_cast<unsigned char>(e[lb - 1]))) --lb;
    while (ra < e.size() && std::isspace(static_cast<unsigned char>(e[ra]))) ++ra;
    const bool partial = (lb > 0 && isArith(e[lb - 1])) || (ra < e.size() && isArith(e[ra]));
    if (L.kind == 2 || partial) {  // number op number, or part of an operand: untouched
      from = opEnd;
      continue;
    }
    // A literal an arithmetic operator binds first is not the compare's whole
    // operand: `s + "c" == "bbc"` compares s + "c", and folding `"c" == "bbc"`
    // left `s + 0`, r(109) where StataBE answers. Its kind still decides a
    // string-vs-number compare above.
    {
      size_t lb = L.begin;
      while (lb > 0 && std::isspace(static_cast<unsigned char>(e[lb - 1]))) --lb;
      size_t ra = R.end;
      while (ra < e.size() && std::isspace(static_cast<unsigned char>(e[ra]))) ++ra;
      static const std::string kArith = "+-*/^";
      if ((lb > 0 && kArith.find(e[lb - 1]) != std::string::npos) ||
          (ra < e.size() && kArith.find(e[ra]) != std::string::npos)) {
        from = opEnd;
        continue;
      }
    }
    const int c = L.text.compare(R.text);
    bool v = false;
    if (op == "==") v = (c == 0);
    else if (op == "!=" || op == "~=") v = (c != 0);
    else if (op == "<") v = (c < 0);
    else if (op == "<=") v = (c <= 0);
    else if (op == ">") v = (c > 0);
    else v = (c >= 0);
    e.replace(L.begin, R.end - L.begin, v ? "1" : "0");
    from = L.begin + 1;
  }
  return true;
}


// The name an expression that failed to evaluate is missing, as Stata reports
// it: the first bare name, a name before `(` being a function call (`exit
// 1+nosuch` and `summarize if nosuch==1` are r(111) "nosuch not found").
std::optional<std::string> firstBareName(const std::string& expr) {
  static const std::regex reName(R"(\b([A-Za-z_]\w*)\b(?!\s*\())");
  std::smatch nm;
  if (!std::regex_search(expr, nm, reName)) return std::nullopt;
  return nm.str(1);
}


// The stored-SCALAR substitution the row plane has always done, lifted out of
// substituteRowExprRefs unchanged (BUG-456 follow-up, 2026-09-09) so gen's
// pre-resolve can run the same substitution on a COPY, purely to DECIDE the
// type of an expression it must not otherwise rewrite. A VARIABLE always wins
// over a same-named scalar -- Stata's rule, and the reason this asks the read
// card before substituting.
//
// name-binding-display-literals: text inside a string literal is data. The
// pass used to run over the raw text, so a scalar whose name occurred inside a
// literal was substituted into it: with `scalar control = 5`, `count if grp ==
// "control"` compared with "5" and counted 0 of 3, `assert tag == "control"`
// failed rc 9, and a string scalar's own inserted literal (`st` -> "treat") was
// substituted again by a scalar `treat` (w2av_name_binding bd_row_cntlit,
// bd_row_cntT, bd_row_keep, bd_row_dropT, bd_row_strsc, bd_row_asrt). The
// literals are protected once (eval::protectLiterals) and a string scalar's
// value goes in as one more protected literal.
std::string substituteStoredScalars(const std::string& in, SessionState& session) {
  if (session.eval.scalars.items().empty()) return in;
  std::vector<std::string> spans;
  std::string e = eval::protectLiterals(in, &spans);
  const std::vector<VarInfo>* card = session.residentReadCard();
  for (const auto& [sn, sv] : session.eval.scalars.items()) {
    if (e.find(sn) == std::string::npos) continue;
    bool isVar = false;
    if (card != nullptr) {
      for (const auto& vi : *card)
        if (vi.name == sn) { isVar = true; break; }
    } else if (session.currentHbin().has_value()) {
      Datastore* ds0 = session.datastore();
      isVar = ds0 != nullptr && ds0->has(sn);
    }
    if (isVar) continue;
    // BUG-710: a STRING scalar (`scalar st = s[2]`) is a string literal here,
    // not its bare text: `assert st == "ab2"` reached the engine as `ab2 ==
    // "ab2"`, an unknown variable. Text holding a `"` keeps the old form.
    if (session.eval.isStringScalar(sn) && sv.find('"') == std::string::npos) {
      spans.push_back("\"" + sv + "\"");
      e = eval::substituteBareName(e, sn, "\"0L" + std::to_string(spans.size() - 1) + "L0\"");
      continue;
    }
    e = eval::substituteBareName(e, sn, eval::valueRefText(sv));  // FU-842
  }
  return eval::restoreLiterals(e, spans);
}


// While a command whose `if` does not read the caller's `_rc` runs with `_rc`
// in that `if`, the lines its handler dispatches with the same qualifier do
// not read it either: recode hands its `if` to generate/replace, which would
// otherwise answer it with the caller's value (`recode x (1=10) if _rc == 111,
// gen(rx)` filled rx[1] = 10; StataBE leaves it missing).
thread_local int tl_rcHiddenDepth = 0;


// FU-169 / BUG-1359: Stata reads the `_rc` system value inside an expression
// (`assert _rc == 0`, aer_estim_oracle.log:412-414; `generate q = _rc`, `keep if
// _rc == 0`, `summarize x1 if _rc == 111`). No row evaluator has the name, so it
// is resolved here, once, for the row callers: the `if` of the commands that
// read the caller's value (ifReadsCallerRc), assert's claim, and
// generate/replace's right-hand side (preResolveGenExpr). Until BUG-1359 only
// runCount did this, so `count if _rc` and `assert _rc` were served and
// gen/replace/keep/drop/summarize reached the engine with `unknown variable
// _rc`. Whole identifier only (substituteBareName's \b rule), never inside a
// "..." literal, and a dataset variable named _rc still wins.
std::string substituteRcValue(const std::string& in, SessionState& session) {
  if (tl_rcHiddenDepth > 0 || in.find("_rc") == std::string::npos) return in;
  const std::string rc = session.eval.locals.get("_rc", "0");
  if (rc.empty() || rc.find_first_not_of("0123456789") != std::string::npos) return in;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card)
      if (vi.name == "_rc") return in;
  } else if (session.currentHbin().has_value()) {
    Datastore* ds0 = session.datastore();
    if (ds0 != nullptr && ds0->has("_rc")) return in;
  }
  std::vector<std::string> spans;
  const std::string e = eval::protectLiterals(in, &spans);
  return eval::restoreLiterals(eval::substituteBareName(e, "_rc", rc), spans);
}


// Whether a command's `if` reads the caller's `_rc`. In Stata many commands run
// captures of their own before they read the `if`, and so see another value:
// after `capture confirm variable nosuch` (rc 111), `regress y x if _rc == 0`,
// `mean`, `pctile`, `egen ... if` and `by g: egen ... if` fit or fill every row
// and `if _rc == 111` is r(2000) or no row, while `recode`, `table` and `ci`
// see neither 0 nor 111 (StataBE 2026-10-29). These are the commands measured
// to see 111. Every other command's `_rc` is left as text, the engine's r(111)
// "unknown variable _rc", as before BUG-1359: an answer read with the caller's
// value would be a silent wrong one.
bool ifReadsCallerRc(const std::string& canonicalCmd) {
  static const std::unordered_set<std::string> kReadsCallerRc = {
      "generate", "replace", "keep",     "drop",   "count",  "assert",
      "summarize", "list",   "tabulate", "tabstat", "corr",  "pwcorr",
      "xtile",    "ttest",   "centile",  "encode",  "duplicates"};
  return kReadsCallerRc.count(canonicalCmd) != 0;
}


// BUG-1327 / BUG-1093: `assert s[1] == "ab"` and `assert r0[1] == ""` read ONE
// cell of a string variable. Neither row evaluator has that form -- the engine
// has no string operand, the PIET row evaluator no `[` -- so the claim was the
// engine's `expr: expected )` (rc 198 under capture) where StataBE answers. A
// subscript whose index is a constant (digits, `_N`, arithmetic on them) is the
// same value on every row, so it is read here, through display's reader
// (readVarObservation), and goes in as a string literal; the claim then takes
// the string-condition route like any other. Stata's rules (oracle 2026-10-29):
// the index truncates toward zero (`s[1.6]` is s[1], `s[2.5]` is s[2]) and an
// index outside 1.._N is "" (`s[0]`, `s[9]`, `s[-1]`, `s[.]`). Left in place: an index
// naming anything but `_N` (`s[_n-1]` varies by row), a numeric variable (the
// engine reads x[k] itself), a matrix, and text holding a `"`. assert only: it
// is refused under by:, where a subscript counts within the group.
std::string substituteStringCells(const std::string& in, SessionState& session) {
  if (in.find('[') == std::string::npos || !session.hasData()) return in;
  std::vector<std::string> spans;
  const std::string e = eval::protectLiterals(in, &spans);
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  std::string out;
  bool any = false;
  size_t i = 0, last = 0;
  int depth = 0;  // parentheses around the current position
  while (i < e.size()) {
    const char c = e[i];
    if (c == '"') {  // a placeholder: data
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
      continue;
    }
    if (!isId0(c) || (i > 0 && (isId(e[i - 1]) || e[i - 1] == '.'))) {
      depth += c == '(' ? 1 : c == ')' ? -1 : 0;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < e.size() && isId(e[j])) ++j;
    // A blank before the `[` keeps it a subscript only inside parentheses:
    // StataBE answers `assert (s [1] == "a")` and refuses `assert s [1] == "a"`
    // r(109), so the bare form stays the engine's refusal.
    size_t k = j;
    while (depth > 0 && k < e.size() && (e[k] == ' ' || e[k] == '\t')) ++k;
    const size_t close = k < e.size() && e[k] == '[' ? e.find(']', k + 1) : std::string::npos;
    if (close == std::string::npos) {
      i = j;
      continue;
    }
    const size_t start = i;
    const std::string name = e.substr(i, j - i);
    const std::string idxText = e.substr(k + 1, close - k - 1);
    i = j;
    if (idxText.find_first_of("[,\"") != std::string::npos ||
        session.eval.findMatrix(name) != nullptr)
      continue;
    // A number (`1e0`) and a missing value (`.`, `.a`) are constants too; any
    // other name but `_N` is not.
    bool constIdx = true;
    for (size_t p = 0; p < idxText.size() && constIdx;) {
      const char ch = idxText[p];
      if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
        size_t q = p;
        while (q < idxText.size() &&
               (std::isdigit(static_cast<unsigned char>(idxText[q])) || idxText[q] == '.'))
          ++q;
        if (q < idxText.size() && (idxText[q] == 'e' || idxText[q] == 'E')) {
          size_t x = q + 1;
          if (x < idxText.size() && (idxText[x] == '+' || idxText[x] == '-')) ++x;
          if (x < idxText.size() && std::isdigit(static_cast<unsigned char>(idxText[x]))) {
            q = x;
            while (q < idxText.size() && std::isdigit(static_cast<unsigned char>(idxText[q]))) ++q;
          }
        } else if (q == p + 1 && ch == '.' && q < idxText.size() && idxText[q] >= 'a' &&
                   idxText[q] <= 'z' && (q + 1 == idxText.size() || !isId(idxText[q + 1]))) {
          ++q;  // `.a`
        }
        p = q;
        continue;
      }
      if (!isId0(ch)) {
        ++p;
        continue;
      }
      size_t q = p;
      while (q < idxText.size() && isId(idxText[q])) ++q;
      constIdx = idxText.compare(p, q - p, "_N") == 0;
      p = q;
    }
    if (!constIdx) continue;
    // A column the card lists as numeric is the engine's (`assert x[1] == 5`),
    // known without opening the frame, which readVarObservation would do.
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      const std::optional<std::string> full = resolveVarName(name, session);
      if (!full) continue;
      const auto it = std::find_if(card->begin(), card->end(),
                                   [&](const VarInfo& vi) { return vi.name == *full; });
      if (it != card->end() && it->dtype != "factor") continue;
    }
    bool idxIsStr = false;
    const std::optional<std::string> idxVal =
        eval::evalArith(idxText, &session.eval, nullptr, &idxIsStr);
    if (!idxVal || idxIsStr) continue;
    // A missing index names no observation: "" (StataBE `s[.] == ""` is true).
    const bool missingIdx = !idxVal->empty() && (*idxVal)[0] == '.' &&
                            (idxVal->size() == 1 ||
                             (idxVal->size() == 2 && (*idxVal)[1] >= 'a' && (*idxVal)[1] <= 'z'));
    char* endp = nullptr;
    const double d = missingIdx ? 0.0 : std::strtod(idxVal->c_str(), &endp);
    if (!missingIdx &&
        (endp == idxVal->c_str() || *endp != '\0' || !std::isfinite(d) || std::fabs(d) > 9e15))
      continue;
    const std::optional<VarObservation> cell =
        readVarObservation(name, static_cast<long long>(std::trunc(d)), session);
    if (!cell || !cell->isString || cell->text.find('"') != std::string::npos) continue;
    spans.push_back("\"" + (cell->inRange ? cell->text : std::string()) + "\"");
    out.append(e, last, start - last);
    out += "\"0L" + std::to_string(spans.size() - 1) + "L0\"";
    last = close + 1;
    i = close + 1;
    any = true;
  }
  if (!any) return in;
  out.append(e, last, std::string::npos);
  return eval::restoreLiterals(out, spans);
}


// name-binding-display-literals: a unique abbreviation of a variable in a row
// expression is that variable, as in Stata ([U] 11.2), and it wins over a
// stored scalar of the same name, as the exact name does: `count if lng > 3`
// and `generate g = lng*2` (variable lngdp) were r(111) "unknown variable"
// from the engine (w2av_name_binding bd_row_abbr, bd_row_genab). The names are
// rewritten to the full name before the scalar pass; quoted text, calls
// (`name(`), missing codes (`.a`) and label names (`"x":lbl`) are left alone, and an ambiguous
// abbreviation stays as typed for the engine's own r(111). The variable lookup
// is display's (resolveVarName), so display, the macro layer and the row plane
// bind a name by one rule.
std::string expandRowVarAbbrevs(const std::string& in, SessionState& session) {
  if (!session.hasData()) return in;
  std::vector<std::string> spans;
  const std::string e = eval::protectLiterals(in, &spans);
  // core/ident.h's scanner tests: a Unicode name (`دخل_2020`) is one name, so its
  // ASCII tail `_2020` is never bound on its own.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  std::string out;
  bool any = false;
  size_t i = 0, last = 0;
  while (i < e.size()) {
    const char c = e[i];
    if (c == '"') {  // a placeholder (or an unterminated quote): data
      const size_t close = e.find('"', i + 1);
      i = close == std::string::npos ? e.size() : close + 1;
      continue;
    }
    // `.a` is a missing code and `"x":lbl` a value-label lookup, not names.
    if (!isId0(c) || (i > 0 && (isId(e[i - 1]) || e[i - 1] == '.' || e[i - 1] == ':'))) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < e.size() && isId(e[j])) ++j;
    size_t k = j;
    while (k < e.size() && (e[k] == ' ' || e[k] == '\t')) ++k;
    if (k < e.size() && e[k] == '(') {
      i = j;
      continue;
    }
    const std::string name = e.substr(i, j - i);
    const std::optional<std::string> full = resolveVarName(name, session);
    if (full && *full != name) {
      out.append(e, last, i - last);
      out += *full;
      last = j;
      any = true;
    }
    i = j;
  }
  if (!any) return in;
  out.append(e, last, std::string::npos);
  return eval::restoreLiterals(out, spans);
}


// e(sample) T2 (internal note esample/SPEC.md §5.1): the ROW
// reading of the token `e(sample)`, outside string literals (eval::
// replaceEsampleToken). One reading for every row caller, decided by
// SessionState::esampleReading:
//   kColumn  -> `(__piet_esample__==SERIAL)`: the hidden mask column the fit
//               installed, whose admitted rows hold that install's serial
//               (rows added since the fit hold missing -> 0, which is
//               Stata's answer for them, w2_esample.log:1076 E6, :1104 E7);
//   kZero    -> `0`: no estimation results, a posted e(), or a model whose
//               column left with the data (w2_esample.log:95-129 A1-A5, :1174
//               E9, :1215 E11, :1272 E14, :1290 E15, :1487 G5, :1634 G11);
//   kRefused -> the named r(198) of SPEC §5.5 (a model PIET has no mask for);
//               `errOut` gets the block and the text is returned unchanged.
// Because this runs BEFORE resolveEstScalarRefs, the resolver's own e(sample)
// arm (the macro layer's observation-1 value) is never reached from a row
// caller: in a row expression e(sample) is a per-row value, not obs 1.
// Layers: the row layer (this) and, through the rewritten text, the CiParser
// and the engine's --keepif/--gen/cond() planes, which see a plain variable
// reference and are unchanged; the macro/arith layer is eval/macros.cpp.
std::string rewriteEsampleToken(const std::string& in, SessionState& session,
                                       std::optional<ojson>* errOut) {
  if (!eval::hasEsampleToken(in)) return in;
  const SessionState::EsampleReading r = session.esampleReading();
  switch (r.kind) {
    case SessionState::EsampleReading::Kind::kColumn:
      return eval::replaceEsampleToken(
          in, std::string("(") + kEsampleColumn + "==" + std::to_string(r.serial) + ")");
    case SessionState::EsampleReading::Kind::kZero:
      return eval::replaceEsampleToken(in, "0");
    case SessionState::EsampleReading::Kind::kRefused:
      break;
  }
  if (errOut != nullptr) {
    *errOut = ojson(eval::errorBlock("engine_error", r.refusal, std::nullopt, std::nullopt,
                                     std::nullopt, "r(198)"));
  }
  return in;
}


// `errOut` (BUG-456): when non-null, receives the error block for the two
// shapes this pass can now refuse -- an unknown coefficient name (r(111)) and a
// string-vs-number constant compare (r(109)). A null errOut keeps the text as
// substituted so far; the engine then refuses it in its own words.
// `readsCallerRc`: false leaves `_rc` as text (ifReadsCallerRc).
std::string substituteRowExprRefs(const std::string& in, SessionState& session,
                                  std::optional<ojson>* errOut ,
                                  bool readsCallerRc ) {
  // FU-875: the text as Stata reads it, blanks inside ( ) and [ ] taken out
  // (core/expr_blanks.h), before any name in it is bound: `generate g = (pri ce)`
  // and `count if (pri ce) == 20` read price, where binding `pri` first made
  // the engine's squeeze join pricece (StataBE 19.5: rc 0, g == price).
  const std::string squeezed = cecon::squeeze_bracketed_blanks(in);
  std::optional<ojson> esErr;
  std::string e = rewriteEsampleToken(squeezed, session, &esErr);  // e(sample) T2, first
  if (esErr.has_value()) {
    if (errOut != nullptr) *errOut = std::move(esErr);
    return e;
  }
  // BUG-1420: c(NAME) is a value in a row expression as in display. cobra has no
  // c(), so `count if c(N) == 3`, `assert c(N) == 3` and `keep if c(N) == 3` were
  // rc 1 "function c() is not supported" where StataBE 19.5 counts, and a name
  // that is no c() key is "c(NAME) undefined", r(133), on an `if`, assert's claim
  // and gen/replace alike. A key Stata has and PIET does not serve stays as text
  // and the engine refuses it.
  if (e.find("c(") != std::string::npos) {
    e = substituteCRefs(e, session);
    if (auto undef = eval::undefinedCRef(e)) {
      if (errOut != nullptr)
        *errOut = ojson(eval::errorBlock("engine_error", *undef, std::nullopt, std::nullopt,
                                         std::nullopt, "r(133)"));
      return e;
    }
  }
  // eb-order T-P1: the same trigger as the macro layer's matrix pre-substitution
  // (eval/macros.cpp evalArith). Keyed on `[` alone, `assert colsof(e(b)) == 3`,
  // `generate k = colsof(B)` and `count if el(B,1,2) == 2` reached the engine as
  // text and were refused rc 1 ("function colsof() is not supported"), while
  // `display colsof(e(b))` answered 3 (w2g_ebmat.log:305 e1_colsof_eb).
  {
    const std::string lc = strutil::lower(e);
    // BUG-1373: rownumb() and mreldif() are the same layer's (`assert
    // rownumb(A,"r2") == 2` was "function rownumb() is not supported", rc 1).
    if (e.find('[') != std::string::npos || lc.find("colsof") != std::string::npos ||
        lc.find("rowsof") != std::string::npos || lc.find("colnumb") != std::string::npos ||
        lc.find("rownumb") != std::string::npos || lc.find("mreldif") != std::string::npos ||
        lc.find("el(") != std::string::npos) {
      e = eval::substituteMatrixRefs(e, session.eval);
    }
  }
  // BUG-456: this used to run ONLY when the text contained `scalar(` -- so the
  // `_b[]`/`_se[]`/e() halves of the resolver never ran on a row predicate
  // without one, and `assert _se[mpg]>0` reached the engine as a variable
  // subscript ("unknown variable _se") while `assert _se[mpg]*scalar(one)>0`
  // passed (capture A2 vs A13, the proof). The trigger is now the union of the
  // resolver's own prefilters. BUG-432 left `_b[`/`_se[` as text for the
  // engine when no e() was live, because the resolver answered `.` there.
  // eb-order T-P3: the resolver now refuses a null coefficient source itself
  // with Stata's r(111) "_b not allowed when e(b) is not present" -- a live e()
  // without an e(b) (w2g_ebmat.log:2196 e15c_gen_z after pca) and no e() at all
  // alike (w2v_estpost_ematrix.log ep15f_local_bx1) -- so the gate is gone and
  // an estpost/erepost e() resolves from the block e(b) comes from.
  const bool wantsEst = e.find("_b[") != std::string::npos || e.find("_se[") != std::string::npos ||
                        e.find("scalar(") != std::string::npos || eval::hasBareEParen(e);
  if (wantsEst) {
    std::string bErr;
    e = eval::resolveEstScalarRefs(e, session.eval, &bErr);
    if (!bErr.empty()) {
      if (errOut != nullptr) {
        // BUG-456 follow-up: r(111) or r(109) -- the resolver says which.
        const std::string code = eval::splitErrCode(&bErr);
        *errOut = ojson(eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt,
                                         std::nullopt, code));
      }
      return e;
    }
  }
  // `_rc` before the abbreviation pass: the system name is never a short form
  // of a variable such as `_rcode`.
  if (readsCallerRc) e = substituteRcValue(e, session);
  // BUG-1330: c() values, the pre-substitution display and gen/replace already
  // ran (substituteCRefs): `assert c(pi) > 3` and `count if c(N) > 0` reached
  // the engine as text, "function c() is not supported" at rc 1. A c() nothing
  // answers is r(133), as StataBE words it.
  e = substituteCRefs(e, session);
  std::string cRc;
  if (auto cErr = unservedCRefError(e, &cRc)) {
    if (errOut != nullptr)
      *errOut = ojson(eval::errorBlock("engine_error", *cErr, std::nullopt, std::nullopt,
                                       std::nullopt, "r(" + cRc + ")"));
    return e;
  }
  e = expandRowVarAbbrevs(e, session);
  e = substituteStoredScalars(e, session);
  // BUG-456 follow-up (2026-09-09): a NUMBER in a STRING-typed argument slot is
  // r(109) here too, and this test must run whether or not the resolver above
  // did -- the macro layer substitutes a PRESENT e-scalar upstream, so
  // `count if strpos(e(N),"2")` reaches this point as `strpos(20.0,"2")` with
  // `wantsEst` false. A stored scalar (`scalar k = 5` -> `strpos(5,"x")`) is
  // the same shape and is caught by the same pass, after its substitution.
  if (errOut != nullptr) {
    std::string badCall;
    if (eval::stringFnNumericArg(e, &badCall)) {
      std::string msg = eval::stringSlotMismatchError(badCall);
      const std::string code = eval::splitErrCode(&msg);
      *errOut = ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt,
                                       std::nullopt, code));
      return e;
    }
  }
  // BUG-456: decide the constant string compares the engine cannot (see the
  // fold's own comment above). A string-vs-number compare is r(109).
  {
    std::string mismatch;
    if (!foldRowConstStringCompares(e, &mismatch) && errOut != nullptr) {
      *errOut = ojson(eval::errorBlock(
          "engine_error",
          "type mismatch: `" + mismatch + "` compares a string with a number",
          std::nullopt, std::nullopt, std::nullopt, "r(109)"));
    }
  }
  return e;
}


// W2D-T01 round 3: test/testparm/lincom/nlcom/estat/erepost act on the model
// already installed and may re-enter setLastModel on it (estimation_m4c.cpp
// setEstHandleM4, the estat re-call; handlers_aer_tables.cpp runErepost). In
// Stata they leave e() alone (test/lincom/nlcom store r() only; repost changes
// b/V and nothing else), so a live posting and the names recorded on top of
// the fit must survive them; none of the six writes the e() stores itself, so
// the stores are put back exactly. A no-op unless a posting is live or a name
// sits on top of the fit. `nlcom ..., post` and `estat ..., post` install a
// NEW e() and are not wrapped.
// fvgroup-operator (see the call site in dispatchImpl): `op.(t1 t2 ...)` ->
// `op.t1 op.t2 ...`, the operator applied to every `#`/`##` atom of a term that
// carries no operator of its own. The varlist arrives split on blanks, so a group
// spans tokens from `op.(first` to `last)`. A group that does not close on a
// token ending in `)`, or holds a `(`/`=`, is left exactly as typed.
std::vector<std::string> expandFactorGroupTokens(const std::vector<std::string>& in) {
  static const std::regex reOpen(
      R"(^(.*#)?(i|c|o|bn|ibn|ib\d+|b\d+|ib\((?:first|last|freq)\))\.\((.*)$)", std::regex::icase);
  const auto applyOp = [](const std::string& op, const std::string& term) {
    std::string out, atom;
    auto flush = [&]() {
      if (atom.empty()) return;
      out += atom.find('.') == std::string::npos ? op + "." + atom : atom;
      atom.clear();
    };
    for (char c : term) {
      if (c == '#') {
        flush();
        out += c;
      } else {
        atom += c;
      }
    }
    flush();
    return out;
  };
  std::vector<std::string> out;
  for (size_t i = 0; i < in.size(); ++i) {
    std::smatch m;
    if (!std::regex_match(in[i], m, reOpen)) {
      out.push_back(in[i]);
      continue;
    }
    const std::string interaction = m.str(1), op = m.str(2);
    std::vector<std::string> terms;
    std::string first = m.str(3);
    size_t j = i;
    bool closed = false, bad = false;
    std::string cur = first;
    for (;;) {
      if (cur.find('(') != std::string::npos || cur.find('=') != std::string::npos) {
        bad = true;
        break;
      }
      const size_t close = cur.find(')');
      if (close != std::string::npos) {
        if (close + 1 != cur.size()) {
          bad = true;
          break;
        }
        if (close > 0) terms.push_back(cur.substr(0, close));
        closed = true;
        break;
      }
      if (!cur.empty()) terms.push_back(cur);
      if (++j >= in.size()) break;
      cur = in[j];
    }
    if (!closed || bad || terms.empty()) {
      out.push_back(in[i]);
      continue;
    }
    // `c.( )` puts its operator on the single-variable terms only: a `#` term
    // inside it keeps its atoms as factors, so `c.(a#b z)` is `a#b c.z`
    // (StataBE 19.5: the same fit as `i.a#i.b c.z`, df_m 36; `c.a#c.b` would be
    // a different, continuous product).
    const bool cOp = op.size() == 1 && (op[0] == 'c' || op[0] == 'C');
    for (const auto& t : terms)
      out.push_back(interaction +
                    (cOp && t.find('#') != std::string::npos ? t : applyOp(op, t)));
    i = j;
  }
  return out;
}
}  // namespace piet::dispatch_detail
