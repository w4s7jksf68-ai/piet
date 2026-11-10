// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/stata_format.h"  // BUG-1395: numlist element text is %15.0g
#include "piet/strutil.h"
#include "core/expr_blanks.h"  // FU-875: Stata's blank squeeze and r(132) pairing rule
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "data/missing.h"  // BUG-921: the extended-missing kind of a cell read by the if condition
#include "dispatch/dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

#include "loops_internal.h"

namespace piet::loops {

using namespace loops_detail;

namespace {

// ---------------------------------------------------------------------------
// _eval_bool_expr — C++ port using the M1 pyeval/exprfns machinery instead of
// a fresh eval engine. Rewrite order mirrors loops.py exactly.

// Regex-based substring replace with a callback (mirrors Python re.sub).
std::string regexReplace(const std::regex& re, const std::string& s,
                          const std::function<std::string(const std::smatch&)>& fn) {
  std::string out;
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  auto end = std::sregex_iterator();
  size_t last = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(s, last, static_cast<size_t>(m.position()) - last);
    out += fn(m);
    last = static_cast<size_t>(m.position() + m.length());
  }
  out.append(s, last, s.size() - last);
  return out;
}

// libc++'s std::regex (ECMAScript grammar) does NOT support lookbehind
// assertions ((?<!...)/(?<=...) — a PCRE/C++23 feature, not in the
// ECMAScript std::regex grammar) — throws std::regex_error at construction.
// loops.py's Python `re` module DOES support lookbehind, so its
// `(?<!X)C(?!Y)` patterns (single-char C not preceded by X, not followed by
// Y) are ported here as manual character scans instead of regex.

// Single `=` (assignment-in-condition) -> `==`, skipping when adjacent to
// =/!/</> (already part of a multi-char comparison operator).
std::string replaceLoneEquals(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto isCmpChar = [](char c) { return c == '=' || c == '!' || c == '<' || c == '>'; };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '=') {
      bool precededByCmp = (i > 0 && isCmpChar(s[i - 1]));
      bool followedByEq = (i + 1 < s.size() && s[i + 1] == '=');
      if (!precededByCmp && !followedByEq) {
        out += "==";
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// BUG-921: a data-plane cell as the condition evaluator's number. The data
// plane codes a missing as a NaN carrying its kind; the evaluator's missing is
// the 2^1023 band (exprfns.h), where `x == .` is true and `. > 1e300` holds.
// Binding the raw NaN made every comparison with a missing cell false.
double cellAsEvalNumber(double v) {
  if (!std::isnan(v)) return v;
  const int k = cecon::missing_kind(v);
  return eval::stataMissingWithKind(k >= cecon::kMissA && k <= cecon::kMissZ ? k : 0);
}

// BUG-921: the row-1 value of a dataset variable as an evaluator value -- the
// number (missing mapped as above) or, for a string variable, its text.
// nullopt when the column has no row 1.
std::optional<eval::PyValue> obsValueAsPy(Datastore& ds, const std::string& name, int64_t row) {
  if (row < 0 || row >= ds.nRows()) return std::nullopt;
  const cecon::Column& col = ds.col(name);
  if (ds.dtype(name) == "factor") {
    const int32_t code = col.codes()[static_cast<size_t>(row)];
    const auto& levels = col.levels;
    return eval::PyValue::str(code >= 0 && static_cast<size_t>(code) < levels.size()
                                  ? levels[static_cast<size_t>(code)]
                                  : std::string());
  }
  const double* d = col.f64();
  if (d == nullptr) return std::nullopt;
  return eval::PyValue::floating(cellAsEvalNumber(d[row]));
}

// BUG-921: explicit subscripts `var[exp]` in an if/while condition read that
// observation (oracle w2_prog_controlflow pc_if_varref: `if i[3] > 250` reads
// i[3] = 300; pc_if_missing: `if d[99] == .` and `if missing(f[7])` past _N are
// missing, so true). Each subscript of a DATASET variable is replaced by its
// value as literal text: a number in parentheses, `.`/`.a`... for a missing
// (the missing-literal pass below turns those into the evaluator's doubles), a
// quoted string for a string variable ("" past _N, as Stata's string cells
// have no missing). Matrices were already substituted (substituteMatrixRefs)
// and _b[]/_se[] are not dataset variables, so neither is touched. Quoted text
// is skipped. A subscript whose index does not evaluate, or a string value
// holding a double quote, is left as it was (the evaluator then refuses it as
// before). nullopt = nothing substituted.
std::optional<std::string> substituteVarSubscripts(const std::string& in, SessionState& session) {
  if (in.find('[') == std::string::npos) return std::nullopt;
  Datastore* ds = nullptr;
  bool dsAsked = false;
  std::string out;
  bool any = false;
  // Unicode names (lane M): the shared identifier byte tests, so `قيمة[2]` and
  // the mixed-script `دخل_2020[1]` are each one subscripted name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  size_t i = 0;
  while (i < in.size()) {
    const char c = in[i];
    if (c == '"') {  // a string literal: copy through its closing quote
      const size_t close = in.find('"', i + 1);
      const size_t stop = close == std::string::npos ? in.size() : close + 1;
      out.append(in, i, stop - i);
      i = stop;
      continue;
    }
    if (!isId0(c) || (i > 0 && (isId(in[i - 1]) || in[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < in.size() && isId(in[j])) ++j;
    size_t k = j;
    while (k < in.size() && (in[k] == ' ' || in[k] == '\t')) ++k;
    const std::string name = in.substr(i, j - i);
    if (k >= in.size() || in[k] != '[') {
      out.append(name);
      i = j;
      continue;
    }
    // Find the matching close bracket.
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t q = k; q < in.size(); ++q) {
      if (in[q] == '[') ++depth;
      if (in[q] == ']' && --depth == 0) {
        close = q;
        break;
      }
    }
    if (close == std::string::npos) {
      out.append(name);
      i = j;
      continue;
    }
    if (name == "_b" || name == "_se" || session.eval.findMatrix(name) != nullptr) {
      out.append(in, i, close + 1 - i);
      i = close + 1;
      continue;
    }
    if (!dsAsked) {
      ds = session.datastore();
      dsAsked = true;
    }
    std::optional<std::string> lit;
    if (ds != nullptr && ds->has(name)) {
      const std::string idxText =
          eval::substituteObsSpecials(trim(in.substr(k + 1, close - k - 1)), session.eval);
      std::optional<std::string> idxVal = eval::evalArith(idxText, &session.eval);
      double idx = 0.0;
      bool idxOk = false;
      if (idxVal.has_value()) {
        char* e = nullptr;
        idx = std::strtod(idxVal->c_str(), &e);
        idxOk = e != idxVal->c_str() && *e == '\0' && std::isfinite(idx);
      }
      if (idxOk) {
        const long long obs = static_cast<long long>(std::trunc(idx));
        const bool isStr = ds->dtype(name) == "factor";
        std::optional<eval::PyValue> v = obsValueAsPy(*ds, name, obs - 1);
        if (isStr) {
          const std::string text = v.has_value() ? v->strVal() : std::string();
          if (text.find('"') == std::string::npos) lit = "\"" + text + "\"";
        } else if (!v.has_value()) {
          lit = " . ";
        } else {
          const double d = v->floatVal();
          if (auto kind = eval::stataExtendedMissingKind(d)) {
            lit = std::string(" ") + cecon::missing_text(*kind) + " ";
          } else {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "(%.17g)", d);
            lit = buf;
          }
        }
      }
    }
    if (lit.has_value()) {
      out += *lit;
      any = true;
    } else {
      out.append(in, i, close + 1 - i);
    }
    i = close + 1;
  }
  if (!any) return std::nullopt;
  return out;
}

}  // namespace

namespace loops_detail {

BoolEvalResult evalBoolExpr(const std::string& expr, SessionState& session) {
  dprof::EScope _z(dprof::kEBoolExpr);
  BoolEvalResult r;
  std::string cleaned = trim(expr);
  if (cleaned.empty()) {
    r.isError = true;
    r.error = errBlock("if/while: empty condition");
    return r;
  }

  // EXPRLAYER-1a. Every regex below is guarded by a find() for its own
  // mandatory literal. This is the A3 idiom this file and macros.cpp already
  // use, applied to the LAST unguarded regexes on the condition path. It is a
  // strict pre-filter in every case — a pattern cannot match text that does not
  // contain the literal it requires — so no match can be missed and no
  // substitution can be skipped.
  //
  // MEASURED, not assumed: the EXPRLAYER zone profile of S01 put 30.6 ms in
  // string-literal extraction and 33.1 ms in the operator rewrite across 15,374
  // condition evaluations, on a corpus (`while `w' < 250`, `if mod(`i',3)==0`)
  // where not one of these six patterns can match anything. The whole cost was
  // the scan that proves the absence.

  // Compound double-quotes `"..."' -> "..." when the body holds no '"'; EM5: one
  // that does stays compound (eval::normalizeCompoundQuotes, nesting-aware) and
  // is extracted whole below. The old non-greedy regex cut `"say "hi" now"' at
  // its first inner "' (w2af mt_if_cq_eq, mt_while_cq; w2d_scan's
  // `strpos(`"`macval(line)'"', ...)` over csv lines holding quotes).
  dprof::ezoneBegin(dprof::kEBoolStrlit);
  if (cleaned.find("`\"") != std::string::npos) {
    cleaned = eval::normalizeCompoundQuotes(cleaned);
  }
  // FU-875: the blanks inside ( ) and [ ] go, as Stata reads the condition, and
  // brackets that do not pair are r(132) before any name is read -- the rule of
  // the other evaluators (core/expr_blanks.h): `if abs(1 2) == 12` is true and
  // `while (`k' < 1 2)` runs to k = 12 (StataBE 19.5). Guarded like the passes
  // above: a condition without a bracket has nothing to squeeze or pair.
  if (cleaned.find_first_of("()[]") != std::string::npos) {
    cleaned = cecon::squeeze_bracketed_blanks(cleaned);
    if (const std::string u = cecon::bracket_imbalance(cleaned); !u.empty()) {
      dprof::ezoneEnd(dprof::kEBoolStrlit);
      r.isError = true;
      r.error =
          eval::errorBlock("engine_error", u, std::nullopt, std::nullopt, std::nullopt, "r(132)");
      return r;
    }
  }

  // Quote-aware operator rewrite: extract string literals first.
  // colnumb(M, "name") / colsof(M) / rowsof(M) in a condition: the same text
  // substitution the display/local layer runs (eval::substituteMatrixRefs),
  // before the literals are swapped out -- colnumb reads its name from one.
  // `if !missing(colnumb(CB, "`col'")) {` stopped the do-file (w2x_fn_string_math
  // fx_colnumb_if). colnumb() of a matrix that does not exist is r(111).
  // A matrix element NAME[i,j] / el(NAME,i,j) is the same substitution: the
  // condition `if MN[1,1]^2 > 3 {` was rc 1 (w2ac_expr_valueref ifcmd_vr_matrix,
  // StataBE takes the branch); valueRefText keeps a negative cell one operand.
  {
    const std::string lower = strutil::lower(cleaned);
    if (lower.find("colnumb") != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find('[') != std::string::npos ||
        lower.find("el(") != std::string::npos) {
      // Unicode names: identifier-class boundary, not ASCII `\b` (an Arabic
      // byte is a non-word character to `\b`, so `مcolnumb(` would match).
      static const std::regex reColnumbMat(
          R"((?:^|[^\w\x80-\xff])colnumb\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,)",
          std::regex::icase);
      for (std::sregex_iterator it(cleaned.begin(), cleaned.end(), reColnumbMat), end; it != end;
           ++it) {
        if (session.eval.findMatrix((*it)[1].str()) == nullptr) {
          r.isError = true;
          r.error = eval::errorBlock("engine_error", "matrix " + (*it)[1].str() + " not found",
                                     std::nullopt, std::nullopt, std::nullopt, "r(111)");
          return r;
        }
      }
      cleaned = eval::substituteMatrixRefs(cleaned, session.eval);
    }
  }
  if (auto subbed = substituteVarSubscripts(cleaned, session)) cleaned = *subbed;

  std::vector<std::string> placeholders;
  static const std::regex reStrLit(R"("[^"]*")");
  std::string pyExpr;
  if (cleaned.find("`\"") != std::string::npos) {
    // EM5: one left-to-right scan; a compound span (nesting) and a plain literal
    // each become one placeholder, restored verbatim for pyeval's lexer.
    pyExpr.reserve(cleaned.size());
    size_t i = 0;
    while (i < cleaned.size()) {
      size_t end = std::string::npos;
      if (eval::startsCompound(cleaned, i)) {
        end = eval::compoundSpanEnd(cleaned, i);
      } else if (cleaned[i] == '"') {
        const size_t close = cleaned.find('"', i + 1);
        if (close != std::string::npos) end = close + 1;
      }
      if (end == std::string::npos) {
        pyExpr += cleaned[i++];
        continue;
      }
      placeholders.push_back(cleaned.substr(i, end - i));
      pyExpr += "__STR" + std::to_string(placeholders.size() - 1) + "__";
      i = end;
    }
  } else {
    pyExpr = cleaned.find('"') != std::string::npos
                 ? regexReplace(reStrLit, cleaned,
                                [&](const std::smatch& m) {
                                  placeholders.push_back(m.str(0));
                                  return "__STR" + std::to_string(placeholders.size() - 1) + "__";
                                })
                 : cleaned;
  }

  dprof::ezoneEnd(dprof::kEBoolStrlit);
  dprof::ezoneBegin(dprof::kEBoolRewrite);

  // Stata -> Python boolean/comparison operators (replaceLoneEquals/
  // replaceStataMissingLiterals below replicate
  // loops.py's lookaround-regex ordering via manual character scans — see
  // their doc comments for why (libc++'s std::regex has no lookbehind)).
  // A3-①: these four were the only NON-static std::regex constructions on any
  // hot path in the tree — every other pattern here and in macros.cpp is already
  // a function-local static. Constructing a std::regex COMPILES it, and the
  // compile dominates: `if` lines measured 136 us against 53 us for a `local`
  // line whose only structural difference is that it does not come through here
  // (interpreter micro-probe, 2026-08-06) — ~21 us per compile, paid on every
  // single condition evaluation of every loop. Same patterns, same order, same
  // results; they are simply compiled once now.
  static const std::regex reOrOr(R"(\|\|)");
  static const std::regex reAndAnd(R"(&&)");
  static const std::regex reNotEqTilde(R"(~=)");
  //
  // EXPRLAYER-1a: each pass is now guarded by the literal it needs, and every
  // guard is RECOMPUTED against the output of the pass before it — never
  // hoisted. That is not caution, it is required: the `|`/`&` passes run twice
  // (the doubled forms first), and a hoisted guard set computed on the ORIGINAL
  // text would not see what an earlier pass introduced or removed. find() is a
  // memchr-class scan, so paying it per pass costs
  // orders of magnitude less than the regex it guards. replaceLoneChar and
  // replaceLoneEquals are full character copies, so skipping one removes an
  // allocation as well as a pass.
  if (pyExpr.find('|') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reOrOr, " or ");
  }
  if (pyExpr.find('&') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reAndAnd, " and ");
  }
  // A lone `|` / `&` stays as written: pyeval reads it as Stata's operator
  // (Tok::Pipe/Tok::Amp), which evaluates every operand -- `if 1 | runiform() > 2`
  // consumes a draw -- and refuses a string (`if 1 | "a"` is r(109)); Python's
  // `or`/`and` skipped the right operand and let the string through (StataBE
  // 19.5).
  if (pyExpr.find('~') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reNotEqTilde, "!=");
  }
  // FU-811: a bare `!` stays as written. pyeval lexes it as Stata's `!`
  // (Tok::Bang) at negation level; the old rewrite to Python's loose `not` made
  // `if !3==2 {` run its branch (oracle w2ac_expr_precedence ifcmd_not_eq: it
  // does not). `^` becomes `**` the way the display/local layer does it
  // (macros.cpp), after the string literals are placeholders; before this, every
  // `if` condition with a `^` was rc 1 (ifcmd_neg_pow, ifcmd_chain_const, ...).
  if (pyExpr.find('^') != std::string::npos) {
    std::string tmp;
    tmp.reserve(pyExpr.size() + 8);
    for (char c : pyExpr) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    pyExpr = tmp;
  }
  if (pyExpr.find('=') != std::string::npos) {
    pyExpr = replaceLoneEquals(pyExpr);
  }

  dprof::ezoneEnd(dprof::kEBoolRewrite);
  dprof::ezoneBegin(dprof::kEBoolTail);

  // Est-scalar refs (_b[]/_se[]/scalar()/e()) — best-effort, mirrors the
  // oracle's `try: ... except Exception: pass`.
  try {
    std::string bErr;
    pyExpr = eval::resolveEstScalarRefs(pyExpr, session.eval, &bErr);
    if (!bErr.empty()) {
      // BUG-456: `if _b[nosuch] == 0 {` is r(111) in Stata and aborts the
      // do-file even under `capture noisily {` (refute3 V9) -- an honest error
      // here, never a `.` that quietly takes the else branch.
      r.isError = true;
      // BUG-456 follow-up: r(109) when the resolver's number reached a
      // string-typed slot (`if strlen(e(nosuch))==7 {`).
      const std::string code = eval::splitErrCode(&bErr);
      r.error = eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt, std::nullopt,
                                 code);
      dprof::ezoneEnd(dprof::kEBoolTail);
      return r;
    }
  } catch (...) {
  }
  // BUG-456: an e-STRING arrives from the resolver as a double-quoted literal
  // (`"regress"`), AFTER the placeholder extraction above and BEFORE
  // replaceStataMissingLiterals below -- so an e(cmdline)/e(depvar) text with a `.` in it
  // (`d.x`, `a.b`) would have been sentinelized (the BUG-443 class). Extract
  // the new literals into the same placeholder list; the restore loop below
  // puts them back untouched, and the identifier scan never sees their text.
  if (pyExpr.find('"') != std::string::npos) {
    pyExpr = regexReplace(reStrLit, pyExpr, [&](const std::smatch& m) {
      placeholders.push_back(m.str(0));
      return "__STR" + std::to_string(placeholders.size() - 1) + "__";
    });
  }

  // BS-091/BS-101 (BETA A4): c(name) refs resolve to a literal — the same
  // shared registry gen/replace/scalar/local/display use (substituteCRefs,
  // cclass.h), so `if c(N) > 500 { ... }` / `while ... c(level) ...` work.
  // Safe here (after the string-literal placeholder extraction above): any
  // literal "c(...)" text INSIDE a quoted string is already a __STRn__
  // placeholder token by this point, so only a real unquoted c(name)
  // reference can match.
  pyExpr = substituteCRefs(pyExpr, session);
  // BUG-1330: a c() nothing answers is StataBE's r(133) `c(name) undefined`, or
  // the named refusal of a real name this build does not serve, as display and
  // assert word it; `if c(bogus) == 1 {` failed at rc 1 with no message.
  std::string cRc;
  if (auto cErr = unservedCRefError(pyExpr, &cRc)) {
    r.isError = true;
    r.error = eval::errorBlock("engine_error", *cErr, std::nullopt, std::nullopt, std::nullopt,
                               "r(" + cRc + ")");
    dprof::ezoneEnd(dprof::kEBoolTail);
    return r;
  }

  // Stata missing: scalar(NAME) unresolved, a bare `.` and `.a`..`.z` -> their
  // exact doubles (exprfns.h; `.` is 2^1023) so `missing <= 0` is False, `.a > .`
  // is True and eval never NameErrors (Stata's missing = +infinity ordering).
  // EXPRLAYER-1a: same strict pre-filter argument. The pattern needs the
  // literal "scalar(" and replaceStataMissingLiterals needs a '.'; both were
  // unconditional, and the rewrite allocates and copies the whole string even
  // when it changes nothing.
  // Unicode names: the boundary is the identifier class (group 1 keeps the
  // character before), as in macros.cpp's substituteScalarCalls scan -- under
  // ASCII `\b` the tail of `مscalar(y)` was rewritten.
  static const std::regex reScalarUnresolved(
      R"((^|[^\w\x80-\xff])scalar\(\s*[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*\))");
  if (pyExpr.find("scalar(") != std::string::npos) {
    // A callback, not a "$1..." format: the literal starts with a digit, and
    // "$18.98..." would read as group 18.
    static const std::string kMissing = eval::stataMissingLiteral(0);
    pyExpr = regexReplace(reScalarUnresolved, pyExpr,
                          [](const std::smatch& m) { return m.str(1) + kMissing; });
  }
  if (pyExpr.find('.') != std::string::npos) {
    pyExpr = eval::replaceStataMissingLiterals(pyExpr);
  }

  // C4-L3 (D2a value plane): the referenced-identifier set of the FINAL
  // rewritten expression, collected while string literals are still __STRn__
  // placeholder tokens so quoted text can never contribute a name. The lazy
  // binder below binds ONLY these — outcome-identical to the historical
  // bind-everything (evaluation consults referenced names only), and what
  // lets a pure-scalar condition (`if _rc`, `if r(N)>0`, `while c<3` — the
  // corpus-dominant shapes, c4_window_attribution §3) skip
  // session.datastore() entirely: on a dirty virtual head that call WAS the
  // EMH window-closer, snapshotting the whole frame for a condition that
  // reads no data. A token immediately preceded by a digit or '.' is a
  // number's exponent tail (1e+307), not a name — skipping it can only
  // under-collect inside already-invalid expressions, where evaluation fails
  // identically in both arms.
  dprof::ezoneEnd(dprof::kEBoolTail);
  std::unordered_set<std::string> exprRefs;
  {
    dprof::EScope _zi(dprof::kEBoolIdentScan);
    // EXPRLAYER-1b: `[A-Za-z_]\w*` walked left to right, in place of a
    // sregex_iterator over the same pattern. `\w` in ECMAScript is exactly
    // [A-Za-z0-9_] (it is NOT locale-dependent, unlike Python's), and
    // sregex_iterator finds leftmost, non-overlapping matches scanning forward
    // — and the greedy `\w*` makes each of them maximal, since nothing after it
    // can force a backtrack. That is exactly this loop, character for
    // character: skip to the first identifier start, run to the end of the
    // identifier characters, resume from there. The
    // "preceded by a digit or '.'" test and the __STR prefix test are
    // transcribed unchanged. selftest cross-checks this scan against the regex
    // over a corpus of condition shapes (selftestBoolIdentScanMatchesRegex).
    const size_t n = pyExpr.size();
    for (size_t i = 0; i < n;) {
      // Unicode names (lane M): the shared byte tests; the regex twin carries
      // the same `\x80-\xff` widening.
      if (!cecon::ident::isIdentStartByte(pyExpr[i])) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < n && cecon::ident::isIdentByte(pyExpr[j])) ++j;
      const bool expTail =
          i > 0 && (std::isdigit(static_cast<unsigned char>(pyExpr[i - 1])) || pyExpr[i - 1] == '.');
      if (!expTail && pyExpr.compare(i, 5, "__STR") != 0) {
        exprRefs.insert(pyExpr.substr(i, j - i));
      }
      i = j;
    }
  }

