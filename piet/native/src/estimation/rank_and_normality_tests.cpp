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
  double mx = 0.0, my = 0.0;
  for (size_t i = 0; i < n; ++i) {
    mx += rx[i];
    my += ry[i];
  }
  mx /= nn;
  my /= nn;

  std::vector<double> X(n), Y(n);
  for (size_t i = 0; i < n; ++i) {
    X[i] = rx[i] - mx;
    Y[i] = ry[i] - my;
  }
  auto powsum = [&](const std::vector<double>& v, int k) {
    double s = 0.0;
    for (double a : v) s += std::pow(a, k);
    return s;
  };
  const double S2 = powsum(X, 2), S3 = powsum(X, 3), S4 = powsum(X, 4);
  const double T2 = powsum(Y, 2), T3 = powsum(Y, 3), T4 = powsum(Y, 4);
  if (S2 == 0.0 || T2 == 0.0) return out;  // a constant variable: no correlation

  double Tobs = 0.0;
  for (size_t i = 0; i < n; ++i) Tobs += X[i] * Y[i];
  const double rho = Tobs / std::sqrt(S2 * T2);

  const double ET2 = S2 * T2 / (nn - 1.0);
  const double e3x = S3 / 3.0, e3y = T3 / 3.0;
  const double ET3 = S3 * T3 / nn + 3.0 * S3 * T3 / (nn * (nn - 1.0)) +
                     36.0 * e3x * e3y / (nn * (nn - 1.0) * (nn - 2.0));
  const double C2x = (S2 * S2 - S4) / 2.0, C2y = (T2 * T2 - T4) / 2.0;
  const double Dx = S4 - S2 * S2 / 2.0, Dy = T4 - T2 * T2 / 2.0;
  const double e4x = (S2 * S2 - 2.0 * S4) / 8.0, e4y = (T2 * T2 - 2.0 * T4) / 8.0;
  const double ET4 = S4 * T4 / nn + 4.0 * S4 * T4 / (nn * (nn - 1.0)) +
                     12.0 * C2x * C2y / (nn * (nn - 1.0)) +
                     24.0 * Dx * Dy / (nn * (nn - 1.0) * (nn - 2.0)) +
                     576.0 * e4x * e4y / (nn * (nn - 1.0) * (nn - 2.0) * (nn - 3.0));

  const double v = ET2;
  const double g3 = ET3 / std::pow(v, 1.5);
  const double g4 = ET4 / (v * v);
  const double rr = 6.0 * (g4 - g3 * g3 - 1.0) / (6.0 + 3.0 * g3 * g3 - 2.0 * g4);
  const double d = 1.0 - 24.0 * (rr + 1.0) /
                             ((rr + 2.0) * (rr + 3.0) * g4 - 3.0 * (rr - 6.0) * (rr + 1.0));

  // CLAMP before the root. When both variables are tie-free the centred ranks
  // are symmetric, so S3 = T3 = 0 exactly, g3 = 0, and the true d is 0 -- but in
  // double precision it lands on the wrong side of zero about half the time.
  // Measured over n = 5..500 with tie-free ranks: d < 0 for 239 of 496 sample
  // sizes, first at n = 13 (-6.66e-16). std::sqrt of that is NaN, and the NaN
  // propagates through alpha, beta, l, u, s and p, so the command would return a
  // MISSING p-value on ordinary continuous data. At d = 0 the clamp correctly
  // yields alpha = beta = r/2, the symmetric beta.
  const double dc = (d > 0.0) ? d : 0.0;
  const double a = rr * (1.0 - std::sqrt(dc)) / 2.0;
  const double b = rr * (1.0 + std::sqrt(dc)) / 2.0;
  const double alpha = (g3 > 0.0) ? a : b;
  const double beta = (g3 > 0.0) ? b : a;

  double p = std::numeric_limits<double>::quiet_NaN();
  double p_l = p, p_u = p;
  if (alpha > 0.0 && beta > 0.0) {
    const double sc = std::sqrt(v * (alpha + beta + 1.0) / (alpha * beta));
    const double l = -alpha * sc, u = beta * sc;
    if (u > l) {
      const double s = (Tobs - l) / (u - l);
      // TWO functions, not one algebraic identity. The manual calls ibeta and
      // ibetatail separately and the difference is not cosmetic: for strongly
      // positive rho, `1 - p_l` annihilates the answer. Measured at n = 74 with
      // tie-free ranks and rho = 0.9999, the subtraction returns exactly 0 where
      // the upper tail is 6.8136e-32. Both display as 0.0000, so a check value
      // cannot see it -- but r(p_u) is a published return and a golden at
      // rtol 1e-9 diverges once the tail drops below about 1e-7.
      p_l = cecon::betai(alpha, beta, s);
      p_u = cecon::betaitail(alpha, beta, s);
      const double pm = std::min(p_l, p_u);
      p = std::min(1.0, 2.0 * pm);
    }
  }

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
  static const std::unordered_set<std::string> kKnown = {"stats", "print", "star", "matrix"};
  static const std::unordered_set<std::string> kUnimpl = {"exact", "pw", "bonferroni", "sidak"};
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimpl)) return ojson(*e);
  bool matrixForm = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "matrix") matrixForm = true;
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
  if (matrixForm) {
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
    out << "\n";
    std::snprintf(buf, sizeof(buf), "%12s |", "");
    out << buf;
    for (size_t j = 0; j < k; ++j) {
      std::snprintf(buf, sizeof(buf), " %s", fitLeft(abbrevCp(vars[j], 8), 8).c_str());
      out << buf;
    }
    out << "\n-------------+" << std::string(9 * k, '-') << "\n";
    for (size_t i = 0; i < k; ++i) {
      std::snprintf(buf, sizeof(buf), "%s |", fitLeft(abbrevCp(vars[i], 12), 12).c_str());
      out << buf;
      for (size_t j = 0; j <= i; ++j) {
        std::snprintf(buf, sizeof(buf), " %8.4f", Rho.data[i][j]);
        out << buf;
      }
      out << "\n";
    }
  }
  return displayBlock(out.str());
}

