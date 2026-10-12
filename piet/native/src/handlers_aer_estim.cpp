// handlers_aer_estim.cpp — AER scope-1 bundle H (ESTIM).
//
// Two kinds of command live here and they are worth telling apart:
//
//   E1  aliases onto estimators PIET already has. `areg` IS regress with an
//       absorb(); `ivreg2`/`xtivreg2` are ivregress under community spellings.
//       These normalise options and delegate -- no new mathematics.
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
// milf-coord/plans/ERRATA-AER-H-SPECS-20260904.md.
//
// The engine-backed half of E2 (tsfilter hp, lpoly, lowess, pca) refuses
// honestly here until the cobra subcommands land: the handler ASKS the engine
// what it supports rather than assuming, so a PIET built against an older cobra
// says so instead of producing a wrong number or a crash.

#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "estimation_m4_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

ojson aerError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// r(198) "invalid syntax". The rc is derived from the error CODE by the BS-100
// taxonomy, not from a reason string -- `engine_error` lands in the generic
// bucket and reads back as rc 1, which is what this wrapper exists to avoid.
ojson aerSyntaxError(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// A refusal whose Stata rc is known but which the coarse message rules do not
// recognise. `reason` is the only channel carrying an exact rc, and it outranks
// the coarse mapping -- without it these come back as the generic rc 1.
ojson aerErrorReason(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// ---------------------------------------------------------------------------
// Reading

// Open the columns this command needs. Mirrors handlers_aer_util.cpp's
// ReadScope: try the section-38 projection first, fall back to the resident
// head, and never half-succeed -- a projection that is missing any requested
// column is discarded rather than silently answering about fewer columns.
class ColumnScope {
 public:
  ColumnScope(SessionState& session, const eval::ParsedCommand& parsed,
              const std::vector<std::string>& cols, const std::string& verb) {
    if (!session.hasData() || !session.currentHbin().has_value()) {
      err_ = ojson(eval::noDataLoaded());
      return;
    }
    const bool wantFilter = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                            (parsed.inRange.has_value() && !parsed.inRange->empty());
    try {
      if (wantFilter) {
        InRangeFilterResult f =
            applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
        if (!f.ok) {
          err_ = inRangeRejectError(f, parsed.inRange.value_or(std::string()), verb);
          return;
        }
        if (f.dataPath != *session.currentHbin()) {
          session.noteProjectionScratch(f.dataPath);
          owned_ = std::make_unique<Datastore>(f.dataPath);
          ds_ = owned_.get();
          return;
        }
        ds_ = session.datastore();
        return;
      }
      std::vector<const std::string*> exprs;
      exprs.reserve(cols.size());
      for (const auto& c : cols) exprs.push_back(&c);
      if (auto want = referencedColumns(session, exprs, "")) {
        std::string projPath;
        std::unique_ptr<Datastore> proj =
            fetchColumns(session, *want, session.workdir(), &projPath);
        if (proj) {
          bool complete = true;
          for (const auto& c : cols) {
            if (!proj->has(c)) {
              complete = false;
              break;
            }
          }
          if (complete) owned_ = std::move(proj);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      err_ = aerError(verb + ": could not read the data -- " + std::string(e.what()), "data_io");
    }
  }

  Datastore* get() const { return ds_; }
  const ojson& error() const { return err_; }
  bool failed() const { return !err_.is_null() || ds_ == nullptr; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  ojson err_;
};

// Pull one numeric column as doubles, NaN for missing. Returns false when the
// column is absent or is a string/factor the statistic cannot use.
bool numericColumn(const Datastore& ds, const std::string& name, std::vector<double>* out,
                   std::string* err) {
  if (!ds.has(name)) {
    *err = "variable " + name + " not found";
    return false;
  }
  const cecon::Column& c = ds.col(name);
  const size_t n = ds.nRows();
  out->assign(n, std::numeric_limits<double>::quiet_NaN());
  // DType carries exactly two shapes at this layer: F64 and Factor. A factor's
  // code IS its numeric value for these statistics (the same reading `egen rank`
  // takes), and a negative code is the missing sentinel.
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    if (codes == nullptr) {
      *err = name + ": column has no readable payload";
      return false;
    }
    for (size_t i = 0; i < n; ++i)
      (*out)[i] = (codes[i] < 0) ? std::numeric_limits<double>::quiet_NaN()
                                 : static_cast<double>(codes[i]);
    return true;
  }
  const double* d = c.f64();
  if (d == nullptr) {
    *err = name + ": column has no readable payload";
    return false;
  }
  for (size_t i = 0; i < n; ++i) (*out)[i] = d[i];
  return true;
}

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

// Does the engine expose this subcommand? Asked, not assumed: a PIET built
// against a cobra without the bundle-H verbs must say so rather than fail
// somewhere less legible.
bool engineHasSubcommand(const std::string& sub) {
  try {
    nlohmann::json opts = runCobraJson({"options", "--json"});
    if (opts.is_object() && opts.contains(sub)) return true;
    if (opts.is_object() && opts.contains("commands") && opts["commands"].is_object())
      return opts["commands"].contains(sub);
    if (opts.is_array()) {
      for (const auto& e : opts)
        if (e.is_string() && e.get<std::string>() == sub) return true;
    }
  } catch (const std::exception&) {
    return false;
  }
  return false;
}

ojson engineTooOld(const std::string& stataCmd, const std::string& sub) {
  return aerError(stataCmd + " requires a cobra build that provides the `" + sub +
                      "` subcommand; this engine does not. Rebuild the engine from a tree that "
                      "includes AER bundle H1, or point $COBRA_BIN at one.",
                  "unsupported_command");
}

}  // namespace

// ===========================================================================
// E1 — aliases

// areg IS regress with an absorb(), so it routes to the same handler with a
// different stataCmd. Everything that makes areg areg (the e(df_a) absorbed
// degrees of freedom, the "Linear regression, absorbing indicators" header)
// belongs to runRegress's areg arm, not to a second copy of the estimator.
// areg's clustered dof (it charges an FE nested in the cluster, where reghdfe
// and xtreg,fe exempt it) is runRegress's areg arm too (BUG-568); that is what
// retired the BUG-684 stop-gap which used to refuse weighted areg + cluster here.
ojson runAreg(const eval::ParsedCommand& parsed, SessionState& session) {
  // BUG-569: areg's option abbreviations. w2b_estim.log measures `a(id)`,
  // `ab(id)`, `abs(id)` (areg_a(id) .. areg_abs(id)), `cl(id)` (areg_a_cl) and
  // `r` (areg_a_r) all at rc 0, but areg has no row in the generated minimum
  // table the dispatcher resolves abbreviations from, and runRegress only reads
  // the full names -- so each of them was refused "option `a` not allowed".
  // Canonicalized here to the names runRegress reads, at the measured floors
  // (absorb 1, cluster 2, robust 1); anything shorter stays as typed and is
  // refused by runRegress's unknown-option net.
  eval::ParsedCommand canon = parsed;
  auto isAbbrev = [](const std::string& key, const std::string& full, size_t floor) {
    return key.size() >= floor && key.size() <= full.size() &&
           full.compare(0, key.size(), key) == 0;
  };
  bool sawAbsorb = false;
  for (auto& [k, v] : canon.opts) {
    (void)v;
    if (isAbbrev(k, "absorb", 1)) {
      k = "absorb";
      sawAbsorb = true;
    } else if (isAbbrev(k, "cluster", 2)) {
      k = "cluster";
    } else if (isAbbrev(k, "robust", 1)) {
      k = "robust";
    }
  }
  // Stata: areg without absorb() is r(198), not a silent plain regression.
  if (!sawAbsorb) return aerSyntaxError("option absorb() required");

  return runRegress(canon, session, "areg");
}

// ivreg2 / xtivreg2 -> runIvRegress. The spellings differ from ivregress in
// their option vocabulary, not in their estimator, so this normalises and
// delegates.
ojson runIvreg2(const eval::ParsedCommand& parsed, SessionState& session,
                const std::string& stataCmd) {
  eval::ParsedCommand p = parsed;

  bool robust = false, gmm2s = false;
  std::string cluster;
  bool fe = false, small = false;
  std::vector<std::pair<std::string, eval::OptValue>> kept;
  for (const auto& kv : p.opts) {
    const std::string& k = kv.first;
    // BUG-569 (review round): the value shape, as runRegress checks it. A
    // valued `r(x)` used to count as robust, and a bare `cl` or an empty
    // `cl()` set no cluster, so the fit ran iid at rc 0.
    if (k == "r" || k == "robust") {
      if (!kv.second.isBool) return aerSyntaxError("option `robust` takes no argument");
      robust = true;
    } else if (k == "cl" || k == "cluster") {
      if (kv.second.isBool || strutil::trim(kv.second.strValue).empty()) {
        return aerSyntaxError("option `cluster` requires a variable: cluster(varname)");
      }
      cluster = kv.second.strValue;
    } else if (k == "gmm2s") {
      gmm2s = true;
    } else if (k == "fe") {
      fe = true;
    } else if (k == "small") {
      small = true;
    } else {
      kept.push_back(kv);
    }
  }

  // `gmm2s` alone is not GMM in the sense ivregress means: without a robust or
  // clustered weight matrix the two-step estimator collapses to 2SLS, so the
  // honest translation is 2sls plus a note rather than a silent method switch.
  const bool weighted = robust || !cluster.empty();
  const std::string method = (gmm2s && weighted) ? "gmm" : "2sls";

  // B4 P0-C: `eval::OptValue{}` here was a WRONG NUMBER, not a style nit. A
  // default-constructed OptValue has isBool == false (tokenizer.h:33), and the
  // only reader of this key — vcovFromOpts (estimation_m4.cpp:509) — matches
  // `k == "robust" && v.isBool && v.boolValue`. So the flag this loop had just
  // parsed was re-emitted in a shape its consumer could not see, `--vcov hc1`
  // was never appended, and `ivreg2 ..., robust` returned the IID standard
  // error while the census certified the option as supported. Measured on
  // auto.csv: se[mpg] 65.3314331 for bare AND robust AND small; StataBE gives
  // 65.33143 / 70.08472 / 66.23260 (oracle_p.log:250, :297, :341).
  if (robust) kept.emplace_back("robust", eval::OptValue::ofBool(true));
  if (!cluster.empty()) {
    eval::OptValue cv;
    cv.strValue = cluster;
    kept.emplace_back("cluster", cv);
  }
  if (fe) {
    if (!session.panelvar.has_value()) {
      return aerError(stataCmd + ": option fe requires a panel setting -- run xtset first",
                      "syntax_error");
    }
    eval::OptValue av;
    av.strValue = *session.panelvar;
    kept.emplace_back("absorb", av);
  }
  p.opts = kept;

  // ivreg2's default inference is large-sample (z); `small` asks for the
  // t/F small-sample correction. ivregress's absorb path forces --small
  // unconditionally, which is right for `ivreghdfe` and wrong here, so the
  // flag is threaded (see the forceSmall argument below) rather than inherited.
  // B4 P0-C, the OTHER half — and it is not the same defect as `robust` above.
  // This line used to read `(void)small;` at the bottom of the function: the
  // flag was parsed, kept out of `kept` so it could not be rejected, and then
  // explicitly DISCARDED. The comment above it claimed "the flag is threaded
  // rather than inherited", which described a wiring that did not exist —
  // `--small` reached cobra only through `allowAbsorb`, i.e. only for
  // `ivreg2 ..., fe`. So `ivreg2 ..., small` certified an option it dropped.
  // Threading it is what the comment already said should happen.
  // `smallDefault=fe` reproduces exactly what `allowAbsorb` used to imply here: ivreg2 reports
  // the small-sample correction when it is absorbing, and not otherwise. It is spelled out now
  // because the two meanings were one bit until 2026-09-09 (BUG-474's split).
  ojson out = runIvRegress(method, p, /*allowAbsorb=*/fe, stataCmd, session,
                           /*smallDefault=*/fe, /*forceSmall=*/small);
  if (gmm2s && !weighted && out.is_object() && out.value("type", std::string()) != "error") {
    out["notes"].push_back(
        "gmm2s without robust or cluster() is 2SLS: the two-step weight matrix "
        "equals the 2SLS one under homoskedasticity.");
  }
  return out;
}

// ===========================================================================
// E2 — computed here

// spearman: rank correlation with Stata's exact p, a four-parameter beta fit to
// the first four permutation moments of T = sum x_i y_i on centred midranks
// ([R] spearman, Methods and formulas).
ojson runSpearman(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.size() != 2) {
    return aerError("spearman: this build supports exactly two variables", "unsupported_command");
  }
  ColumnScope scope(session, parsed, parsed.varlist, "spearman");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  std::vector<double> xs, ys;
  std::string err;
  if (!numericColumn(ds, parsed.varlist[0], &xs, &err)) return aerError("spearman: " + err);
  if (!numericColumn(ds, parsed.varlist[1], &ys, &err)) return aerError("spearman: " + err);

  // Casewise deletion: a pair contributes only when BOTH are present.
  std::vector<double> x, y;
  for (size_t i = 0; i < xs.size(); ++i) {
    if (std::isnan(xs[i]) || std::isnan(ys[i])) continue;
    x.push_back(xs[i]);
    y.push_back(ys[i]);
  }
  const size_t n = x.size();
  if (n < 3) return aerError("spearman: too few observations", "engine_error");

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
  if (S2 == 0.0 || T2 == 0.0) {
    return aerError("spearman: a variable is constant over the sample", "engine_error");
  }

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

  session.eval.rValues.set("N", static_cast<double>(n));
  session.eval.rValues.set("rho", rho);
  session.eval.rValues.set("p", p);
  session.eval.rValues.set("p_l", p_l);
  session.eval.rValues.set("p_u", p_u);

  char buf[256];
  std::ostringstream out;
  std::snprintf(buf, sizeof(buf), "Number of observations = %6d", static_cast<int>(n));
  out << "\n" << buf << "\n";
  std::snprintf(buf, sizeof(buf), "        Spearman's rho = %.4f", rho);
  out << buf << "\n\n";
  out << "Test of H0: " << parsed.varlist[0] << " and " << parsed.varlist[1]
      << " are independent\n";
  std::snprintf(buf, sizeof(buf), "                  Prob = %.4f", p);
  out << buf << "\n";
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
  out << "\nSkewness and kurtosis tests for normality\n";
  out << "                                                         ----- Joint test -----\n";
  out << "    Variable |       Obs   Pr(skewness)   Pr(kurtosis)   Adj chi2(2)  Prob>chi2\n";
  out << "-------------+-----------------------------------------------------------------\n";
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
    std::snprintf(buf, sizeof(buf), "%12s |%10d   ", r.name.substr(0, 12).c_str(),
                  static_cast<int>(r.N));
    out << buf << f4(r.pSkew, 12) << "   " << f4(r.pKurt, 12) << "   " << f2(r.chi2, 11) << "   "
        << f4(r.pChi2, 8) << "\n";
  }
  return displayBlock(out.str());
}

// signrank: Wilcoxon matched-pairs signed-rank test ([R] signrank).
ojson runSignrank(const eval::ParsedCommand& parsed, SessionState& session) {
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

  ColumnScope scope(session, parsed, {lhs, rhs}, "signrank");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  std::vector<double> a, b;
  std::string err;
  if (!numericColumn(ds, lhs, &a, &err)) return aerError("signrank: " + err);
  if (!numericColumn(ds, rhs, &b, &err)) return aerError("signrank: " + err);

  std::vector<double> d;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::isnan(a[i]) || std::isnan(b[i])) continue;
    d.push_back(a[i] - b[i]);
  }
  const size_t n = d.size();
  if (n == 0) return aerError("signrank: no observations", "engine_error");

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
  return displayBlock(out.str());
}

