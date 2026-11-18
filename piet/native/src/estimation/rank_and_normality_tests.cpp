// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// spearman, sktest and signrank: closed-form rank and normality tests.
//
//   E2  closed-form statistics computed HERE, with no engine call at all:
//       spearman, sktest, signrank, centile. Each reads its columns through
//       the section-38 projection and does its own arithmetic on top of
//       core/distributions.h.
//
// The E2 four are why this bundle was specified before it was written. Their
// formulas were checked against the StataNow 19.5 manuals and independently
// recomputed BEFORE implementation, because H's tests would otherwise have been
// written from the same transcription as its code -- a transcription error
// would have been PINNED as the answer instead of caught. Six of the eight
// specs came back wrong. Every correction that survived that review is carried
// at its own site below, with the measurement that forced it, so that a later
// reader cannot quietly "simplify" it back. The record is
// internal note ERRATA-AER-H-SPECS-20260904.md.

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// Option vocabularies, registered so the dispatcher canonicalizes an
// abbreviation (`sktest x, noa`, `spearman a b c, mat`) to the name these
// handlers read; an unregistered net accepted the abbreviation and then missed
// it in its raw-key comparison, silently running the default.
namespace {
const eval::OptVocab& kVocabSktest = eval::registerOptVocab({"sktest"}, {{"noadjust"}, {}});
const eval::OptVocab& kVocabSpearman = eval::registerOptVocab(
    {"spearman"}, {{"stats", "print", "star", "matrix", "pw", "bonferroni", "sidak"}, {"exact"}});
}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Shared arithmetic

// Average ("mid") ranks, the rule `egen rank` already uses (frame.cpp:1805):
// tied observations all receive the mean of the positions they occupy.
std::vector<double> midranks(const std::vector<double>& v) {
  const size_t n = v.size();
  std::vector<size_t> idx(n);
  for (size_t i = 0; i < n; ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&v](size_t a, size_t b) { return v[a] < v[b]; });
  std::vector<double> r(n, 0.0);
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
    const double m = (static_cast<double>(i + 1) + static_cast<double>(j + 1)) / 2.0;
    for (size_t k = i; k <= j; ++k) r[idx[k]] = m;
    i = j + 1;
  }
  return r;
}

// Standard normal CDF via erfc. Used ONLY in the complement direction -- see
// each call site for why `1 - Phi(+x)` is banned here.
double normalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// Phi^-1(p) for p in (0,1). core/distributions.h's z_critical is the TWO-SIDED
// critical value -- it computes Phi^-1(1 - alpha/2), so it cannot reach a lower
// tail below 0.5 at all, and sktest's Royston block needs exactly that
// (Zc = -Phi^-1(exp(-K^2/2)), and exp(-K^2/2) < 0.5 as soon as K^2 > 1.386).
// Same Acklam rational approximation plus the same single Halley refinement, so
// the two agree wherever their domains overlap; kept local because widening
// z_critical's contract would touch every cobra caller it already has.
double invNormal(double p) {
  if (!(p > 0.0) || !(p < 1.0)) return std::numeric_limits<double>::quiet_NaN();
  static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                             -2.759285104469687e+02, 1.383577518672690e+02,
                             -3.066479806614716e+01, 2.506628277459239e+00};
  static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                             -1.556989798598866e+02, 6.680131188771972e+01,
                             -1.328068155288572e+01};
  static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                             -2.400758277161838e+00, -2.549732539343734e+00,
                             4.374664141464968e+00, 2.938163982698783e+00};
  static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                             3.754408661907416e+00};
  const double plow = 0.02425, phigh = 1 - plow;
  double q, r, x;
  if (p < plow) {
    q = std::sqrt(-2 * std::log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  } else if (p <= phigh) {
    q = p - 0.5;
    r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
        (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
  } else {
    q = std::sqrt(-2 * std::log(1 - p));
    x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  }
  const double e = 0.5 * std::erfc(-x * 0.7071067811865476) - p;
  const double u = e * 2.5066282746310002 * std::exp(0.5 * x * x);
  return x - u / (1 + 0.5 * x * u);
}

}  // namespace

// ===========================================================================
// E2 — computed here

// spearman (StataNow 19.5): rank correlation, p from a
// four-parameter beta fit to the first four permutation moments of
// T = sum x_i y_i on centred midranks ([R] spearman, Methods and formulas).
namespace {
struct SpearmanPair {
  size_t n = 0;
  double rho = std::numeric_limits<double>::quiet_NaN();
  double p = std::numeric_limits<double>::quiet_NaN(), p_l = p, p_u = p;
  // spearman.ado's hidden/historical returns: the permutation moments' skewness
  // and kurtosis (only when Moments ran, N >= 4), the old t-based p, and which
  // distribution gave r(p).
  bool moments = false;
  double skewness = p, kurtosis = p, p_tdist = p;
  std::string dist;
};

// One pair over the rows where both are present (Spearman2: the ranks are taken
// within that sample).
SpearmanPair spearmanPair(const std::vector<double>& xs, const std::vector<double>& ys) {
  SpearmanPair out;
  std::vector<double> x, y;
  for (size_t i = 0; i < xs.size(); ++i) {
    if (std::isnan(xs[i]) || std::isnan(ys[i])) continue;
    x.push_back(xs[i]);
    y.push_back(ys[i]);
  }
  const size_t n = x.size();
  out.n = n;
  if (n < 3) return out;
  const std::vector<double> rx = midranks(x), ry = midranks(y);
  const double nn = static_cast<double>(n);

  // rho: correlate on the ranks (centred at their exact mean).
  double rho;
  {
    double mx = 0.0, my = 0.0;
    for (size_t i = 0; i < n; ++i) {
      mx += rx[i];
      my += ry[i];
    }
    mx /= nn;
    my /= nn;
    double sxx = 0.0, syy = 0.0, sxy = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double a = rx[i] - mx, b = ry[i] - my;
      sxx += a * a;
      syy += b * b;
      sxy += a * b;
    }
    if (sxx == 0.0 || syy == 0.0) {
      // A constant variable: correlate's r(rho) is missing, which spearman.ado's
      // `rho >= 1` test takes as 1 (a missing value is above every number): rho 1,
      // p_tdist 0, the moments missing (r(kurtosis) r(skewness) `.`) and the p-value
      // the normal approximation on sqrt(N-1) (vs-stata bank f761c0c5 `spearman x y`
      // with x constant on 4 rows: rho 1, p .0832645, dist normal).
      out.rho = 1.0;
      out.p_tdist = 0.0;
      if (n >= 4) {
        out.moments = true;
        out.kurtosis = out.skewness = std::numeric_limits<double>::quiet_NaN();
      }
      const double z = std::sqrt(nn - 1.0);
      out.p_l = normalCdf(z);
      out.p_u = normalCdf(-z);
      out.p = std::min(1.0, 2.0 * std::min(out.p_l, out.p_u));
      out.dist = "normal";
      return out;
    }
    rho = sxy / std::sqrt(sxx * syy);
  }
  // Spearman2's old t-distribution p (r(p_tdist)); a |rho| of 1 is clamped.
  if (rho >= 1.0) {
    rho = 1.0;
    out.p_tdist = 0.0;
  } else if (rho <= -1.0) {
    rho = -1.0;
    out.p_tdist = 0.0;
  } else {
    const double df = nn - 2.0;
    const double t = std::fabs(rho) * (std::sqrt(df) / std::sqrt(1.0 - std::pow(rho, 2.0)));
    out.p_tdist = std::min(1.0, cecon::t_twosided_pvalue(t, df));
  }

