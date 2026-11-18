// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Estimation diagnostics.
#include "piet/handlers/estimates_tables.h"
#include "piet/handlers/postestimation.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"  // exprUnboundNameRefusal (predict, BUG-1248)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation_internal.h"
#include "rtable_internal.h"
#include "piet/eval/exprfns.h"
#include "estimation/statistics_command_support.h"
#include "piet/datastore.h"
#include "piet/loops.h"  // expandNumlist (estat bgodfrey/archlm lags())
#include "estimation/estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

void postCsdidEventEMatrices(SessionState& session, const ojson& raw) {
  if (!raw.contains("estat_event") || !raw["estat_event"].is_object()) return;
  const ojson& ee = raw["estat_event"];
  auto numOf = [](const ojson& x) -> double {
    return x.is_null() ? std::numeric_limits<double>::quiet_NaN() : x.get<double>();
  };
  auto setE = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& p : session.eval.eMatrices) {
      if (p.first == nm) { p.second = std::move(m); return; }
    }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  if (ee.contains("e_b") && ee["e_b"].is_object()) {
    const ojson& eb = ee["e_b"];
    eval::EvalMatrix b;
    for (const auto& n : eb.value("names", ojson::array())) b.colnames.push_back(n.get<std::string>());
    b.rownames = {"y1"};
    std::vector<double> vals;
    for (const auto& x : eb.value("values", ojson::array())) vals.push_back(numOf(x));
    b.data.push_back(std::move(vals));
    setE("b", std::move(b));
  }
  if (ee.contains("e_V") && ee["e_V"].is_object()) {
    const ojson& eV = ee["e_V"];
    eval::EvalMatrix V;
    for (const auto& n : eV.value("names", ojson::array())) V.colnames.push_back(n.get<std::string>());
    V.rownames = V.colnames;
    for (const auto& row : eV.value("matrix", ojson::array())) {
      std::vector<double> r;
      for (const auto& x : row) r.push_back(numOf(x));
      V.data.push_back(std::move(r));
    }
    setE("V", std::move(V));
  }
}

double chi2UpperTailIntegerDf(double df, double x);  // postestimation/likelihood_ratio_test.cpp

namespace {

// ---------------------------------------------------------------------------
// The estat subcommands every estimation command serves (Stata's default
// handler: ic, summarize, vce, bootstrap, parallel), and the routing of the
// estimator-specific ones. Before this, every subcommand not handled above fell
// through to the csdid aggregator and answered "no csdid result in memory"
// after regress, logit, glm, ...

bool estatAbbrev(const std::string& typed, const std::string& full, size_t minLen) {
  return typed.size() >= minLen && typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0;
}

std::string estatNum(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

std::string estatFmt(const std::string& fmt, double v) {
  if (std::isnan(v)) return formatNumberAsDisplayed(fmt, ".");
  return formatNumberAsDisplayed(fmt, estatNum(v));
}

std::string estatPadLeft(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

double estatE(const std::string& name, SessionState& session) {
  const std::optional<double> v = evalScalarExpr("e(" + name + ")", session);
  if (!v.has_value() || std::isnan(*v) || *v >= eval::kStataMissingSentinel)
    return std::numeric_limits<double>::quiet_NaN();
  return *v;
}

ojson estatNotImplemented(const std::string& sub, const std::string& ecmd) {
  return engineErrorBlockM4("estat " + sub + " after " + (ecmd.empty() ? std::string("this model") : ecmd) +
                                " is a valid Stata command that this version of PIET does not "
                                "implement.",
                            "unsupported_command");
}

ojson estatRefuse(const std::string& msg, const char* rc) {
  return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                std::string(rc)));
}

ojson estatDisplay(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// estat ic: N (n(), else e(N_ic), else e(N)), e(ll_0), e(ll), df (e(df_ic), else
// e(rank)), AIC = -2 ll + 2 df, BIC = -2 ll + ln(N) df; r(S) 1 x 6 with the row
// `.` (estimates stats' default; vs-stata bank 383e5bb3 `estat ic` after
// regress: r(S) `N ll0 ll df AIC BIC`, df 4). No e(ll) is r(321) "likelihood
// information not found in last estimation results" (2b14ebcf, xtreg re).
ojson estatIcM4(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                SessionState& session, const std::string& ecmd) {
  double nOpt = std::numeric_limits<double>::quiet_NaN();
  for (const auto& [k0, v] : opts) {
    const std::string k = strutil::lower(k0);
    if (k == "n") {
      if (v.isBool) return estatRefuse("option n() invalid", "r(198)");
      char* end = nullptr;
      const std::string t = strutil::trim(v.strValue);
      nOpt = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !(nOpt > 0))
        return estatRefuse("option n() invalid", "r(198)");
    } else {
      // df(), aiccorrected, aicconsistent, all: not reproduced here
      return estatNotImplemented("ic, " + k0, ecmd);
    }
  }
  double ll = estatE("ll", session);
  double llNull = estatE("ll_0", session);
  bool withinLl = false;  // ll computed here (below); its df is rank(e(V))
  if (std::isnan(ll)) {
    // areg and xtreg, fe: Stata's e(ll)/e(ll_0) are those of the regression on
    // the within-transformed data, -N/2 (ln(2 pi rss/N) + 1), the null model's
    // on the within total sum of squares rss/(1 - within R2) (vs-stata bank
    // 30b3090c areg / 2ddb8740 xtreg, fe: rss 9.6014402, N 336, within R2
    // .10972452 give e(ll) 120.50992540953604, e(ll_0) 100.98423729123496).
    // Read off the fit's own block, unweighted fits only.
    const auto& lm = session.lastModel();
    std::string model;
    if (const std::string* m = session.eval.eStrings.find("model")) model = *m;
    const bool within = ecmd == "areg" || (ecmd == "xtreg" && model == "fe");
    if (within && lm.has_value() && lm->fitWeight.empty() && lm->result.has_value() &&
        lm->result->is_object()) {
      const ojson& r = *lm->result;
      auto num = [&r](const char* k) {
        return r.contains(k) && r[k].is_number() ? r[k].get<double>() : std::numeric_limits<double>::quiet_NaN();
      };
      const double rss = num("rss"), nObs = num("n_obs"), r2w = num("r2_within");
      if (rss > 0 && nObs > 0 && r2w >= 0 && r2w < 1) {
        const double twoPi = 2.0 * 3.14159265358979323846;
        ll = -0.5 * nObs * (std::log(twoPi * rss / nObs) + 1.0);
        llNull = -0.5 * nObs * (std::log(twoPi * (rss / (1.0 - r2w)) / nObs) + 1.0);
        withinLl = true;
      }
    }
  }
  if (std::isnan(ll)) {
    // Stata's refusal is for the results that carry no likelihood (vs-stata
    // bank 2b14ebcf xtreg, re; 8431f34d newey; 40fa0079 ivregress gmm; the
    // posted results of margins, whose e() has no e(ll)); after any other fit
    // a missing e(ll) is this build's gap, not Stata's answer.
    std::string model;
    if (const std::string* m = session.eval.eStrings.find("model")) model = *m;
    if (ecmd == "newey" || ecmd == "ivregress" || ecmd == "margins" ||
        (ecmd == "xtreg" && (model == "re" || model == "be")))
      return estatRefuse("likelihood information not found in last estimation results", "r(321)");
    return estatNotImplemented("ic", ecmd);
  }
  double n = nOpt;
  if (std::isnan(n)) {
    n = estatE("N_ic", session);
    if (std::isnan(n)) n = estatE("N", session);
  }
  // These fits set e(rank) to the rank of their e(V) (30b3090c areg: 3, the
  // two slopes and _cons), which is what is read for them: the fit's own
  // e(rank) in this build is not that.
  double df = withinLl ? std::numeric_limits<double>::quiet_NaN() : estatE("df_ic", session);
  // reghdfe's estat ic charges e(df_m) + 1 parameters (its estat subcommand passes
  // df(e(df_m)+1) to the default ic computation).
  if (std::isnan(df) && ecmd == "reghdfe") df = estatE("df_m", session) + 1.0;
  if (std::isnan(df) && !withinLl) df = estatE("rank", session);
  if (std::isnan(df)) {
    // rank(e(V)) (estimates stats' fallback: colsof less the zero diagonal of
    // its generalized inverse), read here as the columns of nonzero variance
    // when their block is positive definite; anything else is not decided.
    const auto Va = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
    if (Va.matrix.has_value() && Va.matrix->data.size() == Va.matrix->colnames.size()) {
      const auto& V = Va.matrix->data;
      std::vector<size_t> idx;
      bool square = true;
      for (size_t i = 0; i < V.size(); ++i) {
        square = square && V[i].size() == V.size();
        if (square && V[i][i] != 0) idx.push_back(i);
      }
      const size_t m = idx.size();
      std::vector<std::vector<double>> L(m, std::vector<double>(m, 0.0));
      bool pd = square && m > 0;
      for (size_t i = 0; pd && i < m; ++i)
        for (size_t j = 0; pd && j <= i; ++j) {
          double sum = V[idx[i]][idx[j]];
          for (size_t q = 0; q < j; ++q) sum -= L[i][q] * L[j][q];
          if (i == j) {
            if (!(sum > 1e-13 * std::fabs(V[idx[i]][idx[i]]))) pd = false;
            else L[i][i] = std::sqrt(sum);
          } else {
            L[i][j] = sum / L[j][j];
          }
        }
      if (pd) df = static_cast<double>(m);
    }
  }
  if (std::isnan(df) || std::isnan(n)) return estatNotImplemented("ic", ecmd);
  const double ll0 = llNull;
  const double aic = -2.0 * ll + 2.0 * df;
  const double bic = -2.0 * ll + std::log(n) * df;
  auto& ev = session.eval;
  eval::replaceRClass(ev);
  eval::EvalMatrix S;
  S.rownames = {"."};
  S.colnames = {"N", "ll0", "ll", "df", "AIC", "BIC"};
  S.data = {{n, ll0, ll, df, aic, bic}};
  ev.rMatrices.emplace_back("S", std::move(S));
  eval::declareRPosted(ev, "estat", {"S"});
  std::string t;
  t += "\nAkaike's information criterion and Bayesian information criterion\n\n";
  t += std::string(77, '-') + "\n";
  t += "       Model |          N   ll(null)  ll(model)      df        AIC        BIC\n";
  t += "-------------+---------------------------------------------------------------\n";
  t += "           . |" + estatPadLeft(estatFmt("%10.0fc", n), 11) + estatPadLeft(estatFmt("%9.0g", ll0), 11) +
       estatPadLeft(estatFmt("%9.0g", ll), 11) + estatPadLeft(estatFmt("%6.0f", df), 8) +
       estatPadLeft(estatFmt("%9.0g", aic), 11) + estatPadLeft(estatFmt("%9.0g", bic), 11) + "\n";
  t += std::string(77, '-') + "\n";
  t += "Note: BIC uses N = number of observations. See [R] IC note.\n";
  return estatDisplay(t);
}

// estat vce: r(V) = e(V) (or its correlation matrix), without the columns the
// coefficient table does not show: the base levels unless baselevels /
// allbaselevels, the omitted ones under noomitted, the empty cells under
// noemptycells (vce's upRowCol). The correlation of a zero-variance column is
// missing, of any other diagonal element 1 (vce's MyCorr). Bank fc230785
// `estat vce` after regress: r(V) 4 x 4 = e(V).
ojson estatVceM4(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session, const std::string& ecmd) {
  bool corr = false, cov = false, keepBase = false, dropOmitted = false, dropEmpty = false;
  std::string fmt;
  for (const auto& [k0, v] : opts) {
    const std::string k = strutil::lower(k0);
    if (estatAbbrev(k, "covariance", 3)) cov = true;
    else if (estatAbbrev(k, "correlation", 1) || k == "rho") corr = true;
    else if (estatAbbrev(k, "block", 1) || estatAbbrev(k, "diag", 1) || estatAbbrev(k, "nolines", 6)) {
    } else if (estatAbbrev(k, "format", 1)) {
      if (v.isBool) return estatRefuse("option format() invalid", "r(198)");
      fmt = strutil::trim(v.strValue);
      if (!fmt.empty() && fmt[0] != '%') fmt = "%" + fmt;
    } else if (k == "baselevels" || k == "allbaselevels") keepBase = true;
    else if (k == "noomitted") dropOmitted = true;
    else if (k == "noemptycells") dropEmpty = true;
    else return estatNotImplemented("vce, " + k0, ecmd);  // eigen, equation(), tol(), ...
  }
  if (corr && cov) return estatRefuse("options correlation and covariance are exclusive", "r(198)");
  const auto Va = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
  const auto ba = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  if (!Va.matrix.has_value() || !ba.matrix.has_value()) return estatNotImplemented("vce", ecmd);
  const eval::EvalMatrix& V = *Va.matrix;
  const size_t k = V.colnames.size();
  if (V.data.size() != k || ba.matrix->data.size() != 1 || ba.matrix->data[0].size() != k)
    return estatNotImplemented("vce", ecmd);
  std::vector<double> b = ba.matrix->data[0], se(k);
  for (size_t i = 0; i < k; ++i) {
    if (V.data[i].size() != k) return estatNotImplemented("vce", ecmd);
    se[i] = V.data[i][i] > 0 ? std::sqrt(V.data[i][i]) : std::numeric_limits<double>::quiet_NaN();
  }
  std::vector<size_t> keep;
  bool anyFlag = false;
  for (const auto& n : V.colnames) anyFlag = anyFlag || n.find('.') != std::string::npos;
  if (anyFlag) {
    const auto notes =
        rtable_detail::coefficientNotes(session, V.colnames, b, se, std::vector<bool>(k, false));
    for (size_t i = 0; i < k; ++i) {
      if (!notes[i].has_value()) return estatNotImplemented("vce", ecmd);
      const std::string& nt = *notes[i];
      if ((nt == "(base)" && !keepBase) || (nt == "(omitted)" && dropOmitted) ||
          (nt == "(empty)" && dropEmpty))
        continue;
      keep.push_back(i);
    }
  } else {
    for (size_t i = 0; i < k; ++i) keep.push_back(i);
  }
  eval::EvalMatrix R;
  for (size_t i : keep) R.colnames.push_back(V.colnames[i]);
  R.rownames = R.colnames;
  for (size_t i : keep) {
    std::vector<double> row;
    for (size_t j : keep) {
      double x = V.data[i][j];
      if (corr) {
        const double vi = V.data[i][i], vj = V.data[j][j];
        if (vi == 0 || vj == 0) x = std::numeric_limits<double>::quiet_NaN();
        else if (i == j) x = 1.0;
        else x = x / (std::sqrt(vi) * std::sqrt(vj));
      }
      row.push_back(x);
    }
    R.data.push_back(std::move(row));
  }
  if (fmt.empty()) fmt = corr ? "%8.4f" : "%10.0g";
  std::string t = std::string("\n") + (corr ? "Correlation" : "Covariance") +
                  " matrix of coefficients of " + ecmd + " model\n\n";
  t += "        e(V) |";
  for (const auto& c : R.colnames) t += estatPadLeft(c.substr(c.rfind(':') == std::string::npos ? 0 : c.rfind(':') + 1).substr(0, 10), 11);
  t += "\n";
  for (size_t i = 0; i < R.rownames.size(); ++i) {
    std::string rn = R.rownames[i];
    if (rn.rfind(':') != std::string::npos) rn = rn.substr(rn.rfind(':') + 1);
    t += estatPadLeft(rn.substr(0, 12), 12) + " |";
    for (size_t j = 0; j <= i; ++j) t += estatPadLeft(estatFmt(fmt, R.data[i][j]), 11);
    t += "\n";
  }
  auto& ev = session.eval;
  eval::replaceRClass(ev);
  ev.rMatrices.emplace_back("V", std::move(R));
  eval::declareRPosted(ev, "estat", {"V"});
  return estatDisplay(t);
}

// estat summarize: r(stats) (mean, sd, min, max over e(sample)) of e(depvar)
// and the variables of e(b), each once, _cons left out (estat_summ; bank
// f33f21e1 after `regress price mpg weight foreign`: rows price mpg weight
// foreign). Served for an unweighted fit (or noweights) whose e(b) columns are
// plain variables of one equation and whose estimation sample is recorded; the
// other forms are not implemented here.
ojson estatSummarizeM4(const std::vector<std::string>& varlist,
                       const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                       const std::optional<std::string>& ifExpr, SessionState& session,
                       const std::string& ecmd) {
  if (ifExpr.has_value()) return estatRefuse("if not allowed", "r(101)");
  bool noWeights = false;
  for (const auto& [k0, v] : opts) {
    (void)v;
    const std::string k = strutil::lower(k0);
    if (estatAbbrev(k, "noweights", 5)) noWeights = true;
    else if (estatAbbrev(k, "labels", 3) || estatAbbrev(k, "noheader", 5) || k == "vsquish") {
    } else {
      return estatNotImplemented("summarize, " + k0, ecmd);
    }
  }
  if (!varlist.empty()) return estatNotImplemented("summarize with an equation list", ecmd);
  const auto& lm = session.lastModel();
  if (!lm.has_value()) return estatNotImplemented("summarize", ecmd);
  if (!lm->fitWeight.empty() && !noWeights) return estatNotImplemented("summarize after a weighted fit", ecmd);
  const auto ba = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kNames);
  if (!ba.matrix.has_value()) return estatNotImplemented("summarize", ecmd);
  std::string depvar;
  if (const std::string* d = session.eval.eStrings.find("depvar")) depvar = *d;
  if (depvar.empty()) depvar = lm->depvar;
  std::vector<std::string> vars;
  if (!depvar.empty()) {
    if (depvar.find(' ') != std::string::npos) return estatNotImplemented("summarize", ecmd);
    vars.push_back(depvar);
  }
  for (const auto& full : ba.matrix->colnames) {
    const size_t c = full.rfind(':');
    if (c != std::string::npos && full.substr(0, c) != depvar) return estatNotImplemented("summarize", ecmd);
    const std::string nm = c == std::string::npos ? full : full.substr(c + 1);
    if (nm == "_cons" || nm == "o._cons" || nm == "_se") continue;
    if (nm.find('#') != std::string::npos) return estatNotImplemented("summarize", ecmd);
    if (nm.find('.') != std::string::npos) {
      // A factor level: its indicator, the base level left out (bank ee20372b
      // `regress price mpg weight i.foreign`: rows price mpg weight 1.foreign).
      double lv = 0;
      std::string fv;
      if (!cecon::postest::parse_level_ref(nm, lv, fv)) return estatNotImplemented("summarize", ecmd);
      const std::string flags = nm.substr(0, nm.find('.'));
      if (flags.find('o') != std::string::npos) return estatNotImplemented("summarize", ecmd);
      if (flags.find('b') != std::string::npos && flags.find("bn") == std::string::npos) continue;
    }
    if (std::find(vars.begin(), vars.end(), nm) == vars.end()) vars.push_back(nm);
  }
  const auto rows = est_detail::esampleRowsM4(session, *lm);
  if (!rows.has_value() || rows->empty()) return estatNotImplemented("summarize", ecmd);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return estatNotImplemented("summarize", ecmd);
  eval::EvalMatrix st;
  st.colnames = {"mean", "sd", "min", "max"};
  std::string t = "\n  Estimation sample " + ecmd + "          Number of obs = " + std::to_string(rows->size()) +
                  "\n\n  -------------------------------------------------------------------\n"
                  "      Variable |         Mean      Std. dev.          Min          Max\n"
                  "  -------------+-----------------------------------------------------\n";
  for (const auto& v : vars) {
    std::vector<double> col;
    std::string err;
    double lv = 0;
    std::string src = v;
    const bool level = v.find('.') != std::string::npos && cecon::postest::parse_level_ref(v, lv, src);
    if (!ds->has(src) || !statcmd_detail::numericColumn(*ds, src, &col, &err))
      return estatNotImplemented("summarize", ecmd);
    if (level)
      for (double& x : col)
        if (!(std::isnan(x) || x >= eval::kStataMissingSentinel)) x = x == lv ? 1.0 : 0.0;
    double sum = 0, mn = HUGE_VAL, mx = -HUGE_VAL;
    size_t n = 0;
    for (const int64_t r : *rows) {
      const double x = col[static_cast<size_t>(r)];
      if (std::isnan(x) || x >= eval::kStataMissingSentinel) {
        // estat_summ refuses a variable with missing values in the sample
        return estatRefuse(v + " has missing observations", "r(498)");
      }
      sum += x;
      mn = std::min(mn, x);
      mx = std::max(mx, x);
      ++n;
    }
    const double mean = sum / static_cast<double>(n);
    double ss = 0;
    for (const int64_t r : *rows) {
      const double d = col[static_cast<size_t>(r)] - mean;
      ss += d * d;
    }
    const double sd = n > 1 ? std::sqrt(ss / static_cast<double>(n - 1)) : std::numeric_limits<double>::quiet_NaN();
    st.rownames.push_back(v);
    st.data.push_back({mean, sd, mn, mx});
    t += estatPadLeft(v.substr(0, 13), 15) + " |" + estatPadLeft(estatFmt("%9.0g", mean), 13) +
         estatPadLeft(estatFmt("%9.0g", sd), 14) + estatPadLeft(estatFmt("%9.0g", mn), 13) +
         estatPadLeft(estatFmt("%9.0g", mx), 13) + "\n";
  }
  t += "  -------------------------------------------------------------------\n";
  auto& ev = session.eval;
  eval::replaceRClass(ev);
  ev.rMatrices.emplace_back("stats", std::move(st));
  eval::declareRPosted(ev, "estat", {"stats"});
  return estatDisplay(t);
}

