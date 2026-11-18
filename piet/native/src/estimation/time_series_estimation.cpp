// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Time series estimation.
#include "piet/handlers/estimation.h"
#include <algorithm>
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
#include "core/distributions.h"  // t_critical (shared with cobra)
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
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation/postestimation_internal.h"
#include "estimation_options.h"
#include "statistics_command_support.h"  // fitLeft, abbrevCp (the regress table)

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

ojson runNewey(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `newey` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    // StataBE 19.5: r(111) "time variable not set, use tsset varname ..."
    // (vs-stata bank, newey/prais/dfuller/pperron without tsset).
    return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> lagVal;
  std::optional<double> neweyLevel;
  bool force = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lag" && !v.isBool) lagVal = v.strValue;
    if (k == "force" && v.isBool && v.boolValue) force = true;
    if (k == "level") {
      // level(#): the table's t interval at that level, set on the block below (bank
      // f7530536 `newey price mpg weight, lag(3) level(90)`); Stata's range 10..99.99.
      char* end = nullptr;
      const std::string t = v.isBool ? std::string() : trimM4(v.strValue);
      const double lv = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !(lv >= 10.0 && lv <= 99.99))
        return ojson(eval::errorBlock("engine_error", "level() must be between 10 and 99.99 inclusive",
                                      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
      neweyLevel = lv;
    }
  }
  if (!lagVal.has_value() || trimM4(*lagVal).empty()) {
    return engineErrorBlockM4("syntax_error: option lag() required for `newey`", "syntax_error");
  }

  std::vector<std::string> argv = {"newey",  "--y",  y,      "--x",       joinCommaM4(xs),
                                    "--timevar", *session.timevar, "--lag", trimM4(*lagVal)};
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    argv.push_back("--panelvar");
    argv.push_back(*session.panelvar);
  }
  if (force) argv.push_back("--force");
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (session.delta.has_value()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
    argv.push_back("--delta");
    argv.push_back(buf);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabNewey.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");
  // e(sample) T2: the engine writes the fit's admitted rows over the --data
  // rows (its own --keepif included); installed below, before setLastModel.
  const std::vector<std::string> fitArgv = argv;  // captureFitFilterM4 reads the fit's own flags
  const std::string esampleValues = esampleFitArgs(session, argv, std::string(), 0);
  argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), joined below

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    cecon::native_unlink(esampleValues);
    // Stata's own stops (newey.ado): the spacing check r(198), and a lag that
    // pairs no row, r(2000) "no observations", or one row, r(2001) "insufficient
    // observations" (`mat vecaccum`; StataBE 19.5 lag(7) on 8 rows).
    if (ce.detail == "ts_not_regular")
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(198)")));
    if (ce.detail == "no_obs" || ce.detail == "insufficient_obs")
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt,
                                    std::string(ce.detail == "no_obs" ? "r(2000)" : "r(2001)")));
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "newey", &session);
  if (neweyLevel.has_value() && block.contains("coeftable") && block["coeftable"].is_array() &&
      block.contains("df_r") && block["df_r"].is_number() && block["df_r"].get<double>() > 0) {
    const double crit = cecon::t_critical(1.0 - *neweyLevel / 100.0, block["df_r"].get<double>());
    for (auto& row : block["coeftable"]) {
      if (!row.is_object() || !row.contains("b") || !row["b"].is_number() || !row.contains("se") ||
          !row["se"].is_number() || !(row["se"].get<double>() > 0))
        continue;
      const double b = row["b"].get<double>(), se = row["se"].get<double>();
      row["ci_lo"] = b - crit * se;
      row["ci_hi"] = b + crit * se;
    }
  }
  block["estimator"] = "newey";
  block["depvar"] = y;
  if (raw.contains("lag")) block["lag"] = raw["lag"];

  LastModel lm;
  lm.cobraCmd = "newey";
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, fitArgv);
  lm.dataPath = *session.currentHbin();
  (void)esampleInstallOnHead(session, esampleValues, raw, lm);  // e(sample) T2
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // BUG-1196: e(V) is the fit's HAC covariance
  return block;
}