  // Restore string placeholders.
  for (size_t i = 0; i < placeholders.size(); ++i) {
    std::string key = "__STR" + std::to_string(i) + "__";
    size_t pos = pyExpr.find(key);
    if (pos != std::string::npos) pyExpr.replace(pos, key.size(), placeholders[i]);
  }

  // Build the evaluation namespace: math + Stata fns + session scalars.
  // EXPRLAYER-1c: the 39-entry math+Stata registry is a process constant and is
  // referenced, not rebuilt (39 map nodes + 39 std::function constructions per
  // condition, measured at 25.9 ms across one S01 run). Only `scalar` — whose
  // closure captures this session — and the value bindings below are per-call,
  // and they go into the OWN maps, which outrank the base exactly as a later
  // insertion into the single merged map used to.
  eval::PyNamespace ns;
  {
    dprof::EScope _zn(dprof::kEBoolNs);
    ns.base = &eval::stataMathNamespace();
    eval::addScalarFn(ns, [&session](const std::string& name) -> std::optional<std::string> {
      const std::string* v = session.eval.scalars.find(name);
      return v ? std::optional<std::string>(*v) : std::nullopt;
    });
  }
  dprof::ezoneBegin(dprof::kEBoolBind);

  // _rc: session._locals["_rc"], default "0" if unset/blank/".".
  {
    dprof::EScope _zr(dprof::kEBoolBindRc);
    const std::string* rcRaw = session.eval.locals.find("_rc");
    double rcVal = 0.0;
    if (rcRaw && !trim(*rcRaw).empty() && trim(*rcRaw) != ".") {
      try {
        rcVal = std::stod(*rcRaw);
      } catch (...) {
        rcVal = 0.0;
      }
    }
    ns.consts["_rc"] = eval::PyValue::floating(rcVal);
  }

