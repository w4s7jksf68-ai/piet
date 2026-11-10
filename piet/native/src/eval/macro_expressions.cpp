// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macro expressions: see macro_modules.md for ownership and call flow.
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

#include "macro_expressions_internal.h"
#include "macro_matrices_internal.h"
#include "macro_results_internal.h"
#include "macro_scanners_internal.h"
#include "macro_text_internal.h"
#include "macro_name_scan.h"

namespace piet::eval {
void addSessionRngFns(PyNamespace& ns, EvalSession& session);
void addStataMacroOnlyFns(PyNamespace& ns);
}

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval::macro_detail {


// BUG-456 follow-up (2026-09-09) — A NUMBER IN A STRING-TYPED ARGUMENT SLOT.
//
// The e()/`_b[]` resolvers above substitute a NUMBER for everything that is not
// an e-string: an absent name becomes `.`, an e-scalar becomes its value. That
// is right for arithmetic and for the row plane's constant compares, and WRONG
// the moment the reference sits inside a STRING function -- `strpos(e(nosuch),
// "reg")` became `strpos(.,"reg")`, which the expression evaluator's pythonStr()
// coercion answered 0 at rc 0, and `strlen(e(N))` answered the decimal width of
// the substituted number. Stata refuses every one of those shapes.
//
// MEASURED on StataNow 19.5 BE (scratch oracle.log, 2026-09-09, sysuse auto +
// regress price mpg): `count if strpos(e(nosuch),"reg")` r(109) · `inlist(
// e(nosuch),"regress")` r(109) · `strlen(e(nosuch))==7` r(109) · `substr(
// e(nosuch),1,3)=="reg"` r(109) · `upper(e(nosuch))=="REG"` r(109) · `local p =
// strpos(e(nosuch),"reg")` r(109) · `gen za = strpos(e(nosuch),"reg")` r(109) ·
// `replace price = 1 if strpos(e(nosuch),"reg")` r(109) · with a PRESENT
// e-scalar the same: `strpos(e(N),"7")` r(109), `inlist(e(N),"74")` r(109),
// `local q = strlen(e(N))` r(109), `display strlen(e(nosuch))` r(109). The
// accepting arm is unchanged: `strpos(e(cmd),"reg")` 74, `inlist(e(cmd),
// "regress")` 74, `substr(e(cmd),1,3)=="reg"` 74, `local c = strpos(e(cmd),
// "gres")` 3 -- an e-STRING is a string, so a string function takes it.
//
// The rule is the ARGUMENT's type, not where it came from: Stata gives r(109)
// for a numeric LITERAL (`strpos(5,"a")`, `strlen(5)`), for a numeric VARIABLE
// (`strpos(mpg,"7")`) and for `_b[mpg]`/`_se[]` too (O17-O21 in the same log).
// So the test here is on the resolved TEXT and claims every numeric CONSTANT in
// a string slot, whoever wrote it. A numeric VARIABLE is a data-plane type the
// text cannot see -- named gap, not claimed.
//
// inlist()/inrange() are polymorphic in Stata (all arguments one type): a
// MIXED call is the r(109) (`inlist(e(N),"74")`), while all-numeric is fine
// (`inlist(e(N),74)` -> 74 rows, measured O23).
//
// cond() (review finding 1, 2026-09-09) is polymorphic in a SUBSET of its
// slots, which is why the whole-arglist `poly` flag above could not simply be
// reused for it: slot 0 is the numeric CONDITION and slots 1/2 are the value
// pair that must agree with each other. MEASURED on StataNow 19.5 BE
// (/private/tmp/bug456rev/cond_oracle.log, sentinel CONDQ-ORACLE-END, sysuse
// auto + regress price mpg): `local cd = cond(1,e(nosuch),"x")` r(109) (Q1) ·
// `count if cond(1,e(nosuch),"x")=="x"` r(109) (Q2) · `local c5 =
// cond(1,5,"x")` r(109) (Q3) · `local c6 = cond(1,e(N),"x")` r(109) (Q4). The
// ACCEPTING arms are rc 0 and must stay: `count if cond(1,e(cmd),"x")==
// "regress"` -> 74 (Q5, an e-string is a string) and `local c7 =
// cond(1,"a","b")` -> "a" (Q6) -- a whole-arg-list poly test would turn Q6
// into a false r(109) because slot 0 is the number 1.

bool numericConstToken(const std::string& t) {
  if (t.empty()) return false;
  // `.`, `.a`..`.z` (Stata's extended missings) and any ordinary numeric
  // literal, with the sign the substituted value may carry.
  if (t == ".") return true;
  if (t.size() == 2 && t[0] == '.' && std::isalpha(static_cast<unsigned char>(t[1]))) return true;
  static const std::regex reNum(R"(^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$)");
  return std::regex_match(t, reNum);
}


// The string-typed argument slots of the string family this engine registers
// (exprfns.cpp addStataFns). Three kinds of row, checked in this order:
//   `poly`      -- EVERY argument must be one type (inlist/inrange).
//   `polySlots` -- the listed slots must agree with EACH OTHER, the rest are
//                  not claimed (cond: the value pair 1/2, never the condition).
//   `stringSlots` -- fixed string-typed slots; a numeric constant there is the
//                  r(109).
struct StringFnArgSpec {
  const char* name;
  std::vector<size_t> stringSlots;  // empty + poly=true -> the mixed-type rule
  bool poly = false;
  std::vector<size_t> polySlots;  // the mixed-type rule over a SUBSET of slots
};


const std::vector<StringFnArgSpec>& stringFnArgSpecs() {
  static const std::vector<StringFnArgSpec> specs = {
      {"strlen", {0}},   {"length", {0}},     {"strpos", {0, 1}},  {"substr", {0}},
      {"upper", {0}},    {"lower", {0}},      {"strupper", {0}},   {"strlower", {0}},
      {"proper", {0}},   {"trim", {0}},       {"strtrim", {0}},    {"ltrim", {0}},
      {"rtrim", {0}},    {"strltrim", {0}},   {"strrtrim", {0}},   {"stritrim", {0}},
      {"word", {0}},     {"abbrev", {0}},     {"real", {0}},       {"regexm", {0, 1}},
      {"regexr", {0, 1, 2}}, {"subinstr", {0, 1, 2}}, {"soundex", {0}}, {"soundex_nara", {0}},
      {"ustrfix", {0}},
      {"inlist", {}, true}, {"inrange", {}, true},
      // AER scope-2 B2-3: the business-calendar pair. bofd(calname, date) and
      // dofb(bdate, calname) each carry ONE string slot, and a numeric
      // constant there is Stata's r(109) -- which is also how a SWAPPED call
      // (`bofd(td(15jan2020), "sp500")`) is caught, measured rc 109.
      {"bofd", {0}}, {"dofb", {1}},
      // cond(c, a, b [, missval]): slots 1 and 2 are the polymorphic pair.
      {"cond", {}, false, {1, 2}},
      // BUG-660 fix round: the new macro-layer string functions this
      // exprfns.cpp landing added. strproper/usubinstr are literal ALIASES
      // of proper/subinstr (same function pointer, exprfns.cpp's own
      // registration comment) -- omitting them here would have made
      // `strproper(5)` rc 0 while `proper(5)` is r(109), a contradiction for
      // the SAME underlying call. reverse/strrpos/wordcount/strtoname/
      // strmatch/ustrunescape/ustrregexra are new functions with no sibling
      // entry; added by the same general rule (a string function given a
      // numeric CONSTANT is r(109), the BUG-456 contract this whole table
      // implements) rather than left unclaimed.
      {"strproper", {0}}, {"usubinstr", {0, 1, 2}}, {"reverse", {0}},
      {"strrpos", {0, 1}}, {"wordcount", {0}}, {"strtoname", {0}},
      {"strmatch", {0, 1}}, {"ustrunescape", {0}}, {"ustrregexra", {0, 1, 2}},
  };
  return specs;
}


// Splits the argument list of the call whose '(' is at `open`. Returns false if
// the parentheses do not close (malformed text this pass does not own).
bool splitCallArgs(const std::string& e, size_t open, std::vector<std::string>* args,
                   size_t* closeOut) {
  int depth = 0;
  bool inStr = false;
  size_t argStart = open + 1;
  for (size_t i = open; i < e.size(); ++i) {
    const char c = e[i];
    if (inStr) {
      if (c == '\\' && i + 1 < e.size()) { ++i; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (c == '(') { ++depth; continue; }
    if (c == ')') {
      --depth;
      if (depth == 0) {
        args->push_back(e.substr(argStart, i - argStart));
        *closeOut = i;
        return true;
      }
      continue;
    }
    if (c == ',' && depth == 1) {
      args->push_back(e.substr(argStart, i - argStart));
      argStart = i + 1;
    }
  }
  return false;
}


// Argument kinds for the type test: 1 = string, 2 = numeric constant, 0 = the
// pass does not know (a variable, a call, arithmetic) and does not claim it.
int argKind(const std::string& raw) {
  std::string t = strip(raw);
  if (t.empty()) return 0;
  if (t.front() == '"') return 1;  // a literal (or a literal-headed concat)
  if (numericConstToken(t)) return 2;
  return 0;
}


// Finds the first string-typed slot holding a numeric constant. Returns true
// and fills *call with the offending call's text.
bool stringFnNumericArgImpl(const std::string& e, std::string* call) {
  if (e.find('(') == std::string::npos) return false;
  bool inStr = false;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (inStr) {
      if (c == '\\' && i + 1 < e.size()) { ++i; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (!isIdentStart(c)) continue;  // a Unicode name is skipped whole, too
    if (i > 0 && isIdentChar(e[i - 1])) continue;  // mid-identifier
    size_t j = i;
    while (j < e.size() && isIdentChar(e[j])) ++j;
    const std::string name = e.substr(i, j - i);
    size_t k = j;
    while (k < e.size() && std::isspace(static_cast<unsigned char>(e[k]))) ++k;
    if (k >= e.size() || e[k] != '(') { i = j - 1; continue; }
    const StringFnArgSpec* spec = nullptr;
    for (const auto& s : stringFnArgSpecs())
      if (name == s.name) { spec = &s; break; }
    if (spec == nullptr) { i = j - 1; continue; }
    std::vector<std::string> args;
    size_t close = 0;
    if (!splitCallArgs(e, k, &args, &close)) { i = j - 1; continue; }
    bool bad = false;
    if (spec->poly) {
      bool anyStr = false, anyNum = false;
      for (const auto& a : args) {
        const int kd = argKind(a);
        if (kd == 1) anyStr = true;
        else if (kd == 2) anyNum = true;
      }
      bad = anyStr && anyNum;
    } else if (!spec->polySlots.empty()) {
      // The mixed-type rule over the named slots only. Slots the call does not
      // supply are simply absent from the group (`cond(1,"a")` is a malformed
      // call this pass does not own).
      bool anyStr = false, anyNum = false;
      for (size_t slot : spec->polySlots) {
        if (slot >= args.size()) continue;
        const int kd = argKind(args[slot]);
        if (kd == 1) anyStr = true;
        else if (kd == 2) anyNum = true;
      }
      bad = anyStr && anyNum;
    } else {
      for (size_t slot : spec->stringSlots)
        if (slot < args.size() && argKind(args[slot]) == 2) { bad = true; break; }
    }
    if (bad) {
      std::string text = e.substr(i, close + 1 - i);
      if (text.size() > 72) text = text.substr(0, 69) + "...";
      *call = text;
      return true;
    }
    i = j - 1;  // keep scanning: nested calls are found by this same walk
  }
  return false;
}

}

namespace piet::eval {
using namespace macro_detail;


// The same test, exposed for the ROW/GEN planes. They cannot rely on the call
// above: a PRESENT e-scalar is substituted by the MACRO expansion layer before
// the dispatcher ever sees the line (macros.cpp expandPass), so by then the
// text reads `strpos(20.0,"2")` with no `e(` left in it -- the dispatcher's
// `wantsEst` prefilter is false and the resolver never runs. Measured with a
// debug print at the row call site (2026-09-09): `count if strpos(e(N),"2")`
// arrives as `strpos(20.0,"2")`, while `strpos(e(nosuch),"x")` (an ABSENT name,
// which the macro layer leaves alone) still arrives unresolved. One test, two
// entry points -- never a second copy of the rule.
bool stringFnNumericArg(const std::string& expr, std::string* call) {
  return stringFnNumericArgImpl(expr, call);
}


std::string stringSlotMismatchError(const std::string& call) {
  return "r(109): type mismatch: `" + call + "` passes a number where a string is required";
}


// BUG-456 follow-up: the resolver's `err` channel carries TWO codes now -- the
// historical r(111) (unknown coefficient name, no prefix) and the r(109) the
// string-slot type test raises with an explicit `r(NNN): ` prefix. One splitter
// so the five call sites (evalArith, `scalar =`, if/while, the row plane, gen)
// cannot drift into two spellings of the same mapping.
std::string splitErrCode(std::string* msg) {
  if (msg == nullptr || msg->rfind("r(", 0) != 0) return "r(111)";
  const size_t close = msg->find(')');
  if (close == std::string::npos || close + 1 >= msg->size() || (*msg)[close + 1] != ':') {
    return "r(111)";
  }
  const std::string digits = msg->substr(2, close - 2);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
    return "r(111)";
  }
  const std::string code = msg->substr(0, close + 1);
  *msg = strip(msg->substr(close + 2));
  return code;
}

}

namespace piet::eval::macro_detail {


// ---------------------------------------------------------------------------
// _eval_arith (macros.py)

// A bare `.` and an extended `.a`..`.z` become their exact doubles (exprfns.h
// replaceStataMissingLiterals, shared with loops.cpp and strmutate.cpp) so e.g.
// `substr(s, 3, .)` (missing length == "to end of string") reaches fnSubstr as a
// huge length and arithmetic on missing reaches pyeval's collapse rather than a
// Python SyntaxError. BUG-249: each kind keeps its own double, so string(.a)
// answers `.a`.
std::string replaceBareDotMissing(const std::string& s) {
  return replaceStataMissingLiterals(s);
}


// BUG-249: the way OUT of the evaluator for a kind-carrying missing — `.a`
// renders `.a`, anything else in the missing band renders `.` (including the
// negated band, which Stata shows as `.`; see isStataMissingResult below).
std::string stataMissingText(double d) {
  auto kind = stataExtendedMissingKind(d);
  if (kind && *kind >= 1) return std::string(".") + static_cast<char>('a' + *kind - 1);
  return ".";
}


// BUG-660: evalArith's non-integer-real return uses pythonFloatRepr —
// Python's shortest-round-trip repr(), up to 17 significant digits — for
// BOTH its fast-arithmetic and extended (function-call) paths, and stays
// round-trip-exact THERE (see evalArith's own comment: `scalar s = expr`
// needs the full precision, since std::stod reads it back at full width
// downstream). Stata's own local/global `= <realexpr>` STORAGE is not
// round-trip-exact, though: it caps at 16 significant digits (verified
// against four independent w2c_functions.log macro arms with genuinely
// irrational values — sqrt_macro, normal_macro, invt/invttail/ttail's df1
// and df10 points — every one needs exactly 16 sig figs, never 17) and,
// like every other Stata real renderer in this file (see gFormat above),
// drops the leading `0` before the decimal point (`.33`, not `0.33`).
// `%.16g` already implements Python's own fixed/scientific threshold (switch
// to scientific at exponent>=precision), so this is gFormat's own "strip one
// leading 0" idiom at a different precision, not a new algorithm. Applied at
// the local/global STORE call sites via capNumericTextToStataLocalDigits
// below (BUG-660 fix round), not inside evalArith itself — a `scalar =` call
// site shares evalArith and must NOT be capped (see that function's
// evalArith(d) comment for the regression this move fixes).
//
// Fix round 2: ALSO applied to evalMacroExpr (the `` `=expr' `` twin) below,
// for the same reason -- see that function's own comment at its float-return
// arm. The original BUG-660 landing note said this surface stayed on
// pythonFloatRepr because "no oracle arm exercises it with an irrational
// value"; that was true only because postfileMathNamespace() had no Stata
// functions to BE irrational with. This same fix round widened it to
// postfileStataMathNamespace(), which does, so the gap became live.
// Review finding (v3): pythonIntStrFromDouble renders -0.0 as "-0" (%.0f),
// and -0.0 reaches the renderers below from ordinary arithmetic
// (`max(-0.4,-0.5)*0`, `-0.0`, std::round(-0.4)). StataBE 19.5 measured:
// all of these are 0 in local, global and display -- Stata has no negative
// zero. Every integer rendering in this layer goes through here.
std::string stataIntText(double d) { return pythonIntStrFromDouble(d == 0.0 ? 0.0 : d); }


// Fix round v4 (review): the 16-significant-digit rule above was not Stata's
// rule. StataBE 19.5 measured on 35 values (local, global, `=exp' and
// `=scalar(s)' all agree): the stored text is exactly
// strtrim(string(x, "%18.0g")), i.e. at most 17 characters for the magnitude
// (plus a leading `-`):
//   - fixed point when the integer part has at most 16 digits and, for
//     |x| < 1, when the 16-decimal form keeps at least as many significant
//     digits as e-notation would (.0000333333333333 fixed; 1e-5/3 ->
//     3.33333333333e-06); trailing fraction zeros are dropped, no leading 0;
//   - otherwise e-notation with the mantissa filling the 17 characters
//     (11 decimals for a 2-digit exponent, 10 for a 3-digit one), zeros kept:
//     1e16 -> 1.00000000000e+16, 1e-300 -> 1.0000000000e-300.
// The table-driven selftest arm local_text_rule_18_0g pins every measured row.
std::string stataLocalRealText(double d) {
  if (d == 0.0) return "0";  // also -0.0: Stata has no negative zero
  const std::string sign = d < 0 ? "-" : "";
  const double a = std::fabs(d);
  char buf[400];
  // e-notation candidate: 12 significant digits, 11 for a 3-digit exponent.
  std::snprintf(buf, sizeof(buf), "%.11e", a);
  std::string eText(buf);
  if (eText.size() > 17) {
    std::snprintf(buf, sizeof(buf), "%.10e", a);
    eText = buf;
  }
  const int eDigits = static_cast<int>(eText.find('e')) - 1;  // mantissa digits (minus the '.')
  if (a < 1.0) {
    std::snprintf(buf, sizeof(buf), "%.16f", a);
    std::string f(buf);  // "0.dddddddddddddddd" or "1.0000000000000000" after rounding
    if (f[0] == '0') {
      const size_t firstNz = f.find_first_not_of('0', 2);
      const int fixedSig = firstNz == std::string::npos ? 0 : static_cast<int>(f.size() - firstNz);
      if (fixedSig < eDigits) return sign + eText;
      f.erase(f.find_last_not_of('0') + 1);
      if (f.back() == '.') f.pop_back();
      return sign + f.substr(1);
    }
    return sign + "1";  // rounded up to 1 at 16 decimals
  }
  // |x| >= 1: integer digits k (after rounding at 16-k decimals) must be <= 16.
  std::snprintf(buf, sizeof(buf), "%.0f", std::floor(a));
  const int k = static_cast<int>(std::strlen(buf));
  if (k > 16) return sign + eText;
  std::snprintf(buf, sizeof(buf), "%.*f", 16 - k, a);
  std::string f(buf);
  const size_t dot = f.find('.');
  if ((dot == std::string::npos ? f.size() : dot) > 16) return sign + eText;
  if (dot != std::string::npos) {
    f.erase(f.find_last_not_of('0') + 1);
    if (f.back() == '.') f.pop_back();
  }
  return sign + f;
}

}

namespace piet::eval {
using namespace macro_detail;


// BUG-920: forvalues stores each loop value as macro text by this same rule
// (oracle w2_prog_controlflow pc_fv_frac: 0(0.1)1 assigns `.3`, not
// `0.30000000000000004`, and `.7999999999999999`). loops.cpp declares this
// wrapper itself (macros.h is not the loop runner's to widen), so the loop
// and the local/global store share one rule instead of a second copy of it.
std::string stataMacroRealText(double d) { return stataLocalRealText(d); }

}

namespace piet::eval::macro_detail {


// BUG-660 fix round: the local/global STORAGE sites apply Stata's macro
// real-to-text rule (stataLocalRealText, %18.0g) on top of evalArith's own
// round-trip-exact text, instead of evalArith doing it for every caller (see
// evalArith's own comment on its two float-return arms). Re-parses `text`
// (evalArith's numeric output, pythonFloatRepr-formatted) and re-renders it;
// anything that does not parse as a bare finite double whole-string — a
// STRING result, a missing token (".", ".a") — is returned unchanged, since
// only a genuinely computed real needs the rule.
std::string capNumericTextToStataLocalDigits(const std::string& text) {
  // An INTEGER rendering of at most 16 digits is already Stata's text
  // (%18.0g keeps up to 16 integer digits). Longer integers are re-rendered:
  // StataBE stores `local x = 1e16` as 1.00000000000e+16 (fix round v4).
  {
    const size_t start = (!text.empty() && text[0] == '-') ? 1 : 0;
    if (text.size() > start && text.size() - start <= 16 &&
        text.find_first_not_of("0123456789", start) == std::string::npos) {
      return text;
    }
  }
  // strtod, not std::stod: stod throws out_of_range on a subnormal (ERANGE),
  // and `local x = 5e-324` is an ordinary value (StataBE: 4.9406564584e-324).
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return text;
  char* end = nullptr;
  const double d = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(d)) return text;
  return stataLocalRealText(d);
}


// BUG-1006: a numeric r()/e() spliced by MACRO expansion (`` `r(N)' ``,
// `` `e(r2)' ``) is macro text, so it takes the same %18.0g rule as a stored
// local: count's r(N) is `8`, not `8.0` (w2aq_histogram_family.log every
// hk_*_g record's nonmissing field; w2af_macro_text_file.log mt_num_r_count
// `<8>`, mt_num_r_summean `<6.791666666666667>`, mt_num_e_r2
// `<.0004984216647284>`). A missing value stays its token. The BARE r(N) is an
// expression operand and keeps matrixElementText.
std::string macroRealText(double v) {
  if (std::isnan(v) || std::fabs(v) >= kStataMissingSentinel) return stataMissingText(v);
  return stataLocalRealText(v);
}


// BUG-196: inside the evaluator missing is the band |d| >= 2^1023 (exprfns.h);
// on the way out it has to be a token again. MAGNITUDE, not signed value: Stata
// renders `-.` as `.` (internal capture missing_range_oracle.log,
// row `negated-dot`), and an out-of-range result is `.` on either side
// (`scalar h8 = -1e308`, w2e_float_followups.log:1770). `2*.` and inf are
// admitted too (row `dot-times-two`). c(maxdouble) is one ulp under 2^1023 and
// stays a real number (rows sentinel-exact / sentinel-negated). NaN compares
// false and is handled by its own paths.
//
// Applied at all three of evalArith's exits (fast path, bare-dot retry,
// extended path), so a band value never leaves as a decimal.
bool isStataMissingResult(double d) {
  return std::fabs(d) >= kStataMissingSentinel;
}


// BUG-710: a STRING cell (or a string scalar's value) read into an expression
// becomes a literal the evaluator lexes itself: pushed onto the caller's
// protectStringLiterals spans and referenced by its `"0L<k>L0"` placeholder, so
// no later pre-pass reads its content. nullopt for text holding a `"`, which
// this layer's literals cannot carry; the read is then left in place and the
// line is refused downstream, the rule display's bindBareVarsToObs1 keeps.
std::optional<std::string> stringReadPlaceholder(const std::string& text,
                                                 std::vector<std::string>& spans) {
  if (text.find('"') != std::string::npos) {
    // A value holding `"` is carried as Stata's compound-quoted literal `"..."',
    // which the evaluator lexes verbatim (pyeval.cpp, EM3b). 007's Wave 3 reads
    // `local value = G4_1[1]` from a cell holding the survey question, quotes and
    // newlines included, and StataBE takes it. Only text that would itself close
    // or open a compound level (`"'` or `` `" ``) cannot be wrapped and stays
    // refused.
    if (text.find("\"'") != std::string::npos || text.find("`\"") != std::string::npos)
      return std::nullopt;
    spans.push_back("`\"" + text + "\"'");
    return "\"0L" + std::to_string(spans.size() - 1) + "L0\"";
  }
  spans.push_back("\"" + text + "\"");
  return "\"0L" + std::to_string(spans.size() - 1) + "L0\"";
}


// BUG-710: one EvalSession::varObs reading as expression text -- a number
// parenthesised (a negative value stays one operand), a missing value as its
// `.`/`.a` token in parentheses (replaceBareDotMissing turns it into the
// evaluator's double), a string as a literal. Outside 1.._N the value is
// missing: `.` for a numeric column, "" for a string one (Stata [U] 13.7;
// w2i_varsub a_05..a_07, d_03, d_04). nullopt = the text cannot be carried.
std::optional<std::string> varReadText(const EvalSession::VarObsReading& r,
                                       std::vector<std::string>& spans) {
  if (!r.inRange) return r.isString ? stringReadPlaceholder("", spans) : std::string("(.)");
  if (r.isString) return stringReadPlaceholder(r.text, spans);
  if (r.text.empty()) return std::string("(.)");
  return "(" + r.text + ")";
}


// BUG-710: a BARE dataset variable in a scalar expression (`local k = xd`,
// `scalar k2 = xd`, `local ks = s`) is its first observation, as in StataBE
// (w2i_varsub f_01..f_04; `if xd > 0` reads it the same way, loops.cpp
// BUG-921). Every identifier that is not a call (`name(`), a subscript
// (`name[`, substituteVarSubscripts' business), a member or missing-kind
// letter (`.name`, `.a`), or part of a number or a literal placeholder, and that
// EvalSession::varObs knows as a variable, is replaced by varReadText. A name
// the reader does not know is left exactly where it was, so a scalar, an e()
// alias already resolved or an unknown name reaches the evaluator as before.
// Unset hook (session-less harnesses) = unchanged input.
std::string bindBareVariables(const std::string& in, EvalSession& session,
                              std::vector<std::string>& spans) {
  if (!session.varObs) return in;
  std::string out;
  size_t i = 0, last = 0;
  bool any = false;
  while (i < in.size()) {
    const char c = in[i];
    if (c == '"') {  // a placeholder (or an unterminated quote): data
      const size_t close = in.find('"', i + 1);
      i = close == std::string::npos ? in.size() : close + 1;
      continue;
    }
    if (!isIdentStart(c) || (i > 0 && (isIdentChar(in[i - 1]) || in[i - 1] == '.'))) {
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < in.size() && isIdentChar(in[j])) ++j;
    size_t k = j;
    while (k < in.size() && (in[k] == ' ' || in[k] == '\t')) ++k;
    if (k < in.size() && (in[k] == '(' || in[k] == '[')) {
      i = j;
      continue;
    }
    const std::string name = in.substr(i, j - i);
    std::optional<EvalSession::VarObsReading> r = session.varObs(name, 1);
    std::optional<std::string> text;
    if (r) text = varReadText(*r, spans);
    if (!text) {
      i = j;
      continue;
    }
    out.append(in, last, i - last);
    out += *text;
    last = j;
    i = j;
    any = true;
  }
  if (!any) return in;
  out.append(in, last, std::string::npos);
  return out;
}


// name-binding-display-literals: two literal forms Stata reads and this
// evaluator's Python lexer does not.
//  * `_pi`, the built-in constant: `local l = _pi`, `scalar s = _pi` and
//    `` `=_pi' `` were r(111) / unexpanded, where Stata reads 3.141592653589793
//    (w2av_name_binding bd_loc_pi, bd_sca_pi, bd_inl_pi, bd_dy_ilpi).
//  * A decimal literal with leading zeros, `05`: Python refuses it, so `local l
//    = 05+1` and `display 05+1` were r(198) where Stata reads 6
//    (bd_loc_lead0, bd_sca_lead0, bd_inl_lead0, bd_dy_lead0). The zeros before
//    another digit are dropped; `0`, `0.5`, `.05` and `1e05` are untouched.
// Quoted text (and a protected literal's placeholder) is data and is skipped.
std::string normalizeStataLiterals(const std::string& in) {
  std::string s = in;
  if (s.find("_pi") != std::string::npos) {
    static const std::string kPiText = pythonFloatRepr(3.14159265358979323846);
    std::vector<std::string> spans;
    s = restoreStringLiterals(substituteBareName(protectStringLiterals(s, &spans), "_pi", kPiText),
                              spans);
  }
  if (s.find('0') == std::string::npos) return s;
  std::string out;
  out.reserve(s.size());
  bool inQuote = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') inQuote = !inQuote;
    if (!inQuote && c == '0' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
      // The first digit of a number: no identifier character or `.` before it,
      // and no digit either (a zero inside `105` is not leading).
      size_t b = out.size();
      const bool startsNumber = b == 0 || !(isIdentChar(out[b - 1]) || out[b - 1] == '.');
      if (startsNumber) continue;  // drop this leading zero; the next digit decides again
    }
    out.push_back(c);
  }
  return out;
}


// name-binding-display-literals: a matrix subscript that names its row or
// column, `M["d1",1]` / `M[1,"x"]`, is the index of that name in the matrix's
// stripe ([P] matrix subscripting; Stata takes the bare name or `eq:name`).
// The literal reaches here as a protected placeholder (`"0L<k>L0"`, its text in
// `spans`); the subscript became `M[1,1]` for the element pass, and a name the
// stripe lacks stays as typed for that pass's refusal. Before, the subscript
// was no element at all and `local l = delta["d1",1]` was r(111) `delta not
// found` where StataBE reads 1 (w2av_name_binding bd_loc_matrow; bd_*_matrowa,
// where a scalar d1 beside the row name d1 must not reach the literal).
std::string resolveMatrixStripeSubscripts(const std::string& in, const EvalSession& session,
                                          const std::vector<std::string>& spans) {
  if (in.find('[') == std::string::npos || in.find("\"0L") == std::string::npos) return in;
  static const std::regex reSub(
      R"RX((?:\b|(?=[\x80-\xff]))([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[\s*("0L\d+L0"|\d+)\s*,\s*("0L\d+L0"|\d+)\s*\])RX");
  const auto stripeIndex = [&spans](const std::string& tok,
                                    const std::vector<std::string>& names) -> std::string {
    if (tok.empty() || tok[0] != '"') return tok;
    const std::string lit = restoreStringLiterals(tok, spans);
    if (lit.size() < 2 || lit.front() != '"' || lit.back() != '"') return std::string();
    const std::string want = lit.substr(1, lit.size() - 2);
    for (size_t k = 0; k < names.size(); ++k) {
      const std::string& n = names[k];
      const size_t colon = n.rfind(':');
      if (n == want || (colon != std::string::npos && n.compare(colon + 1, std::string::npos, want) == 0))
        return std::to_string(k + 1);
    }
    return std::string();
  };
  return regexSubFn(reSub, in, [&](const std::smatch& m) -> std::string {
    if (m.str(2)[0] != '"' && m.str(3)[0] != '"') return m.str(0);
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    const std::string r = stripeIndex(m.str(2), mat->rownames);
    const std::string c = stripeIndex(m.str(3), mat->colnames);
    if (r.empty() || c.empty()) return m.str(0);
    return m.str(1) + "[" + r + "," + c + "]";
  });
}

}