// estat classification / estat gof after logit, logistic and probit (lstat and
// lfit): the fit's predicted probability over e(sample) -- or, under `all`, over
// every observation whose depvar and regressors are not missing -- from e(b),
// invlogit(xb) or normal(xb). classification: "+" when Pr >= cutoff (0.5),
// D when depvar != 0; r(ctable) (+ - Total by D ~D Total) and the nine r(P_*)
// percentages (bank 684516c9 after `logit foreign mpg weight`: a 13, b 6, c 9,
// d 46, P_corr 79.72973). gof: Pearson chi2 over the covariate patterns,
// sum (y_j - M_j p_j)^2 / (M_j p_j (1 - p_j)), m patterns, df = m - (e(b)
// columns not omitted) (6fccf0af: m 74, df 71, chi2 52.171601). Served for an
// unweighted fit without offset whose e(b) columns are plain variables and
// _cons; group() (Hosmer-Lemeshow) and the other forms are not implemented here.
ojson estatBinaryM4(const std::string& what, const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                    const std::optional<std::string>& ifExpr, SessionState& session, const std::string& ecmd) {
  double cutoff = 0.5;
  bool all = false;
  for (const auto& [k0, v] : opts) {
    const std::string k = strutil::lower(k0);
    if (k == "all") all = true;
    else if (what == "classification" && estatAbbrev(k, "cutoff", 3)) {
      if (v.isBool) return estatRefuse("option cutoff() invalid", "r(198)");
      char* end = nullptr;
      const std::string t = strutil::trim(v.strValue);
      cutoff = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0') return estatRefuse("option cutoff() invalid", "r(198)");
      if (cutoff < 0 || cutoff > 1) return estatRefuse("cutoff() must be between 0 and 1", "r(198)");
    } else if (what == "gof" && estatAbbrev(k, "table", 1)) {
    } else {
      return estatNotImplemented(what + ", " + k0, ecmd);
    }
  }
  if (ifExpr.has_value()) return estatNotImplemented(what + " with if/in", ecmd);
  const auto& lm = session.lastModel();
  if (!lm.has_value() || !lm->fitWeight.empty()) return estatNotImplemented(what, ecmd);
  if (const std::string* off = session.eval.eStrings.find("offset"); off != nullptr && !off->empty())
    return estatNotImplemented(what, ecmd);
  const auto ba = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  if (!ba.matrix.has_value() || ba.matrix->data.size() != 1) return estatNotImplemented(what, ecmd);
  std::string depvar;
  if (const std::string* d = session.eval.eStrings.find("depvar")) depvar = *d;
  if (depvar.empty()) depvar = lm->depvar;
  if (depvar.empty() || depvar.find(' ') != std::string::npos) return estatNotImplemented(what, ecmd);
  std::vector<std::string> xs;
  std::vector<double> bx;
  double bcons = 0;
  bool hasCons = false;
  double nvar = 0;
  for (size_t j = 0; j < ba.matrix->colnames.size(); ++j) {
    const std::string& full = ba.matrix->colnames[j];
    const size_t c = full.rfind(':');
    if (c != std::string::npos && full.substr(0, c) != depvar) return estatNotImplemented(what, ecmd);
    const std::string nm = c == std::string::npos ? full : full.substr(c + 1);
    const double bj = ba.matrix->data[0][j];
    if (nm == "_cons") {
      hasCons = true;
      bcons = bj;
      nvar += 1;
      continue;
    }
    if (nm.find_first_of(".#") != std::string::npos) return estatNotImplemented(what, ecmd);
    xs.push_back(nm);
    bx.push_back(bj);
    nvar += 1;
  }
  (void)hasCons;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return estatNotImplemented(what, ecmd);
  std::vector<std::vector<double>> X(xs.size());
  std::vector<double> y;
  std::string err;
  if (!ds->has(depvar) || !statcmd_detail::numericColumn(*ds, depvar, &y, &err)) return estatNotImplemented(what, ecmd);
  for (size_t j = 0; j < xs.size(); ++j)
    if (!ds->has(xs[j]) || !statcmd_detail::numericColumn(*ds, xs[j], &X[j], &err))
      return estatNotImplemented(what, ecmd);
  const auto miss = [](double v) { return std::isnan(v) || v >= eval::kStataMissingSentinel; };
  std::vector<int64_t> rows;
  if (all) {
    for (size_t i = 0; i < y.size(); ++i) {
      bool ok = !miss(y[i]);
      for (const auto& col : X) ok = ok && !miss(col[i]);
      if (ok) rows.push_back(static_cast<int64_t>(i));
    }
  } else {
    const auto er = est_detail::esampleRowsM4(session, *lm);
    if (!er.has_value()) return estatNotImplemented(what, ecmd);
    rows = *er;
  }
  if (rows.empty()) return estatNotImplemented(what, ecmd);
  const bool probit = ecmd == "probit";
  std::vector<double> p;
  for (const int64_t r : rows) {
    double xb = bcons;
    for (size_t j = 0; j < xs.size(); ++j) xb += bx[j] * X[j][static_cast<size_t>(r)];
    p.push_back(probit ? 0.5 * std::erfc(-xb / std::sqrt(2.0)) : std::exp(xb) / (1.0 + std::exp(xb)));
  }
  auto& ev = session.eval;
  std::string t;
  if (what == "classification") {
    double a = 0, b = 0, c = 0, d = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      const bool D = y[static_cast<size_t>(rows[i])] != 0;
      const bool pos = p[i] >= cutoff;
      if (D && pos) ++a;
      else if (!D && pos) ++b;
      else if (D) ++c;
      else ++d;
    }
    eval::replaceRClass(ev);
    eval::EvalMatrix ct;
    ct.rownames = {"+", "-", "Total"};
    ct.colnames = {"D", "~D", "Total"};
    ct.data = {{a, b, a + b}, {c, d, c + d}, {a + c, b + d, a + b + c + d}};
    ev.rMatrices.emplace_back("ctable", std::move(ct));
    const std::vector<std::pair<std::string, double>> P = {
        {"P_corr", (a + d) / (a + b + c + d) * 100}, {"P_p1", a / (a + c) * 100}, {"P_n0", d / (b + d) * 100},
        {"P_p0", b / (b + d) * 100},  {"P_n1", c / (a + c) * 100}, {"P_1p", a / (a + b) * 100},
        {"P_0n", d / (c + d) * 100},  {"P_0p", b / (a + b) * 100}, {"P_1n", c / (c + d) * 100}};
    std::vector<std::string> names = {"ctable"};
    for (const auto& [k, v] : P) {
      ev.rValues.set(k, v);
      names.push_back(k);
    }
    eval::declareRPosted(ev, "estat", std::move(names));
    t = "\n" + std::string(ecmd == "probit" ? "Probit" : "Logistic") + " model for " + depvar +
        "\n\n              -------- True --------\nClassified |         D            ~D  |      Total\n";
    t += "-----------+--------------------------+-----------\n";
    t += "     +     |" + estatPadLeft(estatNum(a), 10) + estatPadLeft(estatNum(b), 14) + "  |" + estatPadLeft(estatNum(a + b), 11) + "\n";
    t += "     -     |" + estatPadLeft(estatNum(c), 10) + estatPadLeft(estatNum(d), 14) + "  |" + estatPadLeft(estatNum(c + d), 11) + "\n";
    t += "-----------+--------------------------+-----------\n";
    t += "   Total   |" + estatPadLeft(estatNum(a + c), 10) + estatPadLeft(estatNum(b + d), 14) + "  |" + estatPadLeft(estatNum(a + b + c + d), 11) + "\n\n";
    t += "Classified + if predicted Pr(D) >= " + estatNum(cutoff) + "\nTrue D defined as " + depvar + " != 0\n";
    t += "Correctly classified " + estatFmt("%8.2f", (a + d) / (a + b + c + d) * 100) + "%\n";
    return estatDisplay(t);
  }
  // gof: covariate patterns
  std::map<std::vector<double>, std::array<double, 3>> pat;  // M, yobs, yexp
  for (size_t i = 0; i < rows.size(); ++i) {
    std::vector<double> key;
    for (const auto& col : X) key.push_back(col[static_cast<size_t>(rows[i])]);
    auto& e = pat[key];
    e[0] += 1;
    e[1] += y[static_cast<size_t>(rows[i])] != 0 ? 1 : 0;
    e[2] += p[i];
  }
  double chi2 = 0;
  for (const auto& [key, e] : pat)
    if (e[2] > 0 && e[2] < e[0]) chi2 += (e[1] - e[2]) * (e[1] - e[2]) / (e[2] * (1 - e[2] / e[0]));
  const double m = static_cast<double>(pat.size());
  const double N = static_cast<double>(rows.size());
  const double df = m - nvar;
  const double pv = df > 0 ? chi2UpperTailIntegerDf(df, chi2) : std::numeric_limits<double>::quiet_NaN();
  eval::replaceRClass(ev);
  ev.rValues.set("N", N);
  ev.rValues.set("m", m);
  ev.rValues.set("chi2", chi2);
  ev.rValues.set("df", df);
  ev.rValues.set("p", pv);
  eval::declareRPosted(ev, "estat", {"N", "m", "chi2", "df", "p"});
  t = "\nGoodness-of-fit test after " + std::string(ecmd == "probit" ? "probit" : "logistic") + " model\nVariable: " + depvar +
      "\n\n      Number of observations = " + estatPadLeft(estatFmt("%10.0fc", N), 10) +
      "\nNumber of covariate patterns = " + estatPadLeft(estatFmt("%10.0fc", m), 10) +
      "\n          Pearson chi2(" + estatNum(df) + ") = " + estatFmt("%9.2f", chi2) +
      "\n                 Prob > chi2 = " + estatFmt("%6.4f", pv) + "\n";
  return estatDisplay(t);
}

