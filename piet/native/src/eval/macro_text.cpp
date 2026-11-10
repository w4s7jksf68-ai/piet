// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macro text: see macro_modules.md for ownership and call flow.
#include "piet/eval/macros.h"
#include "estimators/postest_names.h"  // coefFullName: the engine's coefficient-name rule

#include "../dispatch_profile.h"  // WS-D1: per-line regex counter (opt-in, stderr)

#ifndef _WIN32
#include <fnmatch.h>  // WP-W5: POSIX-only; the Windows branch uses globMatchPeriod below
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <unordered_set>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (liftDateLiterals)
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include "macro_scanners_internal.h"
#include "macro_text_internal.h"
#include "macro_name_scan.h"

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval::macro_detail {




// ---------------------------------------------------------------------------
// Small Python-string helpers

bool isPySpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}


std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isPySpace(s[a])) ++a;
  while (b > a && isPySpace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

// Unicode names (lane M): the identifier classes are the ONE shared rule's
// permissive byte tests (src/core/ident.h) -- a byte >= 0x80 is an identifier
// byte, so `local م 1`, `` `م' ``, `scalar سعر = 2` and the mixed-script
// `دخل_2020` are each ONE name (before, the Arabic bytes were word breaks and
// the ASCII tail `_2020` was bound on its own). Every regex twin of a scan
// below carries the same `\x80-\xff` widening, so the selftest cross-checks
// still hold the pairs together.
bool isIdentStart(char c) { return cecon::ident::isIdentStartByte(c); }

bool isIdentChar(char c) { return cecon::ident::isIdentByte(c); }

// Unicode names (lane M): a GLOBAL reference must START with a letter. StataBE
// 19.5 (scratch-r150/uni-m/m2.do): `$ق` expands, but `$€5`, `$٥٠` (Arabic-Indic
// digits) and `$<NBSP>` print literally, and `global ٥ 3` is r(198); a LOCAL
// reference is looser (`` `€' `` expands to "" and `local €x 3` is rc 0), so
// only the `$` forms take this test. Every later character stays permissive --
// `$ق€` reads the global `ق€`, as Stata does. The regexes admit any byte
// >= 0x80 as the start; this is the filter on what they found.
bool globalNameStartOk(const std::string& name) {
  if (name.empty()) return false;
  if (static_cast<unsigned char>(name[0]) < 0x80) return isIdentStart(name[0]);
  char32_t cp = 0;
  return cecon::ident::decodeUtf8(name, 0, &cp) != 0 && cecon::ident::isUnicodeLetter(cp);
}


// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `toLower(...)` call sites
// didn't need touching. Pure dedup, no behavior change. (Note: strip()
// above is a distinct, wider-whitespace helper and is left untouched.)
std::string toLower(std::string s) { return strutil::lower(std::move(s)); }


std::vector<std::string> pySplit(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (isPySpace(c)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}


// Python str.replace(a, b, 1) — replace FIRST occurrence anywhere.
std::string replaceFirst(const std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = s.find(from);
  if (pos == std::string::npos) return s;
  return s.substr(0, pos) + to + s.substr(pos + from.size());
}


std::string regexEscape(const std::string& s) {
  static const std::string meta = R"(\^$.|?*+()[]{})";
  std::string out;
  for (char c : s) {
    if (meta.find(c) != std::string::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}


// re.sub with a callback (replaces ALL matches).
std::string regexSubFn(const std::regex& re, const std::string& input,
                        const std::function<std::string(const std::smatch&)>& fn) {
  std::string out;
  auto begin = std::sregex_iterator(input.begin(), input.end(), re);
  auto end = std::sregex_iterator();
  size_t last = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(input, last, static_cast<size_t>(m.position(0)) - last);
    out.append(fn(m));
    last = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(0));
  }
  out.append(input, last, input.size() - last);
  return out;
}


// Snapshot of finditer results: (full match text, group1 text).
std::vector<std::pair<std::string, std::string>> findAll(const std::regex& re,
                                                          const std::string& s) {
  std::vector<std::pair<std::string, std::string>> out;
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  for (auto it = begin; it != std::sregex_iterator(); ++it) {
    out.emplace_back(it->str(0), it->size() > 1 ? it->str(1) : std::string());
  }
  return out;
}


std::vector<std::pair<std::string, std::string>> findAllGlobalRefs(const std::regex& re,
                                                                   const std::string& s) {
  std::vector<std::pair<std::string, std::string>> out = findAll(re, s);
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const std::pair<std::string, std::string>& r) {
                             return !globalNameStartOk(r.second);
                           }),
            out.end());
  return out;
}