  // The p-value is spearman.ado's (Spearman2 -> Moments -> BetaFit), computed
  // in ITS order of operations: the four permutation moments of T = sum x*y come
  // from running sums over each variable centred at the INTEGER part of its mean
  // (Center: "integers better for recursive formulas"), and the uncentred offset
  // is removed only at the end (m3 - 3 m1 m2 - m1^3, ...). That subtraction is
  // where Stata's digits are decided: an algebraically equal closed form on
  // exactly centred ranks lands 1e-8..1e-6 (relative) away from the p it posts.
  double p = std::numeric_limits<double>::quiet_NaN();
  double p_l = p, p_u = p;
  bool beta = false;
  if (n >= 4) {
    struct Sums {
      double N, mean, sd2, sumx3, sumx2x, sumxxx, sumx4, sumx3x, sumx2x2, sumx2xx, sumxxxx;
    };
    // Center + Recurse for one variable: v holds the centred values.
    auto centre = [&](const std::vector<double>& r) {
      double sum = 0.0;
      for (double a : r) sum += a;
      const double mean = sum / nn;
      const double c = std::fabs(mean) < 1.0 ? mean : std::trunc(mean);
      std::vector<double> v(r.size());
      for (size_t i = 0; i < r.size(); ++i) v[i] = r[i] - c;
      return v;
    };
    auto recurse = [&](std::vector<double> v) {
      std::sort(v.begin(), v.end());
      Sums S{};
      S.N = nn;
      double sum = 0.0;
      for (double a : v) sum += a;
      S.mean = sum / nn;
      double ss = 0.0;
      for (double a : v) ss += (a - S.mean) * (a - S.mean);
      S.sd2 = ss / (nn - 1.0);
      double sx = 0, sx2 = 0, sx3 = 0, sx4 = 0;             // running sum(x^k), through k-1
      double sxx = 0, sx2x = 0, sxxx = 0, sx3x = 0, sx2x2 = 0, sx2xx = 0, sxxxx = 0;
      for (size_t k = 0; k < n; ++k) {
        const double a = v[k];
        if (k >= 1) {
          // each recurrence reads the PREVIOUS row's running sums
          const double nsxxxx = k >= 3 ? sxxxx + a * sxxx : 0.0;
          const double nsx2xx = k >= 2 ? sx2xx + a * (sx2x + a * sxx) : 0.0;
          const double nsxxx = k >= 2 ? sxxx + a * sxx : 0.0;
          const double nsx2x2 = sx2x2 + (a * a) * sx2;
          const double nsx3x = sx3x + a * (sx3 + (a * a) * sx);
          const double nsx2x = sx2x + a * (sx2 + a * sx);
          const double nsxx = sxx + a * sx;
          sxxxx = nsxxxx; sx2xx = nsx2xx; sxxx = nsxxx; sx2x2 = nsx2x2;
          sx3x = nsx3x; sx2x = nsx2x; sxx = nsxx;
        }
        sx += a;
        sx2 += a * a;
        sx3 += a * a * a;
        sx4 += (a * a) * (a * a);
      }
      S.sumx3 = sx3; S.sumx4 = sx4; S.sumx2x = sx2x; S.sumxxx = sxxx;
      S.sumx3x = sx3x; S.sumx2x2 = sx2x2; S.sumx2xx = sx2xx; S.sumxxxx = sxxxx;
      return S;
    };
    const std::vector<double> X = centre(rx), Y = centre(ry);
    double stat = 0.0;
    for (size_t i = 0; i < n; ++i) stat += X[i] * Y[i];
    const Sums a = recurse(X), b = recurse(Y);
    // Every expression below is spearman.ado's, grouped as STATA evaluates it:
    // `/` binds before `*`, and `-` before `+` ([U] 13.2.5 Order of evaluation),
    // so `6*(a)/(b)` is 6*((a)/(b)) and `36*s*t/(N-2)` is (36*s)*(t/(N-2)). The
    // beta fit's d = 1 - 24(r+1)/(...) cancels to a few ulps, and its square
    // root moves alpha and beta by ~1e-8: grouped as C groups it, r(p) is off by
    // up to 5e-7 (relative) on ordinary data; grouped as Stata does, it is
    // Stata's to the last digits (auto: mpg/price, its foreign==0 subsample,
    // and the three pairs of `spearman mpg price weight rep78`).
    const auto sq = [](double v) { return std::pow(v, 2.0); };  // Stata's x^2
    const double m1 = (nn * a.mean) * b.mean;
    const double m2 = ((nn - 1.0) * a.sd2) * b.sd2;
    const double b3 = (3.0 * a.sumx2x) * b.sumx2x + (36.0 * a.sumxxx) * (b.sumxxx / (nn - 2.0));
    const double m3 =
        ((a.sumx3 * b.sumx3 + b3 / (nn - 1.0)) / nn - (3.0 * m1) * m2) - std::pow(m1, 3.0);
    const double b4inner =
        (24.0 * a.sumx2xx) * b.sumx2xx + (576.0 * a.sumxxxx) * (b.sumxxxx / (nn - 3.0));
    const double b4 =
        ((4.0 * a.sumx3x) * b.sumx3x + (12.0 * a.sumx2x2) * b.sumx2x2) + b4inner / (nn - 2.0);
    const double m4 = ((((a.sumx4 * b.sumx4 + b4 / (nn - 1.0)) / nn) - (4.0 * m1) * m3) -
                       (6.0 * sq(m1)) * m2) -
                      std::pow(m1, 4.0);
    const double g4 = m4 / sq(m2);
    const double g3 = m3 / std::pow(std::sqrt(m2), 3.0);
    out.moments = true;
    out.kurtosis = g4;
    out.skewness = g3;

    // BetaFit: the four-parameter beta with these moments.
    const double rr = 6.0 * ((g4 - sq(g3) - 1.0) / (6.0 + (3.0 * sq(g3) - 2.0 * g4)));
    const double den = ((rr + 2.0) * (rr + 3.0)) * g4 - (3.0 * (rr - 6.0)) * (rr + 1.0);
    double d = 1.0 - 24.0 * ((rr + 1.0) / den);
    // d < 0: the symmetric beta (tie-free ranks give g3 = 0 and a d that lands a
    // rounding error either side of 0).
    if (d < 0.0) d = 0.0;
    d = std::sqrt(d);
    const double alpha = g3 > 0.0 ? (rr / 2.0) * (1.0 - d) : (rr / 2.0) * (1.0 + d);
    const double bet = g3 > 0.0 ? (rr / 2.0) * (1.0 + d) : (rr / 2.0) * (1.0 - d);
    if (alpha > 0.0 && bet > 0.0) {
      const double e = std::sqrt(m2 * ((alpha + bet + 1.0) / (alpha * bet)));
      const double lower = m1 - alpha * e, upper = m1 + bet * e;
      if (lower < upper && !std::isnan(lower) && !std::isnan(upper)) {
        const double sc = (stat - lower) / (upper - lower);
        // TWO functions, not one algebraic identity. The manual calls ibeta and
        // ibetatail separately and the difference is not cosmetic: for strongly
        // positive rho, `1 - p_l` annihilates the answer. Measured at n = 74 with
        // tie-free ranks and rho = 0.9999, the subtraction returns exactly 0 where
        // the upper tail is 6.8136e-32. Both display as 0.0000, so a check value
        // cannot see it -- but r(p_u) is a published return and a golden at
        // rtol 1e-9 diverges once the tail drops below about 1e-7.
        p_l = cecon::betai(alpha, bet, sc);
        p_u = cecon::betaitail(alpha, bet, sc);
        beta = true;
      }
    }
  }
  if (!beta) {
    // No beta fit (fewer than 4 observations, or moments it cannot match): the
    // normal approximation on sqrt(N-1) rho.
    const double z = std::sqrt(nn - 1.0) * rho;
    p_l = normalCdf(z);
    p_u = normalCdf(-z);
  }
  p = std::min(1.0, 2.0 * std::min(p_l, p_u));
  out.dist = beta ? "beta" : "normal";