namespace {
// prais's e() from its final regression on the rho-differenced data (prais.ado, method
// identified; bank 7a42b444 `prais close volume`, 12f3b1e2 twostep, fc5bae89
// rhotype(tscorr), 207208b4 on xtset panels): each variable v becomes v - rho L.v,
// and at the start of a run (first period of a panel, or after a gap)
// v sqrt(1 - rho^2); the constant is transformed as a variable. The regression of
// the transformed depvar on the transformed regressors (no constant, total sum of
// squares centred) gives rss, mss = tss - rss, df_m = K - 1, df_r = N - K,
// r2 = mss/tss, r2_a = 1 - (1-r2)(N-1)/df_r, F = (mss/df_m)/(rss/df_r),
// rmse = sqrt(rss/df_r), ll = -N/2 (1 + ln 2pi + ln(rss/N)), and regress's e(beta),
// b_j sd(x*_j)/sd(y*), whose columns keep the transformed variables' temporary names
// (`__00000A` for the first regressor on, in every bank cell of this form).
// Posted for the Prais-Winsten transform with a constant (corc has no e(sample) here).
void postPraisContractM4(SessionState& session, const LastModel& lm, const ojson& raw,
                         bool twostep, const std::string& rhotype) {
  if (!raw.is_object() || !raw.contains("rho") || !raw["rho"].is_number()) return;
  const double rho = raw["rho"].get<double>();
  const auto rows = est_detail::esampleRowsM4(session, lm);
  if (!rows.has_value() || rows->empty() || !session.timevar.has_value()) return;
  const auto ebA = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  if (!ebA.matrix.has_value() || ebA.matrix->data.size() != 1) return;
  const std::vector<std::string>& bn = ebA.matrix->colnames;
  const std::vector<double>& b = ebA.matrix->data[0];
  const size_t K = bn.size();
  if (K < 2 || bn.back() != "_cons") return;
  const double* tc = est_detail::f64ColumnM4(session, *session.timevar);
  const double* pc = session.panelvar.has_value() && !session.panelvar->empty()
                         ? est_detail::f64ColumnM4(session, *session.panelvar)
                         : nullptr;
  const double* yc = est_detail::f64ColumnM4(session, lm.depvar);
  if (tc == nullptr || yc == nullptr) return;
  std::vector<const double*> xc;
  for (size_t j = 0; j + 1 < K; ++j) {
    xc.push_back(est_detail::f64ColumnM4(session, bn[j]));
    if (xc.back() == nullptr) return;
  }
  const double delta = session.delta.has_value() ? *session.delta : 1.0;
  std::vector<int64_t> ord(rows->begin(), rows->end());
  std::stable_sort(ord.begin(), ord.end(), [&](int64_t a, int64_t c) {
    const double pa = pc ? pc[a] : 0.0, pb = pc ? pc[c] : 0.0;
    if (pa != pb) return pa < pb;
    return tc[a] < tc[c];
  });
  const size_t N = ord.size();
  const double s1 = std::sqrt(1.0 - rho * rho);
  std::vector<std::vector<double>> X(N, std::vector<double>(K));
  std::vector<double> y(N);
  for (size_t i = 0; i < N; ++i) {
    const int64_t r = ord[i];
    const bool lag = i > 0 && (pc == nullptr || pc[ord[i - 1]] == pc[r]) &&
                     tc[ord[i - 1]] == tc[r] - delta;
    const int64_t q = lag ? ord[i - 1] : -1;
    auto tr = [&](double v, double vPrev) { return lag ? v - rho * vPrev : v * s1; };
    y[i] = tr(yc[r], lag ? yc[q] : 0.0);
    for (size_t j = 0; j + 1 < K; ++j) X[i][j] = tr(xc[j][r], lag ? xc[j][q] : 0.0);
    X[i][K - 1] = tr(1.0, 1.0);
  }
  const double Nd = static_cast<double>(N), Kd = static_cast<double>(K);
  double rss = 0, ysum = 0;
  for (size_t i = 0; i < N; ++i) {
    double xb = 0;
    for (size_t j = 0; j < K; ++j) xb += X[i][j] * b[j];
    rss += (y[i] - xb) * (y[i] - xb);
    ysum += y[i];
  }
  double tss = 0;
  for (size_t i = 0; i < N; ++i) tss += (y[i] - ysum / Nd) * (y[i] - ysum / Nd);
  const double mss = tss - rss, dfM = Kd - 1.0, dfR = Nd - Kd;
  const double r2 = mss / tss;
  auto put = [&session](const std::string& nm, double v) {
    session.eval.setEScalar(nm, v);
    session.noteEOnFit(nm);
  };
  auto putS = [&session](const std::string& nm, const std::string& v) {
    session.eval.setEString(nm, v);
    session.noteEOnFit(nm);
  };
  put("rss", rss);
  put("mss", mss);
  put("df_m", dfM);
  put("r2", r2);
  put("r2_a", 1.0 - (1.0 - r2) * (Nd - 1.0) / dfR);
  put("F", (mss / dfM) / (rss / dfR));
  put("rmse", std::sqrt(rss / dfR));
  put("ll", -0.5 * Nd * (1.0 + std::log(2.0 * M_PI) + std::log(rss / Nd)));
  put("rank", Kd);
  put("sum_w", Nd);
  put("tol", 1e-6);
  put("max_ic", twostep ? 1.0 : 100.0);
  if (twostep) put("ic", 1.0);
  else if (raw.contains("iters") && raw["iters"].is_number()) put("ic", raw["iters"].get<double>());
  putS("title", "Prais\u2013Winsten AR(1) regression");
  putS("method", twostep ? "twostep" : "iterated");
  putS("tranmeth", "prais");
  putS("rhotype", rhotype.empty() ? "regress" : rhotype);
  putS("predict", "prais_p");
  putS("marginsok", "XB default");
  putS("vce", "ols");
  // e(beta): standardized coefficients of the transformed regression
  auto sd = [N](const std::vector<double>& v) {
    double m = 0;
    for (double x : v) m += x;
    m /= static_cast<double>(N);
    double ss = 0;
    for (double x : v) ss += (x - m) * (x - m);
    return std::sqrt(ss / static_cast<double>(N - 1));
  };
  const double sy = sd(y);
  eval::EvalMatrix beta;
  beta.rownames = {"r1"};
  beta.data.emplace_back();
  static const char* kDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (size_t j = 0; j < K; ++j) {
    std::vector<double> col(N);
    for (size_t i = 0; i < N; ++i) col[i] = X[i][j];
    beta.data[0].push_back(b[j] * sd(col) / sy);
    const size_t idx = 10 + j;
    if (idx >= 36) return;  // the temporary names past __00000Z are not reproduced
    beta.colnames.push_back(std::string("__00000") + kDigits[idx]);
  }
  for (auto& kv : session.eval.eMatrices)
    if (kv.first == "beta") {
      kv.second = beta;
      session.noteEOnFit("beta");
      return;
    }
  session.eval.eMatrices.emplace_back("beta", std::move(beta));
  session.noteEOnFit("beta");
}
}  // namespace