bool hasGlobalRef(const std::regex& re, const std::string& s) {
  for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it)
    if (globalNameStartOk(it->str(1))) return true;
  return false;
}


// Python repr(str) — used in the non-convergence error message ({line!r}).
std::string pyRepr(const std::string& s) {
  bool hasSingle = s.find('\'') != std::string::npos;
  bool hasDouble = s.find('"') != std::string::npos;
  char quote = (hasSingle && !hasDouble) ? '"' : '\'';
  std::string out(1, quote);
  for (unsigned char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == static_cast<unsigned char>(quote)) {
      out += "\\";
      out += static_cast<char>(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    } else {
      // ASCII printable and UTF-8 continuation/lead bytes: Python 3 repr
      // keeps printable non-ASCII literal (corpus avoids non-printable
      // unicode).
      out += static_cast<char>(c);
    }
  }
  out += quote;
  return out;
}

// BUG-501. Everything inside a BALANCED string literal is DATA, and every
// pre-pass in evalArith rewrites OPERATORS and REFERENCES -- so each one was
// mangling literal content, silently. Measured against
// tests/eval/golden/aer_w1/w1b_quoteblind.log and w1b_braceliteral.log:
//
//   strpos("a^b", "b")          the `^` became `**`
//   strpos("A[1,1]", "1")       substituteMatrixRefs substituted the subscript
//   strpos("sc", "c")           resolveEstScalarRefs substituted the scalar
//   strpos("r(mean)", "mean")   the r() resolver RAISED
//   strpos("a{b}c", "b")        isSafeArithExt rejected the whole expression,
//                               so `display` echoed the source text at rc 0
//
// Protected ONCE here rather than by teaching four pre-passes (and their four
// selftest surfaces) about quotes. Each literal is swapped for a placeholder
// built only from characters the whitelists already allow and that NO resolver
// can match: `0L<k>L0` cannot be a Stata name, which must start with a letter or
// an underscore. So the filter keeps its exact character set -- and its regex
// twin keeps holding -- because it never sees literal content at all. Restored
// immediately before the evaluator, whose own lexer handles string literals
// correctly (pyeval.cpp says so at its date-literal case).
//
// ONE-SIDED, like the display scanner's BUG-293(c) fix: an UNTERMINATED quote is
// left exactly where it is, so every degenerate expression this function ever
// judged is judged the same way.
// EM3b: a compound-quoted `"..."' literal (nesting, inner quotes) is ONE literal
// too: `local v = stritrim(`"cmd  a  b"')` (w2af mt_fn_stritrim) reached the
// pre-passes as raw text and failed as r(198).
std::string protectStringLiterals(const std::string& s, std::vector<std::string>* out) {
  out->clear();
  if (s.find('"') == std::string::npos) return s;  // the common case, untouched
  std::string r;
  r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (startsCompound(s, i)) {
      const size_t end = compoundSpanEnd(s, i);
      if (end != std::string::npos) {
        out->push_back(s.substr(i, end - i));
        r += "\"0L" + std::to_string(out->size() - 1) + "L0\"";
        i = end;
        continue;
      }
    }
    if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      if (close != std::string::npos) {
        out->push_back(s.substr(i, close - i + 1));  // the quotes travel with it
        r += "\"0L" + std::to_string(out->size() - 1) + "L0\"";
        i = close + 1;
        continue;
      }
    }
    r += s[i++];
  }
  return r;
}


// BUG-507, the convergence guard's half. `hasMacroRefs` reports an r()/e()/s()
// reference through its `X(` patterns, and the splices above now correctly LEAVE
// one that sits inside a string literal -- so the guard would read that literal as
// unexpanded work and report a circular macro definition for
// `display strpos("r(mean)", "mean")`.
//
// Masking the whole literal is wrong here: Stata DOES expand `` `x' `` and `$x`
// inside double quotes, so an unexpanded MACRO reference in a literal is still a
// real failure and has to stay visible. Only the PARENTHESES are blanked, which
// is exactly the discrimination needed -- every class-reference pattern needs one
// and no macro pattern does (`${...}` uses braces).
std::string maskParensInQuotes(const std::string& s) {
  if (s.find('"') == std::string::npos) return s;
  std::string out = s;
  bool inQ = false;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '"') {
      inQ = !inQ;
      continue;
    }
    // LBL-DOLLAR: '#', not '_' -- a blanked `$(r)` must not read as the global
    // `$_r_` (w2ao lb_var_dollar: `"... $(r)$"` died "did not converge").
    if (inQ && (out[i] == '(' || out[i] == ')')) out[i] = '#';
  }
  return out;
}


