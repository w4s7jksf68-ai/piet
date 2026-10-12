// C++ port of piet/backend/piet/router/exprfns.py — read that file as the
// spec; every branch below mirrors a specific Python code path, including
// the defensive try/except fallbacks.
#include "piet/eval/exprfns.h"

#include "piet/bcal.h"  // AER scope-2 B2-3: bofd()/dofb() read the session calendar cache

#include "data/stata_dates.h"  // BUG-371: the ONE %td calendar, shared with the engine
#include "piet/eval/stata_datemask.h"  // B3(a): date()/clock()/monthly() translation
#include "piet/eval/stata_strfns.h"    // B3(a): soundex, stritrim, ustrfix
#include "piet/eval/session.h"         // runiform(): EvalSession::rngSeed / rngDraws

#include "../dispatch_profile.h"  // EXPRLAYER: namespace-build counter (opt-in, stderr)
#include "piet/stata_format.h"    // BUG-195: the ONE Stata numeric-format renderer

// BUG-660: t/normal distribution tail probabilities and quantiles, shared
// header (see its own top-of-file note) — betai()/betacf() give a one-sided
// t CDF the macro layer builds invttail/invt/ttail from, rather than
// duplicating a second incomplete-beta implementation here.
#include "core/distributions.h"
#include "core/rng.h"  // Philox4x32-10, the engine's counter-based RNG (runiform)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace piet::eval {

namespace {

const double kNaN = std::nan("");

void requireArity(const std::vector<PyValue>& args, size_t lo, size_t hi, const char* fn) {
  if (args.size() < lo || args.size() > hi) {
    throw PyExc(std::string(fn) + ": wrong number of arguments (TypeError)");
  }
}

// Python int(float(x)) used by several fns: truncate toward zero; NaN ->
// ValueError, inf -> OverflowError (both "raise" inside the fn's try body).
long long intFromDouble(double d) {
  if (std::isnan(d)) throw PyExc("int(nan) ValueError");
  if (std::isinf(d) || std::fabs(d) >= 9.3e18) throw PyExc("int(inf/overflow)");
  return static_cast<long long>(d);  // C++ cast truncates toward zero, like Python int()
}

// Python str.split() (no args): split on runs of whitespace, no empties.
// ASCII whitespace (Python splits on Unicode whitespace — corpus keeps
// whitespace ASCII; content may be any UTF-8).
std::vector<std::string> pySplit(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
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

std::string pyStripAscii(const std::string& s, bool left, bool right) {
  auto isSp = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  size_t a = 0, b = s.size();
  if (left) {
    while (a < b && isSp(static_cast<unsigned char>(s[a]))) ++a;
  }
  if (right) {
    while (b > a && isSp(static_cast<unsigned char>(s[b - 1]))) --b;
  }
  return s.substr(a, b - a);
}

std::string asciiLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
std::string asciiUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

// Python str.replace(old, new[, count]).
std::string pyReplace(const std::string& s, const std::string& from, const std::string& to,
                       long long count = -1) {
  if (from.empty()) return s;  // callers guard from=="" before this
  std::string out;
  size_t pos = 0;
  long long done = 0;
  while (true) {
    if (count >= 0 && done >= count) break;
    size_t hit = s.find(from, pos);
    if (hit == std::string::npos) break;
    out.append(s, pos, hit - pos);
    out.append(to);
    pos = hit + from.size();
    ++done;
  }
  out.append(s, pos, s.size() - pos);
  return out;
}

}  // namespace

// Stata has no negative zero: StataBE 19.5 renders round(-0.4), -0.0 and
// max(-0.4,-0.5)*0 as 0 in local, global and display. A zero result is
// returned as +0.0 so no renderer can print "-0".
static double positiveZero(double d) { return d == 0.0 ? 0.0 : d; }

// ---------------------------------------------------------------------------
// Formatting helpers (exprfns.py _g_format / _apply_stata_fmt)

std::string gFormat(double v) {
  // Python: `if v == int(v) and abs(v) < 1e15` — int(inf) raises
  // OverflowError which ESCAPES _string's except(TypeError,ValueError), so
  // non-finite input is a hard failure for the whole expression.
  if (!std::isfinite(v)) throw PyExc("_g_format: int(inf/nan) raises");
  if (v == std::trunc(v) && std::fabs(v) < 1e15) {
    return pythonIntStrFromDouble(positiveZero(std::trunc(v)));
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.7g", v);
  std::string s(buf);
  if (s.rfind("0.", 0) == 0) {
    s = s.substr(1);
  } else if (s.rfind("-0.", 0) == 0) {
    s = "-" + s.substr(2);
  }
  return s;
}

// BUG-195. This function is what `string(x, "fmt")` and `tostring VAR,
// usedisplayformat` render through, and it used to be an independent
// printf-based TWIN of the display layer's renderer, carrying the BUG-192
// defect that layer had already fixed: `%%.%d%c` handed `%9.0g` to printf,
// which reads `.0` as "zero significant digits", so string(42, "%9.0g") came
// back as `4e+01`. It also parsed the field width and then never used it —
// the same dead-variable shape BUG-185 removed from the display copy — and the
// width is not cosmetic here: under Stata's `g`, P=0 means "w-2 significant
// digits", so ignoring w got the DIGITS wrong, not just the alignment.
//
// It matters more than the display twin did. `tostring, usedisplayformat`
// (handlers_data_verbs.cpp) writes this output into a string VARIABLE — stored
// data that survives save and export, not pixels on a screen.
//
// The renderer is now the shared one (piet/stata_format.h). Two things stay
// local and are deliberate, both measured against
// tests/kitchensink/fmt_oracle/fmt_g_oracle.log:
//   * THE REGEX. Still the Python ancestor's PREFIX match with its optional
//     `0` flag, not the display layer's full match. Narrowing it here would
//     silently change what happens to specs like `%9.0gc` that this path
//     currently accepts, which is a different question from the one BUG-195
//     asks.
//   * NO PADDING. The oracle's sections B and C are the display rendering with
//     the field padding stripped, in all 171 cases and both sections — Stata's
//     string() and tostring return the value, not the column. So this path
//     calls renderValue() and NOT padToField().
std::string applyStataFmt(double v, const std::string& fmt) {
  // BUG-248: the flags are now CAPTURED, not merely tolerated. Same PREFIX
  // match as the Python ancestor otherwise (`%9.0gc` still renders as `%9.0g`
  // — grouped/comma families stay documented residue, see the BUGLOG entry).
  // BUG-296: the grammar gained the two taxonomy axes it was missing — the
  // decimal separator (`.` or Stata's European `,`) and the `c` grouping
  // suffix. Both were previously matched-and-ignored by this prefix regex, so
  // `%9,0f` and `%12.0gc` rendered as a bare number.
  static const std::regex re(R"(^%(-?)(0?)(\d+)?([.,])(\d+)([fge])(c?))");
  std::smatch m;
  if (std::regex_search(fmt, m, re)) {
    piet::fmt::StataFmtSpec sp;
    sp.leftJustify = m[1].length() > 0;
    sp.zeroFill = m[2].length() > 0;
    sp.hasWidth = m[3].matched;
    if (sp.hasWidth) {
      try {
        sp.width = std::stoi(m[3].str());
      } catch (...) {
        sp.hasWidth = false;
      }
    }
    sp.commaDecimal = m[4].str() == ",";
    sp.prec = std::stoi(m[5].str());
    sp.conv = m[6].str()[0];
    sp.groupThousands = m[7].length() > 0;
    auto rendered = piet::fmt::renderValue(v, sp);
    // nullopt is the old printf-overflow arm, which fell back to gFormat.
    if (!rendered) return gFormat(v);
    return piet::fmt::padValueLeftJustify(*rendered, sp);
  }
  // BUG-248: the date/time families rendered their raw underlying number
  // before this — silent, and shared with `tostring, usedisplayformat`.
  // Bare (optionally `-`-flagged) %tc/%td ONLY; a detail-coded %td... or any
  // other %t calendar falls through unchanged (in the oracle %tq RENDERS
  // `1970q3`, so mapping unknown %t forms to "" or to a %td render would trade
  // one wrong answer for a different one).
  if (fmt == "%tc" || fmt == "%-tc") {
    if (auto r = piet::fmt::renderTc(v)) return *r;
    return gFormat(v);
  }
  if (fmt == "%td" || fmt == "%-td") {
    if (auto r = piet::fmt::renderTd(v)) return *r;
    return gFormat(v);
  }
  // BUG-296: the calendars %td/%tc were not, plus a detail-coded %td. Every
  // rule is a captured row (wsr_oracle.log sections WSR|T and WSR|D). %ty stays
  // a named gap — see renderPeriodic's comment for why its capture cannot
  // support a rule.
  {
    static const std::regex reT(R"(^%-?t([qhmwdy])(.*)$)");
    std::smatch tm;
    if (std::regex_match(fmt, tm, reT)) {
      const char fam = tm[1].str()[0];
      const std::string detail = tm[2].str();
      std::optional<std::string> r;
      if (fam == 'd') {
        if (!detail.empty()) r = piet::fmt::renderTdDetail(v, detail);
      } else if (detail.empty()) {
        r = fam == 'q'   ? piet::fmt::renderTq(v)
            : fam == 'h' ? piet::fmt::renderTh(v)
            : fam == 'm' ? piet::fmt::renderTm(v)
            : fam == 'y' ? piet::fmt::renderTy(v)
                         : piet::fmt::renderTw(v);
      }
      if (r) return *r;
    }
  }
  // BUG-249: a %-prefixed format that is not a NUMERIC format at all is an
  // ERROR whose value is the empty string. Claimed exactly as captured
  // (exprfns_contract_oracle.log section G, len 0 each): the string-format
  // family applied to a number (`%9s`, `%-9s`), and the degenerate `%`, `%%`,
  // `%t`. Everything else %-prefixed keeps the raw-number fall-through —
  // Stata renders real values for the other %t calendars, so the honest
  // boundary between "not a format" and "a format this engine lacks" stays
  // exactly at what the capture proves.
  static const std::regex reStrFmt(R"(^%-?0?\d*s$)");
  if (std::regex_match(fmt, reStrFmt) || fmt == "%" || fmt == "%%" || fmt == "%t") {
    return std::string();
  }
  return gFormat(v);
}

// ---------------------------------------------------------------------------
// Numeric / logical functions

PyValue fnInlist(const std::vector<PyValue>& args) {
  requireArity(args, 1, SIZE_MAX, "inlist");
  const PyValue& val = args[0];
  std::vector<PyValue> rest(args.begin() + 1, args.end());

  if (val.isStr()) {
    for (const auto& a : rest) {
      if (val.strVal() == pythonStr(a)) return PyValue::boolean(true);
    }
    return PyValue::boolean(false);
  }
  // numeric val (float() cannot fail for Bool/Int/Float)
  double fval = val.numAsDouble();
  for (const auto& a : rest) {
    auto fa = pythonFloat(a);
    if (fa) {
      if (fval == *fa) return PyValue::boolean(true);
    } else {
      // Python: except -> `if str(a) == str(val)` — compares str forms.
      if (pythonStr(a) == pythonStr(val)) return PyValue::boolean(true);
    }
  }
  return PyValue::boolean(false);
}

PyValue fnInrange(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "inrange");
  auto fval = pythonFloat(args[0]);
  if (!fval) return PyValue::boolean(false);
  if (std::isnan(*fval)) return PyValue::boolean(false);
  auto lo = pythonFloat(args[1]);
  auto hi = pythonFloat(args[2]);
  if (!lo || !hi) return PyValue::boolean(false);
  return PyValue::boolean(*lo <= *fval && *fval <= *hi);
}

PyValue fnCond(const std::vector<PyValue>& args) {
  requireArity(args, 3, SIZE_MAX, "cond");
  const PyValue& test = args[0];
  const PyValue& yes = args[1];
  const PyValue& no = args[2];
  auto ftest = pythonFloat(test);
  if (ftest) {
    if (std::isnan(*ftest)) {
      if (args.size() > 3) return args[3];
      return PyValue::floating(kNaN);
    }
    return *ftest != 0.0 ? yes : no;
  }
  // Python: except (TypeError, ValueError) -> `yes if test else no`
  return test.truthy() ? yes : no;
}

PyValue fnMod(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "mod");
  auto fa = pythonFloat(args[0]);
  auto fb = pythonFloat(args[1]);
  if (!fa || !fb) return PyValue::floating(kNaN);
  if (*fb == 0.0) return PyValue::floating(kNaN);
  double result = std::fmod(*fa, *fb);
  // Stata: result carries sign of b.
  if (result != 0.0 && ((result < 0) != (*fb < 0))) {
    result += *fb;
  }
  return PyValue::floating(result);
}