namespace piet::eval {
using namespace macro_detail;


std::optional<std::string> evalArith(const std::string& expr, EvalSession* session,
                                     std::string* raisedError, bool* resultIsString) {
  dprof::EScope _z(dprof::kEEvalArith);
  // name-binding-display-literals: x/0 is missing here, as in Stata (display
  // 1/0 `.`, `local r = 1/0` `.`; w2av_name_binding bd_dy_div0, bd_loc_div0,
  // bd_sca_div0). It was a ZeroDivisionError and so r(198).
  StataDivisionScope stataDivision;
  dprof::ezoneBegin(dprof::kEEvalArithPre);
  std::string cleaned = strip(expr);
  // BUG-501: literals out of harm's way before any pre-pass runs (see
  // protectStringLiterals). No-op for an expression with no quote in it.
  std::vector<std::string> litSpans;
  cleaned = protectStringLiterals(cleaned, &litSpans);
  // FU-875: the blanks inside ( ) and [ ] go, as Stata reads the expression
  // (core/expr_blanks.h): `display abs(1 2)` is 12 and `local m = max(1 2, 3)`
  // 12, as cobra's generate answers them. The literals are placeholders here.
  cleaned = cecon::squeeze_bracketed_blanks(cleaned);
  // ... and brackets that do not pair are r(132) "too few ')' or ']'" before any
  // name is read, as in generate (`display tq(2010q1`, `local m = max(1 2`).
  if (const std::string unbalanced = cecon::bracket_imbalance(cleaned); !unbalanced.empty()) {
    if (raisedError) *raisedError = "r(132): " + unbalanced;
    dprof::ezoneEnd(dprof::kEEvalArithPre);
    return std::nullopt;
  }
  // BUG-1463: a c(name) still here is read through the session's c-class hook
  // (cclass.cpp). display/local/scalar substitute it before they call in, typed
  // (piet::substituteCRefs); the inline `=exp' expansion did not, so `set
  // processors `=min(4, c(processors_max))'' died as "c() is a Stata function
  // this version of PIET does not implement". The hook gives text: a number (or
  // a missing `.`) splices as one, anything else as a string literal.
  if (session != nullptr && session->cValue && cleaned.find("c(") != std::string::npos) {
    static const std::regex reCRef(R"((^|[^\w\x80-\xff.])c\(([A-Za-z_]\w*)\))");
    static const std::regex reNumber(R"(^-?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$|^\.[a-z]?$)");
    cleaned = regexSubFn(reCRef, cleaned, [&](const std::smatch& m) -> std::string {
      const std::optional<std::string> v = session->cValue(m.str(2));
      if (!v.has_value()) return m.str(0);
      if (std::regex_match(*v, reNumber)) return m.str(1) + "(" + *v + ")";
      const std::optional<std::string> lit = stringReadPlaceholder(*v, litSpans);
      return lit.has_value() ? m.str(1) + *lit : m.str(0);
    });
  }
  // BUG-446: td()/tc()/tq()... literals become their value (see liftDateLiterals).
  {
    std::string badLiteral;
    cleaned = liftDateLiterals(cleaned, &badLiteral);
    if (!badLiteral.empty()) {
      if (raisedError) *raisedError = "r(198): " + badLiteral + " invalid";
      dprof::ezoneEnd(dprof::kEEvalArithPre);
      return std::nullopt;
    }
  }
  // BUG-512: `_N` / `_n` (see substituteObsSpecials). Ahead of the matrix
  // pre-pass on purpose: that pass only recognises LITERAL subscripts, and an
  // index is an expression like any other.
  if (session != nullptr) cleaned = substituteObsSpecials(cleaned, *session);
  cleaned = normalizeStataLiterals(cleaned);  // `_pi`, `05`
  if (session != nullptr) cleaned = resolveMatrixStripeSubscripts(cleaned, *session, litSpans);
  // BUG-710: a dataset-variable subscript `VAR[i]` is read NOW, through the one
  // cell reader (EvalSession::varObs), and its value travels on as text: `local a
  // = xd[1]` was r(111) (the matrix pass below named xd a missing matrix) and
  // `scalar b = xd[1]` stored the source text `xd[1]`, which every later read
  // re-evaluated against the data of THAT moment -- a replace, a gsort or a
  // restore moved a scalar defined before it (w2i_varsub A-E, i_02..i_20;
  // FU-1324). After `_N`/`_n` so an index may use them, and ahead of the matrix
  // pre-pass, which keeps every name that is a matrix and every two-index
  // subscript (substituteVarSubscripts skips both).
  if (session != nullptr && session->varObs && cleaned.find('[') != std::string::npos)
    cleaned = substituteVarSubscripts(cleaned, *session, nullptr, nullptr, &litSpans);
  // ^ -> ** (Stata exponentiation)
  {
    std::string tmp;
    tmp.reserve(cleaned.size() + 8);
    for (char c : cleaned) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    cleaned = tmp;
  }
  // \bln\b -> log
  {
    // A3: `ln` is a two-character literal; the regex only fires when it is
    // present, so let a find() decide instead of scanning every expression.
    // Unicode names: identifier-class boundaries (`lnدخل` is a name, not ln).
    static const std::regex reLn(R"((^|[^\w\x80-\xff])ln(?![\w\x80-\xff]))");
    if (cleaned.find("ln") != std::string::npos)
      cleaned = std::regex_replace(cleaned, reLn, "$1log");
  }

  // colnumb(M, "name") reads its name out of a string literal, which
  // protectStringLiterals has already swapped for a `0L<k>L0` placeholder, so
  // substituteMatrixRefs never matched and every colnumb() answered `.`
  // (w2x_fn_string_math fx_colnumb_macro/_fv/_disp/_if). The call's own literal
  // is put back here, and only when the matrix exists -- the substitution then
  // consumes it. colnumb() of a matrix that does not exist is r(111)
  // (fx_colnumb_nomat), never a value.
  if (session != nullptr && !litSpans.empty() &&
      (toLower(cleaned).find("colnumb") != std::string::npos ||
       toLower(cleaned).find("rownumb") != std::string::npos)) {
    static const std::regex reColnumbLit(
        R"RX(\b((?:col|row)numb\s*\(\s*)([A-Za-z_\x80-\xff][\w\x80-\xff]*)(\s*,\s*)("0L\d+L0")(\s*\)))RX", std::regex::icase);
    std::string missingMat;
    cleaned = regexSubFn(reColnumbLit, cleaned, [&](const std::smatch& m) -> std::string {
      if (session->findMatrix(m.str(2)) == nullptr) {
        if (missingMat.empty()) missingMat = m.str(2);
        return m.str(0);
      }
      return m.str(1) + m.str(2) + m.str(3) + restoreStringLiterals(m.str(4), litSpans) + m.str(5);
    });
    if (!missingMat.empty()) {
      if (raisedError) *raisedError = "r(111): matrix " + missingMat + " not found";
      dprof::ezoneEnd(dprof::kEEvalArithPre);
      return std::nullopt;
    }
  }

  // WS-C matrix pre-substitution.
  if (session != nullptr) {
    std::string lower = toLower(cleaned);
    if (cleaned.find('[') != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find("colnumb") != std::string::npos ||
        lower.find("rownumb") != std::string::npos ||
        lower.find("el(") != std::string::npos || lower.find("mreldif") != std::string::npos) {
      cleaned = substituteMatrixRefs(cleaned, *session);
      // FU-118: a matrix operand the pass could not answer is a named error
      // here, on the r(NNN) channel every caller already maps (display,
      // `local =`, `scalar =` -- which then creates no scalar, FU-348).
      const std::string matErr = unresolvedMatrixOperandError(cleaned, *session);
      if (!matErr.empty()) {
        if (raisedError) *raisedError = matErr;
        dprof::ezoneEnd(dprof::kEEvalArithPre);
        return std::nullopt;
      }
    }
  }

  // BUG-1419: scalar(NAME) of a STRING scalar is that string, as the bare name
  // is below: `display scalar(s1) + "x"` with s1 = "-3" is -3x in StataBE 19.5
  // (x1b/a1.do C2). Here it is a protected literal (stringReadPlaceholder), so
  // no later pre-pass reads inside it, as resolveEstScalarRefs' raw one would be.
  if (session != nullptr && !session->stringScalars.empty() &&
      cleaned.find("scalar(") != std::string::npos) {
    static const std::regex reStrScalarCall(
        R"((^|[^\w\x80-\xff])scalar\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))");
    cleaned = regexSubFn(reStrScalarCall, cleaned, [&](const std::smatch& m) -> std::string {
      const std::string* v = session->scalars.find(m.str(2));
      if (v == nullptr || !session->isStringScalar(m.str(2))) return m.str(0);
      if (auto lit = stringReadPlaceholder(*v, litSpans)) return m.str(1) + *lit;
      return m.str(0);
    });
  }
  if (session != nullptr) {
    std::string bErr;
    cleaned = resolveEstScalarRefs(cleaned, *session, &bErr);
    if (!bErr.empty()) {
      // BUG-456: `_b[nosuch]` is r(111) on the local/scalar/display surfaces
      // too, not a silent `.` -- forwarded on the BUG-246/249 `r(NNN): ...`
      // channel that classifyFailedEqExpr already honours.
      // BUG-456 follow-up: r(111) (unknown coefficient) or r(109) (a number
      // in a string-typed slot); splitErrCode keeps whichever the resolver
      // raised instead of stamping r(111) on both.
      if (raisedError) {
        const std::string code = splitErrCode(&bErr);
        *raisedError = code + ": " + bErr;
      }
      dprof::ezoneEnd(dprof::kEEvalArithPre);
      return std::nullopt;
    }
    // _rc — the bare last-command return code local.
    // A3: same shape — `_rc` cannot match without the literal.
    // Unicode names (lane M): the boundary is the identifier class, not
    // ECMAScript's ASCII `\b` -- `\b_rc\b` matched the tail of a variable
    // named `دخل_rc` (an Arabic byte is a non-word character to `\b`).
    static const std::regex reRc(R"((^|[^\w\x80-\xff])_rc(?![\w\x80-\xff]))");
    std::smatch m;
    if (cleaned.find("_rc") != std::string::npos && std::regex_search(cleaned, m, reRc)) {
      std::string rcv = session->locals.get("_rc", "0");
      if (!pythonFloat(rcv)) rcv = "0";
      // A callback, not a "$1..." format: rcv is digits, and "$10" reads as group 10.
      cleaned = regexSubFn(reRc, cleaned, [&rcv](const std::smatch& mm) { return mm.str(1) + rcv; });
    }
    // Bare scalar-name substitution (insertion order; (?!\s*\() spares
    // same-named function CALLS; value inserted literally).
    dprof::EScope _zs(dprof::kEEvalArithScalars);
    for (const auto& [sn, sv] : session->scalars.items()) {
      if (cleaned.find(sn) != std::string::npos) {
        // BUG-710: a dataset variable wins over a same-named scalar (Stata's
        // rule, which loops.cpp's condition binder and the dispatcher's
        // substituteStoredScalars already keep); bindBareVariables reads it.
        if (session->varObs && session->varObs(sn, 1)) continue;
        // BUG-710: a STRING scalar is a literal, not a bare name: `local x =
        // d_st` substituted `ab2` and stored the source text `d_st`
        // (w2i_varsub d_02).
        if (session->isStringScalar(sn)) {
          if (auto lit = stringReadPlaceholder(sv, litSpans)) {
            const std::string litText = *lit;
            cleaned = substituteBareNameWith(cleaned, sn,
                                             [&litText]() -> const std::string& { return litText; });
            continue;
          }
        }
        // EXPRLAYER-5: was a std::regex CONSTRUCTED (i.e. compiled) per scalar
        // per expression — `\bNAME\b(?!\s*\()` walked directly instead.
        cleaned = substituteBareName(cleaned, sn, valueRefText(sv));  // FU-842
      }
    }
    // BUG-710: a bare dataset variable reads observation 1 (bindBareVariables).
    cleaned = bindBareVariables(cleaned, *session, litSpans);
    // BUG-456 follow-up (2026-09-09): the string-slot type test runs AGAIN here,
    // after the stored scalars have been substituted -- `scalar one = 1` then
    // `local sf = strpos(one,"1")` answered 1 at rc 0 where Stata is r(109)
    // (measured, oracle2.log P10), because the resolver's own copy of this test
    // ran before this loop existed to substitute anything.
    if (raisedError != nullptr && raisedError->empty()) {
      std::string badCall;
      if (stringFnNumericArgImpl(cleaned, &badCall)) {
        *raisedError = stringSlotMismatchError(badCall);
        dprof::ezoneEnd(dprof::kEEvalArithPre);
        return std::nullopt;
      }
    }
  }

  dprof::ezoneEnd(dprof::kEEvalArithPre);

  // Fast path: pure-arithmetic whitelist, EMPTY eval namespace.
  dprof::ezoneBegin(dprof::kEEvalArithWhitelist);
  const bool safeArith = isSafeArith(cleaned);  // EXPRLAYER-2: was reSafeArith()
  dprof::ezoneEnd(dprof::kEEvalArithWhitelist);
  if (safeArith) {
    PyNamespace empty;
    PyEvalOutcome out = evalPyExprDetailed(cleaned, empty);
    if (out.status != PyEvalOutcome::Status::Value) {
      // BUG-196: `display (.)` printed the literal `(.)`.
      //
      // A bare `.` is Stata system-missing and has to reach the evaluator as
      // the missing sentinel, or Python raises SyntaxError on it. That
      // substitution lived below, on the EXTENDED path only — and `.`, `(.)`
      // and `2+.` are built entirely from fast-path characters, so they took
      // THIS branch, failed, returned nullopt, and `display` fell through to
      // its echo-the-literal-text fallback. `display .` looked right the whole
      // time purely because echoing the literal text of `.` happens to be the
      // correct answer; nothing was evaluating it.
      //
      // Applied as a RETRY rather than hoisted above the whitelist fork,
      // because evalArith is a hot path (25.6% of S02's profile) and this
      // substitution allocates and walks char by char, with no cheap guard
      // available — `find('.')` does not discriminate, since ordinary decimal
      // literals contain dots. On this shape the success path pays nothing at
      // all: the only inputs that reach the retry are the ones that return
      // nullopt today, so the change is strictly additive in cost as well as
      // in behaviour.
      //
      // It is additive in behaviour because within the fast path's alphabet,
      // [0-9\s+\-*/().^eE], a bare `.` cannot occur in anything Python
      // evaluates successfully: `.` is legal there only inside a float literal
      // — which replaceBareDotMissing leaves alone, since it rewrites a `.`
      // only when neither neighbour is alnum/`_`/`.`, so `3.14`, `.5` and `1.`
      // all pass through untouched — or as attribute access, which needs an
      // identifier, and the only letters admitted here are e/E, undefined in
      // the empty namespace.
      //
      // The retry is TRUSTED ONLY WHEN IT LANDS ON A CLEAN MISSING. PIET has
      // no missing TYPE — the sentinel is an ordinary double and Python
      // arithmetic treats it as one — so an expression like `./2` comes back
      // as 4.49e+307, a number Stata would never print (Stata says `.`; row
      // `dot-div-two`). Emitting it would trade a visibly-unevaluated echo for
      // a confidently wrong number, which is the worse of the two failures. So
      // anything that is not recognisably missing falls back to the nullopt
      // this branch already returned before BUG-196, leaving those cases
      // exactly as they were. Real missing propagation belongs to a missing
      // type in the expression layer and is not claimed here.
      // BUG-1388: the only names this alphabet admits are runs of e/E, and
      // Stata defines none of them (`local d = e + 1` is `e not found`,
      // r(111)). The NameError goes out on the channel the extended path
      // below uses, so the caller's classifier answers r(111) rather than
      // r(198) invalid syntax. A bare-dot retry cannot cure an unbound name.
      if (raisedError && out.status == PyEvalOutcome::Status::Raised &&
          out.error.rfind("NameError", 0) == 0) {
        *raisedError = out.error;
        return std::nullopt;
      }
      const std::string withMissing = replaceBareDotMissing(cleaned);
      if (withMissing == cleaned) return std::nullopt;  // no bare dot to retry with
      const PyEvalOutcome retry = evalPyExprDetailed(withMissing, empty);
      if (retry.status != PyEvalOutcome::Status::Value) return std::nullopt;
      std::optional<double> rd;
      if (retry.value.isFloat()) rd = retry.value.floatVal();
      else if (retry.value.isInt()) rd = static_cast<double>(retry.value.intVal());
      // BUG-249: kind-preserving — a bare `.e` (the one extended code whose
      // letter is inside the fast alphabet) must come back `.e`, not `.`.
      if (rd && isStataMissingResult(*rd)) return stataMissingText(*rd);
      return std::nullopt;
    }
    const PyValue& r = out.value;
    // Python: isinstance(result,(int,float)) — bool unreachable on this
    // path (no comparison chars in the whitelist).
    if (r.isInt() || r.isBool()) return std::to_string(r.intVal());
    if (r.isFloat()) {
      double d = r.floatVal();
      // The same exit test as the retry and the extended path: a missing result
      // (`-1e308`, r(sd) after meanonly substituted as `.`) leaves as its token.
      if (std::isnan(d) || isStataMissingResult(d)) return stataMissingText(d);
      if (pyIsInteger(d)) return stataIntText(d);
      // BUG-660 fix round: evalArith itself stays ROUND-TRIP-EXACT
      // (pythonFloatRepr) for every caller -- `scalar s = expr` stores this
      // text straight into session.scalars, and program.cpp's return/
      // ereturn scalar and handlers_resample.cpp's exp_list both parse it
      // back with std::stod expecting the full precision. Only local/global
      // storage wants Stata's capped 16-sig-fig macro convention, applied by
      // their own call sites (see capNumericTextToStataLocalDigits below),
      // not here.
      return pythonFloatRepr(d);
    }
    return std::nullopt;
  }

  // Extended path (string fns like substr, math namespace): a bare `.` missing
  // must reach the evaluator as the Stata sentinel, not a Python SyntaxError.
  // Unconditional here — this path is not the hot one, and substr(s, 3, .)
  // depends on it (missing length == "to end of string").
  cleaned = replaceBareDotMissing(cleaned);

  dprof::ezoneBegin(dprof::kEEvalArithWhitelist);
  const bool safeArithExt = isSafeArithExt(cleaned);  // EXPRLAYER-2: was reSafeArithExt()
  dprof::ezoneEnd(dprof::kEEvalArithWhitelist);
  if (!safeArithExt) {
    return std::nullopt;  // contains characters that are unsafe
  }
  // BUG-501: the literals go back BEFORE the evaluator, which lexes them itself.
  cleaned = restoreStringLiterals(cleaned, litSpans);

  // EXPRLAYER-1c: this path added NOTHING to the registry, so it can use the
  // process-wide one directly instead of rebuilding 39 map entries per call.
  PyNamespace extNs;
  const PyNamespace* nsPtr = &stataMathNamespace();
  // BUG-660: fileexists() needs the session's cwd, which the process-wide,
  // session-less registry above cannot close over. Added as a per-call
  // override ONLY when the expression actually names it, so the
  // EXPRLAYER-1c fast path stays untouched (no map rebuild) for every other
  // extended-path call — the same "own map wins over base" lookup order
  // PyNamespace::base documents.
  //
  // Review finding (v2): runiform()/runiformint() likewise need the session
  // (its `set seed`, EvalSession::rngSeed) and get the same per-call install.
  if (session != nullptr && cleaned.find("runiform") != std::string::npos) {
    extNs.base = &stataMathNamespace();
    addSessionRngFns(extNs, *session);
    nsPtr = &extNs;
  }
  if (session != nullptr && cleaned.find("fileexists(") != std::string::npos) {
    extNs.base = &stataMathNamespace();
    const std::string cwd = session->cwd;
    extNs.fns["fileexists"] = [cwd](const std::vector<PyValue>& args) -> PyValue {
      if (args.size() != 1) throw PyExc("fileexists: TypeError arity");
      namespace fs = std::filesystem;
      std::string base = !cwd.empty() ? cwd : fs::current_path().u8string();
      fs::path p(pythonStr(args[0]));
      if (p.is_relative()) p = cecon::to_native_path(base) / p;
      std::error_code ec;
      return PyValue::boolean(fs::exists(p, ec));
    };
    nsPtr = &extNs;
  }
  PyEvalOutcome out = evalPyExprDetailed(cleaned, *nsPtr);
  // BUG-374(3): Python's math functions RAISE for an out-of-domain argument
  // (pyeval's mSqrt/mLog/mLog2/mLog10 throw "ValueError: math domain error");
  // Stata returns MISSING. `display sqrt(-1)` was reported as a parse gap and is
  // not one — the grammar handles unary minus fine (parseFactor), the call just
  // threw, and evalArith forwards only NameError/r(NNN) to the caller, so the
  // ValueError was dropped and display classified the empty error as r(198)
  // "invalid syntax". The engine's own expression evaluator already takes the
  // Stata side of this (frame.cpp's fin(): out-of-domain math becomes missing),
  // so this makes the display layer agree with it.
  if (out.status == PyEvalOutcome::Status::Raised &&
      out.error.find("math domain error") != std::string::npos) {
    return std::string(".");
  }
  if (out.status != PyEvalOutcome::Status::Value) {
    // BUG-088: distinguish a genuine raised error (an undefined name — e.g. an
    // unsupported extended macro function `_request(...)`) from a legitimately
    // non-arithmetic expression. Only a NameError is signalled to the caller; a
    // FnObject, a SyntaxError (`foo bar` trailing tokens), or a bad-arity/other
    // failure stays a silent nullopt so ordinary display output is byte-identical.
    // BUG-246/249 widen the channel: an `r(NNN): message` error is a function
    // announcing Stata's EXACT code (fnString's r(109) type mismatch is the
    // first) — forwarded whole, same contract as the NameError arm.
    if (raisedError && out.status == PyEvalOutcome::Status::Raised &&
        (out.error.rfind("NameError", 0) == 0 || out.error.rfind("r(", 0) == 0)) {
      *raisedError = out.error;
    }
    return std::nullopt;
  }
  const PyValue& r = out.value;
  // Stata fns return Python bools; coerce to 1/0 BEFORE numeric handling.
  if (r.isBool()) return r.boolVal() ? "1" : "0";
  if (r.isInt()) return std::to_string(r.intVal());
  if (r.isFloat()) {
    double d = r.floatVal();
    // BUG-249: a result still in the missing band comes back as its missing
    // TOKEN, kind preserved. Before the kind sentinels existed, `.a` reached
    // Python as a SyntaxError and fell back to the caller's literal echo —
    // rewriting it into a finite double (replaceBareDotMissing above) without
    // this gate would have PRINTED that double (`local x = .a` storing
    // 8.99e+307), a confident wrong number replacing a visible non-answer.
    // Any value in the band |d| >= 2^1023 is missing in StataBE's own
    // representation; c(maxdouble), one ulp below, is a real number.
    if (isStataMissingResult(d)) return stataMissingText(d);
    // BUG-374(2): NaN is Stata MISSING and prints as ".", not "nan". It reaches
    // here from any legitimately-missing result — `display mod(7,0)` is the
    // reported case — and slipped through because isStataMissingResult is
    // magnitude-based (its own comment says NaN escapes it) and pyIsInteger
    // rejects non-finite, so the value fell to pythonFloatRepr, which is
    // faithfully Python and answers "nan". The `` `=expr' `` twin in this same
    // file has always had this arm (see evalMacroExpr's `if (std::isnan(fval))
    // return "."`), so the two expression entry points disagreed about what a
    // missing result looks like.
    if (std::isnan(d)) return ".";
    if (pyIsInteger(d)) return stataIntText(d);
    // BUG-660 fix round: round-trip-exact here too — see the fast-path arm's
    // comment above.
    return pythonFloatRepr(d);
  }
  if (r.isStr()) {
    if (resultIsString != nullptr) *resultIsString = true;  // BUG-293
    return r.strVal();
  }
  return std::nullopt;
}

}