  // _N / _n + dataset column first-obs values (Stata's `while c<3` reads
  // row 1 at program-level scope).
  //
  // C4-L3 (D2a, cobra unchanged): LAZY arm first. When the resident card is
  // provably current (the same honest-currency rule every D1 consumer uses),
  // the card answers _N and column name/dtype membership without opening the
  // datastore; session.datastore() — on a dirty virtual head, a full EMH
  // frame snapshot — is paid ONLY when the condition really reads a column's
  // row-1 value (any referenced column; since BUG-921 a column outranks a
  // same-named scalar and a string column binds its text). Anything less than a current card — stale/absent,
  // no dataset, or the PIET_NO_RESIDENT_READ escape arm via
  // residentReadCard()'s gate — falls to the pre-D2 eager path VERBATIM
  // below, including its bind-every-f64 behavior and its
  // materialize-on-dirty side effect (the floor RED arm's reproduction).
  dprof::ezoneBegin(dprof::kEBoolBindCard);
  // BUG-921: column values are collected here and bound AFTER the scalars, so a
  // variable outranks a same-named scalar (Stata's rule, and dispatcher.cpp's
  // for row predicates). Oracle w2_prog_controlflow pc_if_varref: with
  // `scalar i = 999` and variable i (i[1] = 100), `if i > 250` is false and
  // `if i == 999` is false. A string variable binds its row-1 text
  // (`if s == "s 1"`), which used to be unbound and stopped the do-file (rc 1).
  std::unordered_map<std::string, eval::PyValue> colBound;
  bool boundLazily = false;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
    if (cardRows.has_value()) {
      if (exprRefs.count("_N") > 0) {
        ns.consts["_N"] = eval::PyValue::floating(static_cast<double>(*cardRows));
      }
      if (exprRefs.count("_n") > 0) ns.consts["_n"] = eval::PyValue::floating(1.0);
      std::vector<std::string> needValue;
      if (*cardRows > 0) {
        for (const auto& tok : exprRefs) {
          for (const auto& vi : *card) {
            if (vi.name == tok) {
              needValue.push_back(tok);  // BUG-921: string variables too
              break;  // a current card is authoritative for absence too
            }
          }
        }
      }
      if (!needValue.empty()) {
        // Honest flush: the condition genuinely reads data. Same cost and
        // semantics as the eager arm for exactly these columns.
        Datastore* dsv = session.datastore();
        if (dsv != nullptr) {
          for (const auto& tok : needValue) {
            if (!dsv->has(tok)) continue;
            if (auto v = obsValueAsPy(*dsv, tok, 0)) colBound[tok] = *v;
          }
        }
      }
      boundLazily = true;
    }
  }
  if (!boundLazily) {
    Datastore* ds = session.datastore();
    if (ds != nullptr) {
      // EXPRLAYER-4: bind only the names the expression REFERENCES, exactly as
      // the lazy arm above already does. The eager arm bound _N, _n and every
      // f64 column of the dataset — 25 dtype() calls, 25 col() lookups and ~25
      // std::map inserts on the P2 fixture — for conditions like `while `w' <
      // 250` that reference none of them. Measured at 33.4 ms of S01's 50.7 ms
      // of condition time, the largest remaining item in the zone table.
      //
      // OUTCOME-IDENTICAL, on the argument this file already ratified for the
      // lazy arm (C4-L3): evaluation consults REFERENCED names only, and
      // exprRefs is collected from the final rewritten expression, so a name
      // that is not in it cannot be looked up. The DATASTORE CALL IS NOT MOVED
      // — session.datastore() still runs first and unconditionally, so the
      // materialize-on-dirty side effect this arm is deliberately preserved to
      // reproduce (the floor RED arm) happens exactly as before. Only the
      // binding loop narrowed. (BUG-921: the columns now bind after the
      // scalars, so a same-named variable wins.)
      if (exprRefs.count("_N") > 0) {
        ns.consts["_N"] = eval::PyValue::floating(static_cast<double>(ds->nRows()));
      }
      if (exprRefs.count("_n") > 0) ns.consts["_n"] = eval::PyValue::floating(1.0);
      if (ds->nRows() > 0) {
        for (const auto& tok : exprRefs) {
          if (!ds->has(tok)) continue;  // col()/dtype() throw on an absent name
          if (auto v = obsValueAsPy(*ds, tok, 0)) colBound[tok] = *v;  // BUG-921: strings too
        }
      }
    } else {
      ns.consts.emplace("_N", eval::PyValue::floating(0.0));
      ns.consts.emplace("_n", eval::PyValue::floating(1.0));
    }
  }

  dprof::ezoneEnd(dprof::kEBoolBindCard);

  // Named scalars (session._scalars), then the dataset columns collected above
  // on top of them: a variable wins over a same-named scalar (BUG-921; the old
  // order, inherited from loops.py, let the scalar win).
  dprof::ezoneBegin(dprof::kEBoolBindScalars);
  for (const auto& [sname, sval] : session.eval.scalars.items()) {
    auto f = eval::pythonFloat(sval);
    if (f) ns.consts[sname] = eval::PyValue::floating(*f);
  }
  for (auto& [cname, cval] : colBound) ns.consts[cname] = std::move(cval);
  dprof::ezoneEnd(dprof::kEBoolBindScalars);

  dprof::ezoneEnd(dprof::kEBoolBind);

  // `if 1/0 == . {`, `if invttail(5,2) == . {` and `while sqrt(-1) == .` are
  // Stata conditions like any other: no error, and a NaN is `.` (see
  // StataDivisionScope; BUG-1358, BUG-1065).
  eval::StataDivisionScope stataSemantics;
  auto outcome = eval::evalPyExprDetailed(pyExpr, ns);
  if (outcome.status != eval::PyEvalOutcome::Status::Value) {
    r.isError = true;
    // BUG-456: a function announcing Stata's EXACT code (`r(109): type
    // mismatch` from pyEquals on `if e(cmd) == 1 {`) is carried as that code,
    // the way `local =` and display already carry it (BUG-246/249).
    if (outcome.status == eval::PyEvalOutcome::Status::Raised &&
        outcome.error.rfind("r(", 0) == 0 && outcome.error.find(')') != std::string::npos) {
      const std::string rc = outcome.error.substr(0, outcome.error.find(')') + 1);
      const size_t colon = outcome.error.find(": ");
      r.error = eval::errorBlock(
          "engine_error",
          (colon != std::string::npos ? outcome.error.substr(colon + 2) : outcome.error) +
              " (expr=" + cleaned + ")",
          std::nullopt, std::nullopt, std::nullopt, rc);
      return r;
    }
    r.error = errBlock("if/while condition evaluation failed (expr=" + cleaned + ")");
    return r;
  }
  r.value = outcome.value.truthy();
  return r;
}

}  // namespace loops_detail