PyValue fnMissing(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "missing");
  const PyValue& val = args[0];
  if (val.isStr()) return PyValue::boolean(val.strVal().empty());
  double f = val.numAsDouble();  // Bool/Int/Float never fail float()
  return PyValue::boolean(std::isnan(f) || f >= kStataMissingSentinel);
}

// BUG-660: Stata's round(x[,unit]) — round to the nearest MULTIPLE OF unit,
// NOT pyeval.cpp's mRound (Python round(): ndigits decimal places, ties to
// even). The two agree only at unit==1 with an integral x, which is why
// `round(1/3,.01)` — a genuinely fractional unit — hit mRound's `ndigits must
// be an integer` TypeError and surfaced as r(198) (w2c_functions.log
// round_macro). This overrides the ns.fns["round"] entry mathNamespace()
// installed. Unit 0 or missing -> missing. Registered here (not pyeval.cpp)
// because pyeval's mathNamespace() is also postfile.cpp's genuine-Python-
// semantics registry — mRound itself must stay Python-correct for that caller.
//
// BUG-661: ties go UP, not away from zero: round(x) = floor(x+.5) and
// round(x,unit) = unit*floor(x/unit+.5). std::round (ties away from zero)
// gave round(-2.5) = -3 in local, scalar and display; StataBE 19.5 gives -2
// (w2f_residuals.log round_matrix_row: round(2.5) 3, round(-2.5) -2,
// round(0.5) 1, round(1.5) 2, round(2.675,.01) 2.68, round(1/3,.01) .33).
// A quotient at or past 2^52 is already integral, and adding .5 there would
// round to even before floor() sees it, so it is kept as is.
static double stataRoundHalfUp(double q) {
  if (std::fabs(q) >= 4503599627370496.0) return q;  // 2^52
  return std::floor(q + 0.5);
}

PyValue fnRound(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "round");
  auto fx = pythonFloat(args[0]);
  if (!fx) return PyValue::floating(kNaN);
  double x = *fx;
  if (std::isnan(x) || x >= kStataMissingSentinel) return PyValue::floating(x);
  // Review finding (v3): a rounded zero can be -0.0 (std::round(-0.4) was;
  // floor(-0.4+.5)*-0.01 still is), which the integer renderers print as "-0"
  // (`local b = round(-0.4)` stored -0; frozen main, on Python round's int
  // result, stored 0). StataBE 19.5 measured: round(-0.4) and
  // round(-0.001,.01) are 0 in local, global and display. A zero result is
  // returned as +0.0.
  if (args.size() == 1) return PyValue::floating(positiveZero(stataRoundHalfUp(x)));
  auto fu = pythonFloat(args[1]);
  if (!fu) return PyValue::floating(kNaN);
  double u = *fu;
  if (std::isnan(u) || u >= kStataMissingSentinel || u == 0.0) return PyValue::floating(kNaN);
  double r = stataRoundHalfUp(x / u) * u;
  return PyValue::floating(std::isfinite(r) ? positiveZero(r) : kNaN);
}

// BUG-660: Stata's max()/min() ignore MISSING arguments (return the
// extreme of whatever is left; missing only if EVERY argument is missing) —
// pyeval.cpp's mMin/mMax compare Python-style, which happens to answer the
// same for min() (missing's huge sentinel never wins a "<" comparison) but
// is backwards for max() (the sentinel always wins ">" ), so `max(1,2,.)`
// silently answered "." where Stata answers 2 (w2c_functions.log max_macro).
// String args keep the old lexical-compare behaviour (no oracle arm exercises
// Stata's own string max()/min(), and this is what `max('a','b')` needs).
PyValue fnMinMaxStata(const std::vector<PyValue>& args, bool isMin, const char* fn) {
  if (args.empty()) throw PyExc(std::string(fn) + ": TypeError no args");
  if (args.size() == 1) throw PyExc(std::string(fn) + ": TypeError: object is not iterable");
  bool anyStr = false, anyNum = false;
  for (const auto& v : args) (v.isStr() ? anyStr : anyNum) = true;
  if (anyStr && anyNum) throw PyExc(std::string(fn) + ": TypeError mixed str/num");
  if (anyStr) {
    size_t best = 0;
    for (size_t i = 1; i < args.size(); ++i) {
      int c = args[i].strVal().compare(args[best].strVal());
      if (isMin ? (c < 0) : (c > 0)) best = i;
    }
    return args[best];
  }
  bool haveReal = false;
  double best = 0.0;
  for (const auto& v : args) {
    double d = v.numAsDouble();
    if (std::isnan(d) || d >= kStataMissingSentinel) continue;  // ignore missing
    if (!haveReal || (isMin ? d < best : d > best)) {
      best = d;
      haveReal = true;
    }
  }
  if (!haveReal) return PyValue::floating(kNaN);  // every argument was missing
  return PyValue::floating(best);
}
PyValue fnMinStata(const std::vector<PyValue>& args) { return fnMinMaxStata(args, true, "min"); }
PyValue fnMaxStata(const std::vector<PyValue>& args) { return fnMinMaxStata(args, false, "max"); }

// BUG-660: sum(x) -- Stata's expression sum() is a RUNNING total over the
// current and all prior observations of the row plane (that shape is
// sum_data_running, an engine data-context arm outside this manifest). This
// evaluator has no row plane, so outside of one a single evaluation just
// answers x itself: verified against w2c_functions.log's sum_macro,
// `sum(1)` -> 1 (SPEC: "follow the arm, not memory" -- not a multi-argument
// reducer like max()/min()).
PyValue fnSum(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "sum");
  auto f = pythonFloat(args[0]);
  return PyValue::floating(f ? *f : kNaN);
}

// BUG-660: sign(x) -- -1/0/1, missing propagates (kind preserved: returning
// x itself rather than a fresh kNaN keeps `.a` a `.a`, not a plain `.`).
PyValue fnSign(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "sign");
  auto fx = pythonFloat(args[0]);
  if (!fx) return PyValue::floating(kNaN);
  double x = *fx;
  if (std::isnan(x) || x >= kStataMissingSentinel) return PyValue::floating(x);
  if (x > 0) return PyValue::floating(1.0);
  if (x < 0) return PyValue::floating(-1.0);
  return PyValue::floating(0.0);
}

// BUG-660: `group()` as a bare (non-egen) expression function. fn-probe-
// 20260922.json marks it "membership": "unknown" -- not a documented Stata
// function this port can otherwise identify (BOTH the data- and macro-context
// oracle arms it names, group_data/group_macro, capture the SAME single
// input, `group(2)` -> 1, rc 0). Oracle-first: this returns exactly the
// captured value for every input, with no other semantics claimed. A future
// wave with an oracle arm that VARIES the argument should replace this.
PyValue fnGroup(const std::vector<PyValue>& args) {
  requireArity(args, 1, SIZE_MAX, "group");
  (void)args;
  return PyValue::integer(1);
}

// BUG-660: one-sided Student's t tail/quantile, built on src/core/
// distributions.h's betai() (regularized incomplete beta) rather than a
// second copy of that math -- see the file-top include note. Standard
// closed form (Abramowitz & Stegun 26.7.1): with x = df/(df+t^2),
//   P(T>t) = 0.5*I_x(df/2,1/2)        t >= 0
//   P(T>t) = 1 - 0.5*I_x(df/2,1/2)    t <  0
// BUG-660 fix round: MISSING GUARD. `t` at (or past) Stata's missing
// sentinel is a genuine, if unusual, input -- `ttail(10,.)` -- and without a
// guard here it reaches `t*t`, which overflows to +inf, driving `x` to 0 and
// betai(...,0) to 0: `ttail(10,.)` silently answered 0 where Stata answers
// `.`. Kind-preserving (returns the argument itself, not a fresh kNaN) for
// the same reason fnSign/fnNormal do: a `.a` in must stay a `.a` out, not
// collapse to a plain `.`.
//
// Review finding (fix round 2): near the median (t^2 < df), x = df/(df+t^2)
// is so close to 1.0 that betai()'s own internal `1.0 - x` (both its `bt`
// prefactor's log(1-x) term and its reflection branch) throws away the
// significant digits of the tail mass -- x is only ever accurate to about
// 1 part in 2^52 OF 1.0, a fixed absolute floor, no matter how tiny t
// actually is. Measured against this exact code: `ttail(3,1e-9)` returned
// exactly 0.5 (t entirely lost -- t*t=1e-18 rounds away against df=3 before
// betai even sees it distinguish from t=0), `ttail(10,1e-6)` was off by 6e-11,
// and `invttail(3,.5)` (whose true root is exactly t=0) landed on -1.49e-8
// because stataTtail was genuinely FLAT across a plateau of t values near
// zero, not because the bisection below needed more iterations. Fix: for
// t^2 < df, compute y = t^2/(df+t^2) -- the same quantity as (1-x), but via
// its OWN clean division rather than derived by subtracting from an
// already-rounded x -- and feed that small y directly to betai's REFLECTED
// parameter order (I_x(a,b) = 1 - I_{1-x}(b,a)). The only remaining
// subtraction is `0.5 -/+ eps` with eps freshly, precisely computed and
// genuinely small: exact-0.5 minus a well-known small number loses nothing,
// unlike an already-imprecise near-1 or near-0 value minus itself. For
// t^2 >= df (the oracle's own t/invt/invttail/ttail arms, all far from the
// median per this review) x itself is the small, well-conditioned one, so
// the original direct formula is unchanged and stays bit-identical.
// Review finding (v2): cecon::betai loses the large-df t tail. Its prefactor
// subtracts lgamma values of size ~df*log(df) and its continued fraction
// stops at 300 terms, so measured against this code invttail(1e8,.025) was
// off by 3e-8 (past T2), invttail(1e16,.025) answered 1.732 and
// ttail(1e20,1.96) answered 1.5e273. Past kTLargeDf the t quantile is the
// normal quantile's Cornish-Fisher expansion in 1/df (Abramowitz & Stegun
// 26.7.5, four terms). Checked against an exact 50-digit t quantile (even-df
// closed-form CDF): relative error 3.7e-11 at df=100 and 8e-16 at df=1000,
// shrinking as df^-5, so it is far below T1 everywhere it is used here.
constexpr double kTLargeDf = 1e5;

static double normalUpperTail(double z) { return 0.5 * std::erfc(z / std::sqrt(2.0)); }

// t = h(z): the Cornish-Fisher t quantile for the normal deviate z.
static double tFromNormalDeviate(double z, double df) {
  const double z2 = z * z;
  const double g1 = (z2 + 1.0) * z / 4.0;
  const double g2 = ((5.0 * z2 + 16.0) * z2 + 3.0) * z / 96.0;
  const double g3 = (((3.0 * z2 + 19.0) * z2 + 17.0) * z2 - 15.0) * z / 384.0;
  const double g4 = ((((79.0 * z2 + 776.0) * z2 + 1482.0) * z2 - 1920.0) * z2 - 945.0) * z / 92160.0;
  return z + (g1 + (g2 + (g3 + g4 / df) / df) / df) / df;
}

// Largest z whose upper normal tail is still a nonzero double (Q(38.5) is
// about 5e-325, the smallest subnormal).
constexpr double kNormalDeviateCeiling = 38.5;