// prais (BS-078): `prais depvar indepvars [if] [, corc twostep
// rhotype(regress|tscorr)]` -> cobra prais --timevar T [--corc] [--twostep]
// [--rhotype ...]. Oracle D4b: `, twostep` (beertax b=-1997.243 se=281.7461,
// rho=.8034461, dw=1.902251/dw_0=.392886) and `, corc` (beertax b=-1972.609
// se=284.8613, rho=.8038026, dw=1.902573/dw_0=.392886) — BOTH byte-for-byte
// engine matches. Real Stata's DEFAULT (bare `prais`, neither corc nor
// twostep) is the ITERATED Prais-Winsten estimator; cobra's own bare-flag
// default is UNVERIFIED against Stata (no oracle coverage for the bare/no-
// flag form — the oracle only exercises `, twostep` and `, corc`), so it is
// wired through faithfully (same argv shape minus the two flags) but not
// selftest-pinned to a Stata value.
ojson runPrais(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `prais` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    // StataBE 19.5: r(111) "time variable not set, use tsset varname ..."
    // (vs-stata bank, newey/prais/dfuller/pperron without tsset).
    return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  bool corc = false, twostep = false;
  std::optional<std::string> rhotype;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "corc" && v.isBool && v.boolValue) corc = true;
    if (k == "twostep" && v.isBool && v.boolValue) twostep = true;
    if (k == "rhotype" && !v.isBool) rhotype = v.strValue;
  }
  if (corc && twostep) {
    return engineErrorBlockM4("prais: corc and twostep may not be combined.", "syntax_error");
  }
  std::string rhotypeNorm;
  if (rhotype.has_value()) {
    rhotypeNorm = lowerM4(trimM4(*rhotype));
    if (rhotypeNorm != "regress" && rhotypeNorm != "tscorr") {
      return engineErrorBlockM4(
          "prais: rhotype() must be regress or tscorr, got `" + *rhotype + "`", "syntax_error");
    }
  }

  std::vector<std::string> argv = {"prais", "--y", y, "--x", joinCommaM4(xs), "--timevar",
                                    *session.timevar};
  // BUG-1507: after xtset, rho and the transform restart at every panel change
  // (prais.ado `_ts timevar panvar, panel`; StataBE 19.5 i2.do e(rho)
  // .55064701722608933, e(N_gaps) 39), so the engine gets the panel variable.
  if (session.panelvar.has_value() && !session.panelvar->empty()) {
    argv.push_back("--panelvar");
    argv.push_back(*session.panelvar);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (corc) argv.push_back("--corc");
  if (twostep) argv.push_back("--twostep");
  if (!rhotypeNorm.empty()) {
    argv.push_back("--rhotype");
    argv.push_back(rhotypeNorm);
  }
  if (session.delta.has_value()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
    argv.push_back("--delta");
    argv.push_back(buf);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabPrais.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");
  // e(sample) T2: the engine writes the fit's admitted rows over the --data
  // rows (its own --keepif included); installed below, before setLastModel.
  const std::vector<std::string> fitArgv = argv;  // captureFitFilterM4 reads the fit's own flags
  // The engine does not expose the Cochrane-Orcutt rows (apps/cmds/cmds_ts.cpp
  // refuses --esample-out with --corc), so `prais, corc` stays kUnavailable:
  // e(sample) after it is the named r(198).
  const std::string esampleValues =
      corc ? std::string() : esampleFitArgs(session, argv, std::string(), 0);
  argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), joined below

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    cecon::native_unlink(esampleValues);
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "prais", &session);
  block["estimator"] = "prais";
  block["depvar"] = y;
  static const char* kPraisFields[] = {"rho", "dw", "dw_0", "method"};
  for (const char* k : kPraisFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = "prais";
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, fitArgv);
  lm.dataPath = *session.currentHbin();
  (void)esampleInstallOnHead(session, esampleValues, raw, lm);  // e(sample) T2
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // BUG-1196: e(V) is the transformed fit's covariance
  if (!corc) postPraisContractM4(session, lm, raw, twostep, rhotypeNorm);
  return block;
}