// centile: percentiles with Stata's default binomial-interpolated confidence
// interval ([R] centile, Methods and formulas / Default case).
ojson runCentile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.size() != 1) {
    return aerError("centile: this build supports one variable", "unsupported_command");
  }
  static const std::unordered_set<std::string> kKnown = {"centile", "c", "level"};
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown)) return ojson(*e);

  std::vector<double> qs;
  double level = 95.0;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "centile" || k == "c") {
      std::istringstream is(v.strValue);
      double q;
      while (is >> q) qs.push_back(q);
    } else if (k == "level") {
      try {
        level = std::stod(v.strValue);
      } catch (const std::exception&) {
        return aerSyntaxError("centile: level() must be numeric");
      }
    }
  }
  if (qs.empty()) qs.push_back(50.0);
  for (double q : qs)
    if (!(q > 0.0 && q < 100.0)) return aerSyntaxError("centile: centiles must be between 0 and 100");

  ColumnScope scope(session, parsed, parsed.varlist, "centile");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  std::vector<double> raw;
  std::string err;
  if (!numericColumn(ds, parsed.varlist[0], &raw, &err)) return aerError("centile: " + err);
  std::vector<double> xv;
  for (double d : raw)
    if (!std::isnan(d)) xv.push_back(d);
  std::sort(xv.begin(), xv.end());
  const size_t n = xv.size();
  if (n < 2) return aerError("centile: too few observations", "engine_error");
  const double nn = static_cast<double>(n);
  auto xat = [&](long long i) -> double {  // 1-based, clamped as the manual defines
    if (i < 1) return xv.front();
    if (static_cast<size_t>(i) > n) return xv.back();
    return xv[static_cast<size_t>(i) - 1];
  };

  const double alpha = (100.0 - level) / 200.0;
  // F_i = betai(n-i, i+1, 1-q/100) for i < n, and F_n = 1. The manual writes
  // F_{n+1} = 1; the plan's F_n = 1 is the correct computational special case,
  // because at i = n the first shape parameter is 0 and betai cannot be called,
  // and index n+1 is never consulted.
  std::ostringstream out;
  char buf[256];
  out << "\n";
  out << "                                                          Binom. interp.   \n";
  out << "    Variable |       Obs  Percentile    Centile        [" << static_cast<int>(level)
      << "% conf. interval]\n";
  out << "-------------+-------------------------------------------------------------\n";

  session.eval.rValues.set("N", nn);
  session.eval.rValues.set("n_cent", static_cast<double>(qs.size()));

  for (size_t qi = 0; qi < qs.size(); ++qi) {
    const double q = qs[qi];
    const double R = (nn + 1.0) * q / 100.0;
    const long long ri = static_cast<long long>(std::floor(R));
    const double f = R - static_cast<double>(ri);
    const double cq = xat(ri) + f * (xat(ri + 1) - xat(ri));

    // F_{-1} = 0, NOT F_0 = 0. The manual's boundary is at index -1; F_0 is a
    // real value (0.02246709 for auto price at q=5) and it is read twice -- by
    // the t search AND by g = (alpha - F_t)/(F_{t+1} - F_t). Zeroing it leaves
    // the t search right and makes g wrong, which moves ONLY the lower bound of
    // the first centile: measured 3292.8187 against Stata's 3291.232, while
    // every other bound in the same table stayed correct. A single-pin test
    // would have missed it.
    auto F = [&](long long i) -> double {
      if (i < 0) return 0.0;
      if (static_cast<size_t>(i) >= n) return 1.0;
      return cecon::betai(nn - static_cast<double>(i), static_cast<double>(i) + 1.0,
                          1.0 - q / 100.0);
    };
    long long t = 0;
    for (long long i = 0; static_cast<size_t>(i) <= n; ++i) {
      if (F(i) <= alpha && alpha < F(i + 1)) {
        t = i;
        break;
      }
    }
    long long u = static_cast<long long>(n);
    for (long long i = static_cast<long long>(n); i >= 1; --i) {
      if (1.0 - F(i) <= alpha && alpha < 1.0 - F(i - 1)) {
        u = i;
        break;
      }
    }
    const double gd = F(t + 1) - F(t);
    const double g = (gd != 0.0) ? (alpha - F(t)) / gd : 0.0;
    const double hd = F(u) - F(u - 1);
    const double h = (hd != 0.0) ? (alpha - 1.0 + F(u)) / hd : 0.0;
    const double lb = xat(t + 1) + g * (xat(t + 2) - xat(t + 1));
    const double ub = xat(u + 1) - h * (xat(u + 1) - xat(u));

    // %.7g throughout: Stata's default display format for these columns is
    // %9.0g, i.e. seven significant digits, and the widths below are measured
    // off its own output rather than guessed.
    if (qi == 0) {
      std::snprintf(buf, sizeof(buf), "%12s |%10d %10.7g %11.7g %15.7g %11.7g\n",
                    parsed.varlist[0].substr(0, 12).c_str(), static_cast<int>(n), q, cq, lb, ub);
    } else {
      std::snprintf(buf, sizeof(buf), "%12s |%10s %10.7g %11.7g %15.7g %11.7g\n", "", "", q, cq,
                    lb, ub);
    }
    out << buf;

    const std::string sfx = std::to_string(qi + 1);
    session.eval.rValues.set("c_" + sfx, cq);
    session.eval.rValues.set("lb_" + sfx, lb);
    session.eval.rValues.set("ub_" + sfx, ub);
  }
  return displayBlock(out.str());
}