  out.rho = rho;
  out.p = p;
  out.p_l = p_l;
  out.p_u = p_u;
  return out;
}
}  // namespace

// BUG-1014 (w2as_summary_stats_family sx_spearman_*): more than two variables are
// served as spearman.ado serves them -- the `matrix` form: one listwise sample,
// every pair i < j through the pair kernel, r(Rho) (1 on the diagonal), r(P)
// (missing on the diagonal), r(Nobs), and r(N) r(rho) r(p) r(p_l) r(p_u) of the
// LAST pair (`return add` after the loop; sx_spearman_three). Two variables post
// no matrix (rowsof(r(Rho)) r(109) in Stata, sx_spearman_two). A string variable
// is ignored; fewer than two variables named is r(102) (varlist(min=2),
// sx_spearman_ref_one); an option Stata does not have is r(198)
// (sx_spearman_ref_opt; it was accepted and ignored).
ojson runSpearman(const eval::ParsedCommand& parsed, SessionState& session) {
  // stats()/print()/star() choose what the table prints; matrix forces the
  // matrix form. exact, pw, bonferroni and sidak change the numbers and are not
  // served here: named refusals.
  const std::unordered_set<std::string>& kKnown = kVocabSpearman.known;
  const std::unordered_set<std::string>& kUnimpl = kVocabSpearman.unimplemented;
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimpl)) return ojson(*e);
  bool matrixForm = false, pw = false, bonferroni = false, sidak = false;
  bool showRho = false, showObs = false, showP = false;
  double printLvl = -1, starLvl = -1;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "stats" && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string w;
      while (iss >> w) {
        showRho = showRho || w == "rho";
        showObs = showObs || w == "obs";
        showP = showP || w == "p";
      }
    }
    if ((k == "print" || k == "star") && !v.isBool) {
      char* end = nullptr;
      const double d = std::strtod(strutil::trim(v.strValue).c_str(), &end);
      (k == "print" ? printLvl : starLvl) = d;
    }
    if (k == "matrix") matrixForm = true;
    if (k == "pw") pw = true;
    if (k == "bonferroni") bonferroni = true;
    if (k == "sidak") sidak = true;
    // print() and star() are `numlist min=1 max=1 >0 <1` (spearman's syntax):
    // a word that is no number is r(121) "star() invalid -- invalid number"
    // (vs-stata bank 23597affda830e60 `st(rho obs p)`; `st(.01)` runs).
    if ((k == "star" || k == "print") && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string w;
      size_t n = 0;
      auto bad = [&](const std::string& why, const char* rc) {
        return ojson(eval::errorBlock("syntax_error", k + "() invalid -- " + why, std::nullopt,
                                      std::nullopt, std::nullopt, std::string(rc)));
      };
      while (iss >> w) {
        char* end = nullptr;
        const double d = std::strtod(w.c_str(), &end);
        if (end == nullptr || *end != '\0') return bad("invalid number", "r(121)");
        if (!(d > 0 && d < 1)) return bad("invalid numlist has elements outside of allowed range", "r(125)");
        ++n;
      }
      if (n == 0) return bad("invalid numlist has too few elements", "r(122)");
      if (n > 1) return bad("invalid numlist has too many elements", "r(123)");
    }
    if (k == "stats" && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string w;
      while (iss >> w)
        if (w != "rho" && w != "obs" && w != "p")
          return ojson(eval::errorBlock("syntax_error",
                                        "statistic " + w + " not allowed in option stats()",
                                        std::nullopt, std::nullopt, std::nullopt,
                                        std::string("r(198)")));
    }
  }
  if (bonferroni && sidak)
    return ojson(eval::errorBlock("syntax_error", "options bonferroni and sidak may not be combined",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  if (parsed.varlist.size() < 2) {
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(102)")));
  }
  ColumnScope scope(session, parsed, parsed.varlist, "spearman");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  std::vector<std::string> vars;
  for (const auto& v : parsed.varlist) {
    if (!ds.has(v)) return aerError("spearman: variable " + v + " not found");
    if (ds.dtype(v) == "factor") continue;  // "note: v ignored because string variable."
    vars.push_back(v);
  }
  const size_t k = vars.size();
  if (k < 2) {
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(102)")));
  }
  std::vector<std::vector<double>> cols(k);
  for (size_t j = 0; j < k; ++j) {
    std::string err;
    if (!numericColumn(ds, vars[j], &cols[j], &err)) return aerError("spearman: " + err);
  }
  if (k > 2) matrixForm = true;
  // pw (more than two variables): each pair over its own complete rows, ranked
  // there (spearman.ado's PairwiseRanks); otherwise one listwise sample.
  if (matrixForm && !(pw && k > 2)) {
    // marksample: the listwise sample over every variable.
    const size_t nr = cols[0].size();
    for (size_t i = 0; i < nr; ++i) {
      bool miss = false;
      for (size_t j = 0; j < k; ++j) miss = miss || std::isnan(cols[j][i]);
      if (miss)
        for (size_t j = 0; j < k; ++j) cols[j][i] = std::numeric_limits<double>::quiet_NaN();
    }
  }
  const double nanv = std::numeric_limits<double>::quiet_NaN();
  eval::EvalMatrix Rho, P, Nobs;
  for (auto* m : {&Rho, &P, &Nobs}) {
    m->rownames = vars;
    m->colnames = vars;
    m->data.assign(k, std::vector<double>(k, nanv));
  }
  SpearmanPair last;
  for (size_t i = 0; i < k; ++i) {
    size_t cnt = 0;
    for (double v : cols[i]) cnt += std::isnan(v) ? 0 : 1;
    Nobs.data[i][i] = static_cast<double>(cnt);
    Rho.data[i][i] = 1.0;
  }
  for (size_t i = 0; i + 1 < k; ++i) {
    for (size_t j = i + 1; j < k; ++j) {
      last = spearmanPair(cols[i], cols[j]);
      // bonferroni / sidak: each of p, p_l, p_u and p_tdist adjusted for the
      // k(k-1)/2 pairs, min(1, nrho*x) / min(1, 1-(1-x)^nrho) (spearman.ado's
      // Bonferroni and Sidak; vs-stata bank 13b0c8df `spearman mpg price weight
      // rep78, stats(rho p) b`). A pair spearman.ado computed nothing for is left.
      if ((bonferroni || sidak) && !last.dist.empty()) {
        const double nrho = static_cast<double>(k) * (static_cast<double>(k) - 1.0) / 2.0;
        for (double* x : {&last.p, &last.p_l, &last.p_u, &last.p_tdist}) {
          const double a = bonferroni ? nrho * *x : 1.0 - std::pow(1.0 - *x, nrho);
          *x = std::isnan(a) ? 1.0 : std::min(1.0, a);
        }
      }
      Nobs.data[i][j] = Nobs.data[j][i] = static_cast<double>(last.n);
      Rho.data[i][j] = Rho.data[j][i] = last.rho;
      P.data[i][j] = P.data[j][i] = last.p;
    }
  }
  if (!matrixForm) {
    if (last.n == 0) {
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
    }
    if (last.n == 1) {
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    }
    if (last.n < 3) return aerError("spearman: too few observations", "engine_error");
    if (std::isnan(last.rho))
      return aerError("spearman: a variable is constant over the sample", "engine_error");
  }

  eval::replaceRClass(session.eval);
  std::vector<std::string> posted = {"N", "rho", "p", "p_l", "p_u"};
  // The hidden and historical returns spearman.ado leaves (listed by `:
  // r(scalars)` / `: r(macros)` and readable by name): r(kurtosis) r(skewness)
  // when the moments were taken, r(p_tdist), r(dist).
  if (last.n >= 3 && !last.dist.empty()) {
    if (last.moments) {
      session.eval.rValues.set("kurtosis", last.kurtosis);
      session.eval.rValues.set("skewness", last.skewness);
      session.eval.markRHidden("kurtosis");
      session.eval.markRHidden("skewness");
      posted.insert(posted.end(), {"kurtosis", "skewness"});
    }
    session.eval.rValues.set("p_tdist", last.p_tdist);
    session.eval.rStrings.set("dist", last.dist);
    // Hidden: `return list` shows N rho p p_l p_u only (vs-stata bank 07617806
    // `spearman mpg price weight rep78, stats(rho p) bo`).
    session.eval.markRHidden("p_tdist");
    session.eval.markRMacroHidden("dist");
    posted.insert(posted.end(), {"p_tdist", "dist"});
  }
  session.eval.rValues.set("N", static_cast<double>(last.n));
  session.eval.rValues.set("rho", last.rho);
  session.eval.rValues.set("p", last.p);
  session.eval.rValues.set("p_l", last.p_l);
  session.eval.rValues.set("p_u", last.p_u);
  if (matrixForm) {
    session.eval.rMatrices.emplace_back("P", P);
    session.eval.rMatrices.emplace_back("Rho", Rho);
    session.eval.rMatrices.emplace_back("Nobs", Nobs);
    posted.insert(posted.end(), {"P", "Rho", "Nobs"});
  }
  eval::declareRPosted(session.eval, "spearman", std::move(posted));

  char buf[256];
  std::ostringstream out;
  if (!matrixForm) {
    std::snprintf(buf, sizeof(buf), "Number of observations = %6d", static_cast<int>(last.n));
    out << "\n" << buf << "\n";
    std::snprintf(buf, sizeof(buf), "        Spearman's rho = %.4f", last.rho);
    out << buf << "\n\n";
    out << "Test of H0: " << vars[0] << " and " << vars[1] << " are independent\n";
    std::snprintf(buf, sizeof(buf), "                  Prob = %.4f", last.p);
    out << buf << "\n";
  } else {
    // spearman.ado's Display_Table: the observation counts (one number, or the
    // min / avg / max of r(Nobs)), the key when obs or p is shown, then per variable
    // a row for each statistic in stats() (rho by default): a pair whose p-value is
    // above print() is blank, a rho whose p-value is at or below star() starred.
    if (!showRho && !showObs && !showP) showRho = true;
    double mn = nanv, mx = nanv, sum = 0;
    for (const auto& row : Nobs.data)
      for (double v : row) {
        if (std::isnan(mn) || v < mn) mn = v;
        if (std::isnan(mx) || v > mx) mx = v;
        sum += v;
      }
    const auto fc = [](double v) {  // %11.0fc, trimmed
      char b[64];
      std::snprintf(b, sizeof b, "%.0f", std::round(v));
      std::string d = b, outS;
      const bool neg = !d.empty() && d[0] == '-';
      if (neg) d = d.substr(1);
      for (size_t i2 = 0; i2 < d.size(); ++i2) {
        if (i2 > 0 && (d.size() - i2) % 3 == 0) outS += ',';
        outS += d[i2];
      }
      return (neg ? "-" : "") + outS;
    };
    if (mn == mx) {
      out << "\nNumber of observations = " << fc(mn) << "\n";
    } else {
      const std::string a = fc(mn), b = fc(sum / static_cast<double>(k * k)), c = fc(mx);
      const size_t w = std::max({a.size(), b.size(), c.size()});
      const auto pad = [w](const std::string& t) { return std::string(w - t.size(), ' ') + t; };
      out << "\nNumber of observations:\n";
      out << "                      min = " << pad(a) << "\n";
      out << "                      avg = " << pad(b) << "\n";
      out << "                      max = " << pad(c) << "\n";
    }
    if (showObs || showP) {
      out << "\n+-----------------+\n|  Key            |\n|-----------------|\n";
      if (showRho) out << "|   rho           |\n";
      if (showObs) out << "|   Number of obs |\n";
      if (showP) out << "|   p-value       |\n";
      out << "+-----------------+\n";
    }
    out << "\n";
    std::snprintf(buf, sizeof(buf), "%12s |", "");
    out << buf;
    for (size_t j = 0; j < k; ++j) {
      std::snprintf(buf, sizeof(buf), " %s", fitLeft(abbrevCp(vars[j], 8), 8).c_str());
      out << buf;
    }
    out << "\n-------------+" << std::string(9 * k, '-') << "\n";
    const auto shown = [&](size_t i, size_t j) {
      const double pv = P.data[i][j];
      return printLvl == -1 || i == j || (!std::isnan(pv) && pv <= printLvl);
    };
    const auto num = [&](const char* fmt, double v) {
      if (std::isnan(v)) return std::string("      .");
      std::snprintf(buf, sizeof(buf), fmt, v);
      return std::string(buf);
    };
    const std::string cont = std::string(13, ' ') + "| ";
    for (size_t i = 0; i < k; ++i) {
      std::snprintf(buf, sizeof(buf), "%s | ", fitLeft(abbrevCp(vars[i], 12), 12).c_str());
      out << buf;
      if (showRho) {
        for (size_t j = 0; j <= i; ++j) {
          if (!shown(i, j)) { out << std::string(9, ' '); continue; }
          const double pv = P.data[i][j];
          const bool star = i != j && !std::isnan(pv) && pv <= starLvl;
          out << " " << num("%7.4f", Rho.data[i][j]) << (star ? "*" : " ");
        }
        out << "\n";
      }
      if (showObs) {
        if (showRho) out << cont;
        for (size_t j = 0; j <= i; ++j) {
          if (!shown(i, j)) { out << std::string(9, ' '); continue; }
          std::snprintf(buf, sizeof(buf), " %7.0f ", Nobs.data[i][j]);
          out << buf;
        }
        out << "\n";
      }
      if (showP) {
        if (showRho || showObs) out << cont;
        for (size_t j = 0; j <= i; ++j) {
          if (!shown(i, j)) { out << std::string(9, ' '); continue; }
          out << " " << num("%7.4f", P.data[i][j]) << " ";
        }
        out << "\n";
      }
      if (int(showRho) + int(showObs) + int(showP) > 1) out << std::string(13, ' ') << "|\n";
    }
  }
  return displayBlock(out.str());
}

// sktest: D'Agostino-Belanger-D'Agostino (1990) skewness/kurtosis tests with
// Royston's (1991c) adjustment ([R] sktest, Methods and formulas).
ojson runSktest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty()) return aerError("sktest: no variables specified", "syntax_error");
  const std::unordered_set<std::string>& kKnown = kVocabSktest.known;
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown)) return ojson(*e);
  bool noadjust = false;
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "noadjust") noadjust = true;
  }

  // [aweight=] / [fweight=]: sktest.ado's `summarize ..., detail` with the weights
  // (vs-stata bank 841286fd aweight, 71eac487 fweight r(N) 1018): the moments are
  // weighted, N is the row count under aw and the weights' sum under fw; marksample
  // drops a row whose weight is missing or zero.
  std::string wvar;
  bool fweight = false;
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty() && parsed.weightType.has_value()) {
    const std::string t = strutil::lower(*parsed.weightType);
    if (t == "fw" || t == "fweight") fweight = true;
    else if (!(t == "aw" || t == "aweight")) return aerErrorReason("weights not allowed", 101);
    wvar = *parsed.weightVar;
  }
  std::vector<std::string> scopeCols = parsed.varlist;
  if (!wvar.empty()) scopeCols.push_back(wvar);
  ColumnScope scope(session, parsed, scopeCols, "sktest");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();
  std::vector<double> wcol;
  if (!wvar.empty()) {
    std::string werr;
    if (!numericColumn(ds, wvar, &wcol, &werr)) return aerError("sktest: " + werr);
    for (double w : wcol) {
      if (std::isnan(w)) continue;
      if (w < 0) return aerErrorReason("negative weights encountered", 402);
      if (fweight && w != std::floor(w))
        return aerErrorReason("may not use noninteger frequency weights", 401);
    }
  }

  struct Row {
    std::string name;
    double N = 0, pSkew = 0, pKurt = 0, chi2 = 0, pChi2 = 0;
    bool ok = false;
  };
  std::vector<Row> rows;

  for (const auto& vn : parsed.varlist) {
    Row row;
    row.name = vn;
    std::vector<double> raw;
    std::string err;
    // A string variable has no numeric observations: sktest.ado's `summ` finds
    // none, so its row is N 0 with every statistic missing (StataBE 19.5,
    // `sktest make`: rc 0). Its level codes are not data.
    const bool isString = ds.has(vn) && ds.dtype(vn) == "factor";
    if (!isString && !numericColumn(ds, vn, &raw, &err)) return aerError("sktest: " + err);
    std::vector<double> z, zw;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (std::isnan(raw[i])) continue;
      if (!wcol.empty()) {
        const double w = i < wcol.size() ? wcol[i] : std::numeric_limits<double>::quiet_NaN();
        if (std::isnan(w) || w == 0.0) continue;
        zw.push_back(w);
      }
      z.push_back(raw[i]);
    }
    double wsum = 0.0;
    for (double w : zw) wsum += w;
    const double nn = fweight ? wsum : static_cast<double>(z.size());
    row.N = nn;
    const double nanv = std::numeric_limits<double>::quiet_NaN();
    row.pSkew = row.pKurt = row.chi2 = row.pChi2 = nanv;

    // n < 8 is NOT a refusal. Live StataBE 19.5: at n = 7, 2, 1, 0 nonmissing,
    // with zero rows selected by an if, and on a constant variable, sktest
    // returns _rc == 0, prints the row with "." for all four statistics, and
    // CONTINUES to the next variable. The manual sentence about "a minimum of
    // eight observations" is Description prose, not an emitted error -- turning
    // it into one would abort whole commands that Stata completes.
    if (nn >= 8) {
      // the (weighted) central moments; every weight 1 unweighted
      const auto wt = [&](size_t i) { return zw.empty() ? 1.0 : zw[i]; };
      const double W = zw.empty() ? static_cast<double>(z.size()) : wsum;
      double m = 0.0;
      for (size_t i = 0; i < z.size(); ++i) m += wt(i) * z[i];
      m /= W;
      double m2 = 0, m3 = 0, m4 = 0;
      for (size_t i = 0; i < z.size(); ++i) {
        const double e = z[i] - m;
        const double e2 = e * e;
        m2 += wt(i) * e2;
        m3 += wt(i) * e2 * e;
        m4 += wt(i) * e2 * e2;
      }
      m2 /= W;
      m3 /= W;
      m4 /= W;
      if (m2 > 0.0) {
        const double g1 = m3 / std::pow(m2, 1.5);
        const double b2 = m4 / (m2 * m2);

        const double Y = g1 * std::sqrt((nn + 1.0) * (nn + 3.0) / (6.0 * (nn - 2.0)));
        const double beta2 = 3.0 * (nn * nn + 27.0 * nn - 70.0) * (nn + 1.0) * (nn + 3.0) /
                             ((nn - 2.0) * (nn + 5.0) * (nn + 7.0) * (nn + 9.0));
        const double W2 = -1.0 + std::sqrt(2.0 * (beta2 - 1.0));
        const double alph = std::sqrt(2.0 / (W2 - 1.0));
        const double Z1 = (1.0 / std::sqrt(std::log(std::sqrt(W2)))) *
                          std::log(Y / alph + std::sqrt((Y / alph) * (Y / alph) + 1.0));

        const double Eb2 = 3.0 * (nn - 1.0) / (nn + 1.0);
        const double varb2 =
            24.0 * nn * (nn - 2.0) * (nn - 3.0) / ((nn + 1.0) * (nn + 1.0) * (nn + 3.0) * (nn + 5.0));
        const double Xk = (b2 - Eb2) / std::sqrt(varb2);
        const double sqb1 = 6.0 * (nn * nn - 5.0 * nn + 2.0) / ((nn + 7.0) * (nn + 9.0)) *
                            std::sqrt(6.0 * (nn + 3.0) * (nn + 5.0) / (nn * (nn - 2.0) * (nn - 3.0)));
        const double A = 6.0 + 8.0 / sqb1 * (2.0 / sqb1 + std::sqrt(1.0 + 4.0 / (sqb1 * sqb1)));
        const double Z2 = ((1.0 - 2.0 / (9.0 * A)) -
                           std::cbrt((1.0 - 2.0 / A) / (1.0 + Xk * std::sqrt(2.0 / (A - 4.0))))) /
                          std::sqrt(2.0 / (9.0 * A));

        // COMPLEMENT form, never 1 - Phi(+|Z|). The literal spelling loses the
        // tail to cancellation: measured on auto price (Z1 = 4.7544) it is 1.3e-11
        // out where 2*Phi(-|Z|) is 1.1e-14, and beyond |Z| ~ 8.3 the subtraction
        // underflows to exactly 0, making -2 ln P infinite.
        row.pSkew = 2.0 * normalCdf(-std::fabs(Z1));
        row.pKurt = 2.0 * normalCdf(-std::fabs(Z2));
        const double K2 = Z1 * Z1 + Z2 * Z2;

        if (noadjust) {
          row.chi2 = K2;
          row.pChi2 = std::exp(-K2 / 2.0);  // exact chi2(2) upper tail
        } else {
          const double lnn = std::log(nn);
          const double pk = std::exp(-K2 / 2.0);
          // The '.' outcome the manual's Technical note anticipates is a MISSING
          // Royston block, not a generic "unrepresentable": when exp(-K2/2)
          // underflows to 0, Phi^-1 is undefined and Zc/Z/P all go missing, so
          // BOTH the adjusted chi2 and its p print as ".".
          if (pk > 0.0) {
            const double Zc = -invNormal(pk);
            const double Zt = 0.55 * std::pow(nn, 0.2) - 0.21;
            const double a1 = (-5.0 + 3.46 * lnn) * std::exp(-1.37 * lnn);
            const double b1 = 1.0 + (0.854 - 0.148 * lnn) * std::exp(-0.55 * lnn);
            const double a2 = a1 - (2.13 / (1.0 - 2.37 * lnn)) * Zt;
            const double b2p = 2.13 / (1.0 - 2.37 * lnn) + b1;
            double Z;
            if (Zc < -1.0) {
              Z = Zc;
            } else if (Zc < Zt) {
              Z = a1 + b1 * Zc;
            } else {
              Z = a2 + b2p * Zc;
            }
            const double P = normalCdf(-Z);  // never 1 - Phi(Z)
            if (P > 0.0) {
              row.chi2 = -2.0 * std::log(P);
              row.pChi2 = P;
            }
          }
        }
        row.ok = true;
      }
    }
    rows.push_back(row);
  }

  // r() carries the LAST variable's values, missing included.
  const Row& last = rows.back();
  session.eval.rValues.set("N", last.N);
  session.eval.rValues.set("p_skew", last.pSkew);
  session.eval.rValues.set("p_kurt", last.pKurt);
  session.eval.rValues.set("chi2", last.chi2);
  session.eval.rValues.set("p_chi2", last.pChi2);
  // sktest.ado's `return historical` results: r(P_skew) r(P_kurt) r(P_chi2) (the
  // last variable's, as the lower-case names) and r(Utest), the per-variable
  // table without N. Listed by `: r(scalars)` / `: r(matrices)` and readable by
  // name in StataBE 19.5, so they are posted, not merely declared.
  session.eval.rValues.set("P_skew", last.pSkew);
  session.eval.rValues.set("P_kurt", last.pKurt);
  session.eval.rValues.set("P_chi2", last.pChi2);
  // Historical results are hidden: `return list` shows N p_skew p_kurt chi2
  // p_chi2 and r(table) only (vs-stata bank dc346b1e `sktest wage hours tenure`).
  for (const char* h : {"P_skew", "P_kurt", "P_chi2"}) session.eval.markRHidden(h);

  // r(table): one row per variable, in varlist order. Declared in the manual's
  // Stored results and previously unreachable in PIET -- there was no r-class
  // matrix store at all until this bundle added one.
  eval::EvalMatrix tbl;
  tbl.colnames = {"N", "p_skew", "p_kurt", "chi2", "p_chi2"};
  for (const auto& r : rows) {
    tbl.rownames.push_back(r.name);
    tbl.data.push_back({r.N, r.pSkew, r.pKurt, r.chi2, r.pChi2});
  }
  bool replaced = false;
  for (auto& kv : session.eval.rMatrices) {
    if (kv.first == "table") {
      kv.second = tbl;
      replaced = true;
      break;
    }
  }
  if (!replaced) session.eval.rMatrices.emplace_back("table", std::move(tbl));
  eval::EvalMatrix utest;
  utest.colnames = {"P_skew", "P_kurt", "chi2", "P_chi2"};
  for (const auto& r : rows) {
    utest.rownames.push_back(r.name);
    utest.data.push_back({r.pSkew, r.pKurt, r.chi2, r.pChi2});
  }
  replaced = false;
  for (auto& kv : session.eval.rMatrices) {
    if (kv.first == "Utest") {
      kv.second = utest;
      replaced = true;
      break;
    }
  }
  session.eval.rHiddenMatrices.emplace_back("Utest", utest.gen);
  if (!replaced) session.eval.rMatrices.emplace_back("Utest", std::move(utest));

  std::ostringstream out;
  char buf[256];
  // The name column is as wide as the longest name (in CHARACTERS), at least 12
  // (StataBE 19.5: a longest name of 18 characters gives an 18-wide column;
  // scratch-r150/uni-v/p5.do).
  size_t stubW = 12;
  for (const auto& r : rows) stubW = std::max(stubW, cecon::ident::codePointCount(r.name));
  out << "\nSkewness and kurtosis tests for normality\n";
  out << std::string(45 + stubW, ' ') << "----- Joint test -----\n";
  out << fitLeft("Variable", stubW) << " |       Obs   Pr(skewness)   Pr(kurtosis)   Adj chi2(2)  Prob>chi2\n";
  out << std::string(stubW + 1, '-')
      << "+-----------------------------------------------------------------\n";
  auto f4 = [&buf](double v, int w) {
    if (std::isnan(v)) {
      std::snprintf(buf, sizeof(buf), "%*s", w, ".");
    } else {
      std::snprintf(buf, sizeof(buf), "%*.4f", w, v);
    }
    return std::string(buf);
  };
  auto f2 = [&buf](double v, int w) {
    if (std::isnan(v)) {
      std::snprintf(buf, sizeof(buf), "%*s", w, ".");
    } else {
      std::snprintf(buf, sizeof(buf), "%*.2f", w, v);
    }
    return std::string(buf);
  };
  for (const auto& r : rows) {
    std::snprintf(buf, sizeof(buf), "%s |%10d   ", fitLeft(r.name, stubW).c_str(),
                  static_cast<int>(r.N));
    out << buf << f4(r.pSkew, 12) << "   " << f4(r.pKurt, 12) << "   " << f2(r.chi2, 11) << "   "
        << f4(r.pChi2, 8) << "\n";
  }
  return displayBlock(out.str());
}