namespace piet::eval::macro_detail {


// BUG-660: evalMacroExpr's (the `` `=expr' `` inline form) own combined
// registry — mathNamespace(true) (postfile semantics, inf/nan bound) +
// addStataFns(), the same pairing exprfns.cpp's stataMathNamespace() builds
// for evalArith's withInfNan=false base. Before this, `local x = round(...)`
// and the inline `` `=round(...)' `` disagreed on whether round()/scalar()/
// invttail() etc. existed at all — postfileMathNamespace() alone has none of
// them, so every Stata-function call through the inline form was a
// NameError -> r(133) no matter what the function was. Static/process-wide
// for the same EXPRLAYER-1c reason stataMathNamespace() is: rebuilding
// ~115 fn-map entries on every `` `=expr' `` call would undo the whole
// optimization.
const PyNamespace& postfileStataMathNamespace() {
  static const PyNamespace kNs = [] {
    PyNamespace n = mathNamespace(/*withInfNan=*/true);
    addStataFns(n);
    addStataMacroOnlyFns(n);  // BUG-660 fix round: sum/group/runiform(int) here only
    return n;
  }();
  return kNs;
}

}

namespace piet::eval {
using namespace macro_detail;


// ---------------------------------------------------------------------------
// eval_macro_expr (postfile.py)

std::optional<std::string> evalMacroExpr(const std::string& expr, EvalSession& session) {
  dprof::EScope _z(dprof::kEEvalMacroExpr);
  StataDivisionScope stataDivision;  // `=1/0' is `.' (w2av_name_binding bd_inl_div0)
  std::string cleaned = strip(expr);
  if (cleaned.empty()) return std::nullopt;
  // BUG-512: `` `=_N' `` / `` `=_N-1' `` (w2_smalls.log inline_N 12|11). The
  // name passed this function's whitelist and then raised in the evaluator, so
  // the reference was left in the line and expandMacros reported a circular
  // definition after 16 passes -- `forvalues i = 1/`=_N'` died on it.
  cleaned = substituteObsSpecials(cleaned, session);
  cleaned = normalizeStataLiterals(cleaned);  // `_pi`, `05` (name-binding-display-literals)
  // BUG-718: `` `=__d_itrim[1]' `` -- a dataset variable read at an observation
  // (w2c_functions.log itrim_data and twelve sibling `data` records). The name
  // used to reach the whitelist and the evaluator as an unknown identifier, the
  // reference stayed in the line, and expandMacros reported non-convergence
  // after 16 passes (rc 1). After `_N`/`_n` above, so `x[_N]` reads row _N.
  {
    std::optional<std::string> whole;
    cleaned = substituteVarSubscripts(cleaned, session, &whole);
    if (whole) return whole;
  }

  {
    std::string tmp;
    tmp.reserve(cleaned.size() + 8);
    for (char c : cleaned) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    cleaned = tmp;
  }
  {
    // A3: `ln` is a two-character literal; the regex only fires when it is
    // present, so let a find() decide instead of scanning every expression.
    // Unicode names: identifier-class boundaries (`lnدخل` is a name, not ln).
    static const std::regex reLn(R"((^|[^\w\x80-\xff])ln(?![\w\x80-\xff]))");
    if (cleaned.find("ln") != std::string::npos)
      cleaned = std::regex_replace(cleaned, reLn, "$1log");
  }

  {
    std::string lower = toLower(cleaned);
    if (cleaned.find('[') != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find("el(") != std::string::npos ||
        lower.find("mreldif") != std::string::npos) {
      cleaned = substituteMatrixRefs(cleaned, session);
    }
  }

  // Namespace: postfile._MATH_NS (has inf/nan) + session scalars + r()
  // values as bare float names. dict-update order means a scalar named
  // after a function SHADOWS the function (consts win in evalPyExpr).
  // EXPRLAYER-1c: registry by reference; the per-call scalar/r() bindings go
  // into the own map, which outranks the base — the same shadowing the
  // dict-update order produced ("a scalar named after a function SHADOWS the
  // function": a const always outranks a fn, at any level).
  PyNamespace ns;
  ns.base = &postfileStataMathNamespace();  // BUG-660: was postfileMathNamespace() (no Stata fns)
  // Review finding (v2): session-bound runiform()/runiformint() (see
  // evalArith's own install for the rationale).
  if (expr.find("runiform") != std::string::npos) addSessionRngFns(ns, session);
  for (const auto& [sname, sval] : session.scalars.items()) {
    // name-binding-display-literals: a dataset variable (or a unique
    // abbreviation of one) wins over a same-named scalar, as in evalArith; the
    // name is left unbound here, so the expression falls to expandPass's
    // evalArith fallback, which reads the variable (w2av_name_binding
    // bd_inl_varsc `=v'` 10, not scalar v's 5; bd_inl_abbrsc `=lngd'`).
    if (session.varObs && cleaned.find(sname) != std::string::npos && session.varObs(sname, 1))
      continue;
    auto f = pythonFloat(sval);
    if (f) ns.consts[sname] = PyValue::floating(*f);
  }
  for (const auto& [rname, rval] : session.rValues.items()) {
    ns.consts[rname] = PyValue::floating(rval);
  }

  // Whitelist on the SPACE-stripped text (spaces only — not tabs; mirrors
  // cleaned.replace(" ", "")).
  {
    std::string nospace;
    nospace.reserve(cleaned.size());
    for (char c : cleaned) {
      if (c != ' ') nospace.push_back(c);
    }
    std::smatch wm;
    if (!std::regex_match(nospace, wm, rePostfileSafeArith())) return std::nullopt;
  }

  PyEvalOutcome out = evalPyExprDetailed(cleaned, ns);
  if (out.status == PyEvalOutcome::Status::Raised) return std::nullopt;
  if (out.status == PyEvalOutcome::Status::FnObject) {
    // Python: `return str(result)` — a bare builtin name evaluates to the
    // function object; str() of the math builtins is deterministic.
    return "<built-in function " + out.fnName + ">";
  }
  const PyValue& r = out.value;
  // bool -> int, then int/float formatting.
  if (r.isBool()) return r.boolVal() ? "1" : "0";
  if (r.isInt() || r.isFloat()) {
    double fval = r.isFloat() ? r.floatVal() : static_cast<double>(r.intVal());
    if (std::isnan(fval)) return ".";
    // A finite band value (pyeval's collapse and result rule return 2^1023)
    // is missing; printed as a decimal it would be a number.
    if (std::isfinite(fval) && std::fabs(fval) >= kStataMissingSentinel) return stataMissingText(fval);
    // Python: `if fval == int(fval) and not isinf(fval)` — int(inf) raises
    // OverflowError BEFORE the isinf guard, and the whole body sits in a
    // broad try/except -> ±inf yields None (the guard is dead code).
    if (std::isinf(fval)) return std::nullopt;
    // str(int(fval)), no "-0"; 17+ integer digits take Stata's e-notation
    // (fix round v4: `=1e16' is 1.00000000000e+16 in StataBE, fmt2.log).
    if (pyIsInteger(fval) && std::fabs(fval) < 1e16) return stataIntText(fval);
    // BUG-660 fix round 2: was pythonFloatRepr (Python's 17-sig-fig
    // round-trip repr) unconditionally -- fine for a plain arithmetic
    // expression, but this namespace now also reaches Stata's own functions
    // (round/normal/invttail/... -- postfileStataMathNamespace() above), and
    // Stata renders THEIR results the same 16-sig-fig, no-leading-zero way
    // local/global do (stataLocalRealText, applied to that surface via
    // capNumericTextToStataLocalDigits). Before this namespace widening these
    // calls were a NameError this function turned into std::nullopt (left
    // unexpanded); giving them Python-repr precision now would be a NEW
    // silent wrong answer, not the honest refusal they used to be.
    return stataLocalRealText(fval);
  }
  if (r.isStr()) return r.strVal();  // unreachable (whitelist has no quotes)
  return std::nullopt;
}

}