// ===========================================================================
// E2 engine-backed — H1 has landed, so these now compute.
//
// Each still ASKS the engine whether its subcommand exists before using it: a
// PIET built against an older cobra says so by name rather than failing
// somewhere less legible.

namespace {

// mintScratchPath always names a ".hbin"; a setcol payload is a CSV. Same body
// and same reason as handlers_aer_results.cpp's copy -- registering the minted
// .hbin and then writing a .csv beside it would leave the file that actually
// exists unregistered, which is the orphan class the registry exists to close.
std::string estimMintCsv(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";
  registerScratchMint(csv);
  return csv;
}

// The headerless one-value-per-row payload `cobra setcol` reads. An EMPTY line
// is how a missing value is spelled there; "nan" would land a literal token in
// a numeric column.
bool estimWriteCsv(const std::string& path, const std::vector<double>& v) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double d : v) {
    if (!std::isnan(d)) f << d;
    f << "\n";
  }
  return static_cast<bool>(f);
}

// Write one f64 column into the session frame.
bool estimWriteColumn(SessionState& session, const std::string& name,
                      const std::vector<double>& vals, ojson* err) {
  const std::string csv = estimMintCsv(session.workdir(), "aerestim");
  if (!estimWriteCsv(csv, vals)) {
    *err = aerError("could not write the values payload", "data_io");
    return false;
  }
  const std::string out = mintScratchPath(session.workdir(), "aerestim");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name,
                                     "--values", csv, "--dtype", "f64", "--new", "--out", out});
  if (!r.ok) {
    *err = aerError("could not create " + name, "data_io");
    return false;
  }
  // The engine's writer creates --out before it can detect a write failure, and
  // a full scratch volume has reached PIET as a ZERO exit code twice. A
  // session.load on a path that is not there records the head anyway and hands
  // the next open a missing mapping -- a crash rather than a diagnostic.
  if (!cecon::native_exists(out)) {
    *err = aerError("failed writing output dataset (disk full or I/O error)", "data_io");
    return false;
  }
  session.load(out);
  return true;
}

std::vector<double> jsonDoubles(const nlohmann::json& j) {
  std::vector<double> v;
  if (!j.is_array()) return v;
  v.reserve(j.size());
  for (const auto& e : j)
    v.push_back(e.is_number() ? e.get<double>() : std::numeric_limits<double>::quiet_NaN());
  return v;
}

}  // namespace

// `tsfilter hp NEW = Y [, smooth(#) trend(NEW2)]`
ojson runTsfilterHp(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("hpfilter")) return engineTooOld("tsfilter hp", "hpfilter");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  // varlist is `hp NEW = Y`; the filter word was already checked by the arm.
  std::string cycleName, yname;
  {
    std::string joined;
    for (std::size_t i = 1; i < parsed.varlist.size(); ++i) joined += parsed.varlist[i] + " ";
    const std::size_t eq = joined.find('=');
    if (eq == std::string::npos)
      return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");
    cycleName = strutil::trim(joined.substr(0, eq));
    yname = strutil::trim(joined.substr(eq + 1));
  }
  if (cycleName.empty() || yname.empty())
    return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");

  std::string trendName, smooth;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "trend") trendName = v.strValue;
    else if (k == "smooth") smooth = v.strValue;
  }
  // tsset/xtset is required: Stata's tsfilter is r(111) without one.
  if (!session.timevar.has_value())
    return aerErrorReason("tsfilter hp: you must tsset your data first", 111);

  std::vector<std::string> args = {"hpfilter", "--data", *session.currentHbin(), "--y", yname};
  if (!smooth.empty()) { args.push_back("--lambda"); args.push_back(smooth); }
  else {
    // The Ravn-Uhlig default is a function of the TIME-UNIT FORMAT, not a
    // constant 1600 -- %tw is 33,177,600 and %td is 110,930,628,906.25.
    // The display format is carried on the varLabels plane under a reserved
    // prefix, not on a named accessor.
    const std::string fmt = session.eval.varLabels.get(
        std::string(SessionState::kEvalVarFormatPrefix) + *session.timevar, "");
    if (!fmt.empty()) { args.push_back("--tsfmt"); args.push_back(fmt); }
  }
  if (session.panelvar.has_value()) { args.push_back("--panel"); args.push_back(*session.panelvar); }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("tsfilter hp: " + cobraErrorMessage(ce), ce.code);
  }

  ojson err;
  if (!estimWriteColumn(session, cycleName, jsonDoubles(raw["cycle"]), &err)) return err;
  if (!trendName.empty() && !estimWriteColumn(session, trendName, jsonDoubles(raw["trend"]), &err))
    return err;

  session.eval.rValues.set("smooth", raw.value("lambda", 1600.0));
  session.eval.rStrings.set("method", "Hodrick-Prescott");
  session.eval.rStrings.set("varlist", yname);
  session.eval.rStrings.set("filterlist", cycleName);
  if (!trendName.empty()) session.eval.rStrings.set("trendlist", trendName);

  // tsfilter is otherwise SILENT; a short panel is the one exception, and it is
  // a note rather than a refusal (measured: rc 0, that panel left missing).
  std::string noteText;
  if (raw.contains("notes") && raw["notes"].is_array())
    for (const auto& n : raw["notes"]) noteText += "note: " + n.get<std::string>() + "\n";
  ojson mut = mutationResult(session, "tsfilter",
                              trendName.empty() ? std::vector<std::string>{cycleName}
                                                : std::vector<std::string>{cycleName, trendName},
                              {});
  if (noteText.empty()) return mut;
  ojson arr = ojson::array();
  arr.push_back(displayBlock(noteText));
  arr.push_back(mut);
  return arr;
}

// `lowess Y X [, bwidth() mean noweight generate(NEW) nograph]`
ojson runLowess(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("lowess")) return engineTooOld("lowess", "lowess");
  if (parsed.varlist.size() < 2) return aerSyntaxError("lowess: syntax is `lowess yvar xvar`");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  std::string gen, bw;
  bool mean = false, noweight = false, adjust = false, logit = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "generate" || k == "gen") gen = v.strValue;
    else if (k == "bwidth" || k == "bw") bw = v.strValue;
    else if (k == "mean") mean = true;
    else if (k == "noweight") noweight = true;
    else if (k == "adjust") adjust = true;
    else if (k == "logit") logit = true;
  }

  std::vector<std::string> args = {"lowess", "--data",     *session.currentHbin(),
                                   "--y",    parsed.varlist[0], "--x", parsed.varlist[1]};
  if (!bw.empty()) { args.push_back("--bw"); args.push_back(bw); }
  if (mean) args.push_back("--mean");
  if (noweight) args.push_back("--noweight");

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("lowess: " + cobraErrorMessage(ce), ce.code);
  }
  std::vector<double> sm = jsonDoubles(raw["smooth"]);

  // adjust, then logit -- ORDER MATTERS and ADJ_LOGIT does adjust first.
  if (adjust) {
    double sy = 0, ss = 0;
    long long ny = 0, ns = 0;
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(parsed.varlist[0])) {
      std::vector<double> yv;
      std::string e;
      if (numericColumn(*ds, parsed.varlist[0], &yv, &e))
        for (double d : yv)
          if (!std::isnan(d)) { sy += d; ++ny; }
    }
    for (double d : sm)
      if (!std::isnan(d)) { ss += d; ++ns; }
    if (ny > 0 && ns > 0 && ss != 0) {
      const double fac = (sy / static_cast<double>(ny)) / (ss / static_cast<double>(ns));
      for (double& d : sm)
        if (!std::isnan(d)) d *= fac;
    }
  }
  if (logit) {
    // SUBSTITUTION, not a clamp: the threshold is 1e-4 but the replacement is
    // 1/N. A literal clamp to [1e-4, 0.9999] gives ln(1e-4/0.9999) = -9.2102
    // where Stata gives ln((1/N)/(1-1/N)).
    long long n = 0;
    for (double d : sm)
      if (!std::isnan(d)) ++n;
    if (n > 0) {
      const double adj = 1.0 / static_cast<double>(n);
      for (double& d : sm) {
        if (std::isnan(d)) continue;
        if (d < 0.0001) d = adj;
        else if (d > 0.9999) d = 1.0 - adj;
        d = std::log(d / (1.0 - d));
      }
    }
  }

  session.eval.rValues.set("bwidth", raw.value("bwidth", 0.8));  // the SUBSTITUTED value
  session.eval.rValues.set("N", static_cast<double>(raw.value("n_obs", 0)));

  if (!gen.empty()) {
    ojson err;
    if (!estimWriteColumn(session, gen, sm, &err)) return err;
    return mutationResult(session, "lowess", {gen}, {});
  }
  return ojson::array();  // silent; the graph half is the dispatcher's business
}

// `lpoly Y X [, degree() kernel() bwidth() n() nograph generate(GX GS)]`
ojson runLpoly(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("lpoly")) return engineTooOld("lpoly", "lpoly");
  if (parsed.varlist.size() < 2) return aerSyntaxError("lpoly: syntax is `lpoly yvar xvar`");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  std::string gen, kernel, bw, degree, ngrid;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "generate" || k == "gen") gen = v.strValue;
    else if (k == "kernel") kernel = v.strValue;
    else if (k == "bwidth" || k == "bw") bw = v.strValue;
    else if (k == "degree") degree = v.strValue;
    else if (k == "n") ngrid = v.strValue;
  }
  std::vector<std::string> args = {"lpoly", "--data",          *session.currentHbin(),
                                   "--y",   parsed.varlist[0], "--x", parsed.varlist[1]};
  if (!kernel.empty()) { args.push_back("--kernel"); args.push_back(kernel); }
  if (!bw.empty()) { args.push_back("--bw"); args.push_back(bw); }
  if (!degree.empty()) { args.push_back("--degree"); args.push_back(degree); }
  if (!ngrid.empty()) { args.push_back("--n"); args.push_back(ngrid); }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("lpoly: " + cobraErrorMessage(ce), ce.code);
  }

  session.eval.rValues.set("bwidth", raw.value("bwidth", 0.0));
  session.eval.rValues.set("pwidth", raw.value("pwidth", 0.0));
  session.eval.rValues.set("degree", static_cast<double>(raw.value("degree", 0)));
  // r(ngrid) is the count of SUCCESSFUL local regressions, which can be less
  // than the requested grid size ([R] lpoly p.1614).
  session.eval.rValues.set("ngrid", static_cast<double>(raw.value("ngrid", 0)));
  session.eval.rValues.set("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.rStrings.set("kernel", raw.value("kernel", std::string("epanechnikov")));

  if (!gen.empty()) {
    // generate(GX GS): the grid and the smooth, both frame-length and padded
    // with missing beyond the grid.
    std::istringstream gs(gen);
    std::string g1, g2;
    gs >> g1 >> g2;
    const std::vector<double> grid = jsonDoubles(raw["grid"]);
    const std::vector<double> sm = jsonDoubles(raw["smooth"]);
    const std::size_t n = static_cast<std::size_t>(session.datastore()->nRows());
    auto pad = [&](const std::vector<double>& v) {
      std::vector<double> o(n, std::numeric_limits<double>::quiet_NaN());
      for (std::size_t i = 0; i < v.size() && i < n; ++i) o[i] = v[i];
      return o;
    };
    ojson err;
    std::vector<std::string> made;
    if (!g1.empty()) {
      if (!estimWriteColumn(session, g1, pad(grid), &err)) return err;
      made.push_back(g1);
    }
    if (!g2.empty()) {
      if (!estimWriteColumn(session, g2, pad(sm), &err)) return err;
      made.push_back(g2);
    }
    return mutationResult(session, "lpoly", made, {});
  }
  return ojson::array();
}