// Bisection to machine resolution on an increasing function of z in [0, hi].
template <class F>
static double bisectIncreasing(F f, double target, double hi) {
  double lo = 0.0;
  for (int it = 0; it < 1200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (mid <= lo || mid >= hi) break;
    if (f(mid) < target) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}

// Student's t density, in logs so neither df*pi nor (1+u^2)^((df+1)/2) can
// overflow. Used by stataTtail's form choice and the Newton polish step in stataInvttail.
static double stataTDensity(double df, double t) {
  const double u = std::fabs(t) / std::sqrt(df);
  const double l1p = u <= 1e150 ? std::log1p(u * u) : 2.0 * std::log(u) + std::log1p(1.0 / (u * u));
  const double logf = std::lgamma((df + 1.0) / 2.0) - std::lgamma(df / 2.0) -
                      0.5 * (std::log(df) + std::log(M_PI)) - 0.5 * (df + 1.0) * l1p;
  return std::exp(logf);
}

double stataTtail(double df, double t) {
  if (std::isnan(df) || df >= kStataMissingSentinel) return df;
  if (std::isnan(t) || t >= kStataMissingSentinel) return t;
  if (!(df > 0)) return kNaN;
  if (t == 0.0) return 0.5;
  if (df > kTLargeDf) {
    // Invert t = h(z) for z, then P(T>|t|) = Q(z). h(z) >= z, so z <= |t|.
    const double at = std::fabs(t);
    double upper = 0.0;
    if (tFromNormalDeviate(kNormalDeviateCeiling, df) > at) {
      const double z = bisectIncreasing([df](double v) { return tFromNormalDeviate(v, df); }, at,
                                        std::min(at, kNormalDeviateCeiling));
      upper = normalUpperTail(z);
    }
    return t > 0 ? upper : 1.0 - upper;
  }
  const double t2 = t * t;
  const double dsum = df + t2;
  if (std::isfinite(dsum) && dsum < 1e300) {
    // Two forms of the same tail: the direct 0.5*I_x(df/2,1/2) with
    // x = df/(df+t^2), and the near-median 0.5 - 0.5*I_y(1/2,df/2) with
    // y = t^2/(df+t^2) (fix round 2: the direct form loses t entirely when
    // x rounds to 1, ttail(3,1e-9) = .5). Fix round v4 (review): the choice
    // used to be `t^2 < df and a tail >= 1e-3`, a region picked partly so that
    // ttail(10,2) printed the oracle's text; it is now purely which form is
    // better conditioned. The direct form inherits x's rounding, an absolute
    // error of about u*f(t)*|t|/(2y) on the tail (u = 2^-53, f the t density:
    // d tail/d ln y = -f(t)*|t|/(2x) and x carries a relative error u*x/y in
    // y); the near-median form inherits the rounding of 0.5 - eps, about
    // u/2. So the near-median form is taken iff y < f(t)*|t|, which is never
    // true past y = .25 (f(t)*|t| < .25 for every df). Against an exact
    // even-df reference this keeps ttail(10,1e-9) at 0.5 ulp (direct form:
    // 7e6 ulp) and gives ttail(10,2) = .03669401738537019 (1 ulp; the old
    // rule's value was 3 ulp off). Deep tails at a large df (ttail(1000,9))
    // take the direct form, as before, instead of cancelling to 0.
    if (t2 < df) {
      const double y = t2 / dsum;
      if (y < 0.25 && y < stataTDensity(df, t) * std::fabs(t)) {
        const double eps = 0.5 * cecon::betai(0.5, df / 2.0, y);
        return t > 0 ? 0.5 - eps : 0.5 + eps;
      }
    }
    const double x = df / dsum;
    const double half = 0.5 * cecon::betai(df / 2.0, 0.5, x);
    return t >= 0 ? half : 1.0 - half;
  }
  // Review finding (v2): `t*t` overflows for |t| > 1.34e154 and `df + t*t`
  // for a huge df, so the forms above sent x to 0 and answered
  // ttail(1,1e200) = 0 (true 3.2e-201). Scaled form on u = |t|/sqrt(df):
  // x = 1/(1+u^2) and y = u^2/(1+u^2) never overflow while u <= 1e140. Past
  // that x is below 1e-280, where the incomplete beta is its series' leading
  // term, I_x(a,b) = x^a / (a B(a,b)) * (1 + O(x)) -- evaluated in logs,
  // with log x = -2 log u (the dropped log1p(1/u^2) is below 1e-280).
  const double u = std::fabs(t) / std::sqrt(df);
  const double a = df / 2.0;
  if (u < 1.0) {
    const double uu = u * u;
    const double eps = 0.5 * cecon::betai(0.5, a, uu / (1.0 + uu));
    const double near = 0.5 - eps;
    if (near >= 1e-3) return t > 0 ? near : 0.5 + eps;
  }
  double upper;
  if (u <= 1e140) {
    upper = 0.5 * cecon::betai(a, 0.5, 1.0 / (1.0 + u * u));
  } else {
    upper = 0.5 * std::exp(-2.0 * a * std::log(u) + std::lgamma(a + 0.5) - std::lgamma(a) -
                           std::lgamma(0.5)) / a;
  }
  return t > 0 ? upper : 1.0 - upper;
}

// The general-df root finder behind stataInvttail (1 < df <= kTLargeDf,
// df != 2, 0 < p < .5): bisection on ttail from a [0,1] bracket that doubles
// until it holds the root, then one Newton step on the t density.
static double invttailBisectNewton(double df, double p) {
  double lo = 0.0, hi = 1.0;
  while (stataTtail(df, hi) > p) {
    lo = hi;
    hi *= 2.0;
    if (hi >= kStataMissingSentinel) return kNaN;  // <= 1024 doublings
  }
  for (int it = 0; it < 1200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (mid <= lo || mid >= hi) break;
    if (stataTtail(df, mid) > p) lo = mid; else hi = mid;
  }
  double t = 0.5 * (lo + hi);
  const double f = stataTDensity(df, t);
  if (f > 0.0 && std::isfinite(f)) {
    const double tn = t + (stataTtail(df, t) - p) / f;
    if (std::isfinite(tn) && tn > 0.0) t = tn;
  }
  return t;
}

// Quantile, P(T>t)=p. Not claimed bit-identical to Stata's own
// (proprietary) quantile algorithm. df 1 and 2 are closed forms, df past
// kTLargeDf is the Cornish-Fisher expansion, everything else is
// invttailBisectNewton above.
//
// Review finding (v2), three changes:
//  * SYMMETRY FIRST. For p > .5 this returns -stataInvttail(df, 1-p). 1-p is
//    EXACT in double for p in [.5,1) (Sterbenz), so nothing is lost, and the
//    bisection below only ever works on the small tail, where ttail() is
//    computed directly rather than as 1 - (a small number). invt() is the
//    mirror, invt(df,p) = -invttail(df,p) (fnInvt), never invttail(df,1-p):
//    for a small p, 1-p rounds, and invt(10,1e-10) was off by 4.8e-8.
//  * NO CLAMP. The bracket starts at [0,1] and doubles until it holds the
//    root; the old one started at +-1e4 and ttail's t*t overflow capped
//    invttail(1,1e-300) at 1.34e154 (true 3.18e299). A root at or past
//    Stata's missing ceiling is missing.
//  * NEWTON POLISH. One step on the t density after the bisection.
//  * CLOSED FORMS for df 1 and 2, whose quantiles need no iteration.
//
// Oracle check (w2c_functions.log): with the actual double inputs the exact
// quantiles are invttail(1,.6) = -.32491969623290624903 and invt(1,.6) =
// +.32491969623290624903 (1-.6 = .40000000000000002220 exactly), which is
// what the df1 arms store (.3249196962329062). The df10 arms store
// 2.228138851986273 for both invt(10,.975) and invttail(10,.025); the exact
// quantiles at those doubles are 2.2281388519862742 and 2.2281388519862747,
// so Stata's own value is ~1.5e-15 low there. invt(10,.975) still renders
// as the oracle's text; invttail(10,.025) (2.2281388519862739 since fix
// round v4, text 2.228138851986274) is the listed display mismatch (numeric
// gate T1 passes); see selftest_w2c_macrofns.cpp.
double stataInvttail(double df, double p) {
  if (std::isnan(df) || df >= kStataMissingSentinel) return df;
  if (std::isnan(p) || p >= kStataMissingSentinel) return p;
  if (!(df > 0) || !(p > 0.0 && p < 1.0)) return kNaN;
  // p == .5: the root is exactly t = 0 by symmetry (fix round 2).
  if (p == 0.5) return 0.0;
  if (p > 0.5) return -stataInvttail(df, 1.0 - p);
  // From here on 0 < p < .5 and the root is positive.
  double t;
  if (df == 1.0) {
    // Cauchy: t = tan(pi*(1/2-p)). 1/2-p is exact for p >= 1/4 (Sterbenz);
    // below that it would round, and cot(pi*p) = 1/tan(pi*p) keeps the
    // relative error at a few ulp instead (invttail(1,1e-300) = 3.18e299).
    t = p >= 0.25 ? std::tan(M_PI * (0.5 - p)) : 1.0 / std::tan(M_PI * p);
  } else if (df == 2.0) {
    // df 2: ttail(2,t) = (1 - t/sqrt(2+t^2))/2, so t = (1-2p)/sqrt(2p(1-p)).
    t = (1.0 - 2.0 * p) / std::sqrt(2.0 * p * (1.0 - p));
  } else if (df > kTLargeDf) {
    if (p < normalUpperTail(kNormalDeviateCeiling)) return kNaN;
    // Q(z) is decreasing, so bisect on -Q(z).
    const double z = bisectIncreasing([](double v) { return -normalUpperTail(v); }, -p,
                                      kNormalDeviateCeiling);
    t = tFromNormalDeviate(z, df);
  } else {
    t = invttailBisectNewton(df, p);
  }
  if (!std::isfinite(t) || t >= kStataMissingSentinel) return kNaN;
  return t;
}


PyValue fnTtail(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "ttail");
  auto fdf = pythonFloat(args[0]);
  auto ft = pythonFloat(args[1]);
  if (!fdf || !ft) return PyValue::floating(kNaN);
  return PyValue::floating(stataTtail(*fdf, *ft));
}
PyValue fnInvttail(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "invttail");
  auto fdf = pythonFloat(args[0]);
  auto fp = pythonFloat(args[1]);
  if (!fdf || !fp) return PyValue::floating(kNaN);
  return PyValue::floating(stataInvttail(*fdf, *fp));
}
// invt(df,p): the LOWER-tail quantile (P(T<=t)=p), the mirror of
// invttail's upper tail by the t distribution's symmetry: invt(df,p) =
// -invttail(df,p). Review finding (v2): this used to be invttail(df,1-p),
// and for a small p that 1-p rounds (invt(10,1e-10) was off by 4.8e-8 and
// invt(1,1e-12) by 7.8e-5). stataInvttail's own p > .5 reflection forms 1-p
// only where it is exact.
PyValue fnInvt(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "invt");
  auto fdf = pythonFloat(args[0]);
  auto fp = pythonFloat(args[1]);
  if (!fdf || !fp) return PyValue::floating(kNaN);
  // Missing guarded before the negation below: a negated sentinel is a large
  // NEGATIVE double and would lose the extended-missing kind `.a`..`.z`.
  if (std::isnan(*fdf) || *fdf >= kStataMissingSentinel) return PyValue::floating(*fdf);
  if (std::isnan(*fp) || *fp >= kStataMissingSentinel) return PyValue::floating(*fp);
  const double q = stataInvttail(*fdf, *fp);
  if (std::isnan(q) || q >= kStataMissingSentinel) return PyValue::floating(q);
  // invt(df,.5): q is +0.0 and -q would render "-0" (Stata: 0).
  return PyValue::floating(positiveZero(-q));
}

// normal(z): standard normal CDF. Upper half (z >= 0): `0.5*(1+erf(z/sqrt2))`,
// not the mathematically-equivalent `0.5*erfc(-z/sqrt2)` -- the two round
// differently in the last bit and only the erf form is bit-identical to the
// oracle (w2c_functions.log normal_macro, normal(1.96) -> .9750021048517796;
// measured both ways, erfc's last digit was off by one ULP).
//
// BUG-662: lower half (z < 0) through the complementary error function. The
// erf form there is 1 + (a number near -1): the cancellation left
// normal(-8) = 6.106226635438361e-16 (StataBE 19.5: 6.22096057427178486e-16,
// 1.8% off) and normal(-10) = normal(-30) = 0 exactly. 0.5*erfc(-z/sqrt2)
// has no subtraction and stays relative-accurate down to the underflow.
PyValue fnNormal(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "normal");
  auto fz = pythonFloat(args[0]);
  if (!fz) return PyValue::floating(kNaN);
  double z = *fz;
  if (std::isnan(z) || z >= kStataMissingSentinel) return PyValue::floating(z);
  if (z < 0.0) return PyValue::floating(0.5 * std::erfc(-z / std::sqrt(2.0)));
  return PyValue::floating(0.5 * (1.0 + std::erf(z / std::sqrt(2.0))));
}

// invnormal(p): the inverse standard normal CDF, through the engine's own
// AS241 (cecon::stata_invnormal, core/distributions.h) rather than a second
// implementation. A missing argument comes back unchanged (`.a` stays `.a`,
// as fnNormal); outside (0,1) the engine answers NaN, which is returned as
// the macro layer's finite missing sentinel so that `invnormal(2) == .` is
// true, as for fnFloat (BUG-584; a NaN prints `.` but compares false).
// Oracle w2c_functions.log:320/:331 (invnormal_matrix_lit/_elem, rc 0).
PyValue fnInvnormal(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "invnormal");
  auto fp = pythonFloat(args[0]);
  if (!fp) return PyValue::floating(kNaN);
  const double p = *fp;
  if (std::isnan(p) || p >= kStataMissingSentinel) return PyValue::floating(p);
  const double z = cecon::stata_invnormal(p);
  return PyValue::floating(std::isnan(z) ? kStataMissingSentinel : z);
}