namespace piet::eval::macro_detail {


// name-binding-display-literals (review, round 5): the names of Stata 19.5's
// expression functions. An inline `=exp' whose evaluation stops on an unknown
// function expands to nothing at rc 0 only when Stata does not know the
// function either (bd_inl_nofn, `=nosuchfn_q(1)'); a NameError here only says
// that THIS evaluator lacks the name. Design decision: no list existed in
// piet/native, so this one is read off StataNow 19.5 itself -- every
// `{cmd:NAME(}` signature in ado/base/f/f_*.ihlp, the f_*.sthlp entries
// (binomiallc; the d/h/m/q/w anachronisms; normprob, uniform), the out-of-date
// table in f_normprob.sthlp (nchi ... tprob), the _by() family (byable), the
// pre-14 string names (whatsnew10to11: lower ... upper; length, index, trim,
// itrim) and invnorm/normd/normden/issym/uniform0, plus ty. Case matters, as
// it does in Stata (Ftail, not ftail). Erring toward membership costs a loud
// refusal; erring the other way is a silent empty expansion.
// BUG-1420: the names StataBE 19.5 answers in c() -- the 196 keys `creturn list`
// prints, plus the hidden ones measured beside them (seed, namelen,
// rngstate_mt64s; lane expr-sem x1b/a24.do, a25.do), plus 27 more that
// `creturn list` does not print and StataBE 19.5 still answers at rc 0
// (c(matsize) 400, c(flavor) IC, c(timeout1) 30, c(sortseed), c(java) 1, ...;
// over 500 names tried on 2026-11-03, every c(NAME) and `set NAME` in the
// manuals and variants of them), plus 248 more found in the identifier strings
// of Stata's own library. Any other c(NAME) is r(133) "c(NAME)
// undefined" in display, local, scalar and generate alike (c(bogus_setting),
// c(Linesize), c(n), c(seed0), c(dockable)), and `c(NAME)' is "". A hidden key
// missing here would expand to "" where Stata has a value, so a name found
// later belongs in this list. Case matters. The names resolveCValue serves are
// answered before this list is asked, so PIET's own c(obs) and c(sysdir_sample)
// keep their values.
bool isStataCName(const std::string& name) {
  static const std::unordered_set<std::string> kNames = {
      "ALPHA", "MP", "Mons", "Months", "N", "SE", "Wdays", "Weekdays", "adopath", "adosize",
      "alpha", "bit", "born_date", "byteorder", "cformat", "changed", "charlen", "clevel",
      "cmdlen", "coeftabresults", "collect_double", "collect_label", "collect_style",
      "collect_warn", "console", "copycolor", "current_date", "current_time", "dirsep",
      "docx_hardbreak", "docx_maxtable", "docx_paramode", "doeditbackup", "dots", "dp",
      "dtable_style", "dtascomplevel", "dyndoc_version", "edition", "edition_real", "emptycells",
      "epsdouble", "epsfloat", "eqlen", "etable_style", "filedate", "filename", "frame", "fredkey",
      "fvbase", "fvlabel", "fvtrack", "fvwrap", "fvwrapon", "graphics", "hostname", "httpproxy",
      "httpproxyauth", "httpproxyhost", "httpproxyport", "httpproxypw", "httpproxyuser",
      "include_bitmap", "iterlog", "java_heapmax", "java_home", "k", "lapack_openblas", "level",
      "linegap", "linesize", "locale_functions", "locale_icudflt", "locale_ui", "logmsg",
      "logtype", "lstretch", "machine_type", "macrolen", "matacache", "matafavor", "matalibs",
      "matalnum", "matamofirst", "mataoptimize", "matasolvetol", "matastrict", "max_N_theory",
      "max_cmdlen", "max_graphsize", "max_it_cvars", "max_it_fvars", "max_k_theory",
      "max_macrolen", "max_matdim", "max_memory", "max_width_theory", "maxbezierpath", "maxbyte",
      "maxdb", "maxdouble", "maxfloat", "maxint", "maxiter", "maxlong", "maxstrlvarlen",
      "maxstrvarlen", "maxvar", "maxvlabellen", "memory", "min_graphsize", "min_memory", "minbyte",
      "mindouble", "minfloat", "minint", "minlong", "mode", "more", "namelen", "namelenbyte",
      "namelenchar", "niceness", "noisily", "notifyuser", "obs_t", "odbcdriver", "odbcmgr", "os",
      "osdtl", "pagesize", "pdf_maxtable", "pformat", "pi", "playsnd", "printcolor", "processors",
      "processors_lic", "processors_mach", "processors_max", "pwd", "python_exec",
      "python_userpath", "rc", "reshape_favor", "reventries", "revkeyboard", "rmsg", "rmsg_time",
      "rng", "rng_current", "rngseed_mt64s", "rngstate", "rngstate_mt64s", "rngstream", "scheme",
      "scrollbufsize", "searchdefault", "seed", "segmentsize", "sformat", "showbaselevels",
      "showemptycells", "showomitted", "smallestdouble", "smoothfonts", "sort_current",
      "sortmethod", "sortrngstate", "stata_name", "stata_version", "svy_tab_comps", "sysdir_base",
      "sysdir_oldplace", "sysdir_personal", "sysdir_plus", "sysdir_site", "sysdir_stata",
      "table_style", "tabulate_comps", "tabulate_style", "tmpdir", "trace", "tracedepth",
      "traceexpand", "tracehilite", "traceindent", "tracenumber", "tracesep", "type",
      "update_interval", "update_prompt", "update_query", "username", "userversion", "varabbrev",
      "varkeyboard", "version", "width",
      // Hidden: answered by StataBE 19.5, not printed by `creturn list`.
      "charset", "checksum", "debug", "flavor", "flavor_real", "java", "java_version",
      "lapack_mkl", "matsize", "max_N_current", "max_k_current", "max_matsize",
      "max_preservemem", "max_width_current", "min_matsize", "rngstate_kiss32",
      "rngstate_mt64", "seed_kiss32", "seed_mt64", "seed_mt64s", "sortseed", "sysdir_updates",
      "timeout1", "timeout2", "unicode", "varlabelpos", "virtual",
      // Hidden as well, found by asking StataBE 19.5 for c(NAME) over the identifier
      // strings of its own library (review of lane expr-sem, 2026-11-03): rc 0 for
      // each, c(framesmax) 100, c(osclass) MACARM64, c(hasicu) 1, c(tol_1) 1e-09, the
      // collect_dlg_* family "". Their neighbours c(collect_dlg_bogus), c(tol_3) and
      // c(framesmaxx) are r(133), so no prefix stands in for the list.
      "UseConsoleFontLength", "bayesmhtmpfn", "buildfvinfo", "c_compiler", "c_compiler_fullvers",
      "c_compiler_vers", "cmdtitle", "coeftabfail", "collect_catch_backup", "collect_current",
      "collect_dlg__cons_position", "collect_dlg_cell_abbreviate", "collect_dlg_cell_bground_color",
      "collect_dlg_cell_border_color", "collect_dlg_cell_border_pattern",
      "collect_dlg_cell_border_smcl", "collect_dlg_cell_border_width",
      "collect_dlg_cell_cidelimiter", "collect_dlg_cell_cridelimiter",
      "collect_dlg_cell_dborder_color", "collect_dlg_cell_dborder_pattern",
      "collect_dlg_cell_empty", "collect_dlg_cell_fground_color", "collect_dlg_cell_fill_pattern",
      "collect_dlg_cell_font_color", "collect_dlg_cell_font_family", "collect_dlg_cell_font_size",
      "collect_dlg_cell_font_strikeout", "collect_dlg_cell_font_style",
      "collect_dlg_cell_font_underline", "collect_dlg_cell_font_variant",
      "collect_dlg_cell_font_weight", "collect_dlg_cell_halign", "collect_dlg_cell_latex",
      "collect_dlg_cell_margin_width", "collect_dlg_cell_maxLabel", "collect_dlg_cell_maximum",
      "collect_dlg_cell_minLabel", "collect_dlg_cell_minimum", "collect_dlg_cell_nformat",
      "collect_dlg_cell_sformat", "collect_dlg_cell_smcl", "collect_dlg_cell_valign",
      "collect_dlg_cid", "collect_dlg_col_atDelimiter", "collect_dlg_col_barDelimiter",
      "collect_dlg_col_binder", "collect_dlg_col_delimiter", "collect_dlg_col_dups",
      "collect_dlg_col_extraspace", "collect_dlg_col_nodelimiter", "collect_dlg_col_position",
      "collect_dlg_col_width", "collect_dlg_dim", "collect_dlg_dim1", "collect_dlg_dim10",
      "collect_dlg_dim2", "collect_dlg_dim3", "collect_dlg_dim4", "collect_dlg_dim5",
      "collect_dlg_dim6", "collect_dlg_dim7", "collect_dlg_dim8", "collect_dlg_dim9",
      "collect_dlg_dim_autolevels", "collect_dlg_dim_label", "collect_dlg_dim_level",
      "collect_dlg_dim_level_label", "collect_dlg_dim_spanheader", "collect_dlg_export_style_key",
      "collect_dlg_export_style_type", "collect_dlg_export_style_value",
      "collect_dlg_header_fvlevels", "collect_dlg_header_level", "collect_dlg_header_nformat",
      "collect_dlg_header_title", "collect_dlg_item_base", "collect_dlg_item_empty",
      "collect_dlg_item_omit", "collect_dlg_note", "collect_dlg_notes_bground_color",
      "collect_dlg_notes_count", "collect_dlg_notes_fground_color",
      "collect_dlg_notes_fill_pattern", "collect_dlg_notes_font_color",
      "collect_dlg_notes_font_family", "collect_dlg_notes_font_size",
      "collect_dlg_notes_font_strikeout", "collect_dlg_notes_font_style",
      "collect_dlg_notes_font_underline", "collect_dlg_notes_font_variant",
      "collect_dlg_notes_font_weight", "collect_dlg_notes_latex", "collect_dlg_notes_smcl",
      "collect_dlg_notes_tags", "collect_dlg_row_abbreviate", "collect_dlg_row_arrangement",
      "collect_dlg_row_atDelimiter", "collect_dlg_row_barDelimiter", "collect_dlg_row_binder",
      "collect_dlg_row_delimiter", "collect_dlg_row_dups", "collect_dlg_row_indent",
      "collect_dlg_row_length", "collect_dlg_row_nobinder", "collect_dlg_row_nodelimiter",
      "collect_dlg_row_position", "collect_dlg_row_spacer", "collect_dlg_row_span",
      "collect_dlg_row_truncate", "collect_dlg_row_wrap", "collect_dlg_row_wrapon",
      "collect_dlg_show_ci", "collect_dlg_side", "collect_dlg_stars_attach",
      "collect_dlg_stars_delimiter", "collect_dlg_stars_fortags", "collect_dlg_stars_label1",
      "collect_dlg_stars_label2", "collect_dlg_stars_label3", "collect_dlg_stars_label4",
      "collect_dlg_stars_label5", "collect_dlg_stars_nformat", "collect_dlg_stars_order",
      "collect_dlg_stars_prefix", "collect_dlg_stars_pvname", "collect_dlg_stars_results",
      "collect_dlg_stars_shownote", "collect_dlg_stars_suffix", "collect_dlg_stars_type",
      "collect_dlg_stars_value1", "collect_dlg_stars_value2", "collect_dlg_stars_value3",
      "collect_dlg_stars_value4", "collect_dlg_stars_value5", "collect_dlg_tab_atDelimiter",
      "collect_dlg_tab_barDelimiter", "collect_dlg_tab_binder", "collect_dlg_tab_delimiter",
      "collect_dlg_tab_dimBinder", "collect_dlg_tab_dimDelimiter", "collect_dlg_tab_nodelimiter",
      "collect_dlg_tag", "collect_dlg_title", "collect_dlg_title_bground_color",
      "collect_dlg_title_fground_color", "collect_dlg_title_fill_pattern",
      "collect_dlg_title_font_color", "collect_dlg_title_font_family",
      "collect_dlg_title_font_size", "collect_dlg_title_font_strikeout",
      "collect_dlg_title_font_style", "collect_dlg_title_font_underline",
      "collect_dlg_title_font_variant", "collect_dlg_title_font_weight", "collect_dlg_title_latex",
      "collect_dlg_title_smcl", "collect_export_version", "compactvarlist", "compile_number",
      "curl_version", "curscm", "dark_mode", "di_g", "excelsupport", "expr_revar", "filenamelen",
      "framesmax", "getsockopt", "graphexttext", "guixeqcmd", "hasharu", "hasicu", "helpcolor",
      "httpsendport", "httpuseragent", "importcase", "irngalways64", "java_debugger",
      "java_heap_committed", "java_heap_max", "java_heap_usage", "java_initialized",
      "java_javacall_classpath", "java_jconsole", "java_library_path", "java_system_classpath",
      "java_system_java", "java_vendor", "jitterseed", "jvm_override_options", "loghelp",
      "mapsymbols", "marginsatvars", "marginscmd", "marginsdxvars", "marginsmvars", "marginsrevars",
      "mata_hdfe", "matadebug", "matawarningaserror", "max_intpoints", "max_rngstreams",
      "maxoptions", "min_k", "netdebug", "osclass", "pollrate", "polltime", "postunlinktimeout",
      "prefix", "preserve_ttl", "procs_license", "procs_mach", "procs_use", "profiling",
      "regex_current", "regexmethod", "scheme_cscript", "slowrun", "spsssas_support", "sslrelax",
      "stata_serno", "stata_version_major", "staticlink", "suppress_statanow_update",
      "sysdir_sjplus", "sysdir_stbplus", "textresultsmode", "textsize", "tokenlen", "tol_0",
      "tol_1", "tol_2", "tol_r", "tracecode", "tracetime", "unlinktimeout", "update_oride",
      "url_allow_redirects", "useoldlapack", "varlabellen", "varlabellenbyte", "version_rng",
      "version_rng_kiss32", "version_rng_mt64", "version_rng_mt64s", "visible_graph_names"};
  return kNames.count(name) != 0;
}


bool isStataFunctionName(const std::string& name) {
  static const std::unordered_set<std::string> kNames = {
      "Cdhms", "Chms", "Clock", "Clockdiff", "Clockdiff_frac", "Clockpart", "Cmdyhms", "Cofc",
      "Cofd", "F", "Fden", "Ftail", "I", "J", "_by", "_byindex", "_bylastcall", "_byn1", "_byn2",
      "_caller", "_frval", "abbrev", "abs", "acos", "acosh", "age", "age_frac", "asin", "asinh",
      "atan", "atan2", "atanh", "autocode", "betaden", "binomial", "binomiallc", "binomialp",
      "binomialtail", "binormal", "birthday", "bofd", "byteorder", "c", "cauchy", "cauchyden",
      "cauchytail", "ceil", "char", "chi2", "chi2den", "chi2tail", "chiprob", "cholesky", "chop",
      "clip", "clock", "clockdiff", "clockdiff_frac", "clockpart", "cloglog", "cofC", "cofd",
      "coleqnumb", "collatorlocale", "collatorversion", "colnfreeparms", "colnumb", "colsof",
      "comb", "cond", "corr", "cos", "cosh", "d", "daily", "date", "datediff", "datediff_frac",
      "datepart", "day", "daysinmonth", "dayssincedow", "dayssinceweekday", "daysuntildow",
      "daysuntilweekday", "det", "dgammapda", "dgammapdada", "dgammapdadx", "dgammapdx",
      "dgammapdxdx", "dhms", "diag", "diag0cnt", "digamma", "dmy", "dofC", "dofb", "dofc", "dofh",
      "dofm", "dofq", "dofw", "dofy", "dow", "doy", "dunnettprob", "e", "el", "epsdouble",
      "epsfloat", "exp", "expm1", "exponential", "exponentialden", "exponentialtail", "fileexists",
      "fileread", "filereaderror", "filewrite", "firstdayofmonth", "firstdowofmonth",
      "firstweekdayofmonth", "float", "floor", "fmtwidth", "fprob", "frval", "gammaden", "gammap",
      "gammaptail", "get", "h", "hadamard", "halfyear", "halfyearly", "has_eprop", "hh", "hhC",
      "hms", "hofd", "hours", "hypergeometric", "hypergeometricp", "ibeta", "ibetatail",
      "igaussian", "igaussianden", "igaussiantail", "index", "indexnot", "inlist", "inrange", "int",
      "inv", "invF", "invFtail", "invbinomial", "invbinomialtail", "invcauchy", "invcauchytail",
      "invchi", "invchi2", "invchi2tail", "invcloglog", "invdunnettprob", "invexponential",
      "invexponentialtail", "invfprob", "invgammap", "invgammaptail", "invibeta", "invibetatail",
      "invigaussian", "invigaussiantail", "invlaplace", "invlaplacetail", "invlogistic",
      "invlogistictail", "invlogit", "invnF", "invnFtail", "invnbinomial", "invnbinomialtail",
      "invnchi", "invnchi2", "invnchi2tail", "invnibeta", "invnorm", "invnormal", "invnt",
      "invnttail", "invpoisson", "invpoissontail", "invsym", "invt", "invttail", "invtukeyprob",
      "invvech", "invvecp", "invweibull", "invweibullph", "invweibullphtail", "invweibulltail",
      "irecode", "isleapsecond", "isleapyear", "issym", "issymmetric", "itrim", "laplace",
      "laplaceden", "laplacetail", "lastdayofmonth", "lastdowofmonth", "lastweekdayofmonth",
      "length", "ln", "ln1m", "ln1p", "lncauchyden", "lndirichletden", "lnfactorial", "lngamma",
      "lngeometricp", "lnigammaden", "lnigaussianden", "lniwishartden", "lnlaplaceden",
      "lnmvnormalden", "lnnormal", "lnnormalden", "lnparetoden", "lnwishartden", "log", "log10",
      "log1m", "log1p", "logistic", "logisticden", "logistictail", "logit", "lower", "ltrim", "m",
      "matmissing", "matrix", "matuniform", "max", "maxbyte", "maxdouble", "maxfloat", "maxint",
      "maxlong", "mdy", "mdyhms", "mi", "min", "minbyte", "mindouble", "minfloat", "minint",
      "minlong", "minutes", "missing", "mm", "mmC", "mod", "mofd", "month", "monthly", "mreldif",
      "msofhours", "msofminutes", "msofseconds", "nF", "nFden", "nFtail", "nbetaden", "nbinomial",
      "nbinomialp", "nbinomialtail", "nchi", "nchi2", "nchi2den", "nchi2tail", "nextbirthday",
      "nextdow", "nextleapyear", "nextweekday", "nibeta", "normal", "normalden", "normd", "normden",
      "normprob", "now", "npnF", "npnchi", "npnchi2", "npnt", "nt", "ntden", "nttail", "nullmat",
      "plural", "poisson", "poissonp", "poissontail", "previousbirthday", "previousdow",
      "previousleapyear", "previousweekday", "proper", "q", "qofd", "quarter", "quarterly", "r",
      "rbeta", "rbinomial", "rcauchy", "rchi2", "real", "recode", "regexcapture",
      "regexcapturenamed", "regexm", "regexmatch", "regexr", "regexreplace", "regexreplaceall",
      "regexs", "reldif", "replay", "return", "reverse", "rexponential", "rgamma", "rgeometric",
      "rhypergeometric", "rigaussian", "rlaplace", "rlogistic", "rnbinomial", "rnormal", "round",
      "roweqnumb", "rownfreeparms", "rownumb", "rowsof", "rpareto", "rpoisson", "rt", "rtrim",
      "runiform", "runiformint", "rweibull", "rweibullph", "s", "scalar", "seconds", "sign", "sin",
      "sinh", "smallestdouble", "soundex", "soundex_nara", "sqrt", "ss", "ssC", "strcat", "strdup",
      "string", "stritrim", "strlen", "strlower", "strltrim", "strmatch", "strofreal", "strpos",
      "strproper", "strreverse", "strrpos", "strrtrim", "strtoname", "strtrim", "strupper",
      "subinstr", "subinword", "substr", "sum", "sweep", "t", "tC", "tan", "tanh", "tc", "td",
      "tden", "th", "tin", "tm", "tobytes", "today", "tprob", "tq", "trace", "trigamma", "trim",
      "trunc", "ttail", "tukeyprob", "tw", "twithin", "ty", "uchar", "udstrlen", "udsubstr",
      "uisdigit", "uisletter", "uniform", "uniform0", "upper", "ustrcompare", "ustrcompareex",
      "ustrfix", "ustrfrom", "ustrinvalidcnt", "ustrleft", "ustrlen", "ustrlower", "ustrltrim",
      "ustrnormalize", "ustrpos", "ustrregexm", "ustrregexra", "ustrregexrf", "ustrregexs",
      "ustrreverse", "ustrright", "ustrrpos", "ustrrtrim", "ustrsortkey", "ustrsortkeyex",
      "ustrtitle", "ustrto", "ustrtohex", "ustrtoname", "ustrtrim", "ustrunescape", "ustrupper",
      "ustrword", "ustrwordcount", "usubinstr", "usubstr", "vec", "vecdiag", "vech", "vecp", "w",
      "week", "weekly", "weibull", "weibullden", "weibullph", "weibullphden", "weibullphtail",
      "weibulltail", "wofd", "word", "wordbreaklocale", "wordcount", "year", "yearly", "yh", "ym",
      "yofd", "yq", "yw"};
  return kNames.count(name) != 0;
}

}