// `pca varlist [, components() mineigen() covariance correlation]`
ojson runPca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("pca")) return engineTooOld("pca", "pca");
  if (parsed.varlist.size() < 2) return aerErrorReason("pca: too few variables", 102);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  bool cov = false, corr = false;
  std::string comp, mineigen;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "covariance") cov = true;
    else if (k == "correlation") corr = true;
    else if (k == "components" || k == "comp" || k == "factors") comp = v.strValue;
    else if (k == "mineigen") mineigen = v.strValue;
  }
  if (cov && corr) return aerErrorReason("pca: options correlation and covariance are exclusive", 198);

  std::string vars;
  for (std::size_t i = 0; i < parsed.varlist.size(); ++i)
    vars += (i ? "," : "") + parsed.varlist[i];
  std::vector<std::string> args = {"pca", "--data", *session.currentHbin(), "--x", vars};
  if (cov) args.push_back("--covariance");
  if (!comp.empty()) { args.push_back("--comp"); args.push_back(comp); }
  if (!mineigen.empty()) { args.push_back("--mineigen"); args.push_back(mineigen); }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("pca: " + cobraErrorMessage(ce), ce.code);
  }

  const int p = raw.value("p", 0);
  const int f = raw.value("f", 0);
  const std::vector<double> ev = jsonDoubles(raw["eigenvalues"]);
  const std::string ctype = raw.value("ctype", std::string("correlation"));

  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("f", static_cast<double>(f));
  session.eval.setEScalar("rho", raw.value("rho", 0.0));
  if (raw.contains("trace") && raw["trace"].is_number())
    session.eval.setEScalar("trace", raw["trace"].get<double>());
  // A null lndet/cond is Stata's MISSING, which is what the collinear case
  // produces once the relative-1e-8 zeroing has run.
  if (raw.contains("lndet") && raw["lndet"].is_number())
    session.eval.setEScalar("lndet", raw["lndet"].get<double>());
  if (raw.contains("cond") && raw["cond"].is_number())
    session.eval.setEScalar("cond", raw["cond"].get<double>());

  session.eval.setEString("cmd", "pca");
  session.eval.setEString("Ctype", ctype);
  session.eval.setEString("title", "Principal components");
  session.eval.setEString("properties", "nob noV eigen");
  session.eval.setEString("predict", "pca_p");
  session.eval.setEString("marginsnotok", "_ALL");

  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  auto rowvec = [&](const std::vector<double>& v, std::vector<std::string> cn) {
    eval::EvalMatrix m;
    m.data.push_back(v);
    m.rownames = {"r1"};
    m.colnames = std::move(cn);
    return m;
  };
  std::vector<std::string> compNames;
  for (int j = 0; j < p; ++j) compNames.push_back("Comp" + std::to_string(j + 1));
  postM("Ev", rowvec(ev, compNames));

  if (f > 0 && raw.contains("loadings")) {
    const std::vector<double> L = jsonDoubles(raw["loadings"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < f; ++j) row.push_back(L[static_cast<std::size_t>(i * f + j)]);
      m.data.push_back(row);
      m.rownames.push_back(parsed.varlist[static_cast<std::size_t>(i)]);
    }
    for (int j = 0; j < f; ++j) m.colnames.push_back("Comp" + std::to_string(j + 1));
    postM("L", std::move(m));
  }
  if (raw.contains("psi"))
    postM("Psi", rowvec(jsonDoubles(raw["psi"]), parsed.varlist));
  if (raw.contains("means"))
    postM("means", rowvec(jsonDoubles(raw["means"]), parsed.varlist));
  // e(sds) exists ONLY for correlation PCA -- after a covariance run,
  // `confirm matrix e(sds)` is rc 111.
  if (raw.contains("sds"))
    postM("sds", rowvec(jsonDoubles(raw["sds"]), parsed.varlist));
  if (raw.contains("c")) {
    const std::vector<double> C = jsonDoubles(raw["c"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < p; ++j) row.push_back(C[static_cast<std::size_t>(i * p + j)]);
      m.data.push_back(row);
      m.rownames.push_back(parsed.varlist[static_cast<std::size_t>(i)]);
    }
    m.colnames = parsed.varlist;
    postM("C", std::move(m));
  }
  if (raw.contains("components")) {
    const std::vector<double> T4 = jsonDoubles(raw["components"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < 4; ++j) row.push_back(T4[static_cast<std::size_t>(i * 4 + j)]);
      m.data.push_back(row);
      m.rownames.push_back("Comp" + std::to_string(i + 1));
    }
    m.colnames = {"Eigenvalue", "Difference", "Proportion", "Cumulative"};
    postM("components", std::move(m));
  }

  // A LastModel is what makes `ereturn list` willing to speak at all.
  LastModel lm;
  lm.cobraCmd = "pca";
  lm.vcov = "none";
  session.setLastModel(lm);

  std::ostringstream out;
  char buf[256];
  out << "\nPrincipal components/" << ctype << "                 Number of obs    = "
      << std::setw(10) << raw.value("n_obs", 0) << "\n";
  std::snprintf(buf, sizeof(buf), "                                                 Number of comp.  = %10d\n", f);
  out << buf;
  if (raw.contains("trace") && raw["trace"].is_number()) {
    std::snprintf(buf, sizeof(buf), "                                                 Trace            = %10.10g\n",
                  raw["trace"].get<double>());
    out << buf;
  }
  std::snprintf(buf, sizeof(buf), "    Rotation: (unrotated = principal)            Rho              = %10.4f\n",
                raw.value("rho", 0.0));
  out << buf;
  out << "    --------------------------------------------------------------------------\n";
  out << "       Component |   Eigenvalue   Difference         Proportion   Cumulative\n";
  out << "    -------------+------------------------------------------------------------\n";
  if (raw.contains("components")) {
    const std::vector<double> T4 = jsonDoubles(raw["components"]);
    for (int i = 0; i < p; ++i) {
      const double diff = T4[static_cast<std::size_t>(i * 4 + 1)];
      std::snprintf(buf, sizeof(buf), "%16s | %12.6g %12.6g %18.4f %12.4f\n", ("Comp" + std::to_string(i + 1)).c_str(),
                    T4[static_cast<std::size_t>(i * 4 + 0)], diff,
                    T4[static_cast<std::size_t>(i * 4 + 2)], T4[static_cast<std::size_t>(i * 4 + 3)]);
      if (std::isnan(diff))
        std::snprintf(buf, sizeof(buf), "%16s | %12.6g %12s %18.4f %12.4f\n", ("Comp" + std::to_string(i + 1)).c_str(),
                      T4[static_cast<std::size_t>(i * 4 + 0)], ".",
                      T4[static_cast<std::size_t>(i * 4 + 2)], T4[static_cast<std::size_t>(i * 4 + 3)]);
      out << buf;
    }
  }
  out << "    --------------------------------------------------------------------------\n";
  if (raw.contains("dropped") && raw["dropped"].is_array())
    for (const auto& dv : raw["dropped"])
      out << "(" << dv.get<std::string>() << " dropped because of zero variance)\n";
  return displayBlock(out.str());
}

// ===========================================================================
// xi -- indicator expansion
//
// ALWAYS materializes. Stata's xi writes real _I* columns and rewrites the
// command around them; there is no pass-through form, and pretending otherwise
// would leave `describe _I*` and `_b[_Iregion_2]` answering about columns that
// do not exist.
//
// Everything below was measured against live StataBE 19.5 on the kitchensink
// fixture rather than taken from the plan, and two of the plan's statements did
// not survive: `xi, noomit` prints NO mapping line at all (the plan implies one
// is always printed), and the interaction term covers only the RETAINED levels
// of both factors (region 2,3,4 x female 1 -- not the full cross).