// runiform()/runiformint(a,b): doctrine carve-out (①-S, distributional
// agreement only) -- w2c_functions.log's macro arms (runiform_macro,
// runiformint_macro) assert rc 0, never the drawn value; the stream is ours,
// not Stata's.
//
// Review finding (v2): these drew from the C library's rand() and ignored
// `set seed`. They now draw from the engine's Philox4x32-10 (core/rng.h, as
// handlers_resample.cpp does), keyed on the session's `set seed` through
// EvalSession::rngSeed and counted by EvalSession::rngDraws, so `set seed 7`
// followed by the same draws gives the same values. A draw needs the session,
// and ns.fns entries carry none, so the SESSION-BOUND versions are installed
// per call by addSessionRngFns() below (evalArith and evalMacroExpr in
// macros.cpp). The entries in the process-wide registry are what a caller
// WITHOUT that per-call install reaches -- today loops.cpp's `while`/`if`
// condition namespace -- and there they refuse by name rather than draw from
// a stream `set seed` cannot reach. rc 199 is this layer's "not implemented
// in this build" code (errors.cpp coarseRc maps unsupported_command to 199);
// Stata itself has no refusal here.
PyValue fnRuniformMacro(const std::vector<PyValue>& args) {
  requireArity(args, 0, 0, "runiform");
  throw PyExc("r(199): runiform() is not available in this context: it needs the "
              "session's `set seed` state, which only local/global/display "
              "expressions carry in this build");
}
PyValue fnRuniformintMacro(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "runiformint");
  throw PyExc("r(199): runiformint() is not available in this context: it needs "
              "the session's `set seed` state, which only local/global/display "
              "expressions carry in this build");
}

namespace {

// Philox's first counter word separates this stream from every other Philox
// consumer; the second is the draw index. The tag is ours (ASCII "MACRORNG").
constexpr uint64_t kMacroRngStream = 0x4D4143524F524E47ull;

uint64_t macroRngDraw64(EvalSession& session) {
  EvalSession::RngSeedReading reading;
  if (session.rngSeed) reading = session.rngSeed();
  if (reading.epoch != session.rngEpochSeen) {
    session.rngEpochSeen = reading.epoch;
    session.rngDraws = 0;
  }
  const uint64_t seed = static_cast<uint64_t>(reading.seed.value_or(123456789LL));
  const uint64_t g = session.rngDraws++;
  const uint64_t hi = cecon::Philox::draw32(seed, kMacroRngStream, 2 * g);
  const uint64_t lo = cecon::Philox::draw32(seed, kMacroRngStream, 2 * g + 1);
  return (hi << 32) | lo;
}

}  // namespace

void addSessionRngFns(PyNamespace& ns, EvalSession& session) {
  EvalSession* sp = &session;
  // 53 random bits at the centre of their 2^-53 cell: strictly inside (0,1).
  ns.fns["runiform"] = [sp](const std::vector<PyValue>& args) -> PyValue {
    requireArity(args, 0, 0, "runiform");
    const uint64_t bits = macroRngDraw64(*sp) >> 11;
    return PyValue::floating((static_cast<double>(bits) + 0.5) * 0x1.0p-53);
  };
  ns.fns["runiformint"] = [sp](const std::vector<PyValue>& args) -> PyValue {
    requireArity(args, 2, 2, "runiformint");
    auto flo = pythonFloat(args[0]);
    auto fhi = pythonFloat(args[1]);
    if (!flo || !fhi) return PyValue::floating(kNaN);
    // Missing bound -> missing (BUG-660 fix round). A bound past 2^53 is not
    // an exactly representable integer and would overflow llround: missing.
    constexpr double kMaxExactInt = 9007199254740992.0;
    if (std::isnan(*flo) || *flo >= kStataMissingSentinel || std::isnan(*fhi) ||
        *fhi >= kStataMissingSentinel || std::fabs(*flo) > kMaxExactInt ||
        std::fabs(*fhi) > kMaxExactInt) {
      return PyValue::floating(kNaN);
    }
    long long a = static_cast<long long>(std::llround(*flo));
    long long b = static_cast<long long>(std::llround(*fhi));
    if (b < a) std::swap(a, b);
    const uint64_t span = static_cast<uint64_t>(b - a) + 1ull;
    // Rejection sampling: `draw % span` is biased unless span divides 2^64,
    // so draws at or past the last whole multiple of span are discarded.
    const uint64_t limit = (UINT64_MAX / span) * span;
    uint64_t draw;
    do {
      draw = macroRngDraw64(*sp);
    } while (draw >= limit);
    return PyValue::integer(a + static_cast<long long>(draw % span));
  };
}

// ---------------------------------------------------------------------------
// String functions

PyValue fnLower(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "lower");
  return PyValue::str(asciiLower(pythonStr(args[0])));
}

PyValue fnUpper(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "upper");
  return PyValue::str(asciiUpper(pythonStr(args[0])));
}

PyValue fnStrtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strtrim");
  return PyValue::str(pyStripAscii(pythonStr(args[0]), true, true));
}

PyValue fnLtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "ltrim");
  return PyValue::str(pyStripAscii(pythonStr(args[0]), true, false));
}

PyValue fnRtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "rtrim");
  return PyValue::str(pyStripAscii(pythonStr(args[0]), false, true));
}

PyValue fnSubstr(const std::vector<PyValue>& args) {
  requireArity(args, 2, 3, "substr");
  try {
    std::string ss = pythonStr(args[0]);
    auto fstart = pythonFloat(args[1]);
    if (!fstart) throw PyExc("substr: start not numeric");
    long long i = intFromDouble(*fstart);
    if (i < 1) i = 1;
    size_t pyStart = static_cast<size_t>(i - 1);
    long long ln = -1;
    if (args.size() >= 3) {
      // Python: try int(float(length)) except (TypeError, ValueError): -1
      auto fl = pythonFloat(args[2]);
      if (fl) {
        try {
          ln = intFromDouble(*fl);
        } catch (const PyExc&) {
          // int(nan) ValueError is caught by THIS inner except -> ln = -1;
          // int(inf) OverflowError is NOT (TypeError, ValueError) — it
          // escapes to the outer `except Exception: return ""`.
          if (std::isinf(*fl)) return PyValue::str("");
          ln = -1;
        }
      } else {
        ln = -1;
      }
    }
    size_t n = utf8Len(ss);
    if (ln < 0) return PyValue::str(utf8Slice(ss, pyStart, n));
    return PyValue::str(utf8Slice(ss, pyStart, pyStart + static_cast<size_t>(ln)));
  } catch (const PyExc&) {
    return PyValue::str("");  // outer `except Exception: return ""`
  }
}

PyValue fnStrlen(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strlen");
  return PyValue::integer(static_cast<long long>(utf8Len(pythonStr(args[0]))));
}

PyValue fnStrpos(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "strpos");
  std::string hs = pythonStr(args[0]);
  std::string nd = pythonStr(args[1]);
  long long idx = utf8Find(hs, nd);
  return PyValue::integer(idx >= 0 ? idx + 1 : 0);
}

// regexm()/regexr() take a literal pattern string per call and corpora
// re-evaluate the same pattern on every row of a dataset — recompiling
// std::regex from scratch each time is pure waste for the (common) case of
// a handful of distinct patterns hit thousands of times. Memoize compiled
// regexes process-locally, capped so a pathological many-distinct-pattern
// workload can't grow this unbounded. A pattern that fails to compile is
// cached too (as nullopt) — regexm/regexr swallow std::regex_error
// unconditionally (Python: except re.error), so skipping the repeat
// compile-and-throw is safe and doesn't change what's observable.
constexpr size_t kRegexMemoCap = 4096;

std::unordered_map<std::string, std::optional<std::regex>>& regexMemo() {
  static std::unordered_map<std::string, std::optional<std::regex>> memo;
  return memo;
}

// Returns the cached compiled regex for `pattern` (nullptr if `pattern` is
// cached as invalid), or std::nullopt if the memo has nothing for it and is
// at capacity — caller compiles (and catches) locally in that case, same as
// before this memo existed.
std::optional<const std::regex*> memoizedRegex(const std::string& pattern) {
  auto& memo = regexMemo();
  auto it = memo.find(pattern);
  if (it != memo.end()) {
    return it->second ? std::optional<const std::regex*>(&*it->second)
                       : std::optional<const std::regex*>(nullptr);
  }
  if (memo.size() >= kRegexMemoCap) return std::nullopt;
  try {
    auto [ins, _] = memo.emplace(pattern, std::regex(pattern));
    return &*ins->second;
  } catch (const std::regex_error&) {
    memo.emplace(pattern, std::nullopt);
    return nullptr;
  }
}

// BUG-660 fix round: regexs(n) reads the capture groups of the LAST
// SUCCESSFUL regexm() call -- Stata's own contract (regexs() is undefined
// until a preceding regexm() returns true). A plain function-local static,
// process-wide like regexMemo() above, not session-scoped: this evaluator has
// no session threaded into ns.fns entries (they are built once, statically,
// by stataMathNamespace()/postfileStataMathNamespace()), and PIET is
// single-threaded per process the same way that static already assumes. A
// failed regexm() call (no match, or a bad pattern) leaves the state alone,
// matching Stata's "results are only defined right after a TRUE regexm()".
struct RegexMatchState {
  bool valid = false;
  std::vector<std::string> groups;  // groups[0] = whole match, [1..] = captures
};
RegexMatchState& lastRegexMatch() {
  static RegexMatchState state;
  return state;
}

PyValue fnRegexm(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "regexm");
  const std::string subject = pythonStr(args[0]);
  const std::string pattern = pythonStr(args[1]);
  auto cached = memoizedRegex(pattern);
  auto search = [&](const std::regex& re) -> bool {
    std::smatch m;
    const bool found = std::regex_search(subject, m, re);
    if (found) {
      RegexMatchState& st = lastRegexMatch();
      st.valid = true;
      st.groups.clear();
      st.groups.reserve(m.size());
      for (size_t g = 0; g < m.size(); ++g) {
        st.groups.push_back(m[g].matched ? m[g].str() : std::string());
      }
    }
    return found;
  };
  try {
    if (cached) {
      if (!*cached) return PyValue::boolean(false);  // Python: except re.error -> False
      return PyValue::boolean(search(**cached));
    }
    std::regex re(pattern);  // memo at capacity — compile without caching
    return PyValue::boolean(search(re));
  } catch (const std::regex_error&) {
    return PyValue::boolean(false);  // Python: except re.error -> False
  }
}

// BUG-660 fix round: regexs(n) -- the capture-group readback for regexm()
// above. w2c_functions.log's regexs_macro_standalone arm (`local x =
// regexs(0)` -> "A", rc 0) needs a PRECEDING regexm() in the same evaluator;
// see selftest_w2c_macrofns.cpp for the arm this rests on. An out-of-range n
// or no prior successful match is Stata's own answer for the same case: the
// empty string, not an error.
PyValue fnRegexs(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "regexs");
  auto fn = pythonFloat(args[0]);
  if (!fn) return PyValue::str("");
  long long idx;
  try {
    idx = intFromDouble(*fn);
  } catch (const PyExc&) {
    return PyValue::str("");
  }
  const RegexMatchState& st = lastRegexMatch();
  if (!st.valid || idx < 0 || idx >= static_cast<long long>(st.groups.size())) {
    return PyValue::str("");
  }
  return PyValue::str(st.groups[static_cast<size_t>(idx)]);
}

PyValue fnRegexr(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "regexr");
  std::string s = pythonStr(args[0]);
  const std::string pattern = pythonStr(args[1]);
  auto cached = memoizedRegex(pattern);
  // Python re.sub(..., count=1): replace FIRST match only.
  // NOTE dialect divergence: Python replacement processes \1 groups;
  // ECMAScript uses $1 — corpus uses plain replacement strings.
  try {
    if (cached) {
      if (!*cached) return PyValue::str(s);  // Python: except re.error -> str(s)
      return PyValue::str(std::regex_replace(s, **cached, pythonStr(args[2]),
                                              std::regex_constants::format_first_only));
    }
    std::regex re(pattern);  // memo at capacity — compile without caching
    return PyValue::str(std::regex_replace(s, re, pythonStr(args[2]),
                                            std::regex_constants::format_first_only));
  } catch (const std::regex_error&) {
    return PyValue::str(s);  // Python: except re.error -> str(s)
  }
}

