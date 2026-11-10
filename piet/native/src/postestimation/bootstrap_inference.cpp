// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Bootstrap inference.
#include "piet/handlers/estimation.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/stata_format.h"
#include "estimation/estimation_internal.h"
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "estimation/extended_estimation_internal.h"
#include "estimation/estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

namespace extended_fit_detail {

// FU-133 (round 4) and boottest-h0-noest-served-returns: the text boottest.ado
// itself prints for a fit its estimator checks refuse, or "" for a fit it goes
// on to bootstrap. Each message is printed at rc 0, not 198: the whole of
// boottest.ado's body runs inside `cap noi { }` (ado to its close at ado,
// `end` at ado, no re-raise), so its _assert rc(198) only prints. The checks
// run in the ado's own order (ado), each keyed on the condition the ado
// tests:
//  - ado `e(prefix)` "svy": a `svy:` fit is cached with the engine's "svy"
//    block (runSvyPrefix; the same mark postestRefitMatchesFitM4 reads).
//    w2t_boottest_family.log:2491 `Doesn't work after svy` (H1).
//  - ado xtreg with e(model) other than "fe": the ado prints `{`cmd',
//    `e(model)'}` with the braces kept (no SMCL directive). runXtreg caches its
//    RE arm under cobraCmd "xtreg" (its FE arm is feols with stataCmd "xtreg",
//    and be/mle/pa never fit), so cobraCmd "xtreg" is exactly xtreg, re.
//    :1834 `Doesn't work after {xtreg, re}` (E7).
//  - ado two or more words in `fvunab e(absvars)` after reghdfe/ivreghdfe:
//    this counts the absorb() TERMS the fit recorded in lm.fe (reghdfe's --fe
//    list, estimation/linear_estimation.cpp's absorb arm in runRegress; the ivreghdfe list at the IV path's absorb
//    branch above), so an `a#b` term is one term and is served, as there. Truth:
//    p9_stata.log:223/:228/:231 (reghdfe), p5_stata.log:843/:848/:851 (ivreghdfe);
//    w2t :1276/:1282 (D4), :1361/:1367 (D5), :1715/:1721 (E5), :1893/:1899 (E8).
//  - ado `ivregress` with e(estimator) "gmm", or ivreg2 (an ivreghdfe without
//    absorb() counts as ivreg2, ado) with e(model) "gmm2s": the engine's iv
//    reply names its two-step GMM fit "ivgmm" (apps/cmds/common.cpp, cmd_iv's
//    `estimator`), the only record of the method LastModel keeps. An ivreghdfe
//    that absorbs keeps cmd ivreghdfe there and is not this check's.
//    :1405 `GMM no longer supported` (D6). `ivreg2 ..., gmm2s` without robust or
//    cluster() fits as 2SLS (the two weight matrices coincide) but posts e(model)
//    gmm2s, which is what the ado reads: `eModel` (StataBE 19.5, BUG-1223).
//  - `ivregress ..., absorb()` keeps cmd ivregress, which the absvars check above
//    does not read (BUG-1179); runBoottest answers it after its option checks.
std::string boottestStataRefusalM4b(const LastModel& lm, const std::string& eModel) {
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("svy")) {
    return "Doesn't work after svy";
  }
  if (lm.cobraCmd == "xtreg") return "Doesn't work after {xtreg, re}";
  if (lm.fe.size() >= 2) {
    if (lm.stataCmd == "reghdfe") {
      return "Doesn't work after reghdfe with more than one set of absorbed fixed effects or "
             "with absorbed interaction terms";
    }
    // An IV fit's lm.fe is absorb() on the ivreghdfe path or on ivregress's own.
    if ((lm.cobraCmd == "iv" || lm.cobraCmd == "liml") && lm.stataCmd != "ivregress") {
      return "Doesn't work after ivreghdfe with more than one set of absorbed fixed effects or "
             "with absorbed interaction terms";
    }
  }
  if (lm.cobraCmd == "iv" && lm.fe.empty() && lm.result.has_value() && lm.result->is_object()) {
    const std::string est = lm.result->value("estimator", std::string());
    if (est == "ivgmm" || est == "ivgmm_multi") return "GMM no longer supported";
  }
  if ((lm.stataCmd == "ivreg2" || lm.stataCmd == "xtivreg2") && eModel == "gmm2s")
    return "GMM no longer supported";
  return std::string();
}

// boottest.ado's own message: one display block, rc 0, no bootstrap, and r()
// emptied -- every message record of the capture reads `mean=. N=.` after a
// summarize and prints an empty `return list` (B1 :104-:231, D4_r :1293, G1_r
// :2444). replaceRClass, never a clear of rStrings, which also holds s()
// (FU-168/FU-238); the empty declaration makes r() known-empty, so an unset
// r(p) reads `.` and `r(clustvars)' reads empty, as Stata's do.
ojson boottestMessageM4b(SessionState& session, const std::string& text) {
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "boottest", {});
  ojson note;
  note["type"] = "display";
  note["text"] = text;
  return note;
}

// ado: `local ML = e(converged) != . & !inlist("`cmd'", "reghdfe",
// "ivreghdfe", "reghdfejl")`. e(converged) is read where the macro layer reads
// it (eval/macros.cpp resolveEScalar): the fit's cached reply first, then a
// posted e-scalar. logit, probit, poisson, glm, nbreg, tobit and ivprobit carry
// it; regress, areg, reghdfe, xtreg and ivregress do not (bin/main probe
// 2026-10-14: `display e(converged)` 1 and `.` respectively). StataNow's
// `ivregress ..., absorb()` (the Halperin APM fit) sets it: boottest takes that
// fit for an ML one and re-fits it under the null (StataBE 19.5, BUG-1179:
// "Re-running regression with null imposed."), though PIET's reply has no key.
bool boottestFitIsMlM4b(const SessionState& session, const LastModel& lm) {
  if (lm.stataCmd == "reghdfe" || lm.stataCmd == "ivreghdfe") return false;
  if (lm.stataCmd == "ivregress" && !lm.fe.empty()) return true;
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("converged")) {
    const ojson& c = (*lm.result)["converged"];
    if (c.is_boolean()) return true;
    if (c.is_number()) {
      const double v = c.get<double>();
      return std::isfinite(v) && std::fabs(v) < eval::kStataMissingSentinel;
    }
  }
  const double* conv = session.eval.eScalars.find("converged");
  return conv != nullptr && std::isfinite(*conv) && std::fabs(*conv) < eval::kStataMissingSentinel;
}

// boottest after an IV fit is the wild restricted efficient bootstrap
// (boottest: IV = e(instd) set, WRE = !score & IV & reps), served by
// the engine's `wildboot --wre` for the fits whose model this path can rebuild:
// `ivregress 2sls`/`ivregress liml` and `ivreghdfe` (2SLS, one absorbed set --
// two sets answer with the ado's message in boottestStataRefusalM4b, as GMM
// does). Weighted fits stay refused (the engine's WRE takes no weights).
bool boottestIvWreM4b(const LastModel& lm) {
  if (lm.cobraCmd != "iv" && lm.cobraCmd != "liml") return false;
  if (lm.stataCmd != "ivregress" && lm.stataCmd != "ivreghdfe") return false;
  if (lm.ivEndog.empty() || lm.ivInstruments.empty()) return false;
  if (lm.stataCmd == "ivreghdfe" && (lm.fe.size() != 1 || lm.cobraCmd != "iv")) return false;
  if (lm.stataCmd == "ivregress" && !lm.fe.empty()) return false;
  if (lm.result.has_value() && lm.result->is_object()) {
    const std::string est = lm.result->value("estimator", std::string());
    if (est == "ivgmm" || est == "ivgmm_multi" || est == "fuller") return false;
  }
  return true;
}

// The hypothesis before the comma. boottest.ado (ado) wraps it in `{}`
// and hands each `(...)` part to `constraint`, so `x1`, `x1 = 0.0625` and
// `x1=0.0625` (w2t C1 :129, C10 :247, C11 :260) are all one constraint, the
// coefficient on x1 equal to the number (0 when no `=`). The engine tests one
// coefficient against one number (wildboot --param, --null), so exactly that
// shape is read here: a single coefficient name, optionally `= <number>`.
// Anything else -- a second term (`x1 x2`, which bin/main served as a test of x1
// alone), a `{...}` or `(...)` list, a linear combination (`x1+x2=1`, `2*x1=1`),
// `_b[x1]`, or a right-hand side that is not a number -- returns false, and
// runBoottest refuses it by name (PIET has no constraint machinery for
// boottest; no captured line covers those shapes).
bool parseBoottestHypothesisM4b(const eval::ParsedCommand& parsed, std::string& param,
                                std::optional<std::string>& nullValue) {
  std::string text = trimM4(parsed.argText);
  if (text.empty()) text = joinSpaceM4(parsed.varlist);
  static const std::regex kOneCoef(
      R"(^([A-Za-z0-9_][A-Za-z0-9_.#]*)\s*(?:=\s*([+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?))?$)");
  std::smatch m;
  if (!std::regex_match(text, m, kOneCoef)) return false;
  param = m[1].str();
  if (m[2].matched) nullValue = m[2].str();
  return true;
}