namespace {

// Stata's abbrev(): "region" at width 5 becomes "reg~n" -- the head, a tilde,
// and the final character.
std::string abbrevName(const std::string& s, size_t width) {
  if (s.size() <= width || width < 3) return s;
  return s.substr(0, width - 2) + "~" + s.substr(s.size() - 1);
}

std::string stem3(const std::string& s) { return s.size() <= 3 ? s : s.substr(0, 3); }

// The integer levels of a factor-ish numeric column, ascending. Non-integer
// values are a refusal rather than a silent truncation: xi's whole output
// naming scheme is `_Iv_<level>`, and a level of 2.5 has no spelling.
bool xiLevels(const Datastore& ds, const std::string& v, std::vector<long long>* out,
              std::string* err) {
  std::vector<double> col;
  if (!numericColumn(ds, v, &col, err)) return false;
  std::set<long long> seen;
  for (double d : col) {
    if (std::isnan(d)) continue;
    if (d != std::floor(d)) {
      *err = v + ": xi requires integer levels (found a non-integer value)";
      return false;
    }
    seen.insert(static_cast<long long>(d));
  }
  if (seen.empty()) {
    *err = v + ": no non-missing observations";
    return false;
  }
  out->assign(seen.begin(), seen.end());
  return true;
}

struct XiPlan {
  std::vector<std::string> newNames;                     // in creation order
  std::vector<std::pair<std::string, std::string>> gen;  // name -> expression
  std::vector<std::pair<std::string, std::string>> labels;
  std::vector<std::string> displayLines;
  std::vector<std::string> substitution;  // what the term becomes in a rewrite
  // [begin,end) into `substitution` for each TOP-LEVEL term, in source order.
  // Needed because planTerm recurses (an interaction plans its own main
  // effects), so "everything since the last term" is not a boundary.
  std::vector<std::pair<size_t, size_t>> termRanges;
};

// One `i.` term. Returns false and fills `err` on a refusal.
bool planTerm(const Datastore& ds, const std::string& term, const std::string& prefix, bool noomit,
              XiPlan* plan, std::string* err) {
  // `i.a|i.b` is Stata's nested form and is NOT the same expansion as `*`.
  // Refused by name rather than silently treated as an interaction.
  if (term.find('|') != std::string::npos) {
    *err = "xi: the `i.a|i.b` nested form is not implemented in this version of PIET";
    return false;
  }
  const size_t star = term.find('*');
  auto stripI = [](const std::string& s) {
    return (s.size() > 2 && (s.compare(0, 2, "i.") == 0)) ? s.substr(2) : s;
  };

  if (star == std::string::npos) {
    // ---- i.v
    const std::string v = stripI(term);
    if (v == term) {
      *err = "xi: `" + term + "` is not an i. term";
      return false;
    }
    std::vector<long long> lv;
    if (!xiLevels(ds, v, &lv, err)) return false;
    const size_t first = noomit ? 0 : 1;  // the MINIMUM level is the omitted one
    std::vector<std::string> made;
    for (size_t i = first; i < lv.size(); ++i) {
      const std::string nm = prefix + v + "_" + std::to_string(lv[i]);
      plan->newNames.push_back(nm);
      plan->gen.emplace_back(nm, nm + "=cond(missing(" + v + "),.," + v + "==" +
                                     std::to_string(lv[i]) + ")");
      plan->labels.emplace_back(nm, v + "==" + std::to_string(lv[i]));
      made.push_back(nm);
    }
    for (const auto& m : made) plan->substitution.push_back(m);
    // `noomit` prints nothing -- measured, not assumed.
    if (!noomit) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "%-17s %-19s (naturally coded; %s omitted)",
                    ("i." + v).c_str(),
                    (prefix + v + "_" + std::to_string(lv.front()) + "-" +
                     std::to_string(lv.back())).c_str(),
                    (prefix + v + "_" + std::to_string(lv.front())).c_str());
      plan->displayLines.push_back(buf);
    }
    return true;
  }

  // ---- i.a*<rhs>
  const std::string lhs = term.substr(0, star);
  const std::string rhsRaw = term.substr(star + 1);
  const std::string a = stripI(lhs);
  if (a == lhs) {
    *err = "xi: the left side of `*` must be an i. term";
    return false;
  }
  std::vector<long long> la;
  if (!xiLevels(ds, a, &la, err)) return false;
  const size_t firstA = noomit ? 0 : 1;

  // The main effects of a come first, exactly as the simple form makes them.
  if (!planTerm(ds, "i." + a, prefix, noomit, plan, err)) return false;

  const bool rhsFactor = (rhsRaw.size() > 2 && rhsRaw.compare(0, 2, "i.") == 0);
  if (rhsFactor) {
    const std::string b = stripI(rhsRaw);
    std::vector<long long> lb;
    if (!xiLevels(ds, b, &lb, err)) return false;
    if (!planTerm(ds, "i." + b, prefix, noomit, plan, err)) return false;
    const size_t firstB = noomit ? 0 : 1;
    const std::string stub = prefix + stem3(a) + "X" + stem3(b);
    for (size_t i = firstA; i < la.size(); ++i) {
      for (size_t j = firstB; j < lb.size(); ++j) {
        const std::string nm = stub + "_" + std::to_string(la[i]) + "_" + std::to_string(lb[j]);
        plan->newNames.push_back(nm);
        plan->gen.emplace_back(nm, nm + "=cond(missing(" + a + "),.,cond(missing(" + b + "),.,(" +
                                       a + "==" + std::to_string(la[i]) + ")*(" + b +
                                       "==" + std::to_string(lb[j]) + ")))");
        plan->labels.emplace_back(nm, a + "==" + std::to_string(la[i]) + " & " + b +
                                          "==" + std::to_string(lb[j]));
        plan->substitution.push_back(nm);
      }
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%-17s %-19s (coded as above)",
                  ("i." + abbrevName(a, 5) + "*i." + abbrevName(b, 5)).c_str(),
                  (stub + "_#_#").c_str());
    plan->displayLines.push_back(buf);
    return true;
  }

  // factor x continuous
  const std::string c = rhsRaw;
  if (!ds.has(c)) {
    *err = "variable " + c + " not found";
    return false;
  }
  const std::string stub = prefix + stem3(a) + "X" + stem3(c);
  for (size_t i = firstA; i < la.size(); ++i) {
    const std::string nm = stub + "_" + std::to_string(la[i]);
    plan->newNames.push_back(nm);
    plan->gen.emplace_back(nm, nm + "=cond(missing(" + a + "),.,cond(missing(" + c + "),.,(" + a +
                                   "==" + std::to_string(la[i]) + ")*" + c + "))");
    plan->labels.emplace_back(nm, "(" + a + "==" + std::to_string(la[i]) + ")*" + c);
    plan->substitution.push_back(nm);
  }
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%-17s %-19s (coded as above)", ("i." + a + "*" + c).c_str(),
                (stub + "_#").c_str());
  plan->displayLines.push_back(buf);
  return true;
}

// Drop every existing <prefix>* column, which is what Stata does before it
// expands: a stale _Iregion_5 from a previous xi on different data would
// otherwise survive and be picked up by a later `_I*` varlist.
ojson xiDropStale(SessionState& session, const std::string& prefix, std::string* err) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson();
  std::vector<std::string> stale;
  for (const auto& nm : ds->names())
    if (nm.compare(0, prefix.size(), prefix) == 0) stale.push_back(nm);
  if (stale.empty()) return ojson();
  std::string outPath = mintScratchPath(session.workdir(), "xidrop");
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  for (const auto& v : stale) {
    args.push_back("--drop");
    args.push_back(v);
  }
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    *err = cobraErrorMessage(ce);
    return aerError(*err, ce.code);
  }
  session.load(outPath);
  return ojson();
}

// Run the planned --gen chain, one column per call (the plan's serial chain:
// each --out becomes the next --data, so a failure leaves the session on the
// last good head rather than half-expanded).
bool xiRunPlan(SessionState& session, const XiPlan& plan, ojson* errOut) {
  for (const auto& [nm, expr] : plan.gen) {
    std::string outPath = mintScratchPath(session.workdir(), "xigen");
    std::vector<std::string> args = {"eval",  "--data", *session.currentHbin(),
                                     "--gen", expr,     "--out",
                                     outPath};
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      *errOut = aerError("xi: could not create " + nm + " -- " + cobraErrorMessage(ce), ce.code);
      return false;
    }
    session.load(outPath);
  }
  for (const auto& [nm, lab] : plan.labels) session.setVarLabel(nm, lab);
  return true;
}

// Shared front half of both forms: read the options, plan every term, run it.
bool xiExpand(const std::vector<std::string>& terms,
              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
              SessionState& session, XiPlan* plan, ojson* errOut) {
  std::string prefix = "_I";
  bool noomit = false;
  for (const auto& [k, v] : opts) {
    if (k == "prefix") {
      prefix = v.strValue;
      if (prefix.size() > 4) {
        *errOut = aerSyntaxError("xi: prefix() must be at most 4 characters");
        return false;
      }
    } else if (k == "noomit") {
      noomit = true;
    }
  }
  if (!session.hasData() || !session.currentHbin().has_value()) {
    *errOut = ojson(eval::noDataLoaded());
    return false;
  }
  std::string derr;
  ojson dropErr = xiDropStale(session, prefix, &derr);
  if (!dropErr.is_null()) {
    *errOut = dropErr;
    return false;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) {
    *errOut = ojson(eval::noDataLoaded());
    return false;
  }
  bool sawTerm = false;
  for (const auto& t : terms) {
    if (t.find("i.") == std::string::npos) continue;
    sawTerm = true;
    const size_t subBegin = plan->substitution.size();
    std::string err;
    if (!planTerm(*ds, t, prefix, noomit, plan, &err)) {
      *errOut = aerError("xi: " + err);
      return false;
    }
    plan->termRanges.emplace_back(subBegin, plan->substitution.size());
  }
  if (!sawTerm) {
    *errOut = aerSyntaxError("xi: no i. terms found");
    return false;
  }
  return xiRunPlan(session, *plan, errOut);
}

}  // namespace

ojson runXi(const eval::ParsedCommand& parsed, SessionState& session) {
  // xi puts its options BEFORE the termlist -- `xi, noomit i.foreign` -- which
  // is backwards from every other Stata command, so the tokenizer swallows the
  // whole tail as option text and parsed.varlist comes back EMPTY. Re-derive
  // both halves from the raw line: after the comma, a token beginning `i.` is a
  // TERM and anything else is an option. (The comma-less form parses normally,
  // and is taken from parsed.varlist as usual.)
  std::vector<std::string> terms = parsed.varlist;
  std::vector<std::pair<std::string, eval::OptValue>> opts = parsed.opts;
  const size_t comma = parsed.raw.find(',');
  if (comma != std::string::npos) {
    terms.clear();
    opts.clear();
    std::istringstream ts(parsed.raw.substr(comma + 1));
    std::string tk;
    while (ts >> tk) {
      if (tk.compare(0, 2, "i.") == 0) {
        terms.push_back(tk);
        continue;
      }
      const std::string low = strutil::lower(tk);
      if (low == "noomit") {
        eval::OptValue b;
        b.isBool = true;
        b.boolValue = true;
        opts.emplace_back("noomit", b);
      } else if (low.compare(0, 7, "prefix(") == 0 && tk.back() == ')') {
        eval::OptValue p;
        p.strValue = tk.substr(7, tk.size() - 8);
        opts.emplace_back("prefix", p);
      } else {
        return aerSyntaxError("xi: unrecognized option `" + tk + "`");
      }
    }
  }
  XiPlan plan;
  ojson err;
  if (!xiExpand(terms, opts, session, &plan, &err)) return err;
  std::vector<ojson> out;
  std::string text;
  for (const auto& l : plan.displayLines) text += l + "\n";
  ojson mut = mutationResult(session, "xi", plan.newNames, {});
  if (text.empty()) return mut;
  ojson arr = ojson::array();
  arr.push_back(displayBlock(text));
  arr.push_back(mut);
  return arr;
}

