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
    return engineErrorBlockM4("newey: run `tsset timevar` first so PIET knows the time variable.",
                              "unsupported_option");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> lagVal;
  bool force = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lag" && !v.isBool) lagVal = v.strValue;
    if (k == "force" && v.isBool && v.boolValue) force = true;
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
    return engineErrorBlockM4("prais: run `tsset timevar` first so PIET knows the time variable.",
                              "unsupported_option");
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
  return block;
}

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
ojson runDfullerPperron(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                        SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  bool allowDrift = (cobraCmd == "dfuller");
  if (parsed.varlist.size() != 1) {
    return engineErrorBlockM4("syntax_error: `" + cobraCmd + "` requires exactly one variable",
                              "syntax_error");
  }
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return engineErrorBlockM4(
        cobraCmd + ": run `tsset timevar` first so PIET knows the time variable.",
        "unsupported_option");
  }
  std::string v = parsed.varlist[0];

  std::optional<std::string> lagsVal;
  bool noConstant = false, drift = false, trend = false;
  for (const auto& [k, v2] : parsed.opts) {
    if (k == "lags" && !v2.isBool) lagsVal = v2.strValue;
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
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: see runXtPoissonLogit — `drift` is dfuller-only, so it stays a
  // conditional insert here while the registered vocabulary carries the union.
  std::unordered_set<std::string> known = kDfullerPperronBase;
  if (allowDrift) known.insert("drift");
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  block["estimator"] = cobraCmd;
  static const char* kUnitRootFields[] = {
      "Zt", "Zrho", "cv_1", "cv_5", "cv_10", "case", "lags", "newey_west_lags", "test", "var",
  };
  for (const char* k : kUnitRootFields) {
    if (raw.contains(k)) block[k] = raw[k];
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

  std::optional<std::string> lagsVal;
  for (const auto& [k, val] : parsed.opts) {
    if (k == "lags" && !val.isBool) lagsVal = val.strValue;
  }

  std::vector<std::string> argv = {"wntestq", "--var", v};
  if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
    argv.push_back("--lags");
    argv.push_back(trimM4(*lagsVal));
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabWntestq.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

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