namespace piet::eval {
using namespace macro_detail;


std::optional<std::string> undefinedCRef(const std::string& expr) {
  if (expr.find("c(") == std::string::npos) return std::nullopt;
  std::vector<std::string> spans;
  const std::string bare = protectStringLiterals(expr, &spans);
  static const std::regex reC(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
  for (std::sregex_iterator it(bare.begin(), bare.end(), reC), end; it != end; ++it) {
    const std::string name = (*it)[1].str();
    if (!isStataCName(name)) return "c(" + name + ") undefined";
  }
  return std::nullopt;
}


// BUG-1330: a name StataBE's c() answers. isStataCName holds the list (BUG-1420
// measured it, hidden keys included); this is the name the c() refusal and the
// delimited expansion ask by.
bool isStataCClassName(const std::string& name) { return isStataCName(name); }

}

namespace piet::eval::macro_detail {



// BUG-1330: the c() names whose value is a string (quoted in `creturn list`,
// StataNow 19.5 BE), so a value that happens to read as a number (a digits-only
// c(username)) is still spliced as a string.
bool isStataCClassStringName(const std::string& name) {
  static const std::unordered_set<std::string> kNames = {
      "current_date", "current_time", "stata_name", "born_date", "edition", "edition_real",
      "mode", "console", "os", "osdtl", "hostname", "machine_type", "byteorder", "username",
      "sysdir_stata", "sysdir_base", "sysdir_site", "sysdir_plus", "sysdir_personal",
      "sysdir_oldplace", "sysdir_sample", "tmpdir", "adopath", "pwd", "dirsep", "frame",
      "filename", "filedate", "more", "rmsg", "dp", "logtype", "logmsg", "notifyuser", "playsnd",
      "include_bitmap", "iterlog", "showbaselevels", "showemptycells", "showomitted", "fvlabel",
      "fvwrapon", "lstretch", "cformat", "sformat", "pformat", "coeftabresults", "dots",
      "collect_label", "collect_style", "table_style", "etable_style", "dtable_style",
      "tabulate_style", "tabulate_comps", "svy_tab_comps", "collect_warn", "revkeyboard",
      "varkeyboard", "smoothfonts", "graphics", "scheme", "printcolor", "copycolor", "httpproxy",
      "httpproxyhost", "httpproxyauth", "httpproxyuser", "httpproxypw", "update_query",
      "update_prompt", "trace", "tracesep", "traceindent", "traceexpand", "tracenumber",
      "tracehilite", "matastrict", "matalnum", "mataoptimize", "matafavor", "matalibs",
      "matamofirst", "java_heapmax", "java_home", "lapack_openblas", "docx_hardbreak",
      "docx_paramode", "python_exec", "python_userpath", "rng", "rng_current", "rngstate",
      "sortmethod", "sort_current", "sortrngstate", "locale_ui", "locale_functions",
      "locale_icudflt", "type", "searchdefault", "varabbrev", "emptycells", "fvtrack", "fvbase",
      "odbcmgr", "odbcdriver", "fredkey", "collect_double", "reshape_favor", "doeditbackup",
      "alpha", "ALPHA", "Mons", "Months", "Wdays", "Weekdays", "obs_t", "seed", "flavor",
      "virtual", "sysdir_updates", "sortseed"};
  return kNames.count(name) != 0;
}


// BUG-1330: every bare c(name) outside string literals in an inline `=exp'`,
// replaced by its value through the c() hook (piet::substituteCRefs, which the
// other expression sites run, is beyond this layer): a number as itself, any
// other value as a string literal. A name the hook does not answer stays for
// the classifier's refusal.
std::string substituteBareCRefs(const std::string& expr, EvalSession& session) {
  if (!session.cValue || expr.find("c(") == std::string::npos) return expr;
  static const std::regex reBareC(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
  std::vector<std::string> spans;
  std::string work = protectLiterals(expr, &spans);
  std::string out;
  size_t last = 0;
  for (auto it = std::sregex_iterator(work.begin(), work.end(), reBareC);
       it != std::sregex_iterator(); ++it) {
    const std::string name = (*it)[1].str();
    std::optional<std::string> cv = session.cValue(name);
    if (!cv || cv->find('"') != std::string::npos) continue;
    char* end = nullptr;
    const double d = std::strtod(cv->c_str(), &end);
    const bool number = !isStataCClassStringName(name) && !cv->empty() &&
                        ((end == cv->c_str() + cv->size() && std::isfinite(d)) || *cv == ".");
    out.append(work, last, static_cast<size_t>(it->position(0)) - last);
    out += number ? "(" + *cv + ")" : "\"" + *cv + "\"";
    last = static_cast<size_t>(it->position(0) + it->length(0));
  }
  if (last == 0) return expr;
  out.append(work, last, std::string::npos);
  return restoreLiterals(out, spans);
}

}

namespace piet::eval {
using namespace macro_detail;


std::string cClassRefusalRc(const std::string& name) {
  return isStataCClassName(name) ? "198" : "133";
}


std::string cClassRefusalMessage(const std::string& name) {
  return isStataCClassName(name)
             ? "c(" + name + ") is a Stata system value this version of PIET does not provide"
             : "c(" + name + ") undefined";
}

}

namespace piet::eval::macro_detail {


// BUG-718: every `VAR[i]` in a macro-context expression that names a dataset
// variable (EvalSession::varObs) and not a matrix (a one-index subscript of a
// matrix stays for substituteMatrixRefs, as before) is read at observation i:
//   - a numeric value becomes `(value)` and the expression evaluates on;
//   - a STRING value, or a missing one, is answered only when the subscript is
//     the WHOLE expression (`` `=s[1]' `` -- the form the w2c_functions records
//     print): *whole gets the text verbatim and the caller returns it. This
//     layer's whitelist admits no string literal, so inside a larger expression
//     the reference is left in place, exactly as before this fix.
// The index is an integer literal (after `_N`/`_n`) or an expression this layer
// evaluates to an integer; anything else is left in place. Outside 1.._N the
// value is missing (Stata [U] 13.7: `x[0]`, `x[100]` copy a missing value) --
// `.` for a numeric column, "" for a string one (the reader reports the kind).
// Quoted text is data: string literals are protected around the scan.
// `stringLeft` (optional) is set when a string or missing reading was left in
// place inside a larger expression -- the caller's cue to refuse by name.
// BUG-710: `outerSpans` set = evalArith's mode. Its input has had its literals
// protected already (they ARE `outerSpans`), and EVERY reading is substituted,
// in any position, through varReadText: a string becomes a literal on
// `outerSpans`, a missing value its token, so `s[1] + 1` reaches the evaluator
// as `"ab1" + 1` and is its r(109) (w2i_varsub d_05), and `xd[_N]` is `.a`
// (a_03). `whole` is not used in that mode.
std::string substituteVarSubscripts(const std::string& in, EvalSession& session,
                                    std::optional<std::string>* whole, bool* stringLeft,
                                    std::vector<std::string>* outerSpans) {
  if (!session.varObs || in.find('[') == std::string::npos) return in;
  static const std::regex reSub(R"((^|[^\w.$`\x80-\xff])([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[([^\[\],]+)\])");
  static const std::regex reInt(R"(^\s*(-?\d+)\s*$)");
  std::vector<std::string> spans;
  const std::string s = outerSpans != nullptr ? in : protectStringLiterals(in, &spans);
  std::string out;
  size_t last = 0;
  bool changed = false;
  for (auto it = std::sregex_iterator(s.begin(), s.end(), reSub); it != std::sregex_iterator();
       ++it) {
    const std::smatch& m = *it;
    const std::string name = m.str(2);
    const size_t refPos = static_cast<size_t>(m.position(2));
    const size_t refEnd = static_cast<size_t>(m.position(0) + m.length(0));
    if (session.findMatrix(name) != nullptr) continue;
    std::string idxText = m.str(3);
    std::smatch im;
    if (!std::regex_match(idxText, im, reInt)) {
      std::optional<std::string> v = evalMacroExpr(idxText, session);
      if (!v || !std::regex_match(*v, im, reInt)) continue;
      idxText = *v;
      std::regex_match(idxText, im, reInt);
    }
    long long idx = 0;
    try {
      idx = std::stoll(im.str(1));
    } catch (...) {
      continue;
    }
    std::optional<EvalSession::VarObsReading> r = session.varObs(name, idx);
    if (!r) continue;
    if (outerSpans != nullptr) {
      std::optional<std::string> lit = varReadText(*r, *outerSpans);
      if (!lit) continue;
      out += s.substr(last, refPos - last);
      out += *lit;
      last = refEnd;
      changed = true;
      continue;
    }
    std::string text = r->text;
    bool isString = r->isString;
    if (!r->inRange) text = isString ? std::string() : std::string(".");
    if (isString || text.empty() || text[0] == '.') {
      if (refPos == 0 && strip(s.substr(refEnd)).empty() && spans.empty()) {
        *whole = text;
        return in;
      }
      if (stringLeft != nullptr) *stringLeft = true;
      continue;
    }
    out += s.substr(last, refPos - last);
    out += "(" + text + ")";
    last = refEnd;
    changed = true;
  }
  if (!changed) return in;
  out += s.substr(last);
  return outerSpans != nullptr ? out : restoreStringLiterals(out, spans);
}

}