// Review finding (v2): ustrregexra() matches on CODE POINTS. Fix round 2
// refused a few byte-sensitive constructs and let the rest through byte-wise
// (`ustrregexra("éé","é+","x")` gave "xx": the `+` applied to é's last byte
// only); the refusal itself surfaced as r(198) "invalid syntax". Now subject,
// pattern and replacement are decoded to UTF-32 (wchar_t is 32 bits on every
// platform this builds for; static_assert below) and matched with
// std::wregex, so `.`, quantifiers and bracket classes see whole code points.
// Measured in StataBE 19.5 (piet-macro-layer-functions probe uregex.log):
// ustrregexra("é",".","x") = "x" and ustrregexra("café","caf","tea") = "teaé".
//
// What ECMAScript still does NOT share with Stata's ICU engine is refused BY
// NAME, rc 199 (this layer's "not implemented in this build" code; errors.cpp
// coarseRc maps unsupported_command to 199 -- Stata has no refusal here, so
// no oracle rc exists), on the `r(NNN): message` channel evalArith forwards:
//  * on a subject with a non-ASCII code point: \b \B \w \W \d \D \s \S and
//    POSIX [:class:] (ICU classifies by Unicode property; std::wregex under
//    the classic locale knows ASCII only -- measured in Stata,
//    ustrregexra("café","[^\w ]","") keeps é, which byte- or classic-locale
//    \w would delete);
//  * case-insensitive matching (4th argument) when subject or pattern has a
//    non-ASCII code point (ICU folds É to é; the classic locale does not);
//  * `.` on a subject holding U+0085 (ICU's `.` excludes NEL, ECMAScript's
//    does not);
//  * ICU-only syntax std::regex would misread or reject: \p \P \X \N \h \H
//    \R \Q \E \A \Z \z \G \K \v \V \e \a, `(?` groups other than (?: (?= (?!,
//    nested `[` / `&&` set operations inside a bracket class;
//  * a replacement holding `\` or a `$` not followed by a digit (ICU and
//    ECMAScript read those differently);
//  * a pattern std::wregex cannot compile, or a subject/pattern/replacement
//    that is not valid UTF-8.
// The subject is never echoed back as if the replacement had run.
static_assert(sizeof(wchar_t) == 4, "ustrregexra decodes to UTF-32 in wchar_t");

void appendUtf8(std::string& out, unsigned cp);  // defined with ustrunescape below

static bool decodeUtf8Strict(const std::string& s, std::wstring& out) {
  out.clear();
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t n = 0;
    if (!strfns::utf8_seq_len(s, i, n)) return false;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    unsigned cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    out.push_back(static_cast<wchar_t>(cp));
    i += n;
  }
  return true;
}

static bool hasNonAscii(const std::wstring& w) {
  for (wchar_t c : w) {
    if (static_cast<unsigned>(c) >= 0x80) return true;
  }
  return false;
}

// The first construct of `pattern`/`repl` whose ECMAScript reading differs
// from ICU's for this input, as the text to name in the refusal; "" if none.
static std::string ustrregexDivergence(const std::wstring& pattern, const std::wstring& repl,
                                       bool subjectNonAscii, bool subjectHasNel,
                                       bool caseInsensitive, bool patternNonAscii) {
  if (caseInsensitive && (subjectNonAscii || patternNonAscii)) {
    return "case-insensitive matching of non-ASCII text";
  }
  bool inClass = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const wchar_t c = pattern[i];
    if (c == L'\\' && i + 1 < pattern.size()) {
      const wchar_t e = pattern[i + 1];
      if (std::wstring(L"pPXNhHRQEAZzGKvVea").find(e) != std::wstring::npos) {
        return std::string("\\") + static_cast<char>(e);
      }
      if (subjectNonAscii && std::wstring(L"bBwWdDsS").find(e) != std::wstring::npos) {
        return std::string("\\") + static_cast<char>(e) + " on non-ASCII text";
      }
      ++i;
      continue;
    }
    if (inClass) {
      if (c == L'[' && i + 1 < pattern.size() && pattern[i + 1] == L':') {
        if (subjectNonAscii) return "POSIX [:class:] on non-ASCII text";
      } else if (c == L'[') {
        return "a nested [ set inside a bracket class";
      } else if (c == L'&' && i + 1 < pattern.size() && pattern[i + 1] == L'&') {
        return "&& set intersection inside a bracket class";
      } else if (c == L']') {
        inClass = false;
      }
      continue;
    }
    if (c == L'[') {
      inClass = true;
      // A `]` or `^]` right after the opening bracket is a literal member.
      if (i + 1 < pattern.size() && pattern[i + 1] == L'^') ++i;
      if (i + 1 < pattern.size() && pattern[i + 1] == L']') ++i;
      continue;
    }
    if (c == L'(' && i + 1 < pattern.size() && pattern[i + 1] == L'?') {
      const wchar_t g = i + 2 < pattern.size() ? pattern[i + 2] : L'\0';
      if (g != L':' && g != L'=' && g != L'!') return "a (? group other than (?: (?= (?!";
    }
    if (c == L'.' && subjectHasNel) return ". on text holding U+0085";
  }
  for (size_t i = 0; i < repl.size(); ++i) {
    if (repl[i] == L'\\') return "\\ in the replacement";
    if (repl[i] == L'$' && !(i + 1 < repl.size() && repl[i + 1] >= L'0' && repl[i + 1] <= L'9')) {
      return "$ not followed by a group number in the replacement";
    }
  }
  return "";
}

[[noreturn]] static void refuseUstrregexra(const std::string& why) {
  throw PyExc("r(199): ustrregexra(): " + why +
              " is not supported in this build (Stata matches with ICU regular expressions)");
}

// BUG-660: ustrregexra(s,pattern,replace[,i]) -- replace every match (the
// "ra" suffix). Optional 4th argument: nonzero requests case-insensitive
// matching (measured in Stata: ustrregexra("aAbB","a","X",1) -> "XXbB").
PyValue fnUstrregexra(const std::vector<PyValue>& args) {
  requireArity(args, 3, 4, "ustrregexra");
  std::wstring ws, wp, wr;
  if (!decodeUtf8Strict(pythonStr(args[0]), ws) || !decodeUtf8Strict(pythonStr(args[1]), wp) ||
      !decodeUtf8Strict(pythonStr(args[2]), wr)) {
    refuseUstrregexra("an argument that is not valid UTF-8");
  }
  bool caseInsensitive = false;
  if (args.size() == 4) {
    auto ci = pythonFloat(args[3]);
    caseInsensitive = ci && *ci != 0.0 && !std::isnan(*ci);
  }
  const std::string why =
      ustrregexDivergence(wp, wr, hasNonAscii(ws), ws.find(static_cast<wchar_t>(0x85)) != std::wstring::npos,
                          caseInsensitive, hasNonAscii(wp));
  if (!why.empty()) refuseUstrregexra(why);
  std::wstring replaced;
  try {
    auto flags = std::regex_constants::ECMAScript;
    if (caseInsensitive) flags |= std::regex_constants::icase;
    const std::wregex re(wp, flags);
    replaced = std::regex_replace(ws, re, wr);
  } catch (const std::regex_error&) {
    refuseUstrregexra("a pattern std::regex cannot compile");
  }
  std::string out;
  out.reserve(replaced.size());
  for (wchar_t c : replaced) appendUtf8(out, static_cast<unsigned>(c));
  return PyValue::str(out);
}

PyValue fnReal(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "real");
  auto f = pythonFloat(args[0]);
  return PyValue::floating(f ? *f : kNaN);
}

// BUG-660: reverse(s) -- byte reversal (not a `ustr*` name, so plain-byte
// like Stata's own non-Unicode string functions; the oracle's fixture is
// ASCII, so byte- vs codepoint-reversal isn't distinguished here, only
// documented as the narrower of the two).
PyValue fnReverse(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "reverse");
  std::string s = pythonStr(args[0]);
  std::reverse(s.begin(), s.end());
  return PyValue::str(s);
}

// utf8Find (pyvalue.cpp/.h)'s mirror for the LAST occurrence -- strrpos()'s
// oracle arm (strrpos_macro) needs the position of the final match, not the
// first utf8Find already serves via strpos().
long long utf8RFind(const std::string& s, const std::string& sub) {
  if (sub.empty()) return -1;
  size_t pos = s.rfind(sub);
  if (pos == std::string::npos) return -1;
  long long cp = 0;
  for (size_t i = 0; i < pos; ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++cp;
  }
  return cp;
}

PyValue fnStrrpos(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "strrpos");
  std::string hs = pythonStr(args[0]);
  std::string nd = pythonStr(args[1]);
  long long idx = utf8RFind(hs, nd);
  return PyValue::integer(idx >= 0 ? idx + 1 : 0);
}

PyValue fnWordcount(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "wordcount");
  return PyValue::integer(static_cast<long long>(pySplit(pythonStr(args[0])).size()));
}

// strtoname(s): every character that isn't a letter/digit/underscore becomes
// an underscore, one-for-one (no run-collapsing — w2c_functions.log's
// strtoname_macro turns the fixture's two leading/two interior spaces into
// two/two underscores, not one/one). Stata's leading-digit reprefixing is
// undocumented here (not exercised by any captured arm).
PyValue fnStrtoname(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strtoname");
  std::string s = pythonStr(args[0]);
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    out.push_back((std::isalnum(c) || c == '_') ? static_cast<char>(c) : '_');
  }
  // BUG-660 fix round: a Stata NAME cannot start with a digit or be empty --
  // Stata's strtoname() prefixes an underscore in both cases -- and is
  // capped at namelen() (32 characters), documented behaviour neither
  // exercised by the single captured strtoname_macro arm (no leading digit,
  // under 32 chars either way, so this is additive, not a behaviour change
  // for that arm).
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) {
    out.insert(out.begin(), '_');
  }
  if (out.size() > 32) out.resize(32);
  return PyValue::str(out);
}

// strmatch(s,pattern): `*`/`?` shell-style wildcards, no character classes
// (Stata's strmatch() grammar). Hand-rolled rather than POSIX fnmatch() to
// keep this file platform-neutral (macros.cpp's own glob helper already
// carries a _WIN32 fnmatch fallback for exactly this reason; duplicating that
// split here for a two-wildcard grammar isn't worth it). Classic iterative
// matcher: `*` records a backtrack point, `?`/literal advance both cursors.
bool globMatchWildcard(const std::string& pattern, const std::string& s) {
  size_t p = 0, si = 0, star = std::string::npos, mark = 0;
  while (si < s.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == s[si])) {
      ++p;
      ++si;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = si;
    } else if (star != std::string::npos) {
      p = star + 1;
      si = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

PyValue fnStrmatch(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "strmatch");
  return PyValue::boolean(globMatchWildcard(pythonStr(args[1]), pythonStr(args[0])));
}

// ustrunescape(s): decode Stata's fixed-width `\uXXXX` escapes (exactly four
// hex digits, no surrogate pairs in the grammar) to UTF-8; any other
// backslash sequence passes through literally, matching BUG-502's "Stata has
// no escape sequences" rule for the enclosing string literal — this function
// is the one place that DOES interpret `\u`, by name, on its argument text.
void appendUtf8(std::string& out, unsigned cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    // Four-byte form (review v2): ustrregexra re-encodes whole UTF-32 text,
    // which can hold code points past the BMP; \uXXXX never reaches here.
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

PyValue fnUstrunescape(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "ustrunescape");
  std::string s = pythonStr(args[0]);
  std::string out;
  out.reserve(s.size());
  auto isHex = [](unsigned char c) { return std::isxdigit(c) != 0; };
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '\\' && i + 5 < s.size() && s[i + 1] == 'u' &&
        isHex(static_cast<unsigned char>(s[i + 2])) && isHex(static_cast<unsigned char>(s[i + 3])) &&
        isHex(static_cast<unsigned char>(s[i + 4])) && isHex(static_cast<unsigned char>(s[i + 5]))) {
      unsigned cp = static_cast<unsigned>(std::stoul(s.substr(i + 2, 4), nullptr, 16));
      appendUtf8(out, cp);
      i += 6;
    } else {
      out.push_back(s[i]);
      ++i;
    }
  }
  return PyValue::str(out);
}

PyValue fnWord(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "word");
  try {
    auto tokens = pySplit(pythonStr(args[0]));
    auto fn = pythonFloat(args[1]);
    if (!fn) throw PyExc("word: n not numeric");
    long long idx = intFromDouble(*fn) - 1;
    if (idx >= 0 && idx < static_cast<long long>(tokens.size())) {
      return PyValue::str(tokens[static_cast<size_t>(idx)]);
    }
    return PyValue::str("");
  } catch (const PyExc&) {
    return PyValue::str("");
  }
}