// signrank: Wilcoxon matched-pairs signed-rank test ([R] signrank).
// BUG-1014 (w2as_summary_stats_family sx_signrank_*), as signrank runs:
//  - `= exp` may be a number (`signrank x = 0`, sx_signrank_zero): it was read
//    as a variable name, r(111);
//  - a string variable is r(109) (the difference is a type mismatch,
//    sx_signrank_ref_str), and an option other than exact/noexact r(198)
//    (sx_signrank_ref_opt; it was accepted and ignored);
//  - the exact p-values r(p_exact) r(p_l_exact) r(p_u_exact) are posted when
//    N <= 200 (or with `exact`, N <= 2000; r(459) above) unless `noexact`: the
//    permutation distribution of T = sum of the signed ranks over the 2^n sign
//    assignments, the ranks doubled when a tie makes them half-integers (the
//    ado's mata signrank_exact); p_exact = P(|T| >= |t|).
ojson runSignrank(const eval::ParsedCommand& parsed, SessionState& session) {
  static const std::unordered_set<std::string> kKnown = {"exact", "noexact"};
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown)) return ojson(*e);
  bool wantExact = false, noExact = false;
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "exact") wantExact = true;
    if (k == "noexact") noExact = true;
  }
  if (wantExact && noExact) {
    return ojson(eval::errorBlock("syntax_error", "options exact and noexact may not be combined",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }
  // `signrank var = exp` arrives as a varlist whose middle token is "=".
  std::string lhs, rhs;
  {
    std::string joined;
    for (const auto& t : parsed.varlist) joined += t + " ";
    const size_t eq = joined.find('=');
    if (eq == std::string::npos) {
      return aerError("signrank: syntax is `signrank varname = exp`", "syntax_error");
    }
    auto trim = [](std::string s) {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
      return s;
    };
    lhs = trim(joined.substr(0, eq));
    rhs = trim(joined.substr(eq + 1));
  }
  if (lhs.empty() || rhs.empty()) {
    return aerError("signrank: syntax is `signrank varname = exp`", "syntax_error");
  }

  // A numeric constant on the right (`= 0`); anything else names a variable.
  std::optional<double> rhsConst;
  try {
    size_t pos = 0;
    const double v = std::stod(rhs, &pos);
    if (pos == rhs.size()) rhsConst = v;
  } catch (...) {
  }
  ColumnScope scope(session, parsed,
                    rhsConst ? std::vector<std::string>{lhs} : std::vector<std::string>{lhs, rhs},
                    "signrank");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  for (const auto& v : rhsConst ? std::vector<std::string>{lhs} : std::vector<std::string>{lhs, rhs})
    if (ds.has(v) && ds.dtype(v) == "factor")
      return ojson(eval::errorBlock("syntax_error", "type mismatch", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(109)")));
  std::vector<double> a, b;
  std::string err;
  if (!numericColumn(ds, lhs, &a, &err)) return aerError("signrank: " + err);
  if (rhsConst) {
    b.assign(a.size(), *rhsConst);
  } else if (!numericColumn(ds, rhs, &b, &err)) {
    return aerError("signrank: " + err);
  }

  std::vector<double> d;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::isnan(a[i]) || std::isnan(b[i])) continue;
    d.push_back(a[i] - b[i]);
  }
  const size_t n = d.size();
  if (n == 0) {
    return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                  std::nullopt, std::string("r(2000)")));
  }
  const bool exact = (n <= 200 && !noExact) || wantExact;
  if (exact && n > 2000) {
    return ojson(eval::errorBlock("syntax_error",
                                  "number of observations must be <= 2,000 for option exact",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(459)")));
  }

  std::vector<double> absd(n);
  for (size_t i = 0; i < n; ++i) absd[i] = std::fabs(d[i]);
  const std::vector<double> r = midranks(absd);

  double sumPos = 0.0, sumNeg = 0.0, sumZero = 0.0;
  size_t nPos = 0, nNeg = 0, nZero = 0;
  for (size_t i = 0; i < n; ++i) {
    if (d[i] > 0.0) {
      sumPos += r[i];
      ++nPos;
    } else if (d[i] < 0.0) {
      sumNeg += r[i];
      ++nNeg;
    } else {
      sumZero += r[i];
      ++nZero;
    }
  }
  const double nn = static_cast<double>(n), n0 = static_cast<double>(nZero);

  // E(T+) carries the zero correction. The manual prints only n(n+1)/4, which
  // for its own fuel example gives 39 and contradicts its own printed output of
  // 38.5; the corrected form follows from the manual's E(T) = 0 together with
  // T+ = (T + sum|r_j|)/2. Transcribing the printed formula gives z = -2.012026
  // instead of -1.973, and a golden captured from that same transcription could
  // not catch it.
  const double ET = nn * (nn + 1.0) / 4.0 - n0 * (n0 + 1.0) / 4.0;
  const double varUnadj = nn * (nn + 1.0) * (2.0 * nn + 1.0) / 24.0;
  const double dZero = -n0 * (n0 + 1.0) * (2.0 * n0 + 1.0) / 24.0;
  double varAdj = 0.0;
  for (size_t i = 0; i < n; ++i)
    if (d[i] != 0.0) varAdj += r[i] * r[i];
  varAdj /= 4.0;
  const double dTies = varAdj - varUnadj - dZero;

  const double z = (varAdj > 0.0) ? (sumPos - ET) / std::sqrt(varAdj)
                                  : std::numeric_limits<double>::quiet_NaN();
  // Complement form again, and for the same reason: the literal 2(1 - Phi|z|)
  // underflows to exactly 0 for large |z|, which an absolute-tolerance
  // comparator would happily accept as correct.
  const double p_l = normalCdf(z);
  const double p_u = normalCdf(-z);
  const double p = 2.0 * std::min(p_l, p_u);

  // The exact permutation distribution (signrank_exact): integer signed ranks,
  // doubled when a tie made any rank a half-integer; each nonzero rank's sign is
  // + or - with probability 1/2.
  double pExact = std::numeric_limits<double>::quiet_NaN(), pLExact = pExact, pUExact = pExact;
  if (exact) {
    bool integral = true;
    for (size_t i = 0; i < n; ++i) integral = integral && r[i] == std::floor(r[i]);
    std::vector<long long> xs(n);
    long long total = 0, tObs = 0;
    for (size_t i = 0; i < n; ++i) {
      const long long rk = integral ? static_cast<long long>(r[i])
                                    : static_cast<long long>(std::llround(2.0 * r[i]));
      xs[i] = d[i] == 0.0 ? 0 : rk;
      total += xs[i];
      tObs += d[i] > 0.0 ? xs[i] : (d[i] < 0.0 ? -xs[i] : 0);
    }
    // prob[s + total] = P(T = s), T in [-total, total].
    std::vector<double> prob(static_cast<size_t>(2 * total + 1), 0.0), next;
    prob[static_cast<size_t>(total)] = 1.0;
    long long reach = 0;
    for (long long xv : xs) {
      if (xv == 0) continue;
      next.assign(prob.size(), 0.0);
      for (long long sv = -reach; sv <= reach; ++sv) {
        const double pr = prob[static_cast<size_t>(sv + total)];
        if (pr == 0.0) continue;
        next[static_cast<size_t>(sv + xv + total)] += 0.5 * pr;
        next[static_cast<size_t>(sv - xv + total)] += 0.5 * pr;
      }
      reach += xv;
      prob.swap(next);
    }
    double lo = 0.0, hi = 0.0, two = 0.0;
    const long long at = tObs < 0 ? -tObs : tObs;
    for (long long sv = -total; sv <= total; ++sv) {
      const double pr = prob[static_cast<size_t>(sv + total)];
      if (sv <= tObs) lo += pr;
      if (sv >= tObs) hi += pr;
      if (sv >= at || sv <= -at) two += pr;
    }
    pExact = std::min(two, 1.0);
    pLExact = std::min(lo, 1.0);
    pUExact = std::min(hi, 1.0);
  }

  eval::replaceRClass(session.eval);
  std::vector<std::string> posted = {"N", "N_pos", "N_neg", "N_tie", "sum_pos", "sum_neg",
                                     "Var_a", "z", "p", "p_l", "p_u"};
  if (exact) {
    session.eval.rValues.set("p_exact", pExact);
    session.eval.rValues.set("p_l_exact", pLExact);
    session.eval.rValues.set("p_u_exact", pUExact);
    posted.insert(posted.end(), {"p_exact", "p_l_exact", "p_u_exact"});
  }
  session.eval.rValues.set("N", nn);
  session.eval.rValues.set("N_pos", static_cast<double>(nPos));
  session.eval.rValues.set("N_neg", static_cast<double>(nNeg));
  session.eval.rValues.set("N_tie", static_cast<double>(nZero));
  session.eval.rValues.set("sum_pos", sumPos);
  session.eval.rValues.set("sum_neg", sumNeg);
  session.eval.rValues.set("Var_a", varAdj);
  session.eval.rValues.set("z", z);
  session.eval.rValues.set("p", p);
  session.eval.rValues.set("p_l", p_l);
  session.eval.rValues.set("p_u", p_u);
  eval::declareRPosted(session.eval, "signrank", std::move(posted));

  std::ostringstream out;
  char buf[256];
  const double expected = (sumPos + sumNeg + sumZero) / 2.0;
  out << "\nWilcoxon signed-rank test\n\n";
  out << "        Sign |      Obs   Sum ranks    Expected\n";
  out << "-------------+---------------------------------\n";
  std::snprintf(buf, sizeof(buf), "    Positive |%9d %11.10g %11.10g\n", static_cast<int>(nPos),
                sumPos, ET);
  out << buf;
  std::snprintf(buf, sizeof(buf), "    Negative |%9d %11.10g %11.10g\n", static_cast<int>(nNeg),
                sumNeg, ET);
  out << buf;
  std::snprintf(buf, sizeof(buf), "        Zero |%9d %11.10g %11.10g\n", static_cast<int>(nZero),
                sumZero, sumZero);
  out << buf;
  out << "-------------+---------------------------------\n";
  std::snprintf(buf, sizeof(buf), "         All |%9d %11.10g %11.10g\n", static_cast<int>(n),
                sumPos + sumNeg + sumZero, expected * 2.0 / 2.0 * 2.0 / 2.0);
  out << buf;
  std::snprintf(buf, sizeof(buf), "\nUnadjusted variance %11.2f\n", varUnadj);
  out << buf;
  std::snprintf(buf, sizeof(buf), "Adjustment for ties %11.2f\n", dTies);
  out << buf;
  std::snprintf(buf, sizeof(buf), "Adjustment for zeros %10.2f\n", dZero);
  out << buf;
  out << "                     ----------\n";
  std::snprintf(buf, sizeof(buf), "Adjusted variance   %11.2f\n", varAdj);
  out << buf;
  out << "\nH0: " << lhs << " = " << rhs << "\n";
  std::snprintf(buf, sizeof(buf), "         z = %6.3f\n", z);
  out << buf;
  std::snprintf(buf, sizeof(buf), "Prob > |z| = %.4f\n", p);
  out << buf;
  if (exact) {
    std::snprintf(buf, sizeof(buf), "Exact prob = %.4f\n", pExact);
    out << buf;
  }
  return displayBlock(out.str());
}

}  // namespace piet::handlers