// ---------------------------------------------------------------------------
// EXPRLAYER drift guard (see loops.h).

bool selftestBoolIdentScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kCases = {
      "`w' < 250", "w < 250", "mod(i,3)==0", "i==1000", "_rc", "_N > 500", "_n == 1",
      "x1 > 0 & x2 < 1", "__STR0__ == \"a\"", "1e+307 > x", "1.5e-9 < y", "a.b",
      "_x9 + x_9", "abc", "", "   ", "3", "3x", "x3", "e(N) > 0", "scalar(q) != .",
      "__STR10__", "__STRx", "not x", "x or y", "8.988465674311579e+307",
      "f(1)+g2(3)", "_", "__", "___STR0__", "1_x", "x1e5", "9e", "a1b2c3",
      // Unicode names (lane M): a UTF-8 name is one token, the mixed-script
      // `دخل_2020` included (its ASCII tail is NOT a token of its own), and the
      // digit / `.` exponent-tail rule applies to a high byte the same way.
      "قيمة > 0", "دخل_2020 > 1", "x قيمة", "1ق", "ق.ب", "ق1 + _ق", "_2020 + دخل_2020",
      "xملاحظات", "ملاحظات1x", "cond(قيمة>0,1,0)", "__STR0__ == ق",
  };
  static const std::regex reIdentTok(R"([A-Za-z_\x80-\xff][\w\x80-\xff]*)");
  for (const auto& c : kCases) {
    // The regex arm — verbatim the loop this replaced.
    std::set<std::string> viaRegex;
    for (auto it = std::sregex_iterator(c.begin(), c.end(), reIdentTok);
         it != std::sregex_iterator(); ++it) {
      const size_t pos = static_cast<size_t>(it->position());
      if (pos > 0 &&
          (std::isdigit(static_cast<unsigned char>(c[pos - 1])) || c[pos - 1] == '.')) {
        continue;
      }
      std::string tok = it->str();
      if (tok.rfind("__STR", 0) == 0) continue;
      viaRegex.insert(std::move(tok));
    }
    // The scan arm — verbatim the loop in evalBoolExpr.
    std::set<std::string> viaScan;
    const size_t n = c.size();
    for (size_t i = 0; i < n;) {
      if (!cecon::ident::isIdentStartByte(c[i])) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < n && cecon::ident::isIdentByte(c[j])) ++j;
      const bool expTail =
          i > 0 && (std::isdigit(static_cast<unsigned char>(c[i - 1])) || c[i - 1] == '.');
      if (!expTail && c.compare(i, 5, "__STR") != 0) viaScan.insert(c.substr(i, j - i));
      i = j;
    }
    if (viaRegex != viaScan) {
      if (firstDisagreement) {
        std::string a, b;
        for (const auto& t : viaRegex) a += t + " ";
        for (const auto& t : viaScan) b += t + " ";
        *firstDisagreement = "line=[" + c + "] regex={" + a + "} scan={" + b + "}";
      }
      return false;
    }
  }
  return true;
}

}  // namespace piet::loops