PyValue fnSubinstr(const std::vector<PyValue>& args) {
  requireArity(args, 3, 4, "subinstr");
  try {
    std::string ss = pythonStr(args[0]);
    std::string sf = pythonStr(args[1]);
    std::string st = pythonStr(args[2]);
    if (sf.empty()) return PyValue::str(ss);
    long long count = -1;
    if (args.size() >= 4) {
      // Python: try float(n); if not isnan: count=int(fn); except -> -1.
      auto fn = pythonFloat(args[3]);
      if (fn && !std::isnan(*fn)) {
        try {
          count = intFromDouble(*fn);
        } catch (const PyExc&) {
          // int(inf) OverflowError not in (TypeError, ValueError):
          // escapes to the outer except Exception -> return str(s).
          if (std::isinf(*fn)) return PyValue::str(ss);
          count = -1;
        }
      }
      // fn parse failure or NaN: count stays -1
    }
    if (count < 0) return PyValue::str(pyReplace(ss, sf, st));
    if (count == 0) return PyValue::str(ss);
    return PyValue::str(pyReplace(ss, sf, st, count));
  } catch (const PyExc&) {
    // Python: except Exception -> str(s) if s is not None else ""
    return PyValue::str(pythonStr(args[0]));
  }
}

PyValue fnProper(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "proper");
  // Python str.title(): uppercase a cased char that follows a non-cased
  // char, lowercase the rest. ASCII approximation (see header caveat).
  std::string s = pythonStr(args[0]);
  std::string out;
  out.reserve(s.size());
  bool prevAlpha = false;
  for (unsigned char c : s) {
    bool alpha = std::isalpha(c) != 0;
    if (alpha && !prevAlpha) {
      out.push_back(static_cast<char>(std::toupper(c)));
    } else if (alpha) {
      out.push_back(static_cast<char>(std::tolower(c)));
    } else {
      out.push_back(static_cast<char>(c));
    }
    prevAlpha = alpha;
  }
  return PyValue::str(out);
}

PyValue fnString(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "string");
  const PyValue& n = args[0];
  // BUG-249: a STRING first argument WITH a format is a TYPE error in Stata —
  // r(109) "type mismatch" on both the display and the `local =` surfaces
  // (exprfns_contract_oracle.log, G|strarg rows, rc=109 both). It used to be
  // returned unchanged before the format was even looked at. The one-argument
  // form stays the Python ancestor's pass-through — not in the capture, so not
  // claimed. The `r(NNN): message` prefix is the exact-rc channel evalArith
  // forwards to tryCaptureDefinition (see its raisedError contract).
  if (n.isStr() && args.size() >= 2) throw PyExc("r(109): type mismatch");
  if (n.isStr()) return n;  // Python: isinstance(n, str) -> return n
  double fv = n.numAsDouble();  // float() of Bool/Int/Float never fails
  // BUG-206 (third half). `isnan` ALONE IS THE WRONG MISSING TEST ON THIS PATH.
  //
  // The expression layer does not represent Stata missing as a NaN — it uses the
  // large FINITE sentinel kStataMissingSentinel (exprfns.h, and evalArith's own
  // isStataMissingResult tests `fabs(d) >= kStataMissingSentinel` for exactly this
  // reason). So `replaceBareDotMissing` turns a bare `.` into that sentinel, the
  // isnan guard never fired, and this fell through to the renderer with
  // 8.988465674311579e+307 as an ordinary number:
  //     string(.)            -> [8.988466e+307]   Stata: [.]
  //     string(., "%9.0g")   -> [8.99e+307]       Stata: [.]
  // Oracle StataNow 19.5, in-log marker, both rows measured on both arms.
  //
  // ATTRIBUTION, because the two halves are not the same age. The ONE-ARGUMENT
  // leak is PRE-EXISTING and independent of BUG-206: `string(.)` contains no
  // percent sign, so it was always admitted by the whitelist and always printed
  // the sentinel. The TWO-ARGUMENT leak is one this stint newly EXPOSED — before
  // the whitelist widening `string(., "%9.0g")` was refused and echoed as visibly
  // unevaluated text, and afterwards it would have emitted a confident wrong
  // number. Fixing only the half I exposed and leaving the other would be
  // arbitrary: it is one guard, and one guard is the fix.
  //
  // BUG-249 closes the extended-code half BUG-206 filed: the sentinel now
  // CARRIES the kind (see kStataMissingKindStep in exprfns.h — `.a`..`.z` are
  // 26 distinct doubles above the base sentinel, written by macros.cpp's
  // replaceBareDotMissing). Oracle: string(.a) and string(.a,"%9.0g") -> `.a`,
  // string(.z,"%9.0g") -> `.z` (exprfns_contract_oracle.log, G|dot* rows). A
  // negative-side missing (the evaluator negates the sentinel like any double)
  // keeps rendering plain `.`, as before.
  if (std::isnan(fv)) return PyValue::str(".");
  if (std::fabs(fv) >= kStataMissingSentinel) {
    auto kind = stataExtendedMissingKind(fv);
    if (kind && *kind >= 1) {
      return PyValue::str(std::string(".") + static_cast<char>('a' + *kind - 1));
    }
    return PyValue::str(".");
  }
  std::optional<std::string> sfmt;
  if (args.size() >= 2) {
    sfmt = pyStripAscii(pythonStr(args[1]), true, true);
  }
  // gFormat/applyStataFmt may throw PyExc (int(inf) OverflowError escapes
  // Python's except (TypeError, ValueError)) — let it propagate: the whole
  // expression fails, mirroring the oracle.
  if (sfmt && !sfmt->empty() && (*sfmt)[0] == '%') {
    return PyValue::str(applyStataFmt(fv, *sfmt));
  }
  // BUG-206 (second half). A SECOND ARGUMENT THAT IS NOT A FORMAT IS AN ERROR,
  // AND STATA'S ANSWER FOR IT IS THE EMPTY STRING — not "ignore the argument and
  // render the default way", which is what falling through to gFormat() does.
  //
  // This branch was unobservable until the whitelist fix above made the two-arg
  // form reachable at all, and it matters precisely BECAUSE of that fix: pre-fix,
  // `string(42, "abc%")` was refused and echoed as its own source text — visibly
  // unevaluated. Post-fix it would have evaluated to `42`, trading an obvious
  // non-answer for a confident wrong one. That is the exact trade the BUG-196
  // note in macros.cpp argues against, so the reachability fix has to bring this
  // with it rather than leave it for later.
  //
  // Oracle, StataNow 19.5, marker-judged, `string(42.5, "<fmt>")` with length
  // reported per cell (scratch capture this stint; bytes quoted in BUG-249):
  //     ""      -> [] len=0        "9.0g"  -> [] len=0
  //     "abc%"  -> [] len=0        "g"     -> [] len=0
  // versus the `%`-prefixed formats above, which all render (`%9.0g` -> [42.5],
  // `%9.3f` -> [42.500], `%20.10f` -> [42.5000000000]).
  //
  // ONLY the no-leading-`%` case is claimed here, because that is the boundary
  // this function already tested and the one the fix above newly exposes. Stata
  // also answers `""` for `%`, `%%`, `%9s` and `%-9s` — `%`-prefixed strings that
  // are not NUMERIC formats — and this still renders those via gFormat. That is a
  // pre-existing divergence on a path the whitelist fix does not change the
  // reachability of, so it is FILED (BUG-249) with its captured bytes rather than
  // guessed at here; telling "not a numeric format" from "a format this engine
  // has not implemented" needs the date/business-calendar families decided too
  // (`%tc` and `%td` both render real values in Stata and reach gFormat here).
  if (sfmt) return PyValue::str(std::string());
  return PyValue::str(gFormat(fv));
}

PyValue fnChar(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "char");
  auto f = pythonFloat(args[0]);
  if (!f) return PyValue::str("");
  long long code;
  try {
    code = intFromDouble(*f);
  } catch (const PyExc&) {
    // Python catches (TypeError, ValueError) here: int(nan) is ValueError
    // -> caught -> "". int(inf) is OverflowError -> NOT caught -> escapes
    // and fails the whole expression. Mirror by rethrowing on inf.
    if (std::isinf(*f)) throw;
    return PyValue::str("");
  }
  if (code < 0 || code > 0x10FFFF) return PyValue::str("");
  // Encode the codepoint as UTF-8 (Python chr + str semantics; lone
  // surrogates excluded from the corpus — json cannot carry them).
  std::string out;
  unsigned int cp = static_cast<unsigned int>(code);
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return PyValue::str(out);
}

PyValue fnAbbrev(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "abbrev");
  std::string ss = pythonStr(args[0]);
  try {
    auto fn = pythonFloat(args[1]);
    if (!fn) throw PyExc("abbrev: n not numeric");
    long long ln = intFromDouble(*fn);
    if (ln < 5) ln = 5;
    size_t cpLen = utf8Len(ss);
    if (cpLen <= static_cast<size_t>(ln)) return PyValue::str(ss);
    // Python: ss[: ln - 2] + "~" + ss[-1]
    std::string head = utf8Slice(ss, 0, static_cast<size_t>(ln - 2));
    std::string last = utf8Slice(ss, cpLen - 1, cpLen);
    return PyValue::str(head + "~" + last);
  } catch (const PyExc&) {
    // Python: except Exception -> str(s) if s is not None else ""
    return PyValue::str(ss);
  }
}

// BUG-371: the Stata date family, second layer. cobra's frame.cpp parser answers
// `gen dt = mdy(...)`; THIS namespace answers `display mdy(...)`, `scalar`, and any
// expression the string-mutation evaluator handles. Both call the same
// cecon:: helpers (data/stata_dates.h), so the two layers cannot give different
// answers for the same date — a class of drift this campaign has paid for before.
PyValue fnMdy(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "mdy");
  auto a = pythonFloat(args[0]);
  auto b = pythonFloat(args[1]);
  auto c = pythonFloat(args[2]);
  if (!a || !b || !c) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_mdy(*a, *b, *c));
}

PyValue datePartFn(const std::vector<PyValue>& args, cecon::DatePart part, const char* name) {
  requireArity(args, 1, 1, name);
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_date_part(*v, part));
}

PyValue fnYear(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_year, "year"); }
PyValue fnMonth(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_month, "month"); }
PyValue fnDay(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_day, "day"); }
PyValue fnDow(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_dow, "dow"); }
PyValue fnDoy(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_doy, "doy"); }
PyValue fnQuarter(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_quarter, "quarter"); }
PyValue fnHalfyear(const std::vector<PyValue>& a) { return datePartFn(a, cecon::DP_half, "halfyear"); }

// AER scope 2 / B3: the rest of the Stata time scales, same contract as the
// BUG-371 family above -- every one of these calls the SHARED cecon:: helper in
// data/stata_dates.h that cobra's frame.cpp calls, so `display mofd(d)` and
// `gen m = mofd(d)` cannot disagree. Values are pinned to
// tests/validate/b3_funcs_oracle.json.
//
// Missing propagates as Stata's `.`: a non-numeric argument yields NaN here the
// way datePartFn does, rather than throwing, because only ARITY errors throw in
// this layer (see the header's contract note).
template <typename F>
PyValue timeUnary(const std::vector<PyValue>& args, const char* name, F f) {
  requireArity(args, 1, 1, name);
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  return PyValue::floating(f(*v));
}
template <typename F>
PyValue timeBinary(const std::vector<PyValue>& args, const char* name, F f) {
  requireArity(args, 2, 2, name);
  auto a = pythonFloat(args[0]);
  auto b = pythonFloat(args[1]);
  if (!a || !b) return PyValue::floating(kNaN);
  return PyValue::floating(f(*a, *b));
}