namespace {
// dfuller, pperron and wntestq test ONE series: under xtset data the sample selected
// by if/in must lie in a single panel, else "sample may not include multiple panels"
// r(459) (the time-series setup check's one-panel rule: the panel variable's minimum and
// maximum over if/in, missing values aside). Run over the whole panel the engine read
// the panels as one series (`wntestq beertax` on fatality, 48 states: rc 0).
std::optional<ojson> onePanelRefusalM4(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.panelvar.has_value() || session.panelvar->empty()) return std::nullopt;
  const std::string basePath = *session.currentHbin();
  ojson filterErr;
  auto filteredPath = applyIfFilterM4(session, basePath, parsed.ifExpr, filterErr);
  if (!filteredPath.has_value()) return std::nullopt;  // the command reports the if error
  const bool madeTemp = (*filteredPath != basePath);
  std::optional<ojson> verdict;
  try {
    std::optional<Datastore> tempDs;
    Datastore* ds = nullptr;
    if (madeTemp) {
      tempDs.emplace(*filteredPath);
      ds = &*tempDs;
    } else {
      ds = session.datastore();
    }
    const std::string& pv = *session.panelvar;
    if (ds != nullptr && ds->has(pv) && ds->dtype(pv) == "f64") {
      const double* g = ds->col(pv).f64();
      double lo = std::numeric_limits<double>::infinity(), hi = -lo;
      for (int64_t i = 0; i < ds->nRows(); ++i) {
        if (std::isnan(g[i])) continue;
        lo = std::min(lo, g[i]);
        hi = std::max(hi, g[i]);
      }
      if (lo < hi)
        verdict = ojson(eval::errorBlock("engine_error", "sample may not include multiple panels",
                                         std::nullopt, std::nullopt, std::nullopt, "r(459)"));
    }
  } catch (const std::exception&) {
    // an unreadable frame leaves the check to the engine
  }
  if (madeTemp) cecon::native_unlink(*filteredPath);
  return verdict;
}
}  // namespace