// The report boottest prints for one wild-bootstrapped hypothesis (boottest),
// handed to the dispatcher as a display block ahead of the estimation block (StataBE
// 19.5, BUG-1175, 7 clusters, reps(999)):
//
//   Warning: with 7 bootstrap clusters, the number of replications, 999, exceeds the
//   universe of Rademacher draws, 2^7 = 128. Sampling each once.        (one line)
//   Consider Webb weights instead, using weight(webb).
//
//   Wild bootstrap-t, null imposed, 128 replications, Wald test, bootstrap clustering
//   by cl, Rademacher weights:                                          (one line)
//     x1
//
//                               t(6) =    24.4529
//                           Prob>|t| =     0.0000
//
// The warning is printed when 2^G falls short of the replications asked for (Rademacher
// or Mammen weights; :834-838), the note on the level when G > 12 or the weights are not
// Rademacher and level*(B+1)/100 is not whole. `clustering by` names boottest's own
// cluster() (the override), `bootstrap clustering by` the bootstrap clusters.
// The statistic is t(df_r) under small and z otherwise, its line left out
// when PIET does not know boottest's statistic (r(t) is refused then too). `ciStr` is
// r(CIstr) where PIET computed the confidence set (boottestCiM4b), printed after the
// statistic as "95% confidence set for null hypothesis expression: [...]".
std::string boottestReportM4b(const std::string& preNotes, long long repsAsked, const ojson& raw,
                              double level, const std::string& clusterOpt,
                              const std::vector<std::string>& bootClusters,
                              const std::string& hypothesis, bool small,
                              std::optional<double> stat, std::optional<double> dfR,
                              const std::string& ciStr) {
  auto num = [&raw](const char* key) {
    return raw.contains(key) && raw[key].is_number() ? raw[key].get<double>() : std::nan("");
  };
  const double nBoot = num("n_clusters");
  const double reps = num("reps");
  const std::string wt = raw.value("weights", std::string("rademacher"));
  const std::string wtProper = wt.empty() ? wt : static_cast<char>(std::toupper(
                                                     static_cast<unsigned char>(wt[0]))) + wt.substr(1);
  char buf[512];
  std::string out = preNotes;
  if (std::isfinite(nBoot) && nBoot < 63 && (wt == "rademacher" || wt == "mammen") &&
      std::ldexp(1.0, static_cast<int>(nBoot)) < static_cast<double>(repsAsked)) {
    // Mammen weights get the warning too, without "Sampling each once." (they are
    // drawn, not enumerated); the line keeps its trailing blank (StataBE 19.5,
    // BUG-1519: 8 clusters, reps(999), "... universe of Mammen draws, 2^8 = 256. ").
    std::snprintf(buf, sizeof(buf),
                  "\nWarning: with %.0f bootstrap clusters, the number of replications, %lld, "
                  "exceeds the universe of %s draws, 2^%.0f = %.0f. %s"
                  "\nConsider Webb weights instead, using weight(webb).\n",
                  nBoot, repsAsked, wtProper.c_str(), nBoot,
                  std::ldexp(1.0, static_cast<int>(nBoot)),
                  wt == "rademacher" ? "Sampling each once." : "");
    out += buf;
  }
  if (std::isfinite(reps) && reps > 0 && std::isfinite(nBoot) && (nBoot > 12 || wt != "rademacher")) {
    const double lv = level / 100.0 * (reps + 1.0);
    if (std::floor(lv) != lv) {
      std::snprintf(buf, sizeof(buf),
                    "\nNote: The bootstrap usually performs best when the confidence level (here, "
                    "%g%%)\n      times the number of replications plus 1 (%.0f+1=%.0f) is an "
                    "integer.\n",
                    level, reps, reps + 1.0);
      out += buf;
    }
  }
  std::snprintf(buf, sizeof(buf), "\nWild bootstrap-t, null imposed, %.0f replications, Wald test",
                reps);
  out += buf;
  if (!clusterOpt.empty()) out += ", clustering by " + clusterOpt;
  if (!bootClusters.empty()) out += ", bootstrap clustering by " + joinSpaceM4(bootClusters);
  out += ", " + wtProper + " weights:\n  " + hypothesis + "\n";
  if (stat.has_value()) {
    char line1[64];
    if (small && dfR.has_value())
      std::snprintf(line1, sizeof(line1), "t(%.0f)", *dfR);
    else
      std::snprintf(line1, sizeof(line1), "%s", small ? "t" : "z");
    const std::string ptext = small ? "Prob>|t|" : "Prob>|z|";
    const double p = num("p_symmetric");
    auto f104 = [](double v) {
      char b[64];
      if (std::isfinite(v)) std::snprintf(b, sizeof(b), "%10.4f", v);
      else std::snprintf(b, sizeof(b), "%10s", ".");
      return std::string(b);
    };
    out += "\n" + std::string(32 - std::min<size_t>(32, std::strlen(line1)), ' ') + line1 +
           " = " + f104(*stat) + "\n" + std::string(32 - ptext.size(), ' ') + ptext + " = " +
           f104(p);
  }
  if (!ciStr.empty()) {
    std::snprintf(buf, sizeof(buf), "\n\n%g%% confidence set for null hypothesis expression: ",
                  level);
    out += buf + ciStr;
  }
  return out;
}