ojson runXiPrefix(const std::vector<std::string>& terms,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  const std::string& wrapped, SessionState& session, std::string* rewritten,
                  std::string* displayText) {
  XiPlan plan;
  ojson err;
  if (!xiExpand(terms, opts, session, &plan, &err)) return err;
  for (const auto& l : plan.displayLines) *displayText += l + "\n";

  // Rewrite the wrapped command: each i.-bearing TOKEN becomes the columns that
  // term produced, in creation order. Walked token-wise and matched positionally
  // against termRanges, so the Nth i. token consumes the Nth term's range -- not
  // "everything planned so far", which would hand the first token every column.
  std::istringstream is(wrapped);
  std::string tok;
  std::string out;
  size_t termIdx = 0;
  while (is >> tok) {
    const bool isTerm = tok.compare(0, 2, "i.") == 0;
    if (isTerm && termIdx < plan.termRanges.size()) {
      const auto [b, e] = plan.termRanges[termIdx++];
      for (size_t k = b; k < e; ++k) out += (out.empty() ? "" : " ") + plan.substitution[k];
    } else {
      out += (out.empty() ? "" : " ") + tok;
    }
  }
  *rewritten = out;
  return ojson();
}

// ===========================================================================
// AER scope-2 bundle B2 -- `oaxaca` (Ben Jann, SSC 4.1.1)
//
// `oaxaca depvar indepvars, by(groupvar) pooled [relax noisily level(#)]`
//
// ONLY the twofold `pooled` decomposition is computed, because that is the form
// the twelve-paper corpus writes (034: `... sch2-sch531, by(parentedex) relax
// noisily pooled`) and the only form whose inference was VERIFIED before this
// handler existed: milf-coord/plans/oaxaca-se-20260908/REPORT.md reproduces all
// 120 SEs and every e(V)/e(V0) entry of 12 live-Stata fixtures to 3.8e-14, and
// 48 deliberately wrong constructions fail. The threefold DEFAULT (no `pooled`)
// is a NAMED refusal, not a silent fall-through to the twofold numbers: Stata
// answers a different table there, and an rc 0 with the wrong table is the
// cardinal defect class.
//
// What the verification pinned, and this handler must not lose:
//   * `pooled` IMPLIES robust `suest` (oaxaca.hlp: "suest is implied by
//     pooled"), so e(vce)=robust and e(suest)=suest even though nobody typed
//     vce(robust). The engine owns that construction (src/estimators/oaxaca.cpp).
//   * `relax` leaves a dropped coefficient as 0 with SE 0 (NOT missing), and
//     Stata drops the FIRST of two duplicates, keeping the later one
//     (rank_oracle.txt CASE 13) -- the opposite of regress's convention.
//   * Group 1 is the LOWER group value; e(N_1)/e(N_2) are per-group counts.
//
// e(b) is one row whose column names are Stata's `eq:name` spellings
// (`overall:difference`, `explained:x1`, `unexplained:_cons`), so `_b[...]` and
// `_se[...]` resolve by exact match. NAMED DIVERGENCE: PIET's matrix store has
// no separate row/column EQUATION metadata, so `: colnames e(b)` answers the
// joined spelling where Stata answers the bare name plus `: coleq`.
namespace {
const eval::OptVocab& kVocabOaxaca = eval::registerOptVocab(
    {"oaxaca"},
    {{"by", "pooled", "relax", "noisily", "level", "linear", "nolegend"},
     {"swap", "logit", "probit", "nodetail", "adjust", "threefold", "weight", "omega",
      "reference", "split", "svy", "vce", "cluster", "fixed", "suest", "nosuest", "nose",
      "model1", "model2", "x1", "x2", "xb", "eform", "categorical", "detail", "normalize"},
     // oaxaca.ado's syntax line declares one-letter stems the 2-char prefix
     // rule cannot reach: p:ooled l:evel a:djust w:eight o:mega.
     {{"p", "pooled"}, {"l", "level"}, {"a", "adjust"}, {"w", "weight"}, {"o", "omega"}}});

// An engine refusal carrying an exact Stata rc in `detail` ("r(499)") outranks
// the coarse code mapping; `no_obs` is Stata's r(2000); a bad_input with no
// rc is the r(198) class. Never the default engine_error, which reads as rc 1
// and silently breaks `_rc` assertions (the RC defect class this campaign has
// shipped before).
ojson oaxacaEngineError(const CobraError& ce) {
  const std::string msg = "oaxaca: " + cobraErrorMessage(ce);
  static const std::regex reRc(R"(r\((\d+)\))");
  std::smatch m;
  if (std::regex_search(ce.detail, m, reRc)) return aerErrorReason(msg, std::stoi(m.str(1)));
  if (ce.code == "no_obs") return aerErrorReason(msg, 2000);
  if (ce.code == "bad_input" || ce.code == "collinear") return aerErrorReason(msg, 198);
  return aerError(msg, ce.code);
}
}  // namespace

ojson runOaxaca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("oaxaca")) return engineTooOld("oaxaca", "oaxaca");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOaxaca.known,
                                            kVocabOaxaca.unimplemented)) {
    return ojson(*err);
  }
  std::string by, level;
  bool pooled = false, relax = false, noisily = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by") by = strutil::trim(v.strValue);
    else if (k == "pooled") pooled = true;
    else if (k == "relax") relax = true;
    else if (k == "noisily") noisily = true;
    else if (k == "level") level = strutil::trim(v.strValue);
    // `linear` is the default model and `nolegend` suppresses a legend this
    // build never prints: both are exact no-ops, accepted without a note.
  }
  if (by.empty()) return aerErrorReason("oaxaca: option by() required", 198);
  if (!pooled) {
    // The threefold default (and every other twofold weighting) is a different
    // table with its own, unverified inference. Say so by name.
    return ojson(eval::refusedCommand("oaxaca", "oaxaca_threefold_default"));
  }
  if (parsed.varlist.empty()) return aerSyntaxError("oaxaca: syntax is `oaxaca depvar indepvars, by(groupvar) pooled`");

  // The corpus form is `... sch2-sch531`: a RANGE inside an estimation varlist.
  // Estimation rows do not opt into the dispatcher's plain-varlist expansion
  // (they legitimately carry `i.x`/`L.x`), so the expansion is asked for HERE,
  // through the one expander PIET has (BUG-371/BUG-450), on the whole list.
  std::vector<std::string> vars = parsed.varlist;
  bool patterned = false;
  for (const auto& t : vars) {
    if (t.find('*') != std::string::npos || t.find('?') != std::string::npos ||
        t.find('-') != std::string::npos || strutil::lower(t) == "_all") {
      patterned = true;
      break;
    }
  }
  if (patterned) {
    std::string spec;
    for (std::size_t i = 0; i < vars.size(); ++i) spec += (i ? " " : "") + vars[i];
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    vars = ex.tokens;
  }
  for (const auto& t : vars) {
    // Factor-variable and time-series operators have no engine path here.
    if (t.find('.') != std::string::npos)
      return ojson(eval::refusedCommand("oaxaca", "factor_variable_operators"));
  }
  if (vars.size() < 2)
    return aerErrorReason("oaxaca: at least one regressor is required in this build", 198);
  const std::string y = vars[0];
  std::vector<std::string> xs(vars.begin() + 1, vars.end());

  std::vector<std::string> args = {"oaxaca", "--data", *session.currentHbin(), "--y", y,
                                   "--x", est_detail::joinCommaM4(xs), "--by", by, "--pooled"};
  if (relax) args.push_back("--relax");
  if (!level.empty()) { args.push_back("--level"); args.push_back(level); }
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args.push_back("--keepif");
    args.push_back(*parsed.ifExpr);
  }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return oaxacaEngineError(ce);
  }

  // e(): measured on StataBE (tests/validate/b2_oracle.json ox_* and the
  // oaxaca-se-20260908 captures): e(cmd) e(model) e(N) e(N_1) e(N_2) e(vce)
  // e(suest) e(depvar), plus e(b)/e(V).
  const std::vector<std::string> names = raw["e_b"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta = jsonDoubles(raw["e_b"]["values"]);
  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("N_1", static_cast<double>(raw.value("N_1", 0)));
  session.eval.setEScalar("N_2", static_cast<double>(raw.value("N_2", 0)));
  session.eval.setEScalar("level", raw.value("level", 95.0));
  session.eval.setEString("cmd", "oaxaca");
  session.eval.setEString("depvar", y);
  session.eval.setEString("model", "linear");
  session.eval.setEString("vce", "robust");
  session.eval.setEString("suest", "suest");
  session.eval.setEString("by", by);
  session.eval.setEString("title", "Blinder-Oaxaca decomposition");
  session.eval.setEString("properties", "b V");
  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  {
    eval::EvalMatrix b;
    b.data.push_back(beta);
    b.rownames = {"y1"};
    b.colnames = names;
    postM("b", std::move(b));
    eval::EvalMatrix V;
    for (const auto& row : raw["e_V"]["matrix"]) V.data.push_back(jsonDoubles(row));
    V.rownames = names;
    V.colnames = names;
    postM("V", std::move(V));
  }

  // The LastModel is what `_b[]`/`_se[]`/`ereturn list`/outreg2 read; the fit
  // filter is carried so a post-estimation refit reconstructs the same sample.
  LastModel lm;
  lm.cobraCmd = "oaxaca";
  lm.y = y;
  lm.depvar = y;
  lm.x = xs;
  lm.vcov = "robust";
  lm.dataPath = *session.currentHbin();
  est_detail::captureFitFilterM4(lm, args);
  lm.result = ojson(raw);
  session.setLastModel(lm);

  ojson blocks = ojson::array();
  if (noisily) {
    // `noisily` shows the two group regressions and the pooled one in Stata.
    // Those fits are not surfaced by this build; the decomposition is the same.
    blocks.push_back(displayBlock(
        "(oaxaca noisily: the underlying group and pooled regressions are not displayed "
        "in PIET -- decomposition unaffected)"));
  }
  if (raw.contains("warnings") && raw["warnings"].is_array())
    for (const auto& w : raw["warnings"]) blocks.push_back(displayBlock(w.get<std::string>()));
  blocks.push_back(est_detail::cobraToEstimationM4(ojson(raw), "oaxaca", &session));
  return blocks;
}