// dfuller / pperron (BS-079): `dfuller varname [if] [, lags(#)
// noconstant|drift|trend]` / `pperron varname [if] [, lags(#)
// noconstant|trend]` -> cobra dfuller/pperron --var V --timevar T [--lags
// k] [--noconstant|--drift|--trend]. Oracle D4c (dfuller bare: Zt=-6.004,
// cv 1/5/10=-3.453/-2.877/-2.570, p byte-exact MacKinnon p; dfuller lags(2)
// trend: Zt=-5.873, cv=-3.987/-3.427/-3.130), D4d (pperron bare:
// Zrho=-65.302, Zt=-6.041, SAME cv row as dfuller's constant case,
// newey_west_lags=5 auto-selected by the engine) — ALL fields (test stat,
// critical values, AND p) are byte-for-byte engine matches, so unlike this
// item's original "if the engine lacks critical-value fields, pin stat+p
// only" fallback instruction, the FULL table pins here (the engine already
// carries cv_1/cv_5/cv_10 — no gap found). pperron has no `drift` variant in
// real Stata (only noconstant/trend) — honest-rejected there, unlike
// dfuller.
// `in a/b` for the time-series tests, which read the whole frame (a lag reaches
// rows the sample leaves out): the range becomes a condition on the row number,
// joined to the `if`, so the engine's --keepif selects it (dfuller.ado / pperron.ado
// / wntestq.ado mark the sample with `if` and `in` alike). nullopt + `err` when the
// range is malformed or out of bounds.
static std::optional<std::optional<std::string>> tsSampleCondition(
    const eval::ParsedCommand& parsed, SessionState& session, const std::string& cmd, ojson& err) {
  std::optional<std::string> cond = parsed.ifExpr;
  if (!parsed.inRange.has_value() || parsed.inRange->empty()) return cond;
  const InRangeBounds rb = parseInRangeBounds(*parsed.inRange, session.macroObsCount());
  if (!rb.ok) {
    InRangeFilterResult r;
    r.ok = false;
    r.refusal = rb.refusal;
    err = inRangeRejectError(r, *parsed.inRange, cmd);
    return std::nullopt;
  }
  const std::string range =
      "(_n >= " + std::to_string(rb.a) + " & _n <= " + std::to_string(rb.b) + ")";
  if (cond.has_value() && !cond->empty()) return std::optional<std::string>("(" + *cond + ") & " + range);
  return std::optional<std::string>(range);
}