// boottest's confidence set for one hypothesis by test inversion (boottest.mata
// plot() and search(), the one-dimensional case; StataBE 19.5, BUG-1174: 8 clusters,
// 2^8 draws, `boottest x1 = 2` r(CI) (1.9097521809349358, 2.087247394569717), r(CIstr)
// "[1.91, 2.087]", r(plot) 25 x 2). `pAt(r)` is the bootstrap's symmetric p-value with
// the null at r, on the same draws. The method, as boottest's:
//  - the grid: the estimate b plus and minus 1.5 * invnormal(1 - alpha/2) * sqrt(V)
//    (V = r(V)); the bound farther from the tested null is checked first, and while
//    a bound's p-value exceeds alpha the grid is widened by its own width on both
//    sides, at most ten times per side (p at the near bound is not re-read when the
//    far side's widening moves it);
//  - 25 equally spaced points (rangen: a + k(b-a)/24, the ends exact) and the estimate
//    itself at p = 1, inserted in order;
//  - each crossing of alpha between neighbours is refined by Chandrupatla's (1997)
//    hybrid quadratic-bisection search, stopping when a p-value lies within 1/B of
//    alpha (B the feasible replications; the symmetric p's tolerance) or the
//    bracket's relative width falls under ptol .001 (r(ptol)); the bracket end whose p
//    is nearer alpha is the bound;
//  - a non-rejection region open at either end of the grid is unbounded on that side.
// r(plot) is the 25-point grid without the estimate. nullopt when any p-value could not
// be read (no confidence set is posted then, as before).
struct BoottestCiM4b {
  std::vector<std::pair<double, double>> sets;  // NaN = unbounded on that side
  std::vector<double> plotX, plotY;
};
template <class PAt>
std::optional<BoottestCiM4b> boottestCiM4b(PAt&& pAt, double b, double V, double r0, double level,
                                           double bFeas) {
  const double alpha = 1.0 - level / 100.0;
  if (!(alpha > 0) || !(V > 0) || !std::isfinite(b) || !(bFeas > 0)) return std::nullopt;
  double z = std::nan("");
  {
    char expr[64];
    std::snprintf(expr, sizeof(expr), "invnormal(%.17g)", alpha / 2.0);
    if (const auto v = eval::evalArith(expr, nullptr)) {
      try {
        z = std::stod(*v);
      } catch (...) {
      }
    }
  }
  if (!std::isfinite(z)) return std::nullopt;
  const double halfwidth = -1.5 * z * std::sqrt(V);
  const double peak = b;
  double lo = peak - halfwidth, hi = peak + halfwidth;
  double pLo = std::nan(""), pHi = std::nan("");
  auto widenLow = [&] {
    for (int i = 10; i > 0; --i) {
      pLo = pAt(lo);
      if (!(pLo > alpha)) break;
      const double w = hi - lo;
      lo -= w;
      hi += w;
    }
  };
  auto widenHigh = [&] {
    for (int i = 10; i > 0; --i) {
      pHi = pAt(hi);
      if (!(pHi > alpha)) break;
      const double w = hi - lo;
      lo -= w;
      hi += w;
    }
  };
  if (std::fabs(lo - r0) > std::fabs(hi - r0)) {
    widenLow();
    widenHigh();
  } else {
    widenHigh();
    widenLow();
  }
  constexpr int kGrid = 25;
  std::vector<double> X(kGrid), Y(kGrid, std::nan(""));
  for (int k = 0; k < kGrid; ++k) X[k] = k * ((hi - lo) / (kGrid - 1)) + lo;
  X[0] = lo;
  X[kGrid - 1] = hi;
  Y[0] = pLo;
  Y[kGrid - 1] = pHi;
  size_t c;  // the estimate's index, 0-based
  if (peak < lo) {
    c = 0;
  } else if (peak > hi) {
    c = kGrid;
  } else {
    c = static_cast<size_t>(std::floor((peak - lo) / (hi - lo) * (kGrid - 1))) + 1;
  }
  X.insert(X.begin() + static_cast<std::ptrdiff_t>(c), peak);
  Y.insert(Y.begin() + static_cast<std::ptrdiff_t>(c), 1.0);
  for (size_t k = 0; k < Y.size(); ++k)
    if (std::isnan(Y[k])) Y[k] = pAt(X[k]);
  for (double y : Y)
    if (!std::isfinite(y)) return std::nullopt;

  constexpr double kPtol = 1e-3;
  bool failed = false;
  auto search = [&](double f1, double x1, double f2, double x2) {
    double t = 0.5, x3 = x2, f3 = f2;
    for (int guard = 0; guard < 1000; ++guard) {
      const double x = x1 + t * (x2 - x1);
      const double fx = pAt(x);
      if (!std::isfinite(fx)) {
        failed = true;
        return x;
      }
      if ((fx > f1) == (fx > f2)) return x;  // monotonicity broken: as good as it gets
      if ((fx < alpha) == (f1 < alpha)) {
        x3 = x1;
        x1 = x;
        f3 = f1;
        f1 = fx;
      } else {
        x3 = x2;
        x2 = x1;
        x1 = x;
        f3 = f2;
        f2 = f1;
        f1 = fx;
      }
      if (std::fabs(fx - alpha) < 1.0 / bFeas * 1.000001 ||
          std::fabs(x2 - x1) / (std::fabs(x1) + 1.0) < kPtol)
        return std::fabs(f1 - alpha) < std::fabs(f2 - alpha) ? x1 : x2;
      const double phi1 = (f1 - f2) / (f3 - f2);
      const double phi1Sq = phi1 * phi1;
      const double xi1 = (x1 - x2) / (x3 - x2);
      if (phi1Sq > xi1 || xi1 > phi1 + phi1 - phi1Sq) {
        t = 0.5;
      } else {
        t = ((f3 - alpha) / (f1 - f2) + (x3 - x1) / ((x2 - x1) * (f3 - f1)) * (f2 - alpha)) *
            (f1 - alpha) / (f3 - f2);
        t = std::min(0.999999, std::max(0.000001, t));
      }
    }
    failed = true;
    return x1;
  };
  // Crossings: a rising step (p up through alpha) opens a non-rejection region, a
  // falling one closes it; regions open at the grid's ends are unbounded there.
  std::vector<long> ups, downs;
  for (size_t k = 0; k + 1 < Y.size(); ++k) {
    const int d = static_cast<int>(Y[k + 1] > alpha) - static_cast<int>(Y[k] > alpha);
    if (d == 1) ups.push_back(static_cast<long>(k));
    if (d == -1) downs.push_back(static_cast<long>(k));
  }
  BoottestCiM4b out;
  const double kNan = std::nan("");
  if (ups.empty() && downs.empty()) {
    out.sets.push_back({kNan, kNan});
  } else {
    if (ups.empty()) ups.push_back(-1);
    else if (downs.empty()) downs.push_back(-1);
    else {
      if (ups.front() > downs.front()) ups.insert(ups.begin(), -1);
      if (ups.back() > downs.back()) downs.push_back(-1);
    }
    if (ups.size() != downs.size()) return std::nullopt;
    for (size_t i = 0; i < ups.size(); ++i) {
      const double l = ups[i] < 0 ? kNan
                                  : search(Y[ups[i]], X[ups[i]], Y[ups[i] + 1], X[ups[i] + 1]);
      const double h = downs[i] < 0
                           ? kNan
                           : search(Y[downs[i]], X[downs[i]], Y[downs[i] + 1], X[downs[i] + 1]);
      out.sets.push_back({l, h});
    }
  }
  if (failed) return std::nullopt;
  X.erase(X.begin() + static_cast<std::ptrdiff_t>(c));
  Y.erase(Y.begin() + static_cast<std::ptrdiff_t>(c));
  out.plotX = std::move(X);
  out.plotY = std::move(Y);
  return out;
}

// r(CIstr) (boottest): each set as `[lo, hi]` in %10.4g, an unbounded side
// as `(-∞` / `∞)`, sets joined by ` ∪ `, every `-` then printed as the minus sign `−`.
std::string boottestCiStrM4b(const BoottestCiM4b& ci) {
  std::string out;
  auto num = [](double v) { return strutil::trim(fmt::general(v, 10, 4)); };
  for (size_t i = 0; i < ci.sets.size(); ++i) {
    if (i > 0) out += " ∪ ";
    const auto& [l, h] = ci.sets[i];
    out += (std::isnan(l) ? std::string("(-∞") : "[" + num(l)) + ", " +
           (std::isnan(h) ? std::string("∞)") : num(h) + "]");
  }
  std::string minus;
  for (char ch : out) minus += ch == '-' ? std::string("−") : std::string(1, ch);
  return minus;
}

}  // namespace