std::string restoreStringLiterals(const std::string& s, const std::vector<std::string>& spans) {
  if (spans.empty()) return s;
  std::string r;
  r.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"' && i + 4 < s.size() && s[i + 1] == '0' && s[i + 2] == 'L') {
      size_t j = i + 3;
      std::string digits;
      while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) digits += s[j++];
      if (!digits.empty() && j + 2 < s.size() && s[j] == 'L' && s[j + 1] == '0' &&
          s[j + 2] == '"') {
        size_t k = std::string::npos;
        try {
          k = static_cast<size_t>(std::stoul(digits));
        } catch (const std::exception&) {
        }
        if (k < spans.size()) {
          r += spans[k];
          i = j + 3;
          continue;
        }
      }
    }
    r += s[i++];
  }
  return r;
}


// MOVED OUT of the anonymous namespace 2026-09-06 (BUG-432): the dispatcher's
// row-predicate substitution point needs this exact function, and a second copy
// of a `\b` rule is how two layers start disagreeing about what a bare name is.
// Declared in eval/macros.h; nothing else about it changed.
// `\bNAME\b(?!\s*\()` -> `value`, all occurrences, value inserted literally.
// NAME is a scalar name, so the old code built (and COMPILED) one std::regex
// per scalar per expression — the single most expensive shape a hot loop can
// have. The \b rule is implemented in general form rather than assuming NAME
// begins and ends with a word character, so a store key that somehow does not
// still behaves exactly like the regex.
//
// BUG-512: the scan is a template over WHERE THE VALUE COMES FROM, so `_N` can
// ask for the observation count only once a bare `_N` has actually been found
// (substituteObsSpecials below). Same `\b` rule, still one copy of it; the
// public function is the eager instantiation and is byte-for-byte what it was.



}