// Subcommands an estimator's own estat handler serves beyond the defaults
// (Stata's <cmd>_estat programs; minimum abbreviation lengths are theirs).
// After a command listed here, any other subcommand is r(321) "estat X not
// valid"; after a command not listed, an unknown subcommand is reported as not
// implemented (its own handler is not tabulated here).
const std::map<std::string, std::vector<std::pair<std::string, size_t>>>& estatOwnSubcommands() {
  static const std::map<std::string, std::vector<std::pair<std::string, size_t>>> k = {
      {"regress",
       {{"ovtest", 3}, {"hettest", 4}, {"szroeter", 3}, {"vif", 3}, {"imtest", 3}, {"esize", 3},
        {"dwatson", 3}, {"durbinalt", 3}, {"bgodfrey", 3}, {"archlm", 6}, {"sbknown", 7},
        {"sbsingle", 8}, {"sbcusum", 7}, {"moran", 3}}},
      {"logit", {{"classification", 4}, {"gof", 3}, {"auc", 3}}},
      {"logistic", {{"classification", 4}, {"gof", 3}, {"auc", 3}}},
      {"probit", {{"classification", 4}, {"gof", 3}, {"auc", 3}}},
      {"poisson", {{"gof", 3}}},
  };
  return k;
}

// What these subcommands' syntax refuses before anything runs: `estat ic` and
// `estat vce` take options only (`syntax [, ...]`: a varlist is "varlist not
// allowed" r(101), an if "if not allowed" r(101)); after logit, logistic and
// probit, classification, gof and auc refuse a varlist the same way (their
// estat checks for one before it computes; their if/in is the sample's).
std::optional<ojson> estatSyntaxRefusalM4(const std::string& sub, const std::vector<std::string>& varlist,
                                          const std::optional<std::string>& ifExpr, const std::string& ecmd) {
  const bool optionsOnly = sub == "ic" || sub == "vce";
  const bool binary = (ecmd == "logit" || ecmd == "logistic" || ecmd == "probit") &&
                      (estatAbbrev(sub, "classification", 4) || estatAbbrev(sub, "gof", 3) ||
                       estatAbbrev(sub, "auc", 3));
  if (!varlist.empty() && (optionsOnly || binary)) return estatRefuse("varlist not allowed", "r(101)");
  if (ifExpr.has_value() && optionsOnly) return estatRefuse("if not allowed", "r(101)");
  return std::nullopt;
}

ojson estatAfterFitM4(const std::string& sub, const std::vector<std::string>& varlist,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                      const std::optional<std::string>& ifExpr, SessionState& session) {
  std::string ecmd;
  if (const std::string* c = session.eval.eStrings.find("cmd")) ecmd = *c;
  if (ecmd.empty() && session.lastModel().has_value()) {
    const auto& lm = *session.lastModel();
    ecmd = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
  }
  if (sub.empty()) return estatRefuse("subcommand required", "r(321)");
  if (auto refused = estatSyntaxRefusalM4(sub, varlist, ifExpr, ecmd)) return *refused;
  // e(estat_cmd) estat_vce_only (mean, proportion): its subcommands are vce, sd and
  // bootstrap (at least `boot`); any other is "invalid subcommand" r(321) (vs-stata
  // bank acde0da3 `estat size`, 8c741ffc `estat summarize` after `mean price mpg,
  // over(foreign)`). sd is the survey estat's: after anything but mean "mean
  // estimation results not found" r(301), after mean's stdize() r(322); after mean
  // it is not implemented here.
  if (const std::string* ec = session.eval.eStrings.find("estat_cmd");
      ec != nullptr && *ec == "estat_vce_only" && sub != "vce") {
    if (sub == "sd") {
      if (ecmd != "mean") return estatRefuse("mean estimation results not found", "r(301)");
      if (const std::string* sz = session.eval.eStrings.find("stdize"); sz != nullptr && !sz->empty())
        return estatRefuse("estat sd is not allowed with direct standardization", "r(322)");
      return estatNotImplemented(sub, ecmd);
    }
    if (!estatAbbrev(sub, "bootstrap", 4)) return estatRefuse("invalid subcommand " + sub, "r(321)");
  }
  if (sub == "ic") return estatIcM4(opts, session, ecmd);
  if (sub == "vce") return estatVceM4(opts, session, ecmd);
  if (estatAbbrev(sub, "summarize", 2)) return estatSummarizeM4(varlist, opts, ifExpr, session, ecmd);
  if (estatAbbrev(sub, "bootstrap", 4) || estatAbbrev(sub, "parallel", 3))
    return estatNotImplemented(sub, ecmd);
  // xtabond's estat abond: the one-step estimator under the default vce(gmm)
  // computes no Arellano-Bond tests, r(198) (Stata's xtdpd estat; vs-stata bank
  // e74c99c4 `estat abond` after `xtabond frate beertax unemp`). PIET's xtabond
  // is one-step only, so the rule is the fit's vce.
  if (ecmd == "xtabond" && estatAbbrev(sub, "abond", 2)) {
    std::string vce;
    if (const std::string* v = session.eval.eStrings.find("vce")) vce = *v;
    if (vce.empty() && session.lastModel().has_value()) vce = session.lastModel()->vcov;
    if (vce != "robust")
      return estatRefuse("artests not computed for one-step system estimator with vce(gmm)", "r(198)");
    return estatNotImplemented(sub, ecmd);
  }
  if ((ecmd == "logit" || ecmd == "logistic" || ecmd == "probit") &&
      (estatAbbrev(sub, "classification", 4) || estatAbbrev(sub, "gof", 3)))
    return estatBinaryM4(estatAbbrev(sub, "gof", 3) ? "gof" : "classification", opts, ifExpr, session, ecmd);
  const auto& table = estatOwnSubcommands();
  const auto it = table.find(ecmd);
  if (it == table.end()) return estatNotImplemented(sub, ecmd);
  for (const auto& [full, minLen] : it->second)
    if (estatAbbrev(sub, full, minLen)) return estatNotImplemented(sub, ecmd);
  return estatRefuse("estat " + sub + " not valid", "r(321)");
}

}  // namespace