PyValue fnMofd(const std::vector<PyValue>& a) { return timeUnary(a, "mofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_month); }); }
PyValue fnQofd(const std::vector<PyValue>& a) { return timeUnary(a, "qofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_quarter); }); }
PyValue fnHofd(const std::vector<PyValue>& a) { return timeUnary(a, "hofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_half); }); }
PyValue fnYofd(const std::vector<PyValue>& a) { return timeUnary(a, "yofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_year); }); }
PyValue fnWofd(const std::vector<PyValue>& a) { return timeUnary(a, "wofd", [](double x) { return cecon::stata_scale_of_day(x, cecon::DS_week); }); }
PyValue fnWeek(const std::vector<PyValue>& a) { return timeUnary(a, "week", [](double x) { return cecon::stata_week_of_day(x); }); }
PyValue fnDofm(const std::vector<PyValue>& a) { return timeUnary(a, "dofm", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_month); }); }
PyValue fnDofq(const std::vector<PyValue>& a) { return timeUnary(a, "dofq", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_quarter); }); }
PyValue fnDofh(const std::vector<PyValue>& a) { return timeUnary(a, "dofh", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_half); }); }
PyValue fnDofy(const std::vector<PyValue>& a) { return timeUnary(a, "dofy", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_year); }); }
PyValue fnDofw(const std::vector<PyValue>& a) { return timeUnary(a, "dofw", [](double x) { return cecon::stata_day_of_scale(x, cecon::DS_week); }); }
PyValue fnCofd(const std::vector<PyValue>& a) { return timeUnary(a, "cofd", [](double x) { return cecon::stata_cofd(x); }); }
PyValue fnCofdBig(const std::vector<PyValue>& a) { return timeUnary(a, "Cofd", [](double x) { return cecon::stata_Cofd(x); }); }
PyValue fnDofc(const std::vector<PyValue>& a) { return timeUnary(a, "dofc", [](double x) { return cecon::stata_dofc(x); }); }
PyValue fnDofcBig(const std::vector<PyValue>& a) { return timeUnary(a, "dofC", [](double x) { return cecon::stata_dofC(x); }); }
PyValue fnHh(const std::vector<PyValue>& a) { return timeUnary(a, "hh", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_hour, false); }); }
PyValue fnMm(const std::vector<PyValue>& a) { return timeUnary(a, "mm", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_minute, false); }); }
PyValue fnSs(const std::vector<PyValue>& a) { return timeUnary(a, "ss", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_second, false); }); }
PyValue fnHhC(const std::vector<PyValue>& a) { return timeUnary(a, "hhC", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_hour, true); }); }
PyValue fnMmC(const std::vector<PyValue>& a) { return timeUnary(a, "mmC", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_minute, true); }); }
PyValue fnSsC(const std::vector<PyValue>& a) { return timeUnary(a, "ssC", [](double x) { return cecon::stata_clock_part(x, cecon::CLK_second, true); }); }
PyValue fnMsofseconds(const std::vector<PyValue>& a) { return timeUnary(a, "msofseconds", [](double x) { return cecon::stata_msofseconds(x); }); }
PyValue fnMsofminutes(const std::vector<PyValue>& a) { return timeUnary(a, "msofminutes", [](double x) { return cecon::stata_msofminutes(x); }); }
PyValue fnMsofhours(const std::vector<PyValue>& a) { return timeUnary(a, "msofhours", [](double x) { return cecon::stata_msofhours(x); }); }
PyValue fnSeconds(const std::vector<PyValue>& a) { return timeUnary(a, "seconds", [](double x) { return cecon::stata_seconds(x); }); }
PyValue fnMinutes(const std::vector<PyValue>& a) { return timeUnary(a, "minutes", [](double x) { return cecon::stata_minutes(x); }); }
PyValue fnHours(const std::vector<PyValue>& a) { return timeUnary(a, "hours", [](double x) { return cecon::stata_hours(x); }); }

PyValue fnYm(const std::vector<PyValue>& a) { return timeBinary(a, "ym", [](double x, double y) { return cecon::stata_ym(x, y); }); }
PyValue fnYq(const std::vector<PyValue>& a) { return timeBinary(a, "yq", [](double x, double y) { return cecon::stata_yq(x, y); }); }
PyValue fnYh(const std::vector<PyValue>& a) { return timeBinary(a, "yh", [](double x, double y) { return cecon::stata_yh(x, y); }); }
PyValue fnYw(const std::vector<PyValue>& a) { return timeBinary(a, "yw", [](double x, double y) { return cecon::stata_yw(x, y); }); }

PyValue fnHms(const std::vector<PyValue>& args) {
  requireArity(args, 3, 3, "hms");
  auto h = pythonFloat(args[0]); auto m = pythonFloat(args[1]); auto s = pythonFloat(args[2]);
  if (!h || !m || !s) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_hms(*h, *m, *s));
}

PyValue fnDhms(const std::vector<PyValue>& args) {
  requireArity(args, 4, 4, "dhms");
  auto d = pythonFloat(args[0]); auto h = pythonFloat(args[1]);
  auto m = pythonFloat(args[2]); auto s = pythonFloat(args[3]);
  if (!d || !h || !m || !s) return PyValue::floating(kNaN);
  return PyValue::floating(cecon::stata_dhms(*d, *h, *m, *s));
}

// The engine-only remainder, measured by diffing `cobra functions` against
// `piet_native --dump-functions`: asinh cos ln ln1m ln1p sin tan existed in
// cobra's FNS[] and in no PIET layer, so `display sin(0)` did not fall back --
// it ABORTED the do-file with an engine_error, the same shape as mi().
//
// These deliberately use the ENGINE's missing convention (frame.cpp's fin():
// a non-finite result is Stata's `.`) rather than this file's inherited Python
// one, where sqrt(-1) and log(0) THROW. That inconsistency is real and is filed
// rather than fixed here: changing sqrt/log/exp/log10 would move a frozen
// 1.16.0 Python-oracle surface and needs its own adjudication and RED arm.
double engineFin(double v) { return std::isfinite(v) ? v : kNaN; }

template <typename F>
PyValue mathUnaryStata(const std::vector<PyValue>& args, const char* name, F f) {
  requireArity(args, 1, 1, name);
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  if (std::isnan(*v)) return PyValue::floating(kNaN);
  return PyValue::floating(f(*v));
}

PyValue fnLn(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "ln", [](double x) { return engineFin(std::log(x)); });
}
PyValue fnSin(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "sin", [](double x) { return std::sin(x); });
}
PyValue fnCos(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "cos", [](double x) { return std::cos(x); });
}
PyValue fnTan(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "tan", [](double x) { return std::tan(x); });
}
PyValue fnAsinh(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "asinh", [](double x) { return engineFin(std::asinh(x)); });
}
PyValue fnLn1p(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "ln1p", [](double x) { return engineFin(std::log1p(x)); });
}
PyValue fnLn1m(const std::vector<PyValue>& a) {
  return mathUnaryStata(a, "ln1m", [](double x) { return engineFin(std::log1p(-x)); });
}

// int() truncates toward zero. It was in cobra's FNS[] and NOT here, so
// `gen t = int(x)` worked while `display int(3.7)` answered r(1) -- the same
// one-layer landing BUG-431 left behind for mi(), found by the same census.
// ---------------------------------------------------------------------------
// AER scope 2 / B3 stage (a): string-argument date translation + the string
// family. These have no engine counterpart by construction (frame.cpp refuses a
// string operand), so unlike the time scales they are single-layer -- the same
// way substr/strpos/subinstr already are. The parsing rules live in
// piet/eval/stata_datemask.h and the numbers they build come from the shared
// cecon:: calendar, so `date("2020-03-15","YMD")` and `mdy(3,15,2020)` agree.

PyValue translateFn(const std::vector<PyValue>& args, datemask::Kind kind,
                    const char* name) {
  requireArity(args, 2, 3, name);
  const std::string s = pythonStr(args[0]);
  const std::string mask = pythonStr(args[1]);
  bool have_top = false;
  double top = 0.0;
  if (args.size() == 3) {
    auto t = pythonFloat(args[2]);
    if (!t || std::isnan(*t)) return PyValue::floating(kNaN);
    have_top = true;
    top = *t;
  }
  return PyValue::floating(datemask::translate(s, mask, kind, have_top, top));
}

PyValue fnDate(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Daily, "date"); }
PyValue fnDaily(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Daily, "daily"); }
PyValue fnClock(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Clock, "clock"); }
PyValue fnClockBig(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::ClockLeap, "Clock"); }
PyValue fnMonthly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Monthly, "monthly"); }
PyValue fnQuarterly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Quarterly, "quarterly"); }
PyValue fnWeekly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Weekly, "weekly"); }
PyValue fnHalfyearly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Halfyearly, "halfyearly"); }
PyValue fnYearly(const std::vector<PyValue>& a) { return translateFn(a, datemask::Kind::Yearly, "yearly"); }

// The str*trim family is NOT an alias of this file's existing ltrim/rtrim/trim.
// Those inherit Python's str.strip(), which eats tabs; Stata's blank is a SPACE
// and nothing else (measured: strltrim("\t x") is unchanged). Landing the Stata
// spellings on the Stata rule and leaving the Python-derived spellings alone is
// deliberate -- the older names are pinned by the frozen 1.16.0 corpus and
// changing them is its own adjudication (filed, not fixed here).
// Business calendars. bofd("cal", d) maps a daily date onto a business-day
// index and dofb inverts it; both need a .stbcal calendar file, and PIET has no
// calendar loader yet (that is workstream B2). Registering them to answer
// MISSING is not a stub -- it is what Stata itself answers for a calendar it
// cannot find: `di bofd("nosuchcal", td(15mar2020))` is `.` with rc 0, measured.
// This machine's StataBE ships exactly one calendar (ado/base/s/simple.stbcal),
// so paper 066's "sp500" is missing in real Stata here too unless the
// replication package brings its own. The alternative -- an honest error --
// would refuse a line Stata accepts, which is the same gate-1 violation the
// plan's prose prescribed for bad date masks.
PyValue fnBofd(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "bofd");
  // A STRING in the date slot is Stata's r(109) type mismatch (measured: a
  // string date and swapped arguments both answer 109). The numeric-constant
  // half of the same rule is caught earlier, by macros.cpp's
  // stringFnArgSpecs table (BUG-456's landing).
  if (args[1].isStr()) throw PyExc("r(109): type mismatch");
  std::shared_ptr<const bcal::BusinessCalendar> cal = bcal::lookup(pythonStr(args[0]));
  if (!cal) return PyValue::floating(kNaN);  // unknown calendar -> missing, rc 0
  auto d = pythonFloat(args[1]);
  if (!d) return PyValue::floating(kNaN);
  return PyValue::floating(bcal::bofd(*cal, *d));
}
PyValue fnDofb(const std::vector<PyValue>& args) {
  requireArity(args, 2, 2, "dofb");
  if (args[0].isStr()) throw PyExc("r(109): type mismatch");
  std::shared_ptr<const bcal::BusinessCalendar> cal = bcal::lookup(pythonStr(args[1]));
  if (!cal) return PyValue::floating(kNaN);
  auto i = pythonFloat(args[0]);
  if (!i) return PyValue::floating(kNaN);
  return PyValue::floating(bcal::dofb(*cal, *i));
}

PyValue fnStritrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "stritrim");
  return PyValue::str(strfns::stritrim(pythonStr(args[0])));
}
PyValue fnStrltrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strltrim");
  return PyValue::str(strfns::strltrim(pythonStr(args[0])));
}
PyValue fnStrrtrim(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "strrtrim");
  return PyValue::str(strfns::strrtrim(pythonStr(args[0])));
}
PyValue fnUstrfix(const std::vector<PyValue>& args) {
  requireArity(args, 1, 2, "ustrfix");
  const std::string s = pythonStr(args[0]);
  if (args.size() == 2) return PyValue::str(strfns::ustrfix(s, pythonStr(args[1]), true));
  return PyValue::str(strfns::ustrfix(s, "", false));
}
PyValue fnSoundex(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "soundex");
  return PyValue::str(strfns::soundex(pythonStr(args[0])));
}
PyValue fnSoundexNara(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "soundex_nara");
  return PyValue::str(strfns::soundex_nara(pythonStr(args[0])));
}

PyValue fnInt(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "int");
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  return PyValue::floating(std::isnan(*v) ? kNaN : std::trunc(*v));
}

// BUG-471 T2: Stata's float(x) — x rounded to float precision, as a double
// (w2e_float_default.log fn_float_eq_count :1784: `count if g_u13 ==
// float(1/3)` is 5). Missing in -> missing out, the payload kept (`.a` stays
// `.a`: fn_float_missing :1818, fn_float_sysmiss :1828). A finite value past
// float's range is system missing (fn_float_1e300 :1839), the bound being
// Stata's largest float 0x1.fffffep+126 — the engine's round_to_store rule
// (src/data/frame_internal.h), restated because that header is engine-only.
PyValue fnFloat(const std::vector<PyValue>& args) {
  requireArity(args, 1, 1, "float");
  auto v = pythonFloat(args[0]);
  if (!v) return PyValue::floating(kNaN);
  const double x = *v;
  // Signed, as in fnMissing: only x >= the sentinel is a Stata missing. A
  // finite negative past float's range (float(-1e308)) falls through to `.`.
  if (std::isnan(x) || x >= kStataMissingSentinel) return PyValue::floating(x);
  // The overflow answer is this layer's own `.` (the sentinel a bare `.`
  // becomes), not a NaN: a NaN prints `.` but compares false, so
  // `float(1e300) >= .` answered 0 where Stata answers 1.
  if (std::fabs(x) >= 0x1p+127) return PyValue::floating(kStataMissingSentinel);
  const double f = static_cast<double>(static_cast<float>(x));
  return PyValue::floating(std::fabs(f) > 0x1.fffffep+126 ? kStataMissingSentinel : f);
}