namespace piet::eval {
using namespace macro_detail;


std::string substituteBareName(const std::string& in, const std::string& name,
                               const std::string& value) {
  return substituteBareNameWith(in, name, [&value]() -> const std::string& { return value; });
}


// name-binding-display-literals: see macros.h.
std::string protectLiterals(const std::string& s, std::vector<std::string>* spans) {
  return protectStringLiterals(s, spans);
}

std::string restoreLiterals(const std::string& s, const std::vector<std::string>& spans) {
  return restoreStringLiterals(s, spans);
}


// FU-842: see macros.h.
std::string valueRefText(const std::string& text) {
  if (text.empty() || text[0] != '-' || !pythonFloat(text).has_value()) return text;
  return "(" + text + ")";
}


// BUG-512: `_N` / `_n` in a MACRO-context expression.
//
// Stata evaluates `local n = _N`, `scalar s = _N`, `global G = _N`,
// `` `=_N-1' `` and `return scalar N = _N` like any other expression, on
// observation 1: `_N` is the observation count and `_n` is 1
// (tests/eval/golden/aer_w2/w2_smalls.log section A -- local_N 12, local_n 1,
// local_N_expr 25, scalar_N 12, inline_N 12|11, global_N 12). Here neither name
// existed below SessionState, so both reached the evaluator as undefined
// identifiers: the `=` forms fell into classifyFailedEqExpr's unclaimed
// bare-identifier arm and stored their own SOURCE TEXT at rc 0 (`_N*2 + 1`),
// `scalar s = _N` stored `_N`, `return scalar N = _N` posted missing, `display
// _N*2` echoed `_N*2`, and the inline form never converged.
//
// ONE substitution, called by both expression entry points of this layer
// (evalArith and evalMacroExpr), which between them are every consumer:
// local/global/scalar `=`, display, `return scalar`, the resampling exp_list and
// every expandMacros call site. A text substitution like `_rc` beside it, and
// for `_rc`'s reason -- `_N*2 + 1` becomes `12*2 + 1` and stays on evalArith's
// fast path, where a namespace binding would have pushed it off.
//
// Quote-aware on its own account (a caller outside this file has not run
// protectStringLiterals): `"_N"` inside a literal is data and stays text.
// The count is PULLED through EvalSession::obsCount, and only once a bare `_N`
// is really there -- `my_N` or `r2_n` must not cost a Datastore open.
// No hook (evalcase harness, session-less callers) = unchanged input.
std::string substituteObsSpecials(const std::string& in, const EvalSession& session) {
  if (!session.obsCount) return in;
  const bool hasBigN = in.find("_N") != std::string::npos;
  const bool hasSmallN = in.find("_n") != std::string::npos;
  if (!hasBigN && !hasSmallN) return in;  // memchr-class, the common case
  std::vector<std::string> spans;
  std::string s = protectStringLiterals(in, &spans);
  if (hasBigN) {
    // BUG-1449: `_N` as a stored result's NAME -- margins' r(_N), e(_N) after
    // `margins, post` -- is not the observation count (StataBE 19.5
    // `colsof(r(_N))` after `margins` is 1): such an `_N` is masked from the
    // count's substitution below and put back after it.
    bool resultN = false;
    for (size_t p = s.find("_N"); p != std::string::npos; p = s.find("_N", p + 1)) {
      size_t q = p;
      while (q > 0 && (s[q - 1] == ' ' || s[q - 1] == '\t')) --q;
      size_t e = p + 2;
      while (e < s.size() && (s[e] == ' ' || s[e] == '\t')) ++e;
      if (q < 2 || s[q - 1] != '(' || (s[q - 2] != 'r' && s[q - 2] != 'e') ||
          (q >= 3 && isIdentChar(s[q - 3])) || e >= s.size() || s[e] != ')')
        continue;
      s.replace(p, 2, "\x1f" "N");
      resultN = true;
    }
    std::string countText;
    bool asked = false;
    s = substituteBareNameWith(s, "_N", [&]() -> const std::string& {
      if (!asked) {
        countText = std::to_string(session.obsCount());
        asked = true;
      }
      return countText;
    });
    if (resultN)
      for (size_t p = s.find("\x1f" "N"); p != std::string::npos; p = s.find("\x1f" "N", p))
        s.replace(p, 2, "_N");
  }
  if (hasSmallN) {
    static const std::string kOne = "1";
    s = substituteBareNameWith(s, "_n", []() -> const std::string& { return kOne; });
  }
  return restoreStringLiterals(s, spans);
}


// ---------------------------------------------------------------------------
// EM3a/EM3b: verbatim sentinels and compound-quoted spans (see macros.h).

bool hasVerbatim(const std::string& s) {
  return s.find_first_of(std::string{kVerbatimTick, kVerbatimDollar}) != std::string::npos;
}


std::string encodeVerbatim(const std::string& s) {
  if (s.find_first_of("`$") == std::string::npos) return s;
  std::string out = s;
  for (char& c : out) {
    if (c == '`') c = kVerbatimTick;
    else if (c == '$') c = kVerbatimDollar;
  }
  return out;
}


std::string decodeVerbatim(std::string s) {
  if (!hasVerbatim(s)) return s;
  for (char& c : s) {
    if (c == kVerbatimTick) c = '`';
    else if (c == kVerbatimDollar) c = '$';
  }
  return s;
}


bool startsCompound(const std::string& s, size_t i) {
  return i + 1 < s.size() && (s[i] == '`' || s[i] == kVerbatimTick) && s[i + 1] == '"';
}


size_t compoundSpanEnd(const std::string& s, size_t i) {
  if (!startsCompound(s, i)) return std::string::npos;
  int depth = 0;
  size_t j = i;
  while (j < s.size()) {
    if (startsCompound(s, j)) {
      ++depth;
      j += 2;
    } else if (j + 1 < s.size() && s[j] == '"' && s[j + 1] == '\'') {
      j += 2;
      if (--depth == 0) return j;
    } else {
      ++j;
    }
  }
  return std::string::npos;
}


std::string normalizeCompoundQuotes(const std::string& s) {
  if (s.find('"') == std::string::npos) return s;
  if (s.find("`\"") == std::string::npos &&
      s.find(std::string{kVerbatimTick, '"'}) == std::string::npos)
    return s;
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (startsCompound(s, i)) {
      const size_t end = compoundSpanEnd(s, i);
      if (end == std::string::npos) {  // unterminated: left exactly as written
        out.append(s, i, std::string::npos);
        break;
      }
      const std::string body = s.substr(i + 2, end - i - 4);
      if (body.find('"') == std::string::npos) {
        out += '"';
        out += body;
        out += '"';
      } else {
        out.append(s, i, end - i);
      }
      i = end;
      continue;
    }
    if (s[i] == '"') {  // a plain literal: copied through to its closing quote
      const size_t close = s.find('"', i + 1);
      if (close == std::string::npos) {
        out.append(s, i, std::string::npos);
        break;
      }
      out.append(s, i, close + 1 - i);
      i = close + 1;
      continue;
    }
    out += s[i++];
  }
  return out;
}

}