// ===========================================================================
// AER scope-2 B2-4 — `teffects nnmatch` (StataCorp [CAUSAL])
// ===========================================================================
//
// Paper 076 (`code/clean/clean_15_matching.do:366`) is the reason this exists:
//
//   sort PNR
//   teffects nnmatch (outcome i.earnlevel_0 i.ageg ... i.mother_ageg) (treatment), gen(match_id)
//
// `outcome` is a constant 0 there, so the ESTIMATE is not what the paper
// consumes — `match_id1` of the TREATED rows is (lines 382-413 join it to
// control row numbers). That makes two things load-bearing that a coefficient
// gate would never see: the ids are 1-based `_n` of the CURRENT data (MEASURED
// under `if`: an odd-rows-only subsample comes back with ODD ids, so the row
// numbers are the FULL frame's — INFO|nn_edge|ifodd_* in
// tests/eval/golden/aer_estim2_oracle.log), and their ORDER inside a tie is
// Stata's, not ours. Gate ① still wants e(b)/e(V) exact, and it gets them.
//
// Method identity: milf-coord/plans/teffects-nnmatch-20260908/REPORT.md as
// CORRECTED by REFUTE-0.md. The kernel comment in src/estimators/nnmatch.cpp
// carries the H2 rule and its named divergence; this handler owns only the
// surface: parsing, option honesty, e() posting, and gen().
//
// NAMED DIVERGENCES, all of them deliberate:
//   * `e(mscale)`/`e(mstats)` are HIDDEN matrices in Stata and are not posted
//     here (the engine computes them; nothing in PIET reads them back).
//   * `predict te|po|distance` after this command is not wired in this landing;
//     the engine already returns te/po0/po1/dist in the LastModel's result, so
//     it is a wiring step, not a method question. Refused by name until then.
//   * a treatment variable that is not 0/1 is ACCEPTED with control = the lower
//     level (Stata's `ATE:r2vs1.t12` colname is reproduced), but only the
//     colname of that case was measured.
namespace {
const eval::OptVocab& kVocabTeffects = eval::registerOptVocab(
    {"teffects"},
    {{"generate", "gen", "nneighbor", "nn", "ate", "atet", "metric", "vce", "level"},
     // Known to Stata, absent here: each is a DIFFERENT estimator or a rule
     // this investigation never measured. Named, never silently ignored.
     {"biasadj", "ematch", "caliper", "dtolerance", "osample", "control", "tlevel",
      "dmvariables", "pstolerance", "iterate", "aequations", "tolerance", "nolog",
      "coeflegend"},
     // _teffects_nnmatch.ado's syntax line: GENerate(), NNeighbor(), ATET,
     // METric() -- stems the 2-character prefix rule cannot reach on its own.
     {{"gen", "generate"}, {"nn", "nneighbor"}}});

// The two parenthesised groups of `teffects <sub> (…) (…)`. Scanned off the RAW
// text because the tokenizer has no notion of them; only the first two
// top-level groups are read, and everything before the first `(` must be the
// command and sub-command words.
bool teffectsGroups(const std::string& raw, std::vector<std::string>* g1,
                    std::vector<std::string>* g2, std::string* why) {
  std::vector<std::string> groups;
  std::size_t i = 0;
  while (groups.size() < 2 && i < raw.size()) {
    if (raw[i] != '(') { ++i; continue; }
    int depth = 0;
    std::size_t j = i;
    for (; j < raw.size(); ++j) {
      if (raw[j] == '(') ++depth;
      else if (raw[j] == ')') { if (--depth == 0) break; }
    }
    if (j >= raw.size()) { *why = "unbalanced parentheses"; return false; }
    groups.push_back(raw.substr(i + 1, j - i - 1));
    i = j + 1;
  }
  if (groups.size() < 2) { *why = "two parenthesised groups are required"; return false; }
  auto split = [](const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
      if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
        if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        if (c == ',') out.push_back(",");
      } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
  };
  *g1 = split(groups[0]);
  *g2 = split(groups[1]);
  return true;
}

// `vce(robust, nn(3))` / `vce(robust)` / `vce(iid)`. Returns false with a
// message when the spelling is one this build does not answer for.
bool parseTeffectsVce(const std::string& v, int* nn, std::string* why) {
  std::string s = strutil::trim(v);
  std::string head = s, tail;
  const auto comma = s.find(',');
  if (comma != std::string::npos) { head = strutil::trim(s.substr(0, comma));
                                    tail = strutil::trim(s.substr(comma + 1)); }
  if (strutil::lower(head) != "robust") { *why = strutil::lower(head); return false; }
  if (tail.empty()) return true;
  static const std::regex reNn(R"(^nn\s*\(\s*([0-9]+)\s*\)$)");
  std::smatch m;
  if (!std::regex_match(tail, m, reNn)) { *why = tail; return false; }
  *nn = std::stoi(m.str(1));
  return true;
}
}  // namespace