ojson runDfullerPperron(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                        SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  bool allowDrift = (cobraCmd == "dfuller");
  if (parsed.varlist.size() != 1) {
    return engineErrorBlockM4("syntax_error: `" + cobraCmd + "` requires exactly one variable",
                              "syntax_error");
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    // StataBE 19.5: r(111) "time variable not set, use tsset varname ..."
    // (vs-stata bank, newey/prais/dfuller/pperron without tsset).
    return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  }
  std::string v = parsed.varlist[0];
  // A time-series operator on the variable (`dfuller D.gnp96`): the series is
  // generated under tsset onto a copy of the frame and tested there.
  std::string dataPath = *session.currentHbin();
  {
    std::vector<std::string> one = {v};
    ojson tsErr;
    const auto tsFrame = tsVarlistFrameM4(session, one, tsErr);
    if (!tsFrame.has_value()) return tsErr;
    if (!tsFrame->empty()) {
      dataPath = *tsFrame;
      v = one[0];
    }
  }
  ojson inErr;
  const auto sampleCond = tsSampleCondition(parsed, session, cobraCmd, inErr);
  if (!sampleCond.has_value()) return inErr;

  std::optional<std::string> lagsVal;
  bool noConstant = false, drift = false, trend = false, regressTable = false;
  for (const auto& [k, v2] : parsed.opts) {
    if (k == "lags" && !v2.isBool) lagsVal = v2.strValue;
    if (k == "regress" && v2.isBool && v2.boolValue) regressTable = true;
    if ((k == "noconstant" || k == "nocons") && v2.isBool && v2.boolValue) noConstant = true;
    if (k == "drift" && v2.isBool && v2.boolValue) drift = true;
    if (k == "trend" && v2.isBool && v2.boolValue) trend = true;
  }
  int modeCount = (noConstant ? 1 : 0) + (drift ? 1 : 0) + (trend ? 1 : 0);
  if (modeCount > 1) {
    return engineErrorBlockM4(cobraCmd + ": noconstant/drift/trend may not be combined.",
                              "syntax_error");
  }
  if (drift && !allowDrift) {
    return engineErrorBlockM4(
        "Option `drift` is not a valid Stata option for `" + cobraCmd + "` (real Stata's " +
        cobraCmd + " has no drift() form — only noconstant/trend).",
        "unsupported_option");
  }

  std::vector<std::string> argv = {cobraCmd, "--var", v, "--timevar", *session.timevar};
  if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
    argv.push_back("--lags");
    argv.push_back(trimM4(*lagsVal));
  }
  if (noConstant) argv.push_back("--noconstant");
  if (drift) argv.push_back("--drift");
  if (trend) argv.push_back("--trend");
  if (sampleCond->has_value()) {
    argv.push_back("--keepif");
    argv.push_back(**sampleCond);
    // the rows `if` excludes supply lags only within the sample's own panel
    if (session.panelvar.has_value() && !session.panelvar->empty()) {
      argv.push_back("--panelvar");
      argv.push_back(*session.panelvar);
    }
  }

  // BUG-233: see runXtPoissonLogit — `drift` is dfuller-only, so it stays a
  // conditional insert here while the registered vocabulary carries the union.
  std::unordered_set<std::string> known = kDfullerPperronBase;
  if (allowDrift) known.insert("drift");
  known.insert("regress");
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  // the one-panel rule follows the syntax checks (the setup check runs after syntax)
  if (auto refused = onePanelRefusalM4(parsed, session)) return *refused;
  argv.push_back("--data");
  argv.push_back(dataPath);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (raw.contains("var")) raw["var"] = parsed.varlist[0];

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  block["estimator"] = cobraCmd;
  static const char* kUnitRootFields[] = {
      "Zt", "Zrho", "cv_1", "cv_5", "cv_10", "case", "lags", "newey_west_lags", "test", "var",
      "zrho_cv_1", "zrho_cv_5", "zrho_cv_10",
  };
  for (const char* k : kUnitRootFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // `regress`: the test's regression, shown after the test (`regress, nohead`) and
  // returned as r(table) (dfuller.ado / pperron.ado). dfuller regresses D.y on L.y,
  // the lagged differences LD.y ... L#D.y and the trend; pperron y on L.y and the
  // trend; both with a constant unless noconstant. The trend is the time variable
  // less its minimum over the sample, named _trend. The regressors are generated
  // under tsset on the whole frame, the sample then selected, and the fit is the
  // engine's least squares (regress's). e() stays the user's (the ado holds it).
  if (regressTable) {
    const int nlags = cobraCmd == "dfuller" && raw.contains("lags") && raw["lags"].is_number()
                          ? raw["lags"].get<int>()
                          : 0;
    const std::string shown = parsed.varlist[0];
    std::vector<std::string> gens, names;  // engine column, Stata name
    std::string exprs;
    const auto gen = [&](const std::string& col, const std::string& expr, const std::string& nm) {
      gens.push_back(col + "=" + expr);
      exprs += gens.back() + " ";
      names.push_back(nm);
    };
    // the shown operator stacks onto a typed one (`dfuller D.x`: L.D.x is `LD.x`)
    const auto opName = [&](const std::string& op) {
      const size_t dot = shown.find('.');
      return dot == std::string::npos ? op + "." + shown
                                      : op + shown.substr(0, dot) + shown.substr(dot);
    };
    const bool df = cobraCmd == "dfuller";
    gen("__urr_y__", df ? "D." + v : v, df ? opName("D") : shown);
    gen("__urr_l__", "L." + v, opName("L"));
    for (int k = 1; k <= nlags; ++k)
      gen("__urr_d" + std::to_string(k) + "__", "L" + std::to_string(k) + "D." + v,
          opName(k == 1 ? std::string("LD") : "L" + std::to_string(k) + "D"));
    const std::string genFrame = nextPathM4(session.workdir(), "urr_gen");
    std::vector<std::string> eargv = {"eval", "--data", dataPath};
    for (const auto& g : gens) eargv.insert(eargv.end(), {"--gen", g});
    eargv.insert(eargv.end(), {"--out", genFrame});
    ojson fitRaw;
    std::string why;
    try {
      (void)runCobraJsonM4(translate::injectTsContextForExpr(eargv, exprs, session));
      ojson ifErr;
      auto filtered = applyIfFilterM4(session, genFrame, *sampleCond, ifErr);
      if (!filtered.has_value()) return ifErr;
      std::string fitFrame = *filtered;
      std::vector<std::string> xs = {"__urr_l__"};
      for (int k = 1; k <= nlags; ++k) xs.push_back("__urr_d" + std::to_string(k) + "__");
      if (trend) {
        // the minimum is over the test's sample: the rows if/in keep where the tested
        // variable (with its operators) and the time variable are present
        Datastore fds(fitFrame);
        double mn = std::numeric_limits<double>::quiet_NaN();
        const double* tv = fds.col(*session.timevar).f64();
        const double* yv = fds.has(v) ? fds.col(v).f64() : nullptr;
        for (int64_t r = 0; tv != nullptr && r < fds.nRows(); ++r)
          if (!std::isnan(tv[r]) && (yv == nullptr || !std::isnan(yv[r])) &&
              (std::isnan(mn) || tv[r] < mn))
            mn = tv[r];
        char b[64];
        std::snprintf(b, sizeof b, "%.17g", mn);
        const std::string tFrame = nextPathM4(session.workdir(), "urr_trend");
        (void)runCobraJsonM4({"eval", "--data", fitFrame, "--gen",
                              "__urr_t__=" + *session.timevar + "-(" + b + ")", "--out", tFrame});
        fitFrame = tFrame;
        xs.push_back("__urr_t__");
        names.push_back("_trend");
      }
      std::vector<std::string> fargv = {"feols", "--y", "__urr_y__", "--x", joinCommaM4(xs),
                                        "--data", fitFrame, "--tidy"};
      if (noConstant) fargv.push_back("--no-intercept");
      fitRaw = runCobraJsonM4(fargv);
    } catch (const CobraError& ce) {
      why = cobraErrorStrM4(ce);
    } catch (const std::exception& e) {
      why = e.what();
    }
    if (!why.empty())
      return engineErrorBlockM4(cobraCmd + ": the regression table could not be built (" + why + ")");
    ojson fitBlock = cobraToEstimationM4(fitRaw, "feols", &session);
    const ojson& ct = fitBlock.contains("coeftable") ? fitBlock["coeftable"] : ojson::array();
    const double dfR = fitBlock.contains("df_r") && fitBlock["df_r"].is_number()
                           ? fitBlock["df_r"].get<double>()
                           : std::numeric_limits<double>::quiet_NaN();
    // the coefficients in the fit's order, the constant last
    eval::EvalMatrix tbl;
    tbl.rownames = {"b", "se", "t", "pvalue", "ll", "ul", "df", "crit", "eform"};
    tbl.data.assign(9, std::vector<double>());
    // the table is regress's replay: at c(level) (`set level`), its interval b -+ crit se
    const double lvl = static_cast<double>(session.level);
    const double crit = cecon::t_critical(1.0 - lvl / 100.0, dfR);
    const auto num = [](const ojson& r, const char* k) {
      return r.contains(k) && r[k].is_number() ? r[k].get<double>()
                                               : std::numeric_limits<double>::quiet_NaN();
    };
    std::ostringstream os;
    const auto g9 = [](double x) {
      if (std::isnan(x)) return std::string(".");
      char b[64];
      std::snprintf(b, sizeof b, "%.17g", x);
      std::string t = formatNumberAsDisplayed("%9.0g", b);
      while (!t.empty() && t.front() == ' ') t.erase(t.begin());
      return t;
    };
    std::vector<std::pair<std::string, const ojson*>> rows;
    for (size_t i = 0; i < ct.size(); ++i) {
      const std::string nm = ct[i].value("name", std::string());
      if (nm == "_cons" || nm == "(intercept)") continue;
      rows.emplace_back(nm, &ct[i]);
    }
    for (const auto& c : ct)
      if (c.value("name", std::string()) == "_cons" || c.value("name", std::string()) == "(intercept)")
        rows.emplace_back("_cons", &c);
    // engine column -> Stata name
    std::map<std::string, std::string> label = {{"__urr_l__", names[1]}};
    for (int k = 1; k <= nlags; ++k)
      label["__urr_d" + std::to_string(k) + "__"] = names[1 + static_cast<size_t>(k)];
    if (trend) label["__urr_t__"] = "_trend";
    label["_cons"] = "_cons";
    os << "\nRegression table\n" << std::string(78, '-') << "\n";
    {
      char b[256];
      char lv[32];
      std::snprintf(lv, sizeof lv, "%g", lvl);
      std::snprintf(b, sizeof b, "%12s | Coefficient  Std. err.      t    P>|t|     [%s%% conf. interval]\n",
                    statcmd_detail::fitLeft(statcmd_detail::abbrevCp(names[0], 12), 12).c_str(), lv);
      os << b << "-------------+" << std::string(64, '-') << "\n";
    }
    for (const auto& [eng, row] : rows) {
      const auto it = label.find(eng);
      if (it == label.end())
        return engineErrorBlockM4(cobraCmd + ": the regression table names a coefficient it did not fit (" + eng + ")");
      const double b = num(*row, "b"), se = num(*row, "se"), t = num(*row, "t"), p = num(*row, "p"),
                   ll = b - crit * se, ul = b + crit * se;
      tbl.colnames.push_back(it->second);
      const double colv[9] = {b, se, t, p, ll, ul, dfR, crit, 0.0};
      for (int r = 0; r < 9; ++r) tbl.data[static_cast<size_t>(r)].push_back(colv[r]);
      char b2[256];
      std::snprintf(b2, sizeof b2, "%12s | %10s %10s %8.2f   %5.3f    %10s  %10s\n",
                    statcmd_detail::fitLeft(statcmd_detail::abbrevCp(it->second, 12), 12).c_str(), g9(b).c_str(), g9(se).c_str(), t, p,
                    g9(ll).c_str(), g9(ul).c_str());
      os << b2;
    }
    os << std::string(78, '-') << "\n";
    session.eval.rMatrices.erase(
        std::remove_if(session.eval.rMatrices.begin(), session.eval.rMatrices.end(),
                       [](const auto& kv) { return kv.first == "table"; }),
        session.eval.rMatrices.end());
    tbl.gen = eval::nextStoreGeneration();
    session.eval.rMatrices.emplace_back("table", std::move(tbl));
    block["regress_table"] = os.str();
  }
  // No LastModel caching — dfuller/pperron are r-class diagnostic tests in
  // real Stata, not e-class "current model" estimation commands (no
  // margins/test/predict after dfuller in real Stata either).
  return block;
}