namespace {
// The sample of the serial-correlation tests and estat dwatson. Stata takes
// e(sample), requires a time variable (r(111) "time variable not set"), allows
// a panel variable only when the sample holds one panel (r(459) "sample may
// not include multiple panels"), sorts by time and lags the residuals by time
// (r(N_gaps) = tsreport on the sample). The engine refits on the rows its
// varlist admits and lags by data row, so its answer is Stata's only when the
// sample rows are in time order and without gaps. The sample is rebuilt here
// from the data: y and every regressor nonmissing, a regressor `L.v` / `L#.v` /
// `D.v` / `F.v` read at its own time, a factor-variable or interaction term
// (`i.v`, `c.a#c.b`, `a##b`) through each of its variables.
struct SerialSampleM4 {
  enum Status { Ok, NoTimevar, MultiPanel, Unsorted, Unknown } status = Unknown;
  long long gaps = 0;
};

SerialSampleM4 serialSampleM4(SessionState& session, const std::string& y,
                              const std::vector<std::string>& xs, bool hasIf) {
  SerialSampleM4 out;
  if (!session.timevar.has_value()) {
    out.status = SerialSampleM4::NoTimevar;
    return out;
  }
  if (hasIf) return out;
  Datastore* frame = nullptr;
  try {
    frame = session.datastore();
  } catch (...) {
    return out;
  }
  if (frame == nullptr) return out;
  const double delta = session.delta.value_or(1.0);
  if (!(delta > 0)) return out;
  auto numeric = [&](const std::string& v) -> const double* {
    if (!frame->has(v) || frame->dtype(v) != "f64") return nullptr;
    return frame->col(v).f64();
  };
  const double* tcol = numeric(*session.timevar);
  if (tcol == nullptr) return out;
  const double* pcol = nullptr;
  if (session.panelvar.has_value()) {
    pcol = numeric(*session.panelvar);
    if (pcol == nullptr) return out;
  }
  struct Term {
    const double* col;
    std::vector<double> shifts;  // time offsets (in units of delta) that must be nonmissing
  };
  std::vector<Term> terms;
  bool anyShift = false;
  auto addTerm = [&](const std::string& tok) -> bool {
    static const std::regex reTs(R"(^(?:([LlFfDd])(\d*)\.)?([A-Za-z_]\w*)$)");
    // i. c. ib#. ibn. ib(first). 2. 0b. o. ...: an operator that starts with i, c, o or a digit.
    static const std::regex reFvPart(R"(^(?:(?:[iIcCoO]|\d)[A-Za-z0-9()]*\.)?([A-Za-z_]\w*)$)");
    std::smatch m;
    if (std::regex_match(tok, m, reTs)) {
      const double* c = numeric(m.str(3));
      if (c == nullptr) return false;
      Term t{c, {}};
      const std::string op = m.str(1);
      const int k = m.str(2).empty() ? 1 : std::stoi(m.str(2));
      if (op.empty()) t.shifts = {0};
      else if (op == "L" || op == "l") t.shifts = {-static_cast<double>(k)};
      else if (op == "F" || op == "f") t.shifts = {static_cast<double>(k)};
      else
        for (int j = 0; j <= k; ++j) t.shifts.push_back(-static_cast<double>(j));  // D#.v reads v back k periods
      anyShift = anyShift || t.shifts.size() > 1 || t.shifts[0] != 0;
      terms.push_back(std::move(t));
      return true;
    }
    // A factor-variable or interaction term: each of its variables at its own time.
    std::string rest = tok;
    for (size_t p; (p = rest.find("##")) != std::string::npos;) rest.replace(p, 2, "#");
    std::stringstream parts(rest);
    std::string part;
    bool anyPart = false;
    while (std::getline(parts, part, '#')) {
      std::smatch pm;
      if (!std::regex_match(part, pm, reFvPart)) return false;
      const double* c = numeric(pm.str(1));
      if (c == nullptr) return false;
      terms.push_back(Term{c, {0}});
      anyPart = true;
    }
    return anyPart;
  };
  if (!addTerm(y)) return out;
  for (const auto& x : xs)
    if (!addTerm(x)) return out;
  const int64_t n = frame->nRows();
  // Lagged regressors under a panel: the lookup would have to stay inside the
  // panel; not rebuilt here.
  if (pcol != nullptr && anyShift) return out;
  std::map<double, int64_t> rowAt;
  if (anyShift) {
    for (int64_t r = 0; r < n; ++r) {
      if (std::isnan(tcol[r])) continue;
      if (!rowAt.emplace(tcol[r], r).second) return out;  // a repeated time value
    }
  }
  std::vector<int64_t> sample;
  for (int64_t r = 0; r < n; ++r) {
    bool in = true;
    for (const auto& term : terms) {
      for (double sh : term.shifts) {
        int64_t rr = r;
        if (sh != 0) {
          if (std::isnan(tcol[r])) {
            in = false;
            break;
          }
          const auto it = rowAt.find(tcol[r] + sh * delta);
          if (it == rowAt.end()) {
            in = false;
            break;
          }
          rr = it->second;
        }
        if (std::isnan(term.col[rr])) {
          in = false;
          break;
        }
      }
      if (!in) break;
    }
    if (in) sample.push_back(r);
  }
  if (pcol != nullptr) {
    for (const int64_t r : sample) {
      if (std::isnan(pcol[r])) return out;
      if (pcol[r] != pcol[sample.front()]) {
        out.status = SerialSampleM4::MultiPanel;
        return out;
      }
    }
  }
  long long gaps = 0;
  bool any = false;
  double last = 0;
  for (const int64_t r : sample) {
    const double t = tcol[r];
    if (std::isnan(t)) return out;  // Stata marks such rows out; the engine keeps them
    if (any && !(t > last)) {
      out.status = SerialSampleM4::Unsorted;
      return out;
    }
    if (any && t - last > delta * (1 + 1e-12)) ++gaps;
    any = true;
    last = t;
  }
  out.status = SerialSampleM4::Ok;
  out.gaps = gaps;
  return out;
}
}  // namespace