ojson runTeffects(const std::string& sub, const eval::ParsedCommand& parsed,
                  SessionState& session) {
  // Every other teffects estimator is a DIFFERENT method with its own
  // inference; each keeps its own named refusal rather than borrowing this one.
  if (sub != "nnmatch")
    return ojson(eval::refusedCommand("teffects", "teffects_" + sub + "_unimplemented"));
  if (!engineHasSubcommand("nnmatch")) return engineTooOld("teffects nnmatch", "nnmatch");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTeffects.known,
                                            kVocabTeffects.unimplemented)) {
    return ojson(*err);
  }

  std::string genStub, metric, level;
  int nneighbor = 1, vceNn = 2;
  bool atet = false, sawAte = false, sawVce = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "generate" || k == "gen") genStub = strutil::trim(v.strValue);
    else if (k == "nneighbor" || k == "nn") {
      const std::string t = strutil::trim(v.strValue);
      if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos)
        return aerErrorReason("teffects nnmatch: nneighbor(#) must be an integer", 198);
      nneighbor = std::stoi(t);
    } else if (k == "atet") atet = true;
    else if (k == "ate") sawAte = true;
    else if (k == "metric") {
      // `metric(matrix M)` names a user matrix; only the metric WORD decides
      // the refusal, and the reason string must stay a single token.
      metric = strutil::lower(strutil::trim(v.strValue));
      const auto sp = metric.find_first_of(" \t");
      if (sp != std::string::npos) metric = metric.substr(0, sp);
    }
    else if (k == "level") level = strutil::trim(v.strValue);
    else if (k == "vce") {
      sawVce = true;
      std::string why;
      if (!parseTeffectsVce(v.strValue, &vceNn, &why)) {
        // `vce(iid)` was captured once for the record and never verified, and
        // an unknown vcetype is a wrong ANSWER, not a wrong option name.
        for (char& c : why)
          if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_vce_" + why));
      }
    }
  }
  (void)sawVce;
  if (sawAte && atet)
    return aerSyntaxError("teffects nnmatch: options ate and atet are mutually exclusive");
  if (!metric.empty() && metric != "mahalanobis" && metric != "ivariance" &&
      metric != "euclidean")
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_metric_" + metric));
  // `level(#)`: Stata's band, MEASURED at four points on this very command
  // (aer_estim2_oracle.log INFO|nn_level|l0_rc=198, |l9_rc=198, |l9999_rc=0,
  // |l101_rc=198, |labc_rc=198) -- 10.00 <= # <= 99.99, r(198) outside it and
  // r(198) for a non-number. The value is FORWARDED to the engine below; it is
  // not an accept-note, because it moves the reported interval.
  if (!level.empty()) {
    char* endp = nullptr;
    const double lv = std::strtod(level.c_str(), &endp);
    while (endp != nullptr && *endp != '\0' && std::isspace(static_cast<unsigned char>(*endp))) ++endp;
    if (endp == level.c_str() || (endp != nullptr && *endp != '\0') || !std::isfinite(lv) ||
        lv < 10.0 || lv > 99.99)
      return aerErrorReason("teffects nnmatch: level() must be between 10 and 99.99", 198);
  }

  std::vector<std::string> g1, g2;
  std::string why;
  if (!teffectsGroups(parsed.raw, &g1, &g2, &why))
    return aerSyntaxError("teffects nnmatch: syntax is `teffects nnmatch (ovar mvarlist) (tvar) "
                          "[if] [in] [, options]' -- " + why);
  if (g1.size() < 2)
    return aerSyntaxError("teffects nnmatch: the first group needs an outcome and at least one "
                          "matching variable");
  for (const auto& tok : g1) if (tok == ",")
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_omvarlist_options"));
  // `(tvar , tlevel(1))` and the treatment-model varlist of ipw/aipw are other
  // commands' grammar; here the second group is exactly one variable.
  if (g2.size() != 1)
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_tvar_group_options"));

  const std::string y = g1[0], tvar = g2[0];
  // `mvars` is e(mvarlist): the matching variables in TYPED order with the
  // factor/continuous OPERATOR STRIPPED. Measured (aer_estim2_oracle.log
  // INFO|nn_level|mixed_mvarlist and |cdot_mvarlist, plus the 14-fixture
  // oracle.txt): `(y x1 i.a i.b)` -> `x1 a b`, `(y c.x1 i.a)` -> `x1 a`,
  // `(y i.a i.b)` -> `a b`. xs/fvs alone cannot rebuild that order, which is
  // why the list is accumulated here and not from the two split vectors.
  std::vector<std::string> xs, fvs, mvars;
  for (std::size_t i = 1; i < g1.size(); ++i) {
    std::string tok = g1[i];
    if (tok.find('#') != std::string::npos)
      return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_interaction_terms"));
    const std::string low = strutil::lower(tok);
    if (low.rfind("i.", 0) == 0) { fvs.push_back(tok.substr(2)); mvars.push_back(tok.substr(2)); }
    else if (low.rfind("c.", 0) == 0) { xs.push_back(tok.substr(2)); mvars.push_back(tok.substr(2)); }
    else if (tok.find('.') != std::string::npos)
      // `L.x`, `ib2.a`, `o.x` -- an operator whose meaning this build has not
      // measured for the matching metric. Named, not quietly dropped.
      return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_fv_operator"));
    else { xs.push_back(tok); mvars.push_back(tok); }
  }
  if (xs.empty() && fvs.empty())
    return aerSyntaxError("teffects nnmatch: at least one matching variable is required");

  // gen(stub) mints stub1..stubK where K = e(k_nnmax). Stata refuses r(110) on a
  // collision, but it does NOT scan the whole stub* family and it does not do it
  // all at once -- MEASURED, StataBE 19.5 2026-09-09 on the 200-row fixture
  // (default nneighbor(1), k_nnmax 2):
  //
  //   mm1 exists, nneighbor(1)  -> r(110) "variable mm1 already defined /
  //                                        in option generate(mm)"
  //   mm2 exists, nneighbor(1)  -> r(110) "maximum number of nearest neighbors
  //                                        is 2, but variable mm2 already exists"
  //   mm3 exists, nneighbor(1)  -> rc 0, mm1/mm2 created  (mm3 is NOT touched)
  //   mm3 exists, nneighbor(3)  -> r(110) "variable mm3 already defined ..."
  //
  // i.e. the stubs Stata can name BEFORE it estimates are stub1..stub(nneighbor)
  // -- that many are guaranteed -- and it carries a DIFFERENT text for the ones
  // only the fitted K reveals. The earlier 1..64 pre-scan refused the third case
  // too, which is an honest error where Stata answers: a divergence created by a
  // fix. Both halves below are the measured rule.
  if (!genStub.empty()) {
    if (!strutil::isValidStataIdentifier(genStub + "1"))
      return aerSyntaxError("teffects nnmatch: invalid generate() stub `" + genStub + "'");
    Datastore* ds = nullptr;
    try { ds = session.datastore(); } catch (const std::exception&) { ds = nullptr; }
    if (ds != nullptr)
      for (int k = 1; k <= nneighbor; ++k) {
        const std::string nm = genStub + std::to_string(k);
        if (ds->has(nm))
          return aerErrorReason("teffects nnmatch: variable " + nm + " already defined", 110);
      }
  }

  std::vector<std::string> args = {"nnmatch", "--data", *session.currentHbin(), "--y", y,
                                   "--t", tvar};
  if (!xs.empty()) { args.push_back("--x"); args.push_back(est_detail::joinCommaM4(xs)); }
  if (!fvs.empty()) { args.push_back("--fv"); args.push_back(est_detail::joinCommaM4(fvs)); }
  args.push_back("--nn"); args.push_back(std::to_string(nneighbor));
  args.push_back("--vce-nn"); args.push_back(std::to_string(vceNn));
  if (!metric.empty()) { args.push_back("--metric"); args.push_back(metric); }
  if (atet) { args.push_back("--stat"); args.push_back("atet"); }
  if (!level.empty()) { args.push_back("--level"); args.push_back(level); }
  // `if` restricts the ESTIMATION SAMPLE without renumbering the frame: the
  // engine takes it as a MASK over the original rows, so the ids it returns
  // stay frame `_n` -- which is what Stata does (INFO|nn_edge|ifodd_*) and what
  // paper 076's `merge 1:m match_id1` depends on. (`in` is not allowed on this
  // row of the registry, so there is nothing to compose it with.)
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args.push_back("--keepif");
    args.push_back(*parsed.ifExpr);
  }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    // Same rc discipline as oaxacaEngineError: an engine refusal carrying an
    // exact Stata rc outranks the coarse mapping, and the default engine_error
    // (which reads as rc 1) is never what comes back.
    const std::string msg = "teffects nnmatch: " + cobraErrorMessage(ce);
    static const std::regex reRc(R"(r\((\d+)\))");
    std::smatch mrc;
    if (std::regex_search(ce.detail, mrc, reRc))
      return aerErrorReason(msg, std::stoi(mrc.str(1)));
    if (ce.code == "no_obs") return aerErrorReason(msg, 2000);
    if (ce.code == "bad_input" || ce.code == "collinear") return aerErrorReason(msg, 198);
    return aerError(msg, ce.code);
  }

  // ---- gen(stub1..stubK) --------------------------------------------------
  // Ragged ids -> padded long columns, ORDER preserved, `.` beyond |Omega(i)|
  // and on every row the estimation sample excluded.
  if (!genStub.empty()) {
    const int kmax = raw.value("k_nnmax", 0);
    const auto& ids = raw["matches"]["ids"];
    // The second half of the measured collision rule (see the pre-estimation
    // guard above): stub(nneighbor+1)..stubK are the ones only the fitted K
    // names, and Stata's text for them is the one that quotes K. Checked for
    // the WHOLE range before a single column is written, so a collision at
    // stub2 does not leave stub1 minted behind an error.
    {
      Datastore* ds2 = nullptr;
      try { ds2 = session.datastore(); } catch (const std::exception&) { ds2 = nullptr; }
      if (ds2 != nullptr)
        for (int k = nneighbor + 1; k <= kmax; ++k) {
          const std::string nm = genStub + std::to_string(k);
          if (ds2->has(nm))
            return aerErrorReason("teffects nnmatch: maximum number of nearest neighbors is " +
                                      std::to_string(kmax) + ", but variable " + nm +
                                      " already exists",
                                  110);
        }
    }
    for (int k = 0; k < kmax; ++k) {
      std::vector<double> col;
      col.reserve(ids.size());
      for (const auto& row : ids)
        col.push_back(static_cast<int>(row.size()) > k
                          ? static_cast<double>(row[k].get<long long>())
                          : std::numeric_limits<double>::quiet_NaN());
      const std::string nm = genStub + std::to_string(k + 1);
      ojson err;
      if (!estimWriteColumn(session, nm, col, &err)) return err;
    }
  }

  // ---- e() ----------------------------------------------------------------
  const std::vector<std::string> names = raw["e_b"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta = jsonDoubles(raw["e_b"]["values"]);
  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("n0", static_cast<double>(raw.value("n0", 0)));
  session.eval.setEScalar("n1", static_cast<double>(raw.value("n1", 0)));
  session.eval.setEScalar("treated", raw.value("treated_level", 1.0));
  session.eval.setEScalar("control", raw.value("control_level", 0.0));
  session.eval.setEScalar("k_levels", 2.0);
  session.eval.setEScalar("k_nneighbor", static_cast<double>(raw.value("k_nneighbor", 1)));
  session.eval.setEScalar("k_nnmin", static_cast<double>(raw.value("k_nnmin", 0)));
  session.eval.setEScalar("k_nnmax", static_cast<double>(raw.value("k_nnmax", 0)));
  session.eval.setEScalar("k_robust", static_cast<double>(raw.value("k_robust", 2)));
  session.eval.setEScalar("dtolerance", raw.value("dtolerance", 1.4901161193847656e-8));
  // e(level) is deliberately NOT posted: the full `ereturn list` capture of
  // all 14 oracle fixtures (milf-coord/plans/teffects-nnmatch-20260908/
  // oracle.txt SCALARS) has no `level`, and `display e(level)` after
  // `teffects nnmatch ..., level(90)` prints `.`
  // (aer_estim2_oracle.log INFO|nn_level|l90_e_level). Posting it was an
  // invention; the option's ONLY measured effect is on ci_lo/ci_hi.
  session.eval.setEString("cmd", "teffects");
  session.eval.setEString("subcmd", "nnmatch");
  session.eval.setEString("depvar", y);
  session.eval.setEString("tvar", tvar);
  {
    std::string mv;
    for (std::size_t i = 0; i < mvars.size(); ++i) mv += (i > 0 ? " " : "") + mvars[i];
    session.eval.setEString("mvarlist", mv);
  }
  session.eval.setEString("metric", raw.value("metric", std::string("mahalanobis")));
  session.eval.setEString("stat", atet ? "atet" : "ate");
  session.eval.setEString("vce", "robust");
  session.eval.setEString("vcetype", "AI robust");
  session.eval.setEString("title", "Treatment-effects estimation");
  {
    // NOT the literal "0 1": measured on a 1/2-coded treatment
    // (aer_estim2_oracle.log INFO|nn_level|t12_tlevels) Stata answers `1 2`,
    // which is also what e(control)/e(treated)=1/2 and the colname
    // `ATE:r2vs1.t12` in the SAME e() set already said. The levels are
    // integers by construction (the engine refuses a non-integer treatment).
    auto lvl = [](double v) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
      return std::string(buf);
    };
    session.eval.setEString("tlevels", lvl(raw.value("control_level", 0.0)) + " " +
                                           lvl(raw.value("treated_level", 1.0)));
  }
  session.eval.setEString("properties", "b V");
  if (!genStub.empty()) {
    std::string iv;
    for (int k = 1; k <= raw.value("k_nnmax", 0); ++k)
      iv += (k > 1 ? " " : "") + genStub + std::to_string(k);
    session.eval.setEString("indexvar", iv);
  }
  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  {
    eval::EvalMatrix b;
    b.data.push_back(beta);
    b.rownames = {"y1"};
    b.colnames = names;
    postM("b", std::move(b));
    eval::EvalMatrix V;
    for (const auto& row : raw["e_V"]["matrix"]) V.data.push_back(jsonDoubles(row));
    V.rownames = names;
    V.colnames = names;
    postM("V", std::move(V));
  }

  LastModel lm;
  lm.cobraCmd = "nnmatch";
  // BUG-458 publishes e(cmd)/e(depvar)/e(vce) out of the LastModel, and it
  // prefers stataCmd -- without this line e(cmd) would come back `nnmatch`,
  // which is the ENGINE verb, where Stata answers `teffects` (with `nnmatch`
  // in e(subcmd), posted separately above).
  lm.stataCmd = "teffects";
  lm.y = y;
  lm.depvar = y;
  lm.x = xs;
  for (const auto& f : fvs) lm.x.push_back("i." + f);
  lm.vcov = "robust";
  lm.dataPath = *session.currentHbin();
  est_detail::captureFitFilterM4(lm, args);
  lm.result = ojson(raw);
  session.setLastModel(lm);

  ojson blocks = ojson::array();
  if (raw.contains("warnings") && raw["warnings"].is_array())
    for (const auto& w : raw["warnings"]) blocks.push_back(displayBlock(w.get<std::string>()));
  blocks.push_back(est_detail::cobraToEstimationM4(ojson(raw), "nnmatch", &session));
  // The gen() columns are a DATASET MUTATION and mutation_result.h's contract
  // ("every handler TU that mutates the loaded dataset returns" this envelope)
  // binds here exactly as it does for the estimWriteColumn callers above
  // (runTsfilter / runLowess / runLpoly). Without it routes.cpp's
  // `varlist_after` comes back null and the console's Variables pane never
  // learns that stub1..stubK exist (app.js:1007 guards on it).
  if (!genStub.empty()) {
    std::vector<std::string> minted;
    for (int k = 1; k <= raw.value("k_nnmax", 0); ++k) minted.push_back(genStub + std::to_string(k));
    if (!minted.empty()) blocks.push_back(mutationResult(session, "teffects", minted, {}));
  }
  return blocks;
}

}  // namespace piet::handlers