// wntestq (BS-080): `wntestq varname [if] [, lags(#)]` -> cobra wntestq
// --var V [--lags P]. Oracle D4e: bare (Q=839.9262, df=40=lags, p byte-
// exact) and `, lags(10)` (Q=557.2013, df=10) — BOTH engine-exact. Real
// Stata's own default lag rule is min(n/2-2, 40); the engine reproduces
// this AUTOMATICALLY when --lags is omitted (live-verified on both n=336 ->
// df=40 and a synthetic n=30 subset -> df=13 == min(30/2-2,40)=13) — PIET
// does NOT need to replicate the rule itself, it only forwards lags(#) when
// the user actually types it. No --timevar on this CLI at all (rows are
// read in on-disk order) — unlike newey/prais/dfuller/pperron above, no
// prior `tsset` is required.
ojson runWntestq(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() != 1) {
    return engineErrorBlockM4("syntax_error: `wntestq` requires exactly one variable",
                              "syntax_error");
  }
  std::string v = parsed.varlist[0];
  // wntestq needs the data tsset: r(111) "time variable not set, use tsset
  // varname ..." (vs-stata bank 38db1194e1501f76 `wntestq price`).
  if (!session.timevar.has_value() || session.timevar->empty())
    return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  // `wntestq D.gnp96`: the operated series generated under tsset onto a copy.
  std::string dataPath = *session.currentHbin();
  {
    std::vector<std::string> one = {v};
    ojson tsErr;
    const auto tsFrame = tsVarlistFrameM4(session, one, tsErr);
    if (!tsFrame.has_value()) return tsErr;
    if (!tsFrame->empty()) {
      dataPath = *tsFrame;
      v = one[0];
    }
  }
  ojson inErr;
  const auto sampleCond = tsSampleCondition(parsed, session, "wntestq", inErr);
  if (!sampleCond.has_value()) return inErr;

  std::optional<std::string> lagsVal;
  for (const auto& [k, val] : parsed.opts) {
    if (k == "lags" && !val.isBool) lagsVal = val.strValue;
  }

  std::vector<std::string> argv = {"wntestq", "--var", v};
  if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
    argv.push_back("--lags");
    argv.push_back(trimM4(*lagsVal));
  }
  if (sampleCond->has_value()) {
    argv.push_back("--keepif");
    argv.push_back(**sampleCond);
  }
  // Under tsset the lags are TIME lags (wntestq.ado -> ac.ado's L`i'.x): a period
  // with no observation (sp500's weekends) breaks the pairs across it. Read in
  // row order, Q was 3584.00 where Stata's is 2464.37.
  if (session.timevar.has_value() && !session.timevar->empty() &&
      !(session.panelvar.has_value() && !session.panelvar->empty())) {
    argv.push_back("--timevar");
    argv.push_back(*session.timevar);
    if (session.delta.has_value()) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", *session.delta);
      argv.push_back("--delta");
      argv.push_back(buf);
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabWntestq.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  // the one-panel rule follows the syntax checks (the setup check runs after syntax)
  if (auto refused = onePanelRefusalM4(parsed, session)) return *refused;
  argv.push_back("--data");
  argv.push_back(dataPath);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  if (raw.contains("var")) raw["var"] = parsed.varlist[0];
  ojson block = cobraToEstimationM4(raw, "wntestq", &session);
  block["estimator"] = "wntestq";
  static const char* kWntestqFields[] = {"Q", "df", "lags", "var", "test"};
  for (const char* k : kWntestqFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  return block;
}

// sdtest (BS-085): `sdtest varname [if], by(groupvar)` (two-sample
// variance-ratio test) / `sdtest varname [if] = #` (one-sample, against a
// fixed SD) -> cobra sdtest --var V (--by G | --against SD0) [--level L].
// Oracle D5a (by(high)): F=1.3992 exactly, df1=229 df2=105, p_lower/
// p_upper/p all byte-exact against Stata's Pr(F<f)/Pr(F>f)/2*Pr(F>f). The
// `= #` (one-sample) form is wired too (cobra's own --against flag) but has
// no oracle coverage this batch — wiring-only, not Stata-pinned.

}  // namespace piet::handlers