// `reportOut` (BUG-1175): boottest's printed report (boottestReportM4b), a display block
// the dispatcher pushes ahead of the estimation block (declared there: piet/handlers.h
// keeps the two-argument form, below).
ojson runBoottest(const eval::ParsedCommand& parsed, SessionState& session, ojson* reportOut) {
  const auto& lmOpt = session.lastModel();
  // FU-240: ado `_assert "`e(cmd)'" != "", msg(No estimates detected)` inside
  // the ado's `cap noi` block: the message at rc 0 with r() emptied (w2t B1
  // :216/:221, B2 :237/:243/:248 `mean=. N=.`, `return list` :231 empty). This
  // was rc 1 ("No previous estimation result."). A posted e(cmd) with no model
  // behind it (`ereturn post` then `ereturn local cmd`) is a fit Stata would go
  // on to bootstrap, and PIET has nothing to bootstrap: refused by name.
  if (!lmOpt.has_value()) {
    const std::string eCmd = session.eval.eStrings.get("cmd", std::string());
    if (!eCmd.empty()) {
      return engineErrorBlockM4(
          "boottest after e() posted by `" + eCmd + "` without a fitted model is not "
          "supported in this version of PIET: there is no model to bootstrap.",
          "unsupported_option");
    }
    return boottestMessageM4b(session, "No estimates detected");
  }
  const LastModel& lm = *lmOpt;
  // BUG-1210: results read by `estimates use` carry no e(sample); boottest.ado
  // says so inside its `cap noi` block and returns rc 0 with r() empty (StataBE).
  if (lm.esample == LastModel::EsampleState::kNone)
    return boottestMessageM4b(session,
                              "Sample marker for the regression not found. Perhaps the data "
                              "set was cleared and reconstructed");
  // Only specification-proven, unweighted ML models use the score path.
  // Offsets and restricted-sample layouts have independent refit contracts.
  std::string scoreModel;
  if (lm.result && lm.result->is_object() && lm.fe.empty() && lm.fitWeight.empty() &&
      lm.fitKeepif.empty() && lm.fitDataPath.empty() && !lm.result->contains("svy")) {
    const auto& fit = *lm.result;
    bool hasConstant = false;
    const char* ck = fit.contains("coeftable") ? "coeftable" : "coefs";
    if (fit.contains(ck) && fit[ck].is_array()) for (const auto& row : fit[ck]) {
      const std::string name = row.is_object() ? row.value("name", std::string()) : std::string();
      if (name == "_cons" || name == "(intercept)") hasConstant = true;
    }
    if (hasConstant && (lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "poisson") &&
        fit.contains("has_offset") && fit["has_offset"].is_boolean() && !fit["has_offset"].get<bool>() &&
        fit.value("N_cdf", 0) == 0 && fit.value("N_cds", 0) == 0) scoreModel = lm.cobraCmd;
    if (hasConstant && lm.cobraCmd == "glm" && fit.contains("glm_model")) {
      const auto& gm = fit["glm_model"];
      // specToJson stores exposure as a boolean; offset is its variable name.
      if (gm.is_object() && gm.contains("offset") && gm["offset"].is_string() &&
          gm["offset"].get<std::string>().empty() && gm.contains("exposure") &&
          gm["exposure"].is_boolean() && !gm["exposure"].get<bool>() &&
          gm.contains("family") && gm["family"].is_number_integer() &&
          gm.contains("link") && gm["link"].is_number_integer() &&
          (!gm.contains("if") || gm["if"].is_null()) && (!gm.contains("in") || gm["in"].is_null())) {
        if (gm.value("family", -1) == 2 && gm.value("link", -1) == 1) scoreModel = "glm-poisson";
        if (gm.value("family", -1) == 0 && gm.value("link", -1) == 0) scoreModel = "glm-gaussian";
      }
    }
  }
  // boottest.ado's estimator checks (ado) run before its syntax (ado),
  // its option messages (ado) and its boottype() check (ado), so
  // a fit it refuses answers with its message ahead of every check below: E8
  // (w2t_boottest_family.log:1893) prints the estimator text, not "Option cluster
  // not allowed.".
  if (const std::string text =
          boottestStataRefusalM4b(lm, session.eval.eStrings.get("model", std::string()));
      !text.empty()) {
    return boottestMessageM4b(session, text);
  }
  // FU-23 (BUG-758): cluster()/robust by shape, w2l_vce_shape.log (each arm
  // after `regress y x1 x2, vce(cluster id)`).
  //  - boottest_bare_cluster / boottest_robust_val: boottest prints "Option
  //    cluster not allowed." / "Option robust(x1) not allowed." and stops at
  //    rc 0 with nothing returned (r(p) ., r(clustvars) empty). This handler
  //    skipped the bare `cluster` and bootstrapped anyway; `robust(x1)` hit the
  //    reject net r(198). Answered as boottest does: the message, rc 0, r()
  //    emptied, no bootstrap. These are boottest.ado's `syntax` leftovers
  //    (ado), so they come before every refusal of PIET's own below.
  //  - boottest_empty_cluster: `cluster()` is the fit's clustering (p 0,
  //    clustvars id); it was compared as an empty list and refused.
  //  - boottest_ctl_cluster_id2: `cluster(id2)` after a fit clustered on id is
  //    rc 0, "Overriding estimator's cluster/robust settings with cluster(id2)",
  //    clustvars id2. It was refused as a mismatch; the engine's wildboot refits
  //    on --cluster, so the override is forwarded and said in the block's note.
  //  - boottest_opt_c2: `cluster(id id2)` is a two-way bootstrap (rc 0, p .).
  //    The engine's wild bootstrap is one-way (src/inference/wildboot.cpp reads
  //    md.cluster.at(cluster_index), index 0): refused by name, never truncated
  //    to the first variable -- and so is a two-way FIT with no cluster(),
  //    which used to be forwarded as `--cluster a,b` and bootstrapped on `a`.
  // FU-238 (FU-168): r() is emptied through boottestMessageM4b (replaceRClass);
  // the triple clear this used wiped rStrings whole, s() included.
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster == OptShapeM4b::kBare ||
      (vceShape.robust != OptShapeM4b::kAbsent && vceShape.robust != OptShapeM4b::kBare)) {
    return boottestMessageM4b(session, vceShape.cluster == OptShapeM4b::kBare
                                           ? std::string("Option cluster not allowed.")
                                           : "Option robust(" + vceShape.robustRaw +
                                                 ") not allowed.");
  }

  // boottype() (papers 053 and 086 write `boottype(wild)`; DIAGNOSIS Q5). The
  // ado reads it with `syntax, [Wild SCore]` (ado): `wild` is the
  // default after a non-ML fit, so there it changes nothing (w2t C12 :987/:993
  // equal C1 :340/:346); after a Maximum-Likelihood fit (ado) the ado prints
  // its message at rc 0, after the cluster override line it printed at ado
  // (F6 :2377-2378). `score` asks for the score bootstrap, which the engine does
  // not have, and any other value has no captured line: both refused by name.
  bool boottypeWild = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k != "boottype") continue;
    const std::string bt = v.isBool ? std::string() : lowerM4(trimM4(v.strValue));
    if (!scoreModel.empty() && !bt.empty() && std::string("score").compare(0, bt.size(), bt) == 0) continue;
    if (!bt.empty() && std::string("wild").compare(0, bt.size(), bt) == 0) {
      boottypeWild = true;
      continue;
    }
    return engineErrorBlockM4(
        "boottest, boottype(" + (v.isBool ? std::string() : trimM4(v.strValue)) +
            "): this version of PIET runs only the wild bootstrap, boottype(wild); the score "
            "bootstrap and any other boottype() are not implemented.",
        "unsupported_option");
  }
  if (boottypeWild && boottestFitIsMlM4b(session, lm)) {
    std::string text;
    if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
      text = "Overriding estimator's cluster/robust settings with cluster(" +
             joinSpaceM4(vceShape.clusterVars) + ")\n";
    } else if (vceShape.robust == OptShapeM4b::kBare) {
      text = "Overriding estimator's cluster/robust settings with robust\n";
    }
    return boottestMessageM4b(
        session, text + "boottype(wild) not accepted after Maximum Likelihood-based estimation");
  }
  // An absorbed ivregress is an ML fit to boottest (boottestFitIsMlM4b), so with the
  // null imposed it re-runs the command line with constraints() (ado), which
  // ivregress refuses; the ado's `cap noi` body prints both messages at rc 0 and posts
  // nothing (StataBE 19.5, BUG-1179: absorb(a b) and absorb(a) alike, r(p) and r(reps)
  // missing). 1998 is the free constraint number once the hypothesis holds 1999.
  if (lm.stataCmd == "ivregress" && !lm.fe.empty()) {
    return boottestMessageM4b(
        session,
        "\nRe-running regression with null imposed.\n\noption constraints(1998) not allowed\n"
        "Error imposing null. Perhaps ivregress does not accept the constraints(), from(), and "
        "iterate() options");
  }

  if (auto refused = postestTsKeepifRefusalM4("boottest", lm)) return *refused;
  if (lm.cluster.empty()) {
    return engineErrorBlockM4(
        "boottest requires a preceding regression with vce(cluster ...)", "unsupported_option");
  }
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: boottest requires a coefficient name", "syntax_error");
  }
  // IV: the WRE (boottestIvWreM4b) rebuilds the IV model itself, so the OLS
  // refit guard does not apply to it; any other IV fit keeps that guard's refusal.
  const bool ivWre = scoreModel.empty() && boottestIvWreM4b(lm);
  if (ivWre && !lm.fitWeight.empty()) {
    return engineErrorBlockM4(
        "boottest after a weighted `" + lm.stataCmd + "` fit is not supported in this version "
        "of PIET: the engine's wild restricted efficient bootstrap takes no weights.",
        "unsupported_option");
  }
  {
    ojson reject;
    if (scoreModel.empty() && !ivWre && !boottestRefitMatchesFitM4(lm, reject)) return reject;
  }
  // The null: `boottest x1 = 0.0625` is the hypothesis x1 = 0.0625 (w2t C10 :881
  // r(t) -.2359095793768099, C11 :937 the same written `x1=0.0625`). This read
  // parsed.varlist[0] alone, so the spaced form tested x1 = 0 at rc 0 (C1's
  // t 1.001001389919719) and the compact one reached the engine as the
  // coefficient name `x1=0.0625` (rc 1).
  std::string param;
  std::optional<std::string> nullValue;
  if (!parseBoottestHypothesisM4b(parsed, param, nullValue)) {
    return engineErrorBlockM4(
        "boottest hypothesis `" + trimM4(parsed.argText) +
            "`: this version of PIET tests one coefficient against a number (`boottest x1` or "
            "`boottest x1 = 0.5`). A hypothesis with more than one term, a {...} or (...) "
            "constraint list, a linear combination or a right-hand side that is not a number "
            "is not implemented; refused rather than tested as a different hypothesis.",
        "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "null" && nullValue.has_value()) {
      return engineErrorBlockM4(
          "boottest: the null is given twice, as `= " + *nullValue + "` and as null(); give it once.",
          "unsupported_option");
    }
  }
  // The hypothesis as typed: boottest prints it under the report's title and names
  // r(b)'s column, r(V)'s row and column and r(CI)'s row by it (boottest,
  // :997 -- the constraint's own text). StataBE 19.5, BUG-1491: `boottest x1 = 2`
  // names them `x1 = 2`, `x1=2` and `x1  =  2` keep their spacing, plain `x1` is `x1`.
  std::string h0text = trimM4(parsed.argText);
  if (h0text.empty()) h0text = joinSpaceM4(parsed.varlist);
  // A two-way fit records its clusters as ONE comma-joined element (runRegress
  // pushes the `--cluster id,id2` argv value), so the fit's list is split here
  // before it is counted.
  std::vector<std::string> bootClusters;
  for (const auto& c : lm.cluster) {
    std::string spaced = c;
    for (auto& ch : spaced) {
      if (ch == ',') ch = ' ';
    }
    for (const auto& t : splitTokensM4(spaced)) bootClusters.push_back(t);
  }
  const std::vector<std::string> fitClusters = bootClusters;
  if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
    bootClusters = vceShape.clusterVars;
  } else if (splitTokensM4(session.eval.eStrings.get("clustvar", std::string())).size() >
             fitClusters.size()) {
    // ivreghdfe with three or more cluster() names fits on the first two but lists
    // them all in e(clustvar) (BUG-1189), which is what boottest bootstraps over.
    return engineErrorBlockM4(
        "boottest after a fit whose e(clustvar) lists more cluster variables than it was "
        "fitted on (" + session.eval.eStrings.get("clustvar", std::string()) +
            ") is not implemented in this version of PIET: boottest would bootstrap over "
            "all of them. Name the bootstrap clustering in cluster().",
        "unsupported_option");
  }
  if (!scoreModel.empty() && (fitClusters.size() != 1 || bootClusters != fitClusters)) {
    return engineErrorBlockM4("boottest score bootstrap requires the same single error and bootstrap cluster variable", "unsupported_option");
  }
  if (bootClusters.empty()) {
    return engineErrorBlockM4(
        "boottest requires a preceding regression with vce(cluster ...)", "unsupported_option");
  }
  // boottest_opt_c2 (w2l_vce_shape_det.log:4142-4159): `cluster(id id2)` after
  // regress is a multiway bootstrap: the error variance sums every non-empty
  // intersection of the clusterings with parity signs and the bootstrap
  // clusters are their full intersection (boottest, boottest.mata
  // Init) -- the engine's direct-refit kernel (wildboot --cluster a,b).
  // Served for unweighted, unabsorbed least squares and for the IV WRE; an
  // absorbed or weighted multiway bootstrap stays refused, never truncated to
  // the first variable.
  const bool multiway = bootClusters.size() > 1;
  if (multiway && !ivWre &&
      (!scoreModel.empty() || !lm.fe.empty() || !lm.fitWeight.empty())) {
    return engineErrorBlockM4(
        "boottest over more than one cluster variable (" + joinSpaceM4(bootClusters) +
            ") after an absorbed, weighted or maximum-likelihood fit is not implemented in this "
            "version of PIET: the engine's multiway wild bootstrap refits unweighted, "
            "unabsorbed least squares only. Name one variable in cluster().",
        "unsupported_option");
  }
  const bool direct = ivWre || multiway;

  // BUG-163: wildboot has no --keepif on its CLI, so the fitted sample must
  // arrive as a FILE. fitDataPath already is one (runRegress's materialized
  // if/in filter); a --keepif-style fit is materialized here through the same
  // applyIfAndInFilter every other filtered handler uses.
  std::string bootData = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
  if (!lm.fitKeepif.empty()) {
    try {
      auto bfr = piet::handlers::applyIfAndInFilter(session, bootData, lm.fitKeepif, std::nullopt);
      if (bfr.ok) bootData = bfr.dataPath;
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }
  // Direct-refit settings (boottest.ado): small = e(df_r) != . (ado) --
  // ivregress posts it under `small` (e(small) "small"), regress always, and
  // ivreghdfe always (t(29) at w2t_boottest_family.log:1189) though PIET's
  // ivreghdfe does not post e(df_r) itself; FEdfadj (ado) is 0 after
  // regress/ivregress and max(1, e(df_a)) after ivreghdfe.
  bool directSmall = false;
  double feDofAdj = 0.0;
  bool directNoCons = false;
  if (direct) {
    if (!ivWre) {
      directSmall = true;
      directNoCons = boottestFitLacksConstantM4(lm);
    } else if (lm.stataCmd == "ivregress") {
      directSmall = session.eval.eStrings.get("small", std::string()) == "small";
      if (lm.result.has_value() && lm.result->is_object()) {
        bool hasCons = false;
        const char* ck = lm.result->contains("coeftable") ? "coeftable" : "coefs";
        if (lm.result->contains(ck) && (*lm.result)[ck].is_array())
          for (const auto& row : (*lm.result)[ck])
            if (row.is_object() && (row.value("name", std::string()) == "_cons" ||
                                    row.value("name", std::string()) == "(intercept)"))
              hasCons = true;
        if (lm.result->contains("coef") && (*lm.result)["coef"].is_object() &&
            (*lm.result)["coef"].contains("(intercept)"))
          hasCons = true;
        directNoCons = !hasCons;
      }
    } else {
      // ivreghdfe (boottest.ado, the ado's own reading of the fit): `cmd` stays
      // "ivreghdfe" (ado, e(extended_absvars) is set), so the df_a locals
      // that only the reghdfe branch fills (ado) are empty and
      // NFE = max(0e(K1), 0df_a, 0df_a_initial) = 0 (ado): no fixed
      // effect is passed to the bootstrap. e(b) has no _cons and
      // e(partialcons) is not 1, so `cons & !NFE` (ado) leaves the
      // model without a constant. Yet FEdfadj = max(1, e(df_a)) (ado) still
      // enters the dof. So boottest bootstraps the unabsorbed, constant-free 2SLS
      // with the small-sample factor (N-1)/(N-k-df_a) and G/(G-1): Stata's r(t)
      // 2.286824317666255 (D3_ivreghdfe_a1_st :1203) is that model's
      // (`ivregress 2sls yw x1 (endog=z), nocons vce(cluster id) small` t
      // 2.326368879167318 rescaled by sqrt(172/178)). This is boottest's
      // behaviour, mirrored, not a statistical choice. e(df_a) is reghdfe's
      // count for the one absorbed set: its levels less those nested in a
      // cluster variable (the fit reply's `absorbed` / `nested_fe`).
      directSmall = true;
      directNoCons = true;
      double dfA = 0.0;
      if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("absorbed") &&
          (*lm.result)["absorbed"].is_object() && (*lm.result)["absorbed"].size() == 1) {
        const auto& ab = (*lm.result)["absorbed"];
        const std::string feName = ab.begin().key();
        if (ab.begin().value().is_number()) dfA = ab.begin().value().get<double>();
        if (lm.result->contains("nested_fe") && (*lm.result)["nested_fe"].is_array())
          for (const auto& nf : (*lm.result)["nested_fe"])
            if (nf.is_string() && nf.get<std::string>() == feName) dfA = 0.0;
      } else {
        return engineErrorBlockM4(
            "boottest after `ivreghdfe`: the fit reply does not report its absorbed levels, so "
            "boottest's degrees-of-freedom adjustment e(df_a) is unknown; refused.",
            "engine_error");
      }
      feDofAdj = std::max(1.0, dfA);
    }
  }
  // BUG-1175: the printed report (boottestReportM4b), built once the reply is in. Ahead
  // of it, in boottest's order: the override line (ado, after a blank line) and,
  // for a multiway bootstrap, the assumed bootcluster() (ado; PIET takes no
  // bootcluster(), so it is always assumed). reps() is boottest's, 999 by default.
  auto emitReport = [&](const ojson& reply, bool small, std::optional<double> stat,
                        std::optional<double> dfR, const std::string& ciStr) {
    if (reportOut == nullptr) return;
    std::string pre;
    std::string clusterOpt;
    if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
      clusterOpt = joinSpaceM4(vceShape.clusterVars);
      pre += "\nOverriding estimator's cluster/robust settings with cluster(" + clusterOpt + ")\n";
    }
    if (bootClusters.size() > 1)
      pre += "(bootcluster(" + joinSpaceM4(bootClusters) + ") assumed)\n";
    long long repsAsked = 999;
    for (const auto& [k, v] : parsed.opts) {
      if (k != "reps" || v.isBool) continue;
      try {
        repsAsked = std::stoll(trimM4(v.strValue));
      } catch (...) {
      }
    }
    ojson rep;
    rep["type"] = "display";
    rep["text"] = boottestReportM4b(pre, repsAsked, reply, static_cast<double>(session.level),
                                    clusterOpt, bootClusters, h0text, small, stat, dfR,
                                    ciStr);
    *reportOut = rep;
  };
  std::vector<std::string> args;
  if (ivWre) {
    args = {"wildboot", "--data", bootData, "--y", lm.y, "--endog", joinCommaM4(lm.ivEndog),
            "--instruments", joinCommaM4(lm.ivInstruments), "--param", param, "--cluster",
            joinCommaM4(bootClusters), "--wre"};
    if (!lm.ivExog.empty()) {
      args.push_back("--x");
      args.push_back(joinCommaM4(lm.ivExog));
    }
    if (lm.cobraCmd == "liml") args.push_back("--liml");
  } else if (multiway) {
    args = {"wildboot", "--data", bootData, "--y", lm.y, "--x",
            lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x), "--param", param, "--cluster",
            joinCommaM4(bootClusters)};
  } else {
    args = {"wildboot",
            "--data",
            bootData,
            "--y",
            lm.y,
            "--x",
            lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x),
            "--param",
            param,
            "--cluster",
            bootClusters[0]};
  }
  if (direct) {
    if (directSmall) args.push_back("--small");
    if (directNoCons) args.push_back("--no-intercept");
    if (feDofAdj != 0.0) {
      args.push_back("--fe-dof-adj");
      args.push_back(std::to_string(static_cast<long long>(feDofAdj)));
    }
  }
  // The direct kernel takes no FE, weights or singleton flags (ivreghdfe's FE is
  // left out on purpose, see above).
  if (!direct && !scoreModel.empty()) {
    args.push_back("--model");
    args.push_back(scoreModel);
  }
  if (!direct && !lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }
  if (!direct && boottestFitLacksConstantM4(lm)) args.push_back("--no-intercept");
  if (!direct && !lm.fitWeight.empty()) {
    args.push_back("--weight");
    args.push_back(lm.fitWeight);
    args.push_back("--wtype");
    args.push_back(lm.fitWtype);
  }
  if (nullValue.has_value()) {
    args.push_back("--null");
    args.push_back(*nullValue);
  }
  // FU-733: areg and xtreg,fe keep their singleton groups (LastModel::
  // keepSingletons), and cmd_wildboot resamples them too under
  // --keep-singletons (apps/cmds/cmds_linear.cpp, landed with the e(sample) T2
  // fix round), as every other refit forwards it (fitConventionFlagsM4).
  // Without it the bootstrap dropped id 11 and the guard below refused
  // w2_esample_tsrefit.log:393 W3 (xtreg,fe, 59 obs, Stata p .0720720720720721).
  if (!direct && lm.keepSingletons && !lm.fe.empty()) args.push_back("--keep-singletons");

  for (const auto& [k, v] : parsed.opts) {
    if (k == "reps" && !v.isBool) {
      args.push_back("--reps");
      args.push_back(v.strValue);
    } else if (k == "seed" && !v.isBool) {
      args.push_back("--seed");
      args.push_back(v.strValue);
    } else if (k == "weighttype" && !v.isBool) {
      // boottest's five types by their minimum abbreviations (boottest,
      // `syntax, [RADemacher MAMmen NORmal WEBb GAMma]`); the engine takes the full
      // name, which r(weighttype) posts (StataBE 19.5, BUG-1519: weight(mam) is mammen;
      // an empty weight() is the default, rademacher, ado).
      const std::string w = lowerM4(trimM4(v.strValue));
      if (w.empty()) continue;
      std::string full;
      for (const char* name : {"rademacher", "mammen", "normal", "webb", "gamma"})
        if (w.size() >= 3 && std::string(name).compare(0, w.size(), w) == 0) full = name;
      if (full.empty()) {
        return engineErrorBlockM4("weighttype() must be rademacher, mammen, normal, webb or gamma",
                                  "unsupported_option");
      }
      args.push_back("--weights");
      args.push_back(full);
    } else if (k == "null" && !v.isBool) {
      args.push_back("--null");
      args.push_back(v.strValue);
    }
    // cluster(): read by shape above (bootClusters). W:P5 refused a cluster()
    // that differed from the fit's on the premise that it "would silently
    // bootstrap over clusters the model never used"; boottest itself overrides
    // the fit and says so (boottest_ctl_cluster_id2), which the note below
    // carries. nograph: nothing to suppress (vocabulary note at file top).
  }
  // reps() is boottest's, 999 by default (boottest `Reps(integer 999)`); the
  // engine's own default is 9999 (StataBE 19.5, BUG-1546: 30
  // clusters, `boottest x1 = 2, seed(1)` r(reps) 999, r(p) .7977977977977978, where
  // PIET drew 9999 and answered .8159).
  if (std::none_of(parsed.opts.begin(), parsed.opts.end(),
                   [](const auto& kv) { return kv.first == "reps" && !kv.second.isBool; })) {
    args.push_back("--reps");
    args.push_back("999");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabBoottest.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    // boottest runs its body under `cap noi`: an option its syntax does not
    // declare prints "Option weights(webb) not allowed.", runs no test, leaves
    // r() empty and returns rc 0 (StataBE, BUG-1173). Options this build knows
    // of but has not implemented keep their named refusal.
    for (const auto& [k, v] : parsed.opts) {
      if (kKnown.count(k) || kVocabBoottest.unimplemented.count(k)) continue;
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "boottest", {});  // r() is known-empty: r(p) reads `.`
      ojson note;
      note["type"] = "display";
      note["text"] = "Option " + k + (v.isBool ? std::string() : "(" + v.strValue + ")") +
                     " not allowed.";
      return note;
    }
    return *err;
  }

  // Stata's draws (core/rng_mt64.h): boottest runs
  // `if "`seed'"=="" local seed = c(seed)` then `set seed `seed'`, and c(seed)
  // is the current state string, so seed(#) restarts the session stream at #
  // and no seed() continues it. boottest.mata then draws the Rademacher weights
  // from that state. The engine draws them the same way from the
  // state sent here and reports how many it took; the session stream then
  // stands where Stata's would (and seed(#) is now c(seed), as after `set seed`).
  std::optional<long long> bootSeed;
  for (const auto& [k, v] : parsed.opts) {
    if (k != "seed" || v.isBool) continue;
    try {
      size_t pos = 0;
      const std::string t = trimM4(v.strValue);
      const long long sv = std::stoll(t, &pos);
      if (pos == t.size()) bootSeed = sv;
    } catch (...) {
    }
  }
  const cecon::Mt64 bootStream =
      bootSeed.has_value() ? cecon::Mt64(static_cast<uint64_t>(*bootSeed)) : session.seed.stream();
  args.push_back("--rng-mt64");
  args.push_back(bootStream.state_string());
  args.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (bootSeed.has_value()) session.seed = *bootSeed;
  if (raw.is_object() && raw.contains("rng_draws") && raw["rng_draws"].is_number_integer() &&
      raw["rng_draws"].get<long long>() > 0)
    session.seed.stream().discard(static_cast<uint64_t>(raw["rng_draws"].get<long long>()));
  // e(sample) T2: areg and xtreg,fe keep their singleton groups (LastModel::
  // keepSingletons; w2_esample.log:925 D4 areg e(N) 59). The engine's wild
  // bootstrap drops them unless told otherwise (--keep-singletons above), and
  // `areg y x1 x2, absorb(id) vce(cluster id)` (e(N) 59, 11 clusters) then
  // `boottest x1` answered n_obs 58 / 10 clusters at rc 0, the numbers of the
  // fit without id 11. Still refused by name whenever the bootstrap's sample is
  // not the fit's.
  if (!direct && lm.keepSingletons && !lm.fe.empty() && lm.result.has_value() &&
      lm.result->is_object() && lm.result->contains("n_obs") &&
      (*lm.result)["n_obs"].is_number() && raw.is_object() && raw.contains("n_obs") &&
      raw["n_obs"].is_number() &&
      raw["n_obs"].get<double>() != (*lm.result)["n_obs"].get<double>()) {
    return engineErrorBlockM4(
        "boottest after `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
            "` with a singleton group is not supported in this version of PIET -- the "
            "engine's wild bootstrap drops singleton groups, which this fit keeps, so it "
            "would bootstrap " + std::to_string(raw["n_obs"].get<long long>()) +
            " observations instead of the fit's " +
            std::to_string((*lm.result)["n_obs"].get<long long>()) +
            ". Refusing rather than reporting a number that is not the one asked for.",
        "unsupported_option");
  }
  if (direct) {
    // boottest's r() after the WRE / multiway bootstrap (ado):
    // r(t), r(F) = t^2 and r(df_r) under `small`, r(z) and r(chi2) = z^2
    // otherwise (D1_st :1053 z .1434202311956093 with t and df_r `.`; D3_st :1203
    // t 2.286824317666255, df_r 29); r(df_r) is the smallest error-clustering
    // count less one (boottest: boottest_opt_c2 df_r 6 with 30 and 7).
    // r(N_clust) is not posted (`.`). b and V as for the one-way path: V = se^2 *
    // (N-1)/_df (getV, boottest; _df/(N-1) is 1 without small).
    const ojson* fitJson =
        lm.result.has_value() && lm.result->is_object() ? &*lm.result : nullptr;
    const bool sameSample = fitJson != nullptr && fitJson->contains("n_obs") &&
                            (*fitJson)["n_obs"].is_number() && raw.contains("n_obs") &&
                            raw["n_obs"].is_number() &&
                            (*fitJson)["n_obs"].get<double>() == raw["n_obs"].get<double>();
    if (!sameSample) {
      return engineErrorBlockM4(
          "boottest after `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
              "`: the bootstrap's sample is not the fit's (singleton or missing-value rows "
              "differ); refused rather than bootstrapping a different sample.",
          "unsupported_option");
    }
    ojson block = cobraToEstimationM4(raw, "boottest", &session);
    std::string note;
    if (multiway && vceShape.cluster == OptShapeM4b::kMany)
      note = "(bootcluster(" + joinSpaceM4(bootClusters) + ") assumed)";
    if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
      const std::string text = "Overriding estimator's cluster/robust settings with cluster(" +
                               joinSpaceM4(bootClusters) + ")";
      note = note.empty() ? text : (note + "  " + text);
    }
    if (!note.empty()) {
      const std::string cur = block.contains("note") && block["note"].is_string()
                                  ? block["note"].get<std::string>()
                                  : std::string();
      block["note"] = cur.empty() ? note : (cur + "  " + note);
    }
    block["statistic"] = directSmall ? "t" : "z";
    eval::replaceRClass(session.eval);
    const bool haveT = raw.contains("t0") && raw["t0"].is_number();
    if (haveT) {
      const double t = raw["t0"].get<double>();
      session.eval.rValues.set(directSmall ? "t" : "z", t);
      session.eval.rValues.set(directSmall ? "F" : "chi2", t * t);
      if (!directSmall) block["z"] = t;
    }
    if (directSmall && raw.contains("df_r") && raw["df_r"].is_number())
      session.eval.rValues.set("df_r", raw["df_r"].get<double>());
    if (raw.contains("p_symmetric") && raw["p_symmetric"].is_number())
      session.eval.rValues.set("p", raw["p_symmetric"].get<double>());
    if (haveT && raw.contains("se") && raw["se"].is_number() && raw.contains("beta") &&
        raw["beta"].is_number() && raw.contains("null_beta") && raw["null_beta"].is_number()) {
      const double nObs = raw["n_obs"].get<double>();
      const double kModel = static_cast<double>(directNoCons ? 0 : 1) +
                            static_cast<double>(ivWre ? lm.ivExog.size() + lm.ivEndog.size()
                                                      : lm.x.size());
      const double dfres = nObs - kModel - feDofAdj;
      const double se = raw["se"].get<double>();
      if (!directSmall || dfres > 0) {
        eval::EvalMatrix rb;
        rb.rownames = {"r1"};
        rb.colnames = {h0text};
        rb.data.push_back({raw["beta"].get<double>() - raw["null_beta"].get<double>()});
        eval::EvalMatrix rv;
        rv.rownames = {h0text};
        rv.colnames = {h0text};
        rv.data.push_back({directSmall ? se * se * (nObs - 1.0) / dfres : se * se});
        session.eval.rMatrices.emplace_back("V", std::move(rv));
        session.eval.rMatrices.emplace_back("b", std::move(rb));
      }
    }
    session.eval.rValues.set("df", 1.0);
    if (raw.contains("reps") && raw["reps"].is_number())
      session.eval.rValues.set("reps", raw["reps"].get<double>());
    session.eval.rValues.set("NH0s", 1.0);
    session.eval.rValues.set("null", 1.0);
    session.eval.rValues.set("ptol", 0.001);
    session.eval.rValues.set("level", static_cast<double>(session.level));
    session.eval.rStrings.set("clustvars", joinSpaceM4(bootClusters));
    session.eval.rStrings.set("boottype", "wild");
    if (raw.contains("weights") && raw["weights"].is_string())
      session.eval.rStrings.set("weighttype", raw["weights"].get<std::string>());
    session.eval.rStrings.set("statistic", "t");
    session.eval.rStrings.set("robust", "robust");
    for (const auto& [k, v] : parsed.opts)
      if (k == "seed" && !v.isBool && !trimM4(v.strValue).empty())
        session.eval.rStrings.set("seed", trimM4(v.strValue));
    std::vector<std::string> posted = {"NH0s", "reps", "null", "ptol", "level", "p", "df",
                                       "clustvars", "boottype", "weighttype", "statistic",
                                       "robust", "CIstr", "seed", "CI", "plot", "V", "b"};
    if (directSmall) {
      for (const char* nm : {"df_r", "F", "t"}) posted.push_back(nm);
    } else {
      for (const char* nm : {"chi2", "z"}) posted.push_back(nm);
    }
    eval::declareRPosted(session.eval, "boottest", posted);
    emitReport(raw, directSmall,
               haveT ? std::optional<double>(raw["t0"].get<double>()) : std::nullopt,
               raw.contains("df_r") && raw["df_r"].is_number()
                   ? std::optional<double>(raw["df_r"].get<double>())
                   : std::nullopt,
               std::string());
    return block;
  }
  if (!scoreModel.empty()) {
    if (!raw.contains("n_obs") || !raw["n_obs"].is_number() || !lm.result->contains("n_obs") ||
        !(*lm.result)["n_obs"].is_number() || raw["n_obs"] != (*lm.result)["n_obs"])
      return engineErrorBlockM4("boottest score bootstrap: refit sample does not match the original model", "unsupported_option");
    if (!raw.contains("z") || !raw["z"].is_number() || !raw.contains("p_symmetric") || !raw["p_symmetric"].is_number())
      return engineErrorBlockM4("boottest score bootstrap: engine did not publish a score statistic", "engine_error");
    eval::replaceRClass(session.eval);
    const double z = raw["z"].get<double>();
    session.eval.rValues.set("z", z);
    session.eval.rValues.set("chi2", z * z);
    session.eval.rValues.set("p", raw["p_symmetric"].get<double>());
    session.eval.rValues.set("df", 1.0);
    session.eval.rValues.set("reps", raw["reps"].get<double>());
    session.eval.rValues.set("NH0s", 1.0); session.eval.rValues.set("null", 1.0);
    session.eval.rValues.set("ptol", .001); session.eval.rValues.set("level", session.level);
    session.eval.rStrings.set("clustvars", bootClusters[0]);
    session.eval.rStrings.set("boottype", "score"); session.eval.rStrings.set("statistic", "z");
    session.eval.rStrings.set("robust", "robust"); session.eval.rStrings.set("weighttype", raw["weights"].get<std::string>());
    for (const auto& [k, v] : parsed.opts) if (k == "seed" && !v.isBool) session.eval.rStrings.set("seed", trimM4(v.strValue));
    eval::declareRPosted(session.eval, "boottest", {"z", "chi2", "p", "df", "reps", "NH0s", "null", "ptol", "level",
      "clustvars", "boottype", "statistic", "robust", "weighttype", "seed", "CI", "CIstr", "plot", "b", "V"});
    ojson block = cobraToEstimationM4(raw, "boottest", &session);
    block["z"] = z;
    block["statistic"] = "z";
    return block;
  }
  ojson block = cobraToEstimationM4(raw, "boottest", &session);
  if (vceShape.cluster == OptShapeM4b::kOne) {
    const std::string text =
        "Overriding estimator's cluster/robust settings with cluster(" + bootClusters[0] + ")";
    const std::string cur = block.contains("note") && block["note"].is_string()
                                ? block["note"].get<std::string>()
                                : std::string();
    block["note"] = cur.empty() ? text : (cur + "  " + text);
  }
  // boottest is r-class: it replaces all of r() (w2t C1_r :351 `mean=. N=.`
  // after a summarize) through replaceRClass, which keeps s(); this cleared
  // rStrings whole (FU-238). The set it posts after a one-hypothesis wild
  // bootstrap of a clustered least-squares fit is the `return list` at w2t
  // :356-:381 (C1), from ado: scalars NH0s reps null ptol
  // level p df df_r F t; macros clustvars boottype weighttype statistic robust
  // CIstr seed; matrices CI plot V b. All of it is declared, so a name PIET does
  // not post reads as a named r(111) (rTableAfterCommand's rUnposted), never as
  // the `.` an undeclared name gets -- that `.` is what Stata prints only for a
  // name outside its set, r(N_clust) and r(z) after least squares (C1_st :346).
  // Posted, each as boottest.ado computes it for this case:
  //  - p: the symmetric p (ado; T5, TOLERANCE.md §3.5); reps: the engine's
  //    draw count, 2^G when it enumerates (ado, repsname);
  //  - t: the statistic at the fit's estimates (ado), F = t^2 (ado);
  //    see boottestT below for when PIET knows boottest's t;
  //  - df 1 (one constraint), df_r = G - 1 (boottest, one cluster
  //    variable; C1_st :346 df_r 29 with 30 clusters), NH0s 1, null 1 (the
  //    null is imposed: this build has no `nonull`), ptol .001 and level
  //    c(level) (the syntax defaults at ado; neither option is in this
  //    command's vocabulary);
  //  - clustvars, boottype wild, weighttype (the engine's draw type), statistic
  //    t, robust robust (ado, a cluster is set), seed (ado: the seed()
  //    text; without seed() it is c(seed), which PIET does not have).
  //  - b and V (ado, boottest): the numerator R*b - r and
  //    the denominator with boottest's small-sample factor _df/(N-1) divided
  //    OUT (getV: statDenom / smallsample^2, boottest), so
  //    V = se^2 * (N-1)/_df where t = b/se is boottest's t: W1/W3/W5_st :342/
  //    :399/:463 (areg, xtreg,fe, reghdfe) agree to 1e-15. One cell each,
  //    named by the hypothesis as typed (h0text; ado, BUG-1491).
  // Not posted, so refused: CIstr and CI (no confidence set is computed), plot,
  // seed without seed(), and t/F/b/V when boottestT has no value.
  std::optional<double> boottestT;
  std::optional<double> boottestSe;  // the se with boottestT = b / boottestSe
  std::optional<double> boottestDf;  // boottest _df
  const ojson* fitJson =
      lm.result.has_value() && lm.result->is_object() ? &*lm.result : nullptr;
  const bool sameSample = fitJson != nullptr && fitJson->contains("n_obs") &&
                          (*fitJson)["n_obs"].is_number() && raw.contains("n_obs") &&
                          raw["n_obs"].is_number() &&
                          (*fitJson)["n_obs"].get<double>() == raw["n_obs"].get<double>();
  auto fitNumber = [&](const char* key) -> std::optional<double> {
    if (fitJson == nullptr || !fitJson->contains(key) || !(*fitJson)[key].is_number()) {
      return std::nullopt;
    }
    return (*fitJson)[key].get<double>();
  };
  if (raw.contains("t0") && raw["t0"].is_number()) {
    const double t0 = raw["t0"].get<double>();
    // The engine scales t0 by cluster_ssf, the reghdfe convention: (N-1)/(N-k)
    // with k = the slopes plus every absorbed level except one per FE nested in
    // the cluster -- the fit's own cluster_dof_k when it clusters on the same
    // variable. boottest uses _df = N - kZ - FEdfadj:
    //  - regress: no FE, the same k (C0 :295, C1 :346, C7-C9 :713/:769/:825);
    //  - reghdfe: FEdfadj = max(1, e(df_a)) (ado), the engine's k when the
    //    bootstrap clusters on the fit's cluster variable (E3 :1601, E4 :1668);
    //    under another cluster() the fit's df_a and the engine's nesting can part
    //    ways, so t is left unposted;
    //  - xtreg, fe: FEdfadj = 0 without dfadj (ado; this build's xtreg
    //    takes no dfadj), so _df = N - kZ while the engine charged one more
    //    (E6_st :1792 t 1.268217344494529 against the engine's 1.2646499187370834
    //    = E4's, FU-742): t0 * sqrt((N - kZ) / (N - cluster_dof_k)), kZ the
    //    engine's slope count k_vars (runXtreg rewrites the reply's df_m to
    //    xtreg's e(df_m)), N and cluster_dof_k the fit's, on the fit's cluster.
    //  - areg (FU-732): FEdfadj = the number of absorbed groups (ado), an FE
    //    nested in the cluster included, which is the fit's own clustered dof
    //    (BUG-568, --keep-nested-fe-dof) and not cluster_ssf's: t = (b - null) / the areg
    //    table's se, on the fit's cluster (W1_st :342 t 1.694959401854210;
    //    E2_areg_id_st :1535, id nested in id).
    const bool sameCluster = lm.cluster.size() == 1 && lm.cluster[0] == bootClusters[0];
    const std::optional<double> n = sameSample ? fitNumber("n_obs") : std::nullopt;
    const std::optional<double> kEngine = fitNumber("cluster_dof_k");
    const double seEngine =
        raw.contains("se") && raw["se"].is_number() ? raw["se"].get<double>() : std::nan("");
    if (lm.fe.empty()) {
      boottestT = t0;
      if (n && kEngine && *n - *kEngine > 0) {
        boottestSe = seEngine;
        boottestDf = *n - *kEngine;
      }
    } else if (sameCluster && lm.stataCmd == "reghdfe") {
      boottestT = t0;
      if (n && kEngine && *n - *kEngine > 0) {
        boottestSe = seEngine;
        boottestDf = *n - *kEngine;
      }
    } else if (sameCluster && lm.stataCmd == "xtreg" && lm.cobraCmd == "feols") {
      const std::optional<double> kz = fitNumber("k_vars");
      if (n && kz && kEngine && *n - *kz > 0 && *n - *kEngine > 0) {
        const double scale = std::sqrt((*n - *kz) / (*n - *kEngine));
        boottestT = t0 * scale;
        boottestSe = seEngine / scale;
        boottestDf = *n - *kz;
      }
    } else if (sameCluster && lm.stataCmd == "areg" && n && kEngine && *n - *kEngine > 0 &&
               fitJson->contains("se") && (*fitJson)["se"].is_object() &&
               (*fitJson)["se"].contains(param) && (*fitJson)["se"][param].is_number() &&
               raw.contains("beta") && raw["beta"].is_number() && raw.contains("null_beta") &&
               raw["null_beta"].is_number()) {
      const double seFit = (*fitJson)["se"][param].get<double>();
      if (seFit > 0) {
        boottestT = (raw["beta"].get<double>() - raw["null_beta"].get<double>()) / seFit;
        boottestSe = seFit;
        boottestDf = *n - *kEngine;
      }
    }
  }
  // K6 (FIX-SPEC.md): the block shows the t boottest reports; where PIET does
  // not know it, the engine's t0 stays on display and r(t) refuses.
  if (boottestT.has_value()) block["t0"] = *boottestT;
  eval::replaceRClass(session.eval);
  if (raw.contains("p_symmetric") && raw["p_symmetric"].is_number()) {
    session.eval.rValues.set("p", raw["p_symmetric"].get<double>());
  }
  if (boottestT.has_value()) {
    session.eval.rValues.set("t", *boottestT);
    session.eval.rValues.set("F", *boottestT * *boottestT);
  }
  std::string ciStr;
  if (boottestT.has_value() && boottestSe.has_value() && boottestDf.has_value() &&
      std::isfinite(*boottestSe) && raw.contains("beta") && raw["beta"].is_number() &&
      raw.contains("null_beta") && raw["null_beta"].is_number() && sameSample) {
    const double nObs = raw["n_obs"].get<double>();
    const double vb = *boottestSe * *boottestSe * (nObs - 1.0) / *boottestDf;
    eval::EvalMatrix rb;
    rb.rownames = {"r1"};
    rb.colnames = {h0text};
    rb.data.push_back({raw["beta"].get<double>() - raw["null_beta"].get<double>()});
    eval::EvalMatrix rv;
    rv.rownames = {h0text};
    rv.colnames = {h0text};
    rv.data.push_back({vb});
    session.eval.rMatrices.emplace_back("V", std::move(rv));
    session.eval.rMatrices.emplace_back("b", std::move(rb));
    // BUG-1174: r(CI), r(CIstr) and r(plot), by test inversion over the null
    // (boottestCiM4b): the same bootstrap re-run with --null r, the same draws (the
    // same --rng-mt64 state; an enumerated universe is the same in any case), its
    // symmetric p read back -- on the fit's own sample, or not at all.
    if (raw.contains("reps") && raw["reps"].is_number()) {
      auto pAt = [&](double r) -> double {
        std::vector<std::string> a = args;
        char rb17[64];
        std::snprintf(rb17, sizeof(rb17), "%.17g", r);
        bool replaced = false;
        for (size_t i = 0; i + 1 < a.size(); ++i)
          if (a[i] == "--null") {
            a[i + 1] = rb17;
            replaced = true;
          }
        if (!replaced) {
          a.push_back("--null");
          a.push_back(rb17);
        }
        try {
          const ojson rr = runCobraJsonM4(a);
          if (rr.contains("p_symmetric") && rr["p_symmetric"].is_number() &&
              rr.contains("n_obs") && rr["n_obs"] == raw["n_obs"])
            return rr["p_symmetric"].get<double>();
        } catch (const CobraError&) {
        }
        return std::nan("");
      };
      if (const auto ci = boottestCiM4b(pAt, raw["beta"].get<double>(), vb,
                                        raw["null_beta"].get<double>(),
                                        static_cast<double>(session.level),
                                        raw["reps"].get<double>())) {
        eval::EvalMatrix cm;
        cm.colnames = {"lo", "hi"};
        for (const auto& [l, h] : ci->sets) {
          cm.rownames.push_back(h0text);
          cm.data.push_back({l, h});
        }
        eval::EvalMatrix pm;
        pm.colnames = {"c1", "c2"};
        for (size_t k = 0; k < ci->plotX.size(); ++k) {
          pm.rownames.push_back("r" + std::to_string(k + 1));
          pm.data.push_back({ci->plotX[k], ci->plotY[k]});
        }
        ciStr = boottestCiStrM4b(*ci);
        session.eval.rMatrices.emplace_back("CI", std::move(cm));
        session.eval.rMatrices.emplace_back("plot", std::move(pm));
        session.eval.rStrings.set("CIstr", ciStr);
      }
    }
  }
  session.eval.rValues.set("df", 1.0);
  if (raw.contains("n_clusters") && raw["n_clusters"].is_number()) {
    session.eval.rValues.set("df_r", raw["n_clusters"].get<double>() - 1.0);
  }
  if (raw.contains("reps") && raw["reps"].is_number()) {
    session.eval.rValues.set("reps", raw["reps"].get<double>());
  }
  session.eval.rValues.set("NH0s", 1.0);
  session.eval.rValues.set("null", 1.0);
  session.eval.rValues.set("ptol", 0.001);
  session.eval.rValues.set("level", static_cast<double>(session.level));
  session.eval.rStrings.set("clustvars", bootClusters[0]);
  session.eval.rStrings.set("boottype", "wild");
  if (raw.contains("weights") && raw["weights"].is_string()) {
    session.eval.rStrings.set("weighttype", raw["weights"].get<std::string>());
  }
  session.eval.rStrings.set("statistic", "t");
  session.eval.rStrings.set("robust", "robust");
  for (const auto& [k, v] : parsed.opts) {
    if (k == "seed" && !v.isBool && !trimM4(v.strValue).empty()) {
      session.eval.rStrings.set("seed", trimM4(v.strValue));
    }
  }
  eval::declareRPosted(session.eval, "boottest",
                       {"NH0s", "reps", "null", "ptol", "level", "p", "df", "df_r", "F", "t",
                        "clustvars", "boottype", "weighttype", "statistic", "robust", "CIstr",
                        "seed", "CI", "plot", "V", "b"});
  emitReport(raw, /*small=*/true, boottestT,
             raw.contains("n_clusters") && raw["n_clusters"].is_number()
                 ? std::optional<double>(raw["n_clusters"].get<double>() - 1.0)
                 : std::nullopt,
             ciStr);
  return block;
}

ojson runBoottest(const eval::ParsedCommand& parsed, SessionState& session) {
  return runBoottest(parsed, session, nullptr);
}

// ===========================================================================
// tobit (BS-069, BETA D2 batch): `tobit depvar indepvars [if], ll[(#)]
// ul[(#)] [level(#)]` -> `cobra tobit --ll/--ul`. Oracle:
// internal capture d2-oracle-stata.log
// (D2a: ul(15000), D2b: ll(11000)) — both coefficients AND the reported
// variance/uncensored/censored counts match cobra's `tobit` byte-for-byte
// (live-verified against this exact engine build, no convention gap here
// unlike D1's ivregress SE denominator). At least one of ll/ul is required
// (matches both Stata's own requirement and cobra's own usage banner: "at
// least one of --ll/--ul required").

}  // namespace piet::handlers