ojson runEstat(const std::string& subcmdIn, const std::vector<std::string>& rawVarlistAfterSub,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               const std::optional<std::string>& ifExpr, SessionState& session) {
  std::string sub = lowerM4(subcmdIn);
  // BUG-1248: estat.ado refuses an empty e(cmd) before it reads its subcommand:
  // with no estimates every `estat` -- a diagnostic with an explicit varlist, a
  // csdid aggregate, an unknown subcommand, none at all -- is r(301) "last
  // estimates not found" (StataBE 19.5; noPreviousEstimation's r(301) here). It
  // answered r(198), r(199) or rc 1. A posting with no command name behind it
  // (`ereturn post b V`) is the same empty e(cmd): `estat ic`, `estat vce`, bare
  // `estat` are r(301) there too.
  const std::string* eCmd = session.eval.eStrings.find("cmd");
  if (!session.eval.eLive() || (session.ePostedLive && (eCmd == nullptr || eCmd->empty())))
    return noPreviousEstimation();

  static const std::map<std::string, std::string> kEstatToDiag = {
      {"hettest", "hettest"}, {"ovtest", "ovtest"},     {"vif", "vif"},
      {"dwatson", "dwatson"}, {"bgodfrey", "bgodfrey"}, {"archlm", "archlm"},
      // BS-021 (BETA D5 batch) fix: `estat durbinalt` was previously
      // ALIASED onto cobra's `bgodfrey` command — a mislabeled WRONG test
      // (Breusch-Godfrey LM, not Durbin's alternative test). The engine
      // has its OWN dedicated `durbinalt` CLI (`cobra durbinalt --y --x
      // [--lags "N"] [--small] [--robust] [--nomiss0]`), live-verified
      // against oracle D5b: bare (chi2=372.934, df=1) and `, small
      // lags(2)` (F=189.005, df=(2,331)) both byte-for-byte engine
      // matches — routed to its own diagCmd now, with its own known-
      // options set (small/lags) below, rather than sharing the empty
      // known-set every other row in this map uses.
      {"durbinalt", "durbinalt"},
  };
  auto diagIt = kEstatToDiag.find(sub);
  if (diagIt != kEstatToDiag.end()) {
    std::string diagCmdPeek = diagIt->second;
    // BUG-1222: estat hettest is regress's (regress_estat). After any other
    // estimator StataBE answers r(321) whatever the varlist -- "estat hettest not
    // valid" after areg, xtreg fe/re, ivregress, newey, prais, logit, probit and
    // poisson, "invalid subcommand hettest" from reghdfe's own estat -- and after
    // a robust or clustered regress r(498) (StataBE 19.5, scratch-laneA/agents/N1
    // b2.do, p-margins s3.do/s4.do; `[aw]` and `, rhs` are served). The z-list
    // form used to test a plain-OLS refit at rc 0 after all of them.
    if (diagCmdPeek == "hettest") {
      const std::string* ecmd = session.eval.eStrings.find("cmd");
      if (ecmd != nullptr && !ecmd->empty() && *ecmd != "regress" && *ecmd != "anova") {
        return ojson(eval::errorBlock(
            "engine_error", *ecmd == "reghdfe" ? "invalid subcommand hettest" : "estat hettest not valid",
            std::nullopt, std::nullopt, std::nullopt, "r(321)"));
      }
      // A pweighted fit is refused before the VCE is read: r(101) "hettest not
      // appropriate with pweighted data" (the variance test's own check order).
      const std::string* ewtype = session.eval.eStrings.find("wtype");
      if (ecmd != nullptr && *ecmd == "regress" && ewtype != nullptr && *ewtype == "pweight") {
        return ojson(eval::errorBlock("engine_error", "hettest not appropriate with pweighted data",
                                      std::nullopt, std::nullopt, std::nullopt, "r(101)"));
      }
      const std::string* evce = session.eval.eStrings.find("vce");
      if (ecmd != nullptr && *ecmd == "regress" && evce != nullptr &&
          (*evce == "robust" || *evce == "cluster")) {
        return ojson(eval::errorBlock(
            "engine_error", "hettest not appropriate after estimation using robust or clustered VCE",
            std::nullopt, std::nullopt, std::nullopt, "r(498)"));
      }
    }
    // BS-097: the hettest/ovtest/vif/dwatson/bgodfrey/archlm diagnostic
    // branch reads NO options at all today (real Stata has e.g. `vif,
    // uncentered` / `bgodfrey/archlm, lags(#)` — none implemented here) —
    // known-set is the empty set. `durbinalt` (BS-021, BETA D5 batch) is
    // the ONE exception: real Stata's `estat durbinalt [, small lags(#)]`
    // genuinely has two options, and cobra's OWN durbinalt CLI accepts
    // both — its known-set is {small, lags} instead. Branch-scoped
    // deliberately: the csdid sub-branch below (`estat event, window(...)
    // post`) has its OWN, non-empty known set, so a single net at function
    // entry would either wrongly reject `window`/`post`/`small`/`lags`
    // here or wrongly accept them for the WRONG branch — neither is
    // correct, hence one check per branch.
    // BUG-233: the one copy of this vocabulary now lives in the file-scope
    // registration block (see top of file); this binding keeps every existing
    // use — including the reject net below — reading the same set.
    const std::unordered_set<std::string>& kDurbinaltKnownOpts = kVocabEstatDurbinalt.known;
    // bgodfrey takes lags(numlist) and small, archlm lags(numlist) (Stata's
    // `estat bgodfrey [, lags(numlist) nomiss0 small]`, `estat archlm [,
    // lags(numlist)]`); the engine's own --lags/--small serve them. nomiss0
    // (and durbinalt's force/robust) keep the generic refusal.
    static const std::unordered_set<std::string> kBgodfreyKnownOpts = {"lags", "small"};
    static const std::unordered_set<std::string> kArchlmKnownOpts = {"lags"};
    // hettest, rhs: the explanatory variables of the variance test are the
    // fit's own regressors instead of the fitted values (bank 6dc113cd: chi2(3)
    // 11.181101957129172 after `regress price mpg weight foreign`).
    static const std::unordered_set<std::string> kHettestKnownOpts = {"rhs"};
    if (auto err = eval::rejectUnknownOptions(
            opts, diagCmdPeek == "durbinalt"  ? kDurbinaltKnownOpts
                  : diagCmdPeek == "bgodfrey" ? kBgodfreyKnownOpts
                  : diagCmdPeek == "archlm"   ? kArchlmKnownOpts
                  : diagCmdPeek == "hettest"  ? kHettestKnownOpts
                                              : std::unordered_set<std::string>{})) {
      return *err;
    }
    // The serial-correlation tests refuse a robust or clustered fit before
    // anything else (r(198)): bgodfrey after any e(vcetype) Robust; durbinalt
    // and archlm after vce(cluster) and, without their `force` option (not
    // implemented here), after vce(robust).
    if (diagCmdPeek == "bgodfrey" || diagCmdPeek == "durbinalt" || diagCmdPeek == "archlm") {
      const std::string* vcetype = session.eval.eStrings.find("vcetype");
      const std::string* clustvar = session.eval.eStrings.find("clustvar");
      const bool robust = vcetype != nullptr && *vcetype == "Robust";
      const bool clustered = clustvar != nullptr && !clustvar->empty();
      auto refuse198 = [](const std::string& msg) {
        return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt, "r(198)"));
      };
      if (diagCmdPeek == "bgodfrey" && robust)
        return refuse198("This command does not work after regress, robust");
      if (diagCmdPeek == "archlm" && clustered)
        return refuse198("estat archlm does not work after regress, vce(cluster clustvar)");
      if (diagCmdPeek == "durbinalt" && clustered)
        return refuse198("This command does not work after specifying the cluster() option");
      if (diagCmdPeek == "archlm" && robust)
        return refuse198("you must specify force after regress, vce(robust)");
      if (diagCmdPeek == "durbinalt" && robust)
        return refuse198("you must specify force after regress, robust");
    }
    bool hettestRhs = false;
    for (const auto& [k, v] : opts)
      if (k == "rhs" && v.isBool && v.boolValue) hettestRhs = true;
    if (hettestRhs && !rawVarlistAfterSub.empty())
      return engineErrorBlockM4("estat hettest with both a varlist and rhs is not implemented in this "
                                "version of PIET; nothing was computed.",
                                "unsupported_option");
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    std::string diagCmd = diagIt->second;
    // FU-504: cobra's diagnostics (cmds_diag.cpp) read only --y/--x and refit
    // least squares WITH a constant; they have no --no-intercept. After
    // `regress y x1 x2, noconstant` the fallback below (and hettest's z-list
    // path, which keeps the fit's y/x) therefore tested the with-constant model
    // at rc 0: `estat hettest` printed the same F/chi2 as after `regress y x1 x2`.
    // No oracle captures these diagnostics after a no-constant fit, so refuse by
    // name (r(198)) on the two paths that take y/x from the last model. An
    // explicit `estat <diag> y x1 x2` still runs as typed.
    auto noConstantDiagReject = [&sub](const LastModel& lm) {
      const std::string fitCmd = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
      return engineErrorBlockM4(
          "estat " + sub + " after `" + fitCmd +
              " ..., noconstant` is not supported in this version of PIET -- the engine's "
              "diagnostic refits the model with a constant, so it would test a different model "
              "than the one fitted. Refusing rather than reporting a statistic that is not the "
              "one asked for.",
          "unsupported_option");
    };
    std::vector<std::string> varlist = rawVarlistAfterSub;
    // BS-024 (BETA Phase C batch C1): captured BEFORE the fallback below
    // (which appends lastModel's y/x into `varlist` when the user gave
    // none) — distinguishes "the user actually typed a varlist" from "we
    // fell back to lastModel", needed by the hettest-specific y/xs split
    // further down.
    bool userGaveVarlist = !varlist.empty();
    if (varlist.empty()) {
      const auto& lmOpt = session.lastModel();
      if (lmOpt.has_value() && !lmOpt->y.empty() && !lmOpt->x.empty()) {
        // BS-013 (BETA A4): the diagnostic family has NO --fe flag on
        // cobra's own CLI at all (verified: apps/cmds/cmds_diag.cpp's
        // cmd_hettest/cmd_ovtest/cmd_vif/cmd_dwatson/cmd_durbinalt/
        // cmd_bgodfrey/cmd_archlm each read only --y/--x/--keepif — there is
        // no fixed-effects parameter to forward even in principle). Running
        // a plain-OLS diagnostic against a lastModel that actually had fixed
        // effects would silently answer a DIFFERENT question than the one
        // just estimated — honest-reject instead (this codebase's
        // established "silent-wrong -> honest reject" idiom, e.g. BS-023/
        // BS-052) rather than fabricate FE support the engine doesn't have.
        // Only the varlist-FALLBACK path is affected: an explicit
        // `estat hettest y x1 x2` still runs exactly as the user asked.
        if (!lmOpt->fe.empty()) {
          return engineErrorBlockM4(
              "estat " + sub + ": not supported after a fixed-effects model in this version of "
              "PIET (the fixed-effects structure cannot be forwarded to this diagnostic). Pass "
              "an explicit varlist instead, e.g. `estat " + sub + " y x1 x2`.",
              "unsupported_option");
        }
        // BUG-204's remaining half, and the same argument one line up. These
        // diagnostics are OLS constructs: cobra's cmd_hettest/cmd_ovtest/cmd_vif/
        // ... read only --y/--x and refit least squares. After an ML fit the
        // varlist fallback therefore answers a DIFFERENT question than the one
        // just estimated — measured: `estat hettest` after `logit` returned
        // F .0064338 / chi2 .0025107 with rc 0, while Stata rejects it outright.
        // Same shape as BUG-203/BUG-297, whose `predict` half is already closed:
        // an OLS refit smuggled in behind a non-OLS fit. Honest reject, and only
        // on the FALLBACK path — an explicit varlist still runs as asked.
        static const std::set<std::string> kOlsLikeForDiag = {
            "feols", "regress", "reghdfe", "ivregress", "ivreghdfe", "areg", "xtreg", "newey",
            "prais", "reg3",
        };
        if (!lmOpt->cobraCmd.empty() && kOlsLikeForDiag.count(lmOpt->cobraCmd) == 0) {
          return engineErrorBlockM4(
              "estat " + sub + ": not supported after `" + lmOpt->cobraCmd + "` — this diagnostic "
              "is a least-squares construct and PIET would refit OLS behind your model, answering "
              "a different question. Stata rejects it here too. Pass an explicit varlist if you "
              "mean the OLS diagnostic, e.g. `estat " + sub + " y x1 x2`.",
              "unsupported_option");
        }
        if (postestFitLacksConstantM4(*lmOpt)) return noConstantDiagReject(*lmOpt);
        varlist.push_back(lmOpt->y);
        for (const auto& x : lmOpt->x) varlist.push_back(x);
      }
    }
    if (varlist.empty()) {
      return engineErrorBlockM4("estat " + sub + ": no variables — run a regression first or " +
                                "pass a varlist explicitly.");
    }
    std::string y;
    std::vector<std::string> xs;
    std::vector<std::string> hettestZ;  // BS-024: explicit aux vars → cobra's --z
    if (userGaveVarlist && diagCmd == "hettest") {
      // BS-024: `estat hettest z1 z2` — z1/z2 are the AUXILIARY explanatory
      // variables for the Breusch-Pagan test (real Stata: "estat hettest
      // [varlist] ... = BP using z1 z2 as explanatory vars"), NOT a
      // replacement (y, x) pair for the base regression. Previously this
      // arm applied the SAME varlist[0]-is-y / rest-is-x split used by the
      // lastModel FALLBACK path above to an EXPLICIT user varlist too —
      // e.g. `estat hettest spirits unemp` silently became y=spirits,
      // x=unemp (dropping "spirits" as a test regressor entirely and
      // testing an unrelated income~unemp model instead of the actually-
      // fitted income~beertax one), which is why the varlist form's chi2
      // came back with the wrong df and the wrong value (df=1 off one
      // dropped variable, instead of Stata's real df=2 using both). The
      // base regression's y AND x must stay the LAST-FITTED model's (the
      // residuals under test come from income~beertax, not a refit); the
      // user varlist maps to cobra hettest's dedicated `--z Z1,Z2` flag
      // (aux explanatory vars for the variance regression) — NOT to --x,
      // which would silently refit the base model on the aux vars (the
      // first attempt's bug: chi2 came back from the wrong base model).
      const auto& lmOptHettest = session.lastModel();
      if (!lmOptHettest.has_value() || lmOptHettest->y.empty()) {
        return engineErrorBlockM4(
            "estat hettest: no variables — run a regression first or pass a varlist "
            "explicitly.");
      }
      if (postestFitLacksConstantM4(*lmOptHettest)) return noConstantDiagReject(*lmOptHettest);
      y = lmOptHettest->y;
      xs = lmOptHettest->x;
      hettestZ = varlist;
    } else {
      y = varlist[0];
      xs = std::vector<std::string>(varlist.begin() + 1, varlist.end());
    }
    std::vector<std::string> args = {diagCmd, "--y", y};
    if (!xs.empty()) {
      args.push_back("--x");
      args.push_back(joinCommaM4(xs));
    }
    if (!hettestZ.empty()) {
      args.push_back("--z");
      args.push_back(joinCommaM4(hettestZ));
    }
    // hettest, rhs: the variance regression's explanatory variables are the
    // fit's own regressors as the engine builds them from --x (factor
    // variables and interactions expanded, the base level out), not their
    // names re-read as plain columns.
    if (hettestRhs && diagCmd == "hettest" && hettestZ.empty()) args.push_back("--rhs");
    const bool serialTest = diagCmd == "durbinalt" || diagCmd == "bgodfrey" || diagCmd == "archlm";
    const bool timeTest = serialTest || diagCmd == "dwatson";
    // The fit-sample guards. These tests read the residuals of the fit in
    // e(sample) (weighted like the fit); the engine refits unweighted OLS on
    // the rows its varlist admits. A weighted fit is refused here; a fit on a
    // restricted sample is refused below when the engine's N is not e(N).
    const bool usesFit = !userGaveVarlist || diagCmd == "hettest";
    const bool sampleGuarded = diagCmd == "hettest" || timeTest;
    SerialSampleM4 tsSample;
    if (timeTest) {
      tsSample = serialSampleM4(session, y, xs, ifExpr.has_value());
      if (tsSample.status == SerialSampleM4::NoTimevar)
        return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                      std::nullopt, std::nullopt, std::nullopt, "r(111)"));
      if (tsSample.status == SerialSampleM4::MultiPanel)
        return ojson(eval::errorBlock("engine_error", "sample may not include multiple panels", std::nullopt,
                                      std::nullopt, std::nullopt, "r(459)"));
    }
    if (sampleGuarded && usesFit) {
      const std::string* wt = session.eval.eStrings.find("wtype");
      if (wt != nullptr && !wt->empty())
        return engineErrorBlockM4("estat " + diagCmd + " after a weighted fit ([" + *wt +
                                      "]) is not implemented in this version of PIET; nothing was computed.",
                                  "unsupported_option");
    }
    if (timeTest) {
      // The engine lags the residuals by data row: that is the time lag only
      // when the sample rows are in time order without gaps.
      if (tsSample.status == SerialSampleM4::Unsorted)
        return engineErrorBlockM4("estat " + diagCmd + " on data not sorted by the time variable is not "
                                  "implemented in this version of PIET (sort by it first); nothing was computed.",
                                  "unsupported_option");
      if (tsSample.status != SerialSampleM4::Ok)
        return engineErrorBlockM4("estat " + diagCmd + ": PIET cannot rebuild this test's sample from the time "
                                  "variable (an if qualifier, a lagged regressor under a panel, a repeated or "
                                  "missing time value, or an operator it does not read); this is not implemented "
                                  "in this version of PIET; nothing was computed.",
                                  "unsupported_option");
      if (tsSample.gaps > 0)
        return engineErrorBlockM4("estat " + diagCmd + " on a sample with gaps in the time variable is "
                                  "not implemented in this version of PIET; nothing was computed.",
                                  "unsupported_option");
    }
    // BS-021 (BETA D5 batch): durbinalt's own small/lags() options -> cobra
    // durbinalt's --small/--lags. Oracle D5b: bare = chi2 form (no --small,
    // no --lags — matches the engine's own bare default of lag order 1);
    // `, small lags(2)` = F form with lag order 2 (engine's `--lags "2"`
    // means "test lag orders 1 through 2", i.e. Stata's own lags(#) max-
    // order semantics — live-verified NOT a literal single-lag selector).
    // bgodfrey / archlm read the same two options (archlm lags() only). lags()
    // is a numlist of positive integers (`lags(1/3)` = 1 2 3, bank a95898db),
    // expanded here: the engine reads a plain list.
    if (serialTest) {
      bool small = false;
      std::optional<std::string> lagsVal;
      for (const auto& [k, v] : opts) {
        if (k == "small" && v.isBool && v.boolValue) small = true;
        if (k == "lags" && !v.isBool) lagsVal = v.strValue;
      }
      if (small) args.push_back("--small");
      if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
        // The numlist is read sorted ascending (`lags(3 1)` tests lag 1, then
        // 3: r(lags) "1 3" and the columns of r(chi2) in that order).
        std::vector<std::string> el;
        bool ok = loops::expandNumlist(trimM4(*lagsVal), &el) && !el.empty();
        std::vector<long long> lagOrders;
        for (const auto& t : el) {
          double v = 0;
          try {
            v = std::stod(t);
          } catch (...) {
            ok = false;
          }
          if (!ok || v < 1 || v != std::floor(v)) {
            ok = false;
            break;
          }
          lagOrders.push_back(static_cast<long long>(v));
        }
        std::sort(lagOrders.begin(), lagOrders.end());
        std::string list;
        for (const long long v : lagOrders) list += (list.empty() ? "" : " ") + std::to_string(v);
        if (!ok)
          return ojson(eval::errorBlock("syntax_error", "invalid numlist has elements outside of allowed range",
                                        std::nullopt, std::nullopt, std::nullopt, "r(125)"));
        args.push_back("--lags");
        args.push_back(list);
      }
    }
    if (ifExpr.has_value()) {
      args.push_back("--keepif");
      args.push_back(*ifExpr);
    }
    args.push_back("--data");
    args.push_back(*session.currentHbin());
    args.push_back("--tidy");
    // A regressor under a time-series operator (`regress change L.change`,
    // bank 8ab22613) needs the session's tsset to be read by the engine.
    if (serialTest) {
      static const std::regex reTs(R"(^[LlFfDdSs]\d*\.)");
      bool ts = std::regex_search(y, reTs);
      for (const auto& x : xs) ts = ts || std::regex_search(x, reTs);
      if (ts) args = translate::injectTsContext(args, session);
    }
    ojson raw;
    try {
      raw = runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    ojson diagBlock = cobraToEstimationM4(raw, diagCmd, &session);
    // The engine refits on the rows its own varlist admits; after a fit
    // restricted by if/in that is a different sample: refuse when its N is not
    // the fit's e(N) (the engine's sample holds the fit's, so equal N is the
    // same rows).
    if (sampleGuarded && usesFit) {
      // e(N): the posted scalar, else the fit block's n_obs (a fit's e()
      // scalars are not all installed in eScalars).
      double fitN = std::numeric_limits<double>::quiet_NaN();
      if (const double* eN = session.eval.eScalars.find("N")) fitN = *eN;
      else if (session.lastEstBlock.has_value() && session.lastEstBlock->contains("n_obs") &&
               (*session.lastEstBlock)["n_obs"].is_number())
        fitN = (*session.lastEstBlock)["n_obs"].get<double>();
      const double rawN = raw.value("n_obs", std::numeric_limits<double>::quiet_NaN());
      if (std::isfinite(fitN) && rawN != fitN)
        return engineErrorBlockM4("estat " + diagCmd + " after a fit on a restricted sample is not implemented "
                                  "in this version of PIET; nothing was computed.",
                                  "unsupported_option");
    }
    // BUG-1496: hettest and ovtest are r-class and replace r() with their test
    // alone (StataBE 19.5, scratch-laneA/agents/file-new-1/i14.do, rlife/h1s.do:
    // `return list` after `estat hettest` holds exactly r(p), r(df), r(chi2), the
    // regress's r(table) and r(level) gone; after `estat ovtest` r(p), r(F), r(df)
    // (the numerator df), r(df_r)). Posted in reverse of that listing, which
    // `return list` prints newest first.
    if (diagCmd == "hettest" || diagCmd == "ovtest") {
      auto num = [&raw](const char* k) {
        return raw.contains(k) && raw[k].is_number() ? raw[k].get<double>()
                                                     : std::numeric_limits<double>::quiet_NaN();
      };
      const std::vector<std::pair<std::string, double>> posted =
          diagCmd == "hettest"
              ? std::vector<std::pair<std::string, double>>{{"chi2", num("chi2")},
                                                            {"df", num("df")},
                                                            {"p", num("p")}}
              : std::vector<std::pair<std::string, double>>{{"df_r", num("df_r")},
                                                            {"df", num("df_m")},
                                                            {"F", num("F")},
                                                            {"p", num("p")}};
      eval::replaceRClass(session.eval);
      std::vector<std::string> names;
      for (const auto& [k, v] : posted) {
        session.eval.rValues.set(k, v);
        names.push_back(k);
      }
      eval::declareRPosted(session.eval, "estat", std::move(names));
    }
    // The serial-correlation tests are r-class: r(lags) the lag orders,
    // r(N) r(N_gaps) r(k), and one 1 x L matrix per statistic with columns
    // lags:1 ... lags:L -- r(chi2) (r(F) and r(df_r) under small; durbinalt's
    // F form too), r(arch) for archlm, r(df), r(p) (bank 13f1664f bgodfrey,
    // 8dfbf1df / e697622a durbinalt, 05834961 bgodfrey lags(1 2), 8ab22613
    // archlm lags(1 2)). They are reached only on a sample in time order
    // without gaps (guards above), so r(N_gaps) is 0.
    // estat dwatson: r(dw) r(k) r(N) r(N_gaps) (bank 327659eb after regress on
    // a tsset auto: dw 1.3529578421370878, k 4, N 74, N_gaps 0), under the
    // same sample guards.
    if (diagCmd == "dwatson" && raw.contains("dw") && raw["dw"].is_number()) {
      const double rawN = raw.value("n_obs", std::numeric_limits<double>::quiet_NaN());
      eval::replaceRClass(session.eval);
      std::vector<std::string> names = {"dw", "k", "N", "N_gaps"};
      session.eval.rValues.set("dw", raw["dw"].get<double>());
      session.eval.rValues.set("k", raw.contains("k") && raw["k"].is_number() ? raw["k"].get<double>()
                                                                               : static_cast<double>(xs.size() + 1));
      session.eval.rValues.set("N", rawN);
      session.eval.rValues.set("N_gaps", static_cast<double>(tsSample.gaps));
      eval::declareRPosted(session.eval, "estat", std::move(names));
    }
    if (serialTest && raw.contains("lags") && raw["lags"].is_array() && !raw["lags"].empty()) {
      const ojson& lagsArr = raw["lags"];
      const size_t L = lagsArr.size();
      std::vector<std::string> cols;
      std::string lagsText;
      for (const auto& v : lagsArr) {
        const std::string t = std::to_string(v.get<long long>());
        cols.push_back("lags:" + t);
        lagsText += (lagsText.empty() ? "" : " ") + t;
      }
      const bool fform = raw.value("form", std::string("chi2")) == "F";
      const std::string statKey = diagCmd == "archlm" ? "chi2" : (fform ? "F" : "chi2");
      const std::string statName = diagCmd == "archlm" ? "arch" : statKey;
      std::vector<std::pair<std::string, std::string>> mats = {{statName, statKey}, {"df", "df"}};
      if (fform && diagCmd != "archlm") mats.push_back({"df_r", "df_r"});
      mats.push_back({"p", "p"});
      bool shapes = true;
      for (const auto& m : mats)
        shapes = shapes && raw.contains(m.second) && raw[m.second].is_array() && raw[m.second].size() == L;
      if (shapes) {
        eval::replaceRClass(session.eval);
        std::vector<std::string> names;
        for (const auto& [rname, key] : mats) {
          eval::EvalMatrix m;
          m.rownames = {"r1"};
          m.colnames = cols;
          std::vector<double> row;
          for (const auto& v : raw[key])
            row.push_back(v.is_number() ? v.get<double>() : std::numeric_limits<double>::quiet_NaN());
          m.data.push_back(std::move(row));
          session.eval.rMatrices.emplace_back(rname, std::move(m));
          names.push_back(rname);
        }
        const double n = raw.value("n_obs", std::numeric_limits<double>::quiet_NaN());
        session.eval.rValues.set("N", n);
        names.push_back("N");
        double k = raw.contains("k") && raw["k"].is_number() ? raw["k"].get<double>()
                                                              : static_cast<double>(xs.size() + 1);
        session.eval.rValues.set("k", k);
        names.push_back("k");
        session.eval.rValues.set("N_gaps", static_cast<double>(tsSample.gaps));
        names.push_back("N_gaps");
        session.eval.rStrings.set("lags", lagsText);
        names.push_back("lags");
        eval::declareRPosted(session.eval, "estat", std::move(names));
      }
    }
    return diagBlock;
  }

  // estat endogenous / overid / firststage (BS-082, BETA D5 batch; BETA
  // Phase G REWIRE): IV post-estimation diagnostics after `ivregress
  // 2sls`/`ivreghdfe` (BS-063, BETA D1). Oracle D5c: `ivregress 2sls income
  // emppop (beertax = spirits unemp)` (bare, no vce()) then `estat
  // endogenous`/`estat overid`/`estat firststage`.
  //
  // estat endogenous (Durbin score chi2 / Wu-Hausman F) / estat overid
  // (Sargan chi2 / Basmann): the D5 batch's own honest-reject above was
  // correct about `cobra iv --tidy`'s OWN payload (no durbin/wu-hausman
  // field of any kind; its one overidentification field, hansen_j, is a
  // DIFFERENT GMM-weighted J statistic, absent entirely under the model's
  // default iid vcov) — but wrong that the engine has "no such test at
  // all": a SEPARATE cobra CLI command, `cobra iv-diag` (hyphenated —
  // apps/cmds/cmds_diag.cpp::cmd_ivdiag on the cobra side, NOT part of
  // `cobra iv`'s own --tidy output), computes exactly this classical-iid
  // Durbin/Wu-Hausman/Sargan/Basmann family directly from the raw
  // data/varlist (`--data --y --endog --instruments [--x] [--keepif]`),
  // independent of whatever `cobra iv --tidy` did or didn't emit. This
  // branch now re-invokes it, reconstructing the endog/exog/instruments
  // split from LastModel's ivEndog/ivExog/ivInstruments (session_state.h;
  // `lm.x` alone conflates endog+exog and cannot be un-mixed).
  //
  // iv-diag is classical/iid-only (no --vcov flag on its CLI at all,
  // src/estimators/iv_diagnostics.h's own doc comment: "Robust/HAC
  // (Wooldridge) ... not implemented") — mirrors real Stata's own
  // vce()-dependent test-family switch (a non-default vce() makes Stata
  // itself compute a DIFFERENT robust score test). Forwarding the
  // classical statistic after a robust/clustered fit would silently answer
  // a different question than the one actually estimated, so this is
  // gated on lm.vcov=="iid" (LastModel's own default) and honestly rejects
  // otherwise, rather than fabricating a robust-form endogeneity/overid
  // test the engine does not have.
  //
  // estat overid specifically: iv-diag's own JSON either carries a real
  // {"sargan","sargan_p","basmann","basmann_p","df"} block (m > p1,
  // overidentified) or a `{"note": "just-identified ..."}` placeholder (m
  // == p1) — real Stata itself says "not overidentified" rather than
  // fabricating a stat in the exactly-identified case; this mirrors that
  // rather than inventing zeros. NOTE (basmann's actual distribution): the
  // engine's OWN implementation computes r.basmann_p via chi2_pvalue(...,
  // overid_df) — the SAME df and distribution family as Sargan, not an
  // F(df1,df2) the way real Stata's Basmann is traditionally described —
  // so this renders it labeled "Basmann chi2" (what the engine actually
  // computes), not "Basmann F" (what real Stata's test is traditionally
  // named), to avoid asserting a distribution/df pair this engine build
  // does not actually use.
  //
  // estat firststage (UNCHANGED from the D5 batch): the engine's
  // `first_stage_F`/`first_stage_F_iid` (per-endogenous-regressor dict) IS
  // present under every --vcov tried and matches the oracle's
  // F(2,332)=2.56678 byte-for-byte — this still reads it straight off the
  // STORED raw `cobra iv --tidy` payload (session.lastModel()->result)
  // rather than re-running cobra, no iv-diag involved. Real Stata's own
  // firststage table also reports R-sq/Adjusted R-sq/Partial R-sq per
  // regressor plus a minimum-eigenvalue statistic and a Stock-Yogo weak-
  // instrument critical-value table — NONE of these appear anywhere in the
  // engine's iv payload (live-verified against the full key list) and
  // Prob>F itself is not computed engine-side either (no F-distribution CDF
  // exists anywhere in this codebase to compute it PIET-side without
  // fabricating a value) — so this renders ONLY the F statistic per
  // endogenous regressor (oracle-matched), leaving every other column
  // absent rather than fabricated (same "show what's real, never fabricate
  // the rest" precedent as reg3's r2_within/dof_resid null fallback,
  // cobraToEstimationM4 above).
  if (sub == "endogenous" || sub == "overid" || sub == "firststage") {
    if (auto err = eval::rejectUnknownOptions(opts, {})) return *err;
    const auto& lmOptIv = session.lastModel();
    if (!lmOptIv.has_value() || lmOptIv->cobraCmd != "iv" || !lmOptIv->result.has_value()) {
      return engineErrorBlockM4(
          "estat " + sub + ": no `ivregress 2sls`/`ivreghdfe` result in memory. Run an IV "
          "regression first.",
          "unsupported_option");
    }
    const ojson& ivRaw = *lmOptIv->result;
    if (sub == "endogenous" || sub == "overid") {
      if (lmOptIv->vcov != "iid") {
        return engineErrorBlockM4(
            "estat " + sub + ": not supported after a non-default vce() in this version of PIET "
            "— the engine's iv-diag test (Durbin/Wu-Hausman/Sargan/Basmann) is classical/iid-only; "
            "a robust or clustered fit would need a robust score test this engine does not "
            "implement. Re-run `ivregress 2sls`/`ivreghdfe` with the default vce() to use estat " +
                sub + ".",
            "unsupported_option");
      }
      std::vector<std::string> diagArgv = {"iv-diag", "--y", lmOptIv->y, "--endog",
                                            joinCommaM4(lmOptIv->ivEndog), "--instruments",
                                            joinCommaM4(lmOptIv->ivInstruments)};
      if (!lmOptIv->ivExog.empty()) {
        diagArgv.push_back("--x");
        diagArgv.push_back(joinCommaM4(lmOptIv->ivExog));
      }
      if (ifExpr.has_value()) {
        diagArgv.push_back("--keepif");
        diagArgv.push_back(*ifExpr);
      }
      diagArgv.push_back("--data");
      diagArgv.push_back(lmOptIv->dataPath);
      ojson diagRaw;
      try {
        diagRaw = runCobraJsonM4(diagArgv);
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
      if (sub == "endogenous") {
        if (!diagRaw.contains("endogeneity") || !diagRaw["endogeneity"].is_object()) {
          return engineErrorBlockM4(
              "estat endogenous: the engine's iv-diag call returned no endogeneity block.");
        }
        const ojson& e = diagRaw["endogeneity"];
        ojson block;
        block["type"] = "estimation";
        block["estimator"] = "estat_endogenous";
        block["durbin_chi2"] = e.value("durbin", 0.0);
        block["durbin_df"] = e.value("durbin_df", 0);
        block["durbin_p"] = e.value("durbin_p", 0.0);
        block["wu_hausman_F"] = e.value("wu_hausman", 0.0);
        block["wu_hausman_df"] = e.value("wu_df", 0);
        block["wu_hausman_df_r"] = e.value("wu_df_r", 0.0);
        block["wu_hausman_p"] = e.value("wu_p", 0.0);
        return block;
      }
      // overid
      if (!diagRaw.contains("overid") || !diagRaw["overid"].is_object()) {
        return engineErrorBlockM4(
            "estat overid: the engine's iv-diag call returned no overid block.");
      }
      const ojson& o = diagRaw["overid"];
      ojson block;
      block["type"] = "estimation";
      block["estimator"] = "estat_overid";
      if (o.contains("note")) {
        // BUG-187 (P2-PERF ARC-P). Exactly identified (instrument count ==
        // endogenous-regressor count): there is no overidentifying restriction
        // to test, and we never fabricated a statistic for it — the TEXT was
        // always honest. The RETURN CODE was not. Stata answers r(498), and
        // its convention is that an unavailable test is an ERROR, so do-files
        // are written as `capture estat overid` / `if _rc == 0 { use the
        // Sargan p }`. Returning a note with rc 0 sends that branch into
        // statistics which were never produced — the human reading the
        // transcript learned the right thing and the program did not.
        //
        // The message is carried unchanged; only its channel moves.
        return eval::errorBlock(
            "engine_error",
            o.value("note",
                    std::string("model is exactly identified: no overidentifying restrictions to "
                                 "test (instrument count equals endogenous-regressor count).")),
            std::nullopt, std::nullopt, std::nullopt, "r(498)");
      }
      block["sargan_chi2"] = o.value("sargan", 0.0);
      block["sargan_df"] = o.value("df", 0);
      block["sargan_p"] = o.value("sargan_p", 0.0);
      // "Basmann chi2", not "Basmann F" — see the header comment above:
      // this engine build's own r.basmann_p is a chi2_pvalue(...,
      // overid_df) call, the SAME distribution/df as Sargan, not an
      // F(df1,df2) the way real Stata's Basmann is traditionally named.
      block["basmann_chi2"] = o.value("basmann", 0.0);
      block["basmann_df"] = o.value("df", 0);
      block["basmann_p"] = o.value("basmann_p", 0.0);
      return block;
    }
    // firststage
    if (!ivRaw.contains("first_stage_F") || !ivRaw["first_stage_F"].is_object() ||
        ivRaw["first_stage_F"].empty()) {
      return engineErrorBlockM4(
          "estat firststage: the stored IV result has no first-stage F statistic (re-run the "
          "ivregress/ivreghdfe fit).",
          "unsupported_option");
    }
    ojson block;
    block["type"] = "estimation";
    block["estimator"] = "estat_firststage";
    ojson rows = ojson::array();
    for (auto it = ivRaw["first_stage_F"].begin(); it != ivRaw["first_stage_F"].end(); ++it) {
      ojson row;
      row["name"] = it.key();
      row["F"] = it.value();
      rows.push_back(row);
    }
    block["firststage"] = rows;
    block["note"] =
        "First-stage F only (oracle-matched) — R-sq/Adjusted R-sq/Partial R-sq/Prob>F/minimum-"
        "eigenvalue/weak-instrument critical values are not computed by the engine and are not "
        "shown (not fabricated).";
    return block;
  }

  // csdid/DiD aggregate.
  if (sub.empty() && session.lastModel().has_value() && session.lastModel()->cobraCmd == "csdid") {
    return engineErrorBlockM4(
        "estat (no subcommand) — try `estat simple`, `estat event`, "
        "`estat pretrend`, `estat calendar`, or `estat group` after csdid.",
        "unsupported_command");
  }
  const auto& lmOpt = session.lastModel();
  if (sub == "ic" && session.eval.eStrings.get("cmd") == "ml") {
    if (auto refused = estatSyntaxRefusalM4(sub, rawVarlistAfterSub, ifExpr, "ml")) return *refused;
    if (auto err = eval::rejectUnknownOptions(opts, {})) return ojson(*err);
    const double* ll = session.eval.eScalars.find("ll");
    const double* n = session.eval.eScalars.find("N");
    const double* rank = session.eval.eScalars.find("rank");
    if (ll == nullptr || n == nullptr || rank == nullptr || !std::isfinite(*ll) || !(*n > 0))
      return ojson(eval::errorBlock("engine_error", "likelihood information not found in last estimation results",
                                    std::nullopt, std::nullopt, std::nullopt, std::string("r(321)")));
    eval::EvalMatrix information;
    information.rownames = {"."};
    information.colnames = {"N", "ll_0", "ll", "df", "AIC", "BIC"};
    const double* ll0 = session.eval.eScalars.find("ll_0");
    information.data = {{*n, ll0 ? *ll0 : std::nan(""), *ll, *rank,
                         -2.0 * *ll + 2.0 * *rank, -2.0 * *ll + std::log(*n) * *rank}};
    eval::replaceRClass(session.eval);
    session.eval.rMatrices.emplace_back("S", std::move(information));
    return ojson{{"type", "display"}, {"text", "AIC = " + std::to_string(-2.0 * *ll + 2.0 * *rank) +
                                             "; BIC = " + std::to_string(-2.0 * *ll + std::log(*n) * *rank)}};
  }
  // eb-order T-P3: `estat ic` after a posting with no fitted model (estpost)
  // finds no e(ll): Stata's r(321) "likelihood information not found in last
  // estimation results" (w2v_estpost_ematrix.log ep14_estat_ic, ep14n_estat_ic).
  if (!lmOpt.has_value() && sub == "ic" && session.eval.eLive() &&
      session.eval.coefSource() == nullptr) {
    return ojson(eval::errorBlock("engine_error",
                                  "likelihood information not found in last estimation results",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(321)")));
  }
  if (!lmOpt.has_value() || lmOpt->cobraCmd != "csdid") {
    // The default subcommands (ic, summarize, vce) after any fit, and the
    // estimator-specific ones routed by e(cmd): a subcommand Stata refuses is its
    // r(321), one Stata serves and this build does not is reported as not
    // implemented (it used to answer "no csdid result in memory" after every
    // estimator).
    return estatAfterFitM4(sub, rawVarlistAfterSub, opts, ifExpr, session);
  }
  static const std::set<std::string> kValid = {"simple", "event", "calendar", "group", "pretrend"};
  if (!kValid.count(sub)) {
    return engineErrorBlockM4("estat " + sub +
                              ": not supported. After csdid, use one of: "
                              "simple, event, pretrend, calendar, group.",
                              "unsupported_option");
  }

  // BS-097: only `estat event` reads any options (window()/post below); the
  // other 4 csdid sub-forms (simple/calendar/group/pretrend) read none —
  // branch-scoped by `sub` for the same reason as the diagnostic branch
  // above (event's known set must not leak into the other four, and vice
  // versa).
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  // csdid-estat-estore-post: `estat simple` takes estore()/post (csdid_estat.ado
  // csdid_simple `syntax, [estore(name) ... post ...]`; w2ag log:1447 ag_spost_live).
  const std::unordered_set<std::string>& kEstatEventKnownOpts = kVocabEstatEvent.known;
  static const std::unordered_set<std::string> kEstatSimpleKnownOpts = {"estore", "post"};
  if (auto err = eval::rejectUnknownOptions(opts, sub == "event"    ? kEstatEventKnownOpts
                                                  : sub == "simple" ? kEstatSimpleKnownOpts
                                                                    : std::unordered_set<std::string>{})) {
    return *err;
  }
  std::optional<std::string> estoreName;
  bool doPost = false;
  for (const auto& [k, v] : opts) {
    if (k == "post") doPost = true;
    if (k == "estore" && !v.isBool) estoreName = trimM4(v.strValue);
  }
  if (estoreName.has_value() && !strutil::isValidStataIdentifier(*estoreName)) {
    return ojson(eval::errorBlock("syntax_error", "estore(): " + *estoreName + " invalid name",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }

  LastModel lm = *lmOpt;
  ojson raw = lm.result.value_or(ojson::object());

  // `estat group` needs the per-cohort aggregation (cobra `groups` array,
  // engine >= 0.20.0 / contract 1.10.0). A cached result without it means the
  // pinned cobra predates the aggregation — error honestly rather than show a
  // wrong table.
  if (sub == "group" && !raw.contains("groups")) {
    return engineErrorBlockM4(
        "estat group: the csdid result has no per-cohort aggregation "
        "(cobra engine too old — expected a `groups` array). Re-run csdid "
        "with a cobra build that includes the group aggregation.");
  }

  if (sub == "event") {
    std::optional<std::string> winOpt;
    for (const auto& [k, v] : opts) {
      if (k == "window" && !v.isBool) winOpt = v.strValue;
    }
    if (winOpt.has_value() && !winOpt->empty()) {
      std::istringstream iss(*winOpt);
      std::vector<std::string> parts;
      std::string tok;
      while (iss >> tok) parts.push_back(tok);
      std::string wantWin;
      for (size_t i = 0; i < parts.size(); ++i) {
        if (i) wantWin += ",";
        wantWin += parts[i];
      }
      std::string haveWin;
      if (raw.contains("window") && raw["window"].is_array()) {
        for (size_t i = 0; i < raw["window"].size(); ++i) {
          if (i) haveWin += ",";
          std::ostringstream oss;
          oss << raw["window"][i];
          haveWin += oss.str();
        }
      }
      if (!wantWin.empty() && wantWin != haveWin) {
        std::vector<std::string> argv = {"csdid", "--data", lm.dataPath, "--y", lm.y};
        if (!lm.x.empty()) {
          argv.push_back("--x");
          argv.push_back(joinCommaM4(lm.x));
        }
        argv.push_back("--ivar");
        argv.push_back(lm.ivar.value_or(""));
        argv.push_back("--time");
        argv.push_back(lm.timevar.value_or(""));
        argv.push_back("--gvar");
        argv.push_back(lm.gvar.value_or(""));
        if (lm.method.has_value()) {
          argv.push_back("--method");
          argv.push_back(*lm.method);
        }
        if (lm.comparison.has_value()) {
          argv.push_back("--comparison");
          argv.push_back(*lm.comparison);
        }
        if (lm.pscoretrim.has_value()) {
          argv.push_back("--pscoretrim");
          argv.push_back(*lm.pscoretrim);
        }
        if (!lm.cluster.empty()) {
          argv.push_back("--cluster");
          argv.push_back(lm.cluster[0]);
        }
        // csdid-estat-estore-post: the fit's `long2` and `if` are part of the
        // model the window re-run must reproduce (w2ag log:670-683 ag_l2ny_ev:
        // `long2 notyet` + window(-2 2) gives Pre_avg Post_avg Tm3 Tm2 Tp0 Tp1
        // Tp2, where the short cells give another set).
        if (lm.long2) argv.push_back("--long2");
        if (!lm.fitKeepif.empty()) {
          argv.push_back("--keepif");
          argv.push_back(lm.fitKeepif);
        }
        argv.push_back("--window");
        argv.push_back(wantWin);
        argv.push_back("--tidy");
        try {
          raw = runCobraJsonM4(argv);
          LastModel updated = lm;
          updated.result = raw;
          // The re-run is the SAME e() (same fitSerial): setLastModel keeps its
          // block and e(b)/e(V), and the e() macros csdid wrote on top of the
          // fit (e(agg), e(clustvar), ...) are put back as they were.
          auto strings = session.eval.eStrings;
          auto scalars = session.eval.eScalars;
          auto matrices = session.eval.eMatrices;
          auto onFit = session.eOnFitNames;
          session.setLastModel(updated);
          session.eval.eStrings = std::move(strings);
          session.eval.eScalars = std::move(scalars);
          session.eval.eMatrices = std::move(matrices);
          session.eOnFitNames = std::move(onFit);
        } catch (const CobraError& ce) {
          return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
        }
      }
    }
  }

  // csdid-estat-estore-post (csdid_estat.ado csdid_simple / csdid_event): the
  // aggregate is posted as a NEW e() (`ereturn post b V`, e(cmd) estat,
  // e(cmdline), e(agg)), stored under estore() when given, and -- without
  // `post` -- csdid's e() is put back (`est restore lastreg`). r(b), r(V),
  // r(table) and r(agg) are returned either way. Records: w2ag log:1359
  // ag_evst_restore, :1372 ag_evst_cs `estat|event|8|...`, :1447 ag_spost_live,
  // :1564 ag_simple_nopost_r, :1616 ag_event_nopost_r, :1691 ag_evwin_post.
  // estat pretrend (csdid_estat.ado csdid_pretrend, rclass): r(chi2) r(df)
  // r(pchi2) (vs-stata bank 84764e22).
  if (sub == "pretrend" && raw.contains("pretrend") && raw["pretrend"].is_object()) {
    const ojson& pt = raw["pretrend"];
    if (pt.contains("chi2") && pt["chi2"].is_number() && pt.contains("df") &&
        pt["df"].is_number() && pt.contains("p") && pt["p"].is_number()) {
      eval::replaceRClass(session.eval);
      session.eval.rValues.set("chi2", pt["chi2"].get<double>());
      session.eval.rValues.set("df", pt["df"].get<double>());
      session.eval.rValues.set("pchi2", pt["p"].get<double>());
      eval::declareRPosted(session.eval, "estat", {"chi2", "df", "pchi2"});
    }
    return csdidToEstimationM4(raw, sub);
  }
  if (sub == "simple" || sub == "event" || sub == "calendar" || sub == "group") {
    const std::optional<CsdidPostingM4> P = csdidPostingM4(
        raw, sub == "event" ? std::string("estat_event") : "agg_" + sub);
    if (P.has_value()) {
      ojson display = csdidToEstimationM4(raw, sub);
      const ojson coeftable = csdidPostedCoeftableM4(*P);
      display["coeftable"] = coeftable;

      // r(): csdid_estat's programs are rclass; r(table) is the coefficient table's on the
      // posted b/V (no e(df_r): z rows, df missing, crit invnormal(.975)).
      eval::replaceRClass(session.eval);
      {
        const size_t K = P->names.size();
        eval::EvalMatrix tbl;
        tbl.rownames = {"b", "se", "z", "pvalue", "ll", "ul", "df", "crit", "eform"};
        tbl.colnames = P->names;
        tbl.data.assign(9, std::vector<double>(K, std::numeric_limits<double>::quiet_NaN()));
        for (size_t j = 0; j < K; ++j) {
          const ojson& row = coeftable[j];
          auto num = [&row](const char* key) {
            return row.contains(key) && row[key].is_number() ? row[key].get<double>()
                                                              : std::numeric_limits<double>::quiet_NaN();
          };
          tbl.data[0][j] = num("b");
          tbl.data[1][j] = num("se");
          tbl.data[2][j] = num("t");
          tbl.data[3][j] = num("p");
          tbl.data[4][j] = num("ci_lo");
          tbl.data[5][j] = num("ci_hi");
          tbl.data[7][j] = kZ975;
          tbl.data[8][j] = 0.0;
        }
        // csdid_estat.ado returns its b / V as plain matrices (row r1; vs-stata
        // bank 565a3ef3 `estat event`); `estat simple`'s single ATT is a
        // 1 x 1 named c1 / r1 (bank 579dd768), its table column still ATT.
        // event / calendar / group also return the aggregate without its
        // averages, r(bb) / r(vv) (event drops Pre_avg Post_avg, calendar
        // CAverage, group GAverage: banks 565a3ef3, c1fbc6f0, 01b9d1c3), and
        // `estat event` names itself in r(cmd).
        const std::vector<std::string> rnames =
            sub == "simple" ? std::vector<std::string>{"c1"} : P->names;
        eval::EvalMatrix rb;
        rb.data.push_back(P->b);
        rb.rownames = {"r1"};
        rb.colnames = rnames;
        eval::EvalMatrix rV;
        for (size_t i = 0; i < K; ++i)
          rV.data.emplace_back(P->V.begin() + static_cast<std::ptrdiff_t>(i * K),
                               P->V.begin() + static_cast<std::ptrdiff_t>((i + 1) * K));
        // estat calendar / group return the average's covariance with the
        // components as 0, as estat event does (cmds_panel.cpp event_V; banks
        // c1fbc6f0, 01b9d1c3: r(V)[1,2..] = 0).
        if (sub == "calendar" || sub == "group")
          for (size_t j = 1; j < K; ++j) rV.data[0][j] = rV.data[j][0] = 0.0;
        rV.rownames = sub == "simple" ? std::vector<std::string>{"r1"} : P->names;
        rV.colnames = rnames;
        std::vector<std::string> declared = {"table", "b", "V", "agg"};
        const size_t skip = sub == "event" ? 2 : (sub == "simple" ? K : 1);
        if (sub != "simple" && K > skip) {
          eval::EvalMatrix bb, vv;
          bb.rownames = {"r1"};
          bb.data.emplace_back(P->b.begin() + static_cast<std::ptrdiff_t>(skip), P->b.end());
          for (size_t i = skip; i < K; ++i) {
            bb.colnames.push_back(P->names[i]);
            vv.rownames.push_back(P->names[i]);
            vv.colnames.push_back(P->names[i]);
            vv.data.emplace_back(P->V.begin() + static_cast<std::ptrdiff_t>(i * K + skip),
                                 P->V.begin() + static_cast<std::ptrdiff_t>((i + 1) * K));
          }
          session.eval.rMatrices.emplace_back("bb", std::move(bb));
          session.eval.rMatrices.emplace_back("vv", std::move(vv));
          declared.push_back("bb");
          declared.push_back("vv");
        }
        session.eval.rMatrices.emplace_back("table", std::move(tbl));
        session.eval.rMatrices.emplace_back("b", std::move(rb));
        session.eval.rMatrices.emplace_back("V", std::move(rV));
        session.eval.rStrings.set("agg", sub);
        if (sub == "event") {
          session.eval.rStrings.set("cmd", "estat");
          declared.push_back("cmd");
        }
        eval::declareRPosted(session.eval, "estat", declared);
      }
      if (sub == "calendar" || sub == "group") return csdidToEstimationM4(raw, sub);

      if (doPost || estoreName.has_value()) {
        // The live e() as it stands, moved (not copied) so that putting it back
        // restores the very same estimation identity (liveEstimationIdentity).
        struct Snapshot {
          std::optional<LastModel> lm;
          std::optional<ojson> block;
          std::optional<ojson> lmResult;
          eval::OrderedFloatDict scalars;
          eval::OrderedDict strings;
          std::vector<std::pair<std::string, eval::EvalMatrix>> matrices;
          bool posted = false;
          std::vector<std::string> onFit;
          std::string activeName;
          ojson estAdded;
          std::string estAddedFp;
        } snap;
        if (!doPost) {
          snap.lm = session.lastModel();
          snap.block = std::move(session.lastEstBlock);
          snap.lmResult = session.eval.lastModelResult;
          snap.scalars = session.eval.eScalars;
          snap.strings = session.eval.eStrings;
          snap.matrices = session.eval.eMatrices;
          snap.posted = session.ePostedLive;
          snap.onFit = session.eOnFitNames;
          snap.activeName = session.activeEstimatesName;
          snap.estAdded = session.estAdded;
          snap.estAddedFp = session.estAddedFp;
        }
        // The estat e(): no fitted model behind it (ereturn post b V).
        session.lastModel_reset();
        session.eval.eScalars = eval::OrderedFloatDict();
        session.eval.eStrings.clear();
        session.eval.eMatrices.clear();
        ojson eblk;
        eblk["type"] = "estimation";
        eblk["estimator"] = "estat";
        eblk["estat"] = sub;
        eblk["depvar"] = "";
        eblk["coeftable"] = coeftable;
        session.lastEstBlock = eblk;
        postCsdidPostingM4(session, *P);
        session.eval.setEString("cmd", "estat");
        session.eval.setEString("cmdline", "estat " + sub);
        session.eval.setEString("agg", sub);
        session.eval.setEString("properties", "b V");
        session.ePostedLive = true;
        if (estoreName.has_value()) {
          ojson stored = handlers::runEststo(*estoreName, std::nullopt, session, "est",
                                             /*viaEststo=*/false);
          if (stored.value("type", std::string()) == "error") return stored;
        }
        if (!doPost) {
          if (snap.lm.has_value()) {
            session.setLastModel(*snap.lm);
          } else {
            session.lastModel_reset();
          }
          session.lastEstBlock = std::move(snap.block);
          session.eval.lastModelResult = std::move(snap.lmResult);
          session.eval.eScalars = std::move(snap.scalars);
          session.eval.eStrings = std::move(snap.strings);
          session.eval.eMatrices = std::move(snap.matrices);
          session.ePostedLive = snap.posted;
          session.eOnFitNames = std::move(snap.onFit);
          session.activeEstimatesName = std::move(snap.activeName);
          session.estAdded = std::move(snap.estAdded);
          session.estAddedFp = std::move(snap.estAddedFp);
        }
      }
      return display;
    }
  }

  if (sub == "event" && doPost) postCsdidEventEMatrices(session, raw);

  return csdidToEstimationM4(raw, sub);
}

// ===========================================================================
// BETA campaign Phase D batch D4/D5: time-series + post-estimation/diag/misc
// wirings (BS-077/078/079/080/081/085/021/082/086/083/084/087). Oracle:
// internal capture d45-oracle-stata.log
// (StataBE 19.5 on piet/packaging/sample/fatalities.csv, `gen high=(beertax
// >.5)` + `encode state, gen(snum)` + `gen t=_n` + `tsset t`; sections
// D4a-D4f, D5a-D5g). durbinalt's fix (BS-021) and the endogenous/overid/
// firststage IV diagnostics (BS-082) live inside runEstat above; the
// margins ,at() linear-only gate (BS-086) lives inside runMargins above.
// tsfill (BS-081) lives in time/time_fill.cpp (runTsfill; a data-mutation
// verb — this file's tsfill-adjacent handlers
// below all read the SAME session.timevar/panelvar/delta tsset/xtset spine
// runTsfill does).

// newey (BS-077): `newey depvar indepvars [if], lag(#)` -> cobra newey
// --timevar T --lag M. Oracle D4a (lag(2): beertax b=-1697.15 se=353.9058,
// spirits b=1384.099 se=292.4321, _cons b=12323.98 se=533.9508 — byte-for-
// byte engine match, including df_r=333). Real Stata's lag() is REQUIRED
// (Stata itself errors "option lag() required" if omitted) — this handler
// rejects the same way, not a silent default. The time variable is read
// from the session's own `tsset timevar` spine (the SAME context BS-075's
// xtpoisson/xtlogit read session.panelvar from — see runXtPoissonLogit's
// header comment above — mirrored here for the time-series-only case).
// Real Stata's newey also has t()/level() options; none have oracle coverage
// this batch and are left as honest rejects via the unknown-option net below.
// BUG-1503: after xtset the panel variable goes to the engine (--panelvar: the
// Newey-West sum pairs rows of one panel only), and `force` is forwarded: Stata
// refuses a time variable that is not regularly spaced, as `xtset id t` makes
// it (t repeats across panels), r(198) "t is not regularly spaced" unless force
// (newey.ado Checkt, checked by the engine). StataBE 19.5 (i3.do): force gives
// _se[x1] .04395156556168258, as `xtset id tt` (tt unique) does without it.

}  // namespace piet::handlers