// sktest: D'Agostino-Belanger-D'Agostino (1990) skewness/kurtosis tests with
// Royston's (1991c) adjustment ([R] sktest, Methods and formulas).
ojson runSktest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty()) return aerError("sktest: no variables specified", "syntax_error");
  static const std::unordered_set<std::string> kKnown = {"noadjust"};
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown)) return ojson(*e);
  bool noadjust = false;
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "noadjust") noadjust = true;
  }

  ColumnScope scope(session, parsed, parsed.varlist, "sktest");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

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
    if (!numericColumn(ds, vn, &raw, &err)) return aerError("sktest: " + err);
    std::vector<double> z;
    for (double d : raw)
      if (!std::isnan(d)) z.push_back(d);
    const double nn = static_cast<double>(z.size());
    row.N = nn;
    const double nanv = std::numeric_limits<double>::quiet_NaN();
    row.pSkew = row.pKurt = row.chi2 = row.pChi2 = nanv;

    // n < 8 is NOT a refusal. Live StataBE 19.5: at n = 7, 2, 1, 0 nonmissing,
    // with zero rows selected by an if, and on a constant variable, sktest
    // returns _rc == 0, prints the row with "." for all four statistics, and
    // CONTINUES to the next variable. The manual sentence about "a minimum of
    // eight observations" is Description prose, not an emitted error -- turning
    // it into one would abort whole commands that Stata completes.
    if (z.size() >= 8) {
      double m = 0.0;
      for (double d : z) m += d;
      m /= nn;
      double m2 = 0, m3 = 0, m4 = 0;
      for (double d : z) {
        const double e = d - m;
        const double e2 = e * e;
        m2 += e2;
        m3 += e2 * e;
        m4 += e2 * e2;
      }
      m2 /= nn;
      m3 /= nn;
      m4 /= nn;
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
