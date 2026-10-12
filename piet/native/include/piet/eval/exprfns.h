// Stata expression function library — C++ port of
// piet/backend/piet/router/exprfns.py (the frozen 1.16.0 oracle).
//
// Python's functions are dynamically typed and DEFENSIVE: wrong argument
// "types" return NaN/"" instead of raising (Stata's silent-missing
// convention) — but wrong ARITY raises TypeError before the body runs,
// which the eval() callers catch as full-expression failure. The port
// mirrors both behaviors: body failures are absorbed per-function, arity
// failures throw PyExc.
//
// The functions plug into pyeval's PyNamespace (see addStataFns) so
// macros._eval_arith-style evaluation gets the same names Python injects
// via STATA_FNS / STATA_FNS_SESSION.
//
// Known parity caveats (documented in the M1 report; corpus scoped
// accordingly):
//   - lower/upper/proper/trim family: Python str methods are full-Unicode;
//     the port is ASCII-correct (identity on non-ASCII, which also matches
//     Python for caseless scripts like CJK).
//   - regexm/regexr: Python `re` vs std::regex ECMAScript dialect; and
//     re.sub uses \1 backreferences where ECMAScript uses $1. Corpus uses
//     dialect-neutral patterns and plain replacements.
#ifndef PIET_NATIVE_EVAL_EXPRFNS_H
#define PIET_NATIVE_EVAL_EXPRFNS_H

#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"

namespace piet::eval {

// Stata missing sentinel base: `.` and undefined scalar() map to this in
// the if/while evaluator so missing compares greater than any real value.
constexpr double kStataMissingSentinel = 8.988465674311579e+307;

// BUG-249: EXTENDED missing (`.a`..`.z`) in the expression layer.
//
// The sentinel above is 2^1023 and carries no KIND, so `string(.a)` could only
// ever answer `.` (the data plane keeps the kind in NaN payload bits, this
// layer's textual substitution kept nothing). The 26 extended codes now map to
// 26 DISTINCT representable doubles above the base sentinel,
//     .<k-th letter>  ->  2^1023 * (1 + k/1024),        k = 1..26
// all exactly representable (k/1024 is far above 2^-52 resolution at this
// scale) and all strictly below DBL_MAX (2^1024*(1-2^-53)). Every existing
// `>= kStataMissingSentinel` missing test keeps answering true for them, and
// Stata's own ordering `. < .a < ... < .z` falls out of the arithmetic.
// stataExtendedMissingKind() inverts the mapping (0 = plain `.`, 1..26 = a..z,
// nullopt = not a missing). Absorption is accepted and documented: at 2^1023
// adding any ordinary number returns the SAME double, so `2+.a` is
// indistinguishable from `.a` here — the capture claims only direct
// `string(.a[, fmt])`, and arithmetic-on-missing keeps answering `.`-family.
constexpr double kStataMissingKindStep = 8.988465674311579e+307 / 1024.0;  // 2^1013

inline double stataMissingWithKind(int kind) {  // kind 0 = `.`, 1..26 = .a...z
  return kStataMissingSentinel + kStataMissingKindStep * kind;
}

inline std::optional<int> stataExtendedMissingKind(double v) {
  if (!(v >= kStataMissingSentinel) || !(v <= 1.7e308)) return std::nullopt;
  const int k = static_cast<int>((v - kStataMissingSentinel) / kStataMissingKindStep + 0.5);
  return (k >= 0 && k <= 26) ? std::optional<int>(k) : std::nullopt;
}

// scalar(name) lookup: returns the stored string value or nullopt.
// TODO(M2): bind to the full SessionState scalar store once the C++
// session/datastore lands; M1 callers bind the EvalSession ordered map.
using ScalarLookupFn = std::function<std::optional<std::string>(const std::string&)>;

// AER scope 2 / B3(c). Does this expression contain a RUNNING sum -- Stata's
// sum(), the one expression function whose value depends on the rows before it?
//
// It matters at exactly one decision: `gen y = <rhs> if <c>` is normally sent to
// the engine as `y = cond((c),(rhs),.)`, which is equivalent for every other
// function but makes a running total accumulate over the rows the qualifier
// excluded. When this answers true the qualifier rides separately as --gen-mask.
//
// Quote-aware on purpose: a `sum(` inside a string literal is data, and reaching
// into literals is the defect class of BUG-443. Word-bounded on both sides, so
// `rowsum(` and `mysum(` do not match. A false positive would only take the
// (equally correct) mask path, so the bias is safe either way.
inline bool exprHasRunningSum(const std::string& e) {
  char quote = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c != 's' || e.compare(i, 3, "sum") != 0) continue;
    if (i > 0) {
      const char p = e[i - 1];
      if (std::isalnum(static_cast<unsigned char>(p)) || p == '_' || p == '.') continue;
    }
    size_t j = i + 3;
    while (j < e.size() && (e[j] == ' ' || e[j] == '\t')) ++j;
    if (j < e.size() && e[j] == '(') return true;
  }
  return false;
}

// Add the STATA_FNS entries (exprfns.py) to `ns`.
void addStataFns(PyNamespace& ns);

// Add the session-bound scalar() entry (exprfns.STATA_FNS_SESSION).
void addScalarFn(PyNamespace& ns, ScalarLookupFn lookup);

// Individual functions, exposed for the selftest smoke leg and the
// macro-engine helpers that call them directly.
PyValue fnInlist(const std::vector<PyValue>& args);
PyValue fnInrange(const std::vector<PyValue>& args);
PyValue fnCond(const std::vector<PyValue>& args);
PyValue fnMod(const std::vector<PyValue>& args);
PyValue fnMissing(const std::vector<PyValue>& args);
PyValue fnLower(const std::vector<PyValue>& args);
PyValue fnUpper(const std::vector<PyValue>& args);
PyValue fnStrtrim(const std::vector<PyValue>& args);
PyValue fnLtrim(const std::vector<PyValue>& args);
PyValue fnRtrim(const std::vector<PyValue>& args);
PyValue fnSubstr(const std::vector<PyValue>& args);
PyValue fnStrlen(const std::vector<PyValue>& args);
PyValue fnStrpos(const std::vector<PyValue>& args);
PyValue fnRegexm(const std::vector<PyValue>& args);
PyValue fnRegexr(const std::vector<PyValue>& args);
PyValue fnReal(const std::vector<PyValue>& args);
PyValue fnWord(const std::vector<PyValue>& args);
PyValue fnSubinstr(const std::vector<PyValue>& args);
PyValue fnProper(const std::vector<PyValue>& args);
PyValue fnString(const std::vector<PyValue>& args);
PyValue fnChar(const std::vector<PyValue>& args);
PyValue fnAbbrev(const std::vector<PyValue>& args);

// _g_format / _apply_stata_fmt (exprfns.py) — also used by string().
// gFormat throws PyExc on non-finite input (mirrors int(inf) OverflowError
// escaping Python's except (TypeError, ValueError) clause in _g_format).
std::string gFormat(double v);
std::string applyStataFmt(double v, const std::string& fmt);

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_EXPRFNS_H