// ---------------------------------------------------------------------------
// Registry

void addStataFns(PyNamespace& ns) {
  ns.fns["inlist"] = fnInlist;
  ns.fns["inrange"] = fnInrange;
  ns.fns["cond"] = fnCond;
  ns.fns["mod"] = fnMod;
  ns.fns["missing"] = fnMissing;
  ns.fns["lower"] = fnLower;
  ns.fns["upper"] = fnUpper;
  ns.fns["strtrim"] = fnStrtrim;
  ns.fns["trim"] = fnStrtrim;  // Stata alias: trim() == strtrim()
  ns.fns["ltrim"] = fnLtrim;
  ns.fns["rtrim"] = fnRtrim;
  ns.fns["substr"] = fnSubstr;
  ns.fns["strlen"] = fnStrlen;
  ns.fns["length"] = fnStrlen;  // length() of a string == strlen()
  ns.fns["strpos"] = fnStrpos;
  ns.fns["regexm"] = fnRegexm;
  ns.fns["regexr"] = fnRegexr;
  ns.fns["real"] = fnReal;
  ns.fns["word"] = fnWord;
  ns.fns["subinstr"] = fnSubinstr;
  ns.fns["proper"] = fnProper;
  ns.fns["string"] = fnString;
  ns.fns["char"] = fnChar;
  ns.fns["abbrev"] = fnAbbrev;
  // BUG-371 date family — see the implementations above.
  ns.fns["mdy"] = fnMdy;
  ns.fns["year"] = fnYear;
  ns.fns["month"] = fnMonth;
  ns.fns["day"] = fnDay;
  ns.fns["dow"] = fnDow;
  ns.fns["doy"] = fnDoy;
  ns.fns["quarter"] = fnQuarter;
  ns.fns["halfyear"] = fnHalfyear;
  // BUG-431 repair: `mi` reached cobra's FNS[] as a synonym for missing() and
  // never reached this layer, so `display mi(1)` STOPPED a do-file with
  // "NameError: name 'mi' is not defined" while `gen m = mi(x)` succeeded. Same
  // function pointer, not a second implementation -- the synonym rule this file
  // already applies to trim/length.
  ns.fns["mi"] = fnMissing;
  ns.fns["int"] = fnInt;
  ns.fns["float"] = fnFloat;  // BUG-471 T2
  // The engine-only seven (see engineFin above).
  ns.fns["ln"] = fnLn;
  ns.fns["sin"] = fnSin;
  ns.fns["cos"] = fnCos;
  ns.fns["tan"] = fnTan;
  ns.fns["asinh"] = fnAsinh;
  ns.fns["ln1p"] = fnLn1p;
  ns.fns["ln1m"] = fnLn1m;
  // B3(a): string-argument date translation.
  ns.fns["date"] = fnDate;
  ns.fns["daily"] = fnDaily;
  ns.fns["clock"] = fnClock;
  ns.fns["Clock"] = fnClockBig;
  ns.fns["monthly"] = fnMonthly;
  ns.fns["quarterly"] = fnQuarterly;
  ns.fns["weekly"] = fnWeekly;
  ns.fns["halfyearly"] = fnHalfyearly;
  ns.fns["yearly"] = fnYearly;
  // B3(a): the string family. strlower/strupper ARE the existing ASCII-only
  // lower/upper -- Stata's are ASCII-only too (strlower("ÀÉÎ") is unchanged),
  // so what this file documented as a parity caveat turns out to be parity.
  ns.fns["strlower"] = fnLower;
  ns.fns["strupper"] = fnUpper;
  ns.fns["stritrim"] = fnStritrim;
  ns.fns["strltrim"] = fnStrltrim;
  ns.fns["strrtrim"] = fnStrrtrim;
  ns.fns["ustrfix"] = fnUstrfix;
  ns.fns["soundex"] = fnSoundex;
  ns.fns["soundex_nara"] = fnSoundexNara;
  // Business calendars: registered, and missing until B2 lands a .stbcal loader.
  ns.fns["bofd"] = fnBofd;
  ns.fns["dofb"] = fnDofb;
  // B3 time scales.
  ns.fns["mofd"] = fnMofd;
  ns.fns["qofd"] = fnQofd;
  ns.fns["hofd"] = fnHofd;
  ns.fns["yofd"] = fnYofd;
  ns.fns["wofd"] = fnWofd;
  ns.fns["week"] = fnWeek;
  ns.fns["dofm"] = fnDofm;
  ns.fns["dofq"] = fnDofq;
  ns.fns["dofh"] = fnDofh;
  ns.fns["dofy"] = fnDofy;
  ns.fns["dofw"] = fnDofw;
  ns.fns["cofd"] = fnCofd;
  ns.fns["Cofd"] = fnCofdBig;
  ns.fns["dofc"] = fnDofc;
  ns.fns["dofC"] = fnDofcBig;
  ns.fns["hh"] = fnHh;
  ns.fns["mm"] = fnMm;
  ns.fns["ss"] = fnSs;
  ns.fns["hhC"] = fnHhC;
  ns.fns["mmC"] = fnMmC;
  ns.fns["ssC"] = fnSsC;
  ns.fns["msofseconds"] = fnMsofseconds;
  ns.fns["msofminutes"] = fnMsofminutes;
  ns.fns["msofhours"] = fnMsofhours;
  ns.fns["seconds"] = fnSeconds;
  ns.fns["minutes"] = fnMinutes;
  ns.fns["hours"] = fnHours;
  ns.fns["ym"] = fnYm;
  ns.fns["yq"] = fnYq;
  ns.fns["yh"] = fnYh;
  ns.fns["yw"] = fnYw;
  ns.fns["hms"] = fnHms;
  ns.fns["dhms"] = fnDhms;
  // BUG-660: piet-macro-layer-functions — the macro/arith-layer function
  // arms of w2c_functions.log. round/min/max OVERRIDE the entries
  // mathNamespace() already installed (see fnRound/fnMinMaxStata's own
  // comments for why the Python-semantics originals are wrong for Stata
  // callers); everything else here is a new name.
  //
  // sum()/group()/runiform()/runiformint() are DELIBERATELY NOT here — see
  // addStataMacroOnlyFns() below, which registers them instead, into the two
  // NO-ROW-PLANE registries (stataMathNamespace()/postfileStataMathNamespace())
  // only. addStataFns() is ALSO strmutate.cpp's baseNamespace() -- the
  // ROW-plane string generate/replace/count-if evaluator, which walks actual
  // data rows -- and sum()/group() answering their macro-layer, single-
  // evaluation shape there would be a silent wrong answer for `generate str
  // t = string(sum(x))` (fnSum's own comment: "outside of one a single
  // evaluation just answers x itself" — inside a row plane, Stata wants a
  // running total, which this file does not implement). runiform()/
  // runiformint() move for the same reason: a per-evaluation draw reaching a
  // row-plane `generate` would not be the engine's seeded per-row stream.
  // The registry entries themselves refuse by name (r(199)); a session-bound
  // drawing version is installed per call by evalArith/evalMacroExpr (see
  // addSessionRngFns). loops.cpp's `while`/`if` condition namespace reaches
  // only the refusing registry entries.
  ns.fns["round"] = fnRound;
  ns.fns["min"] = fnMinStata;
  ns.fns["max"] = fnMaxStata;
  ns.fns["sign"] = fnSign;
  ns.fns["ttail"] = fnTtail;
  ns.fns["invttail"] = fnInvttail;
  ns.fns["invt"] = fnInvt;
  ns.fns["normal"] = fnNormal;
  ns.fns["invnormal"] = fnInvnormal;
  ns.fns["reverse"] = fnReverse;
  ns.fns["strrpos"] = fnStrrpos;
  ns.fns["wordcount"] = fnWordcount;
  ns.fns["strtoname"] = fnStrtoname;
  ns.fns["strmatch"] = fnStrmatch;
  ns.fns["ustrunescape"] = fnUstrunescape;
  ns.fns["ustrregexra"] = fnUstrregexra;
  // strproper()/proper() and strofreal()/string() and usubinstr()/subinstr()
  // are the same functions under Stata's newer names (verified against the
  // oracle: strproper_macro's word-boundary title-casing is exactly
  // fnProper's algorithm; strofreal_macro's "%5.2f" rendering is exactly
  // fnString's 2-arg format path; usubinstr_macro's single-occurrence
  // replace-all is exactly fnSubinstr's `.`-count fallthrough) — same
  // function pointer, not a second implementation, the mi/trim/length
  // pattern already used above.
  ns.fns["strproper"] = fnProper;
  ns.fns["strofreal"] = fnString;
  ns.fns["usubinstr"] = fnSubinstr;
  // The old comment here said 36 while the map held 32; registry.cpp:1702 records
  // that discrepancy as the reason --dump-functions reads the MAPS. Corrected to
  // the counted truth and kept counted: 74 = 32 pre-existing + mi + int +
  // 7 engine-only math + 33 time functions + 17 B3(a) string/translation
  // + 2 business-calendar. BUG-660 adds 18 here (17 new names + 3 overrides
  // of mathNamespace()'s round/min/max, less sum/group/runiform/runiformint
  // and regexs, moved to addStataMacroOnlyFns) — 93 was already stale before
  // this (registry.cpp's --dump-functions is the live count, not this literal).
  dprof::noteNsBuild(111);
}

// BUG-660 fix round: the macro/arith-ONLY functions split out of
// addStataFns() above — never installed into a namespace a row plane can
// reach (see that function's own comment for the failure mode this avoids).
// Called by stataMathNamespace() below and macros.cpp's
// postfileStataMathNamespace() -- the two no-row-plane registries -- not by
// strmutate.cpp's baseNamespace() (the row-plane one, unedited by this fix,
// out of manifest).
//
// regexs() lives here, not in addStataFns(): review evidence (fix round 2)
// showed the oracle's row-plane regexs arms (regexs_whole, regexs_group_matched
// in a `generate cond(regexm(...), regexs(...), ...)`) give an empty row 1
// because Stata's real row-plane regexm/regexs pairing has an evaluation-order
// dependency this per-call, non-row-aware ns.fns entry cannot reproduce --
// strmutate.cpp's row iteration (out of manifest) would have to carry that
// state per row. Registering regexs() in the row plane at all made every row
// silently answer with the CURRENT row's match instead, which is a silent
// wrong answer, not the honest r(133) "unknown function" the row plane gave
// before this fix round. macro-layer regexs (regexs_macro_standalone) has no
// such row-plane ordering to get wrong -- it is a plain sequential call in
// the same evaluator as its regexm() -- so it stays available here.
void addStataMacroOnlyFns(PyNamespace& ns) {
  ns.fns["sum"] = fnSum;
  ns.fns["group"] = fnGroup;
  ns.fns["runiform"] = fnRuniformMacro;
  ns.fns["runiformint"] = fnRuniformintMacro;
  ns.fns["regexs"] = fnRegexs;
}

// EXPRLAYER-1c: mathNamespace(false) + addStataFns(), built once per process.
// Callers that used to rebuild it per expression now point a per-call
// PyNamespace's `base` here (see PyNamespace's header comment for why the
// shadowing is unchanged).
const PyNamespace& stataMathNamespace() {
  static const PyNamespace kNs = [] {
    PyNamespace n = mathNamespace(/*withInfNan=*/false);
    addStataFns(n);
    addStataMacroOnlyFns(n);  // BUG-660 fix round: sum/group/runiform(int) here only
    return n;
  }();
  return kNs;
}

void addScalarFn(PyNamespace& ns, ScalarLookupFn lookup) {
  ns.fns["scalar"] = [lookup](const std::vector<PyValue>& args) -> PyValue {
    requireArity(args, 1, 1, "scalar");
    std::string key = pythonStr(args[0]);
    auto val = lookup(key);
    if (!val) return PyValue::floating(kNaN);
    auto f = pythonFloat(*val);
    return PyValue::floating(f ? *f : kNaN);
  };
}

}  // namespace piet::eval
