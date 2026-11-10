// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Postestimation context.
#include "piet/handlers.h"
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

#include "postestimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

namespace postest_detail {

// Stata's "last estimates not found" r(301) (w2u_binary_ml_edges.log:965-977: `predict`
// after a fit that stopped at r(2000) is rc 301), never the rc 1 of a bare engine_error.
// The message is kept: internal script replay_package.py classifies it by text.
ojson noPreviousEstimation() {
  return ojson(eval::errorBlock("engine_error", "No previous estimation result. Run a regression first.",
                                std::nullopt, std::nullopt, std::nullopt, std::string("r(301)")));
}

// eb-order T-P3: with no fitted model, a live e() that posted no e(b) (an
// estpost posting, a restored or `estimates use`d one) is not "no estimates" for
// every command. Stata's test, lincom and predict answer r(321) "requested
// action not valid after most recent estimation command" there
// (w2v_estpost_ematrix.log ep14_test, ep14_lincom, ep14_predict, ep14n_*,
// ep14s_predict/_lincom), while testparm, nlcom and margins keep r(301) "last
// estimates not found" (ep14_testparm, ep14_nlcom, ep14_margins). Every other
// case is noPreviousEstimation's r(301).
ojson noPreviousEstimationFor(const SessionState& session, bool invalidAfterPosting) {
  if (invalidAfterPosting && session.eval.eLive() && session.eval.coefSource() == nullptr)
    return ojson(eval::errorBlock("engine_error",
                                  "requested action not valid after most recent estimation command",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(321)")));
  return noPreviousEstimation();
}

// The clustered small-sample convention the FIT ran under, as the feols flags
// runRegress (handlers.cpp) pushed for it -- the same keys, so the refit never
// answers under a different convention than the table it follows:
//  - BUG-568 --keep-nested-fe-dof: the areg word with an absorbed effect and a
//    cluster/twoway VCE (w2h_postest_refit.log:1081 A_cl_lincomx1 r(se)
//    .27856854160853 = _se[x1]; reghdfe/xtreg,fe exempt the nested FE, :2005
//    H_cl_ctl .2613712121703248).
//  - FU-365/BUG-848 --multiway-subset-factors: regress/areg with a twoway or
//    multiway VCE (w2l_vce_shape_det.log:263 regress_vce_c2_sex1
//    .0643525900770342, :619 areg_vce_c2_sex1 .0778721308909151).
// LastModel records no flag of its own; lm.stataCmd is the only record of the
// command word (runRegress keys both flags on it). Empty for every other fit,
// whose argv stays byte-identical.
//  - FU-386 --multiway-subset-factors after xtreg,fe with a twoway or multiway
//    VCE (runXtreg; w2s_multiway_class.log:696 mw_xt_cg_se .0532737214421747); the
//    engine exempts the panel FE nested in the clusters (minus = rank + dfax).
//  - FU-406: areg's --keep-nested-fe-dof under a three-way (`multiway`) VCE too
//    (log:1165 mw_areg_c3 .0720220388954095).
std::vector<std::string> fitConventionFlagsM4(const LastModel& lm) {
  std::vector<std::string> flags;
  if (lm.cobraCmd != "feols") return flags;
  if (lm.stataCmd == "areg" && !lm.fe.empty() &&
      (lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway"))
    flags.push_back("--keep-nested-fe-dof");
  if ((lm.stataCmd == "regress" || lm.stataCmd == "reg" || lm.stataCmd == "areg" ||
       lm.stataCmd == "xtreg") &&
      (lm.vcov == "twoway" || lm.vcov == "multiway"))
    flags.push_back("--multiway-subset-factors");
  // e(sample) T2: the fit kept its singleton groups (areg, xtreg,fe, reghdfe's
  // keepsingletons), so the refit does too (cmds_postest.cpp postest_fit_spec):
  // dropping them refits a different sample (w2_esample.log:897 D3 59, :925 D4
  // 59 against the default drop's 58).
  if (lm.keepSingletons && !lm.fe.empty()) flags.push_back("--keep-singletons");
  // BUG-1067: the fit omitted the later member of each collinear set (reghdfe,
  // areg, xtreg,fe), so the refit omits the same columns: otherwise `test`/`lincom`
  // after `reghdfe y x z` (z = 2*x) refit with x omitted and answered for _b[z].
  if (lm.collinearForward) flags.push_back("--collinear-forward");
  // Stata regress's exact-fit rule (runRegress pushes --regress-exact-fit on the
  // same condition): the refit of `test`/`lincom`/`nlcom` sees the fit's V = 0, so
  // `test x` after an exact `regress` answers F and p missing as StataBE does.
  // BUG-1055: areg and an aw/fw/iw-weighted regress too, under the classic VCE.
  if ((((lm.stataCmd == "regress" || lm.stataCmd == "reg") && lm.fe.empty()) ||
       lm.stataCmd == "areg") &&
      lm.fitWtype != "pw" && lm.fitWtype != "pweight" && (lm.vcov.empty() || lm.vcov == "iid") &&
      lm.cluster.empty() &&
      !(lm.result.has_value() && lm.result->is_object() && lm.result->contains("svy")))
    flags.push_back("--regress-exact-fit");
  return flags;
}

// FU-541: a logit/probit fitted `asis` kept its perfect predictors, so every
// engine refit of it (lincom/nlcom/test, predict, margins) must skip Stata's
// perfect-prediction screen too; the engine marks such a reply `asis`. Any other
// fit adds nothing, so those argvs are byte-identical.
// FU-426: the same refits carry the fit's maximize options, which the engine
// echoes as the flags it was given (`ml_max_argv`, cmds_mle.cpp
// echo_ml_max_options), so the refit stops at the iterate the fit posted: after
// `logit d x1 x2 i.c, tolerance(1e-12) ...` (e(ic) 10) `test 2.c` answered from
// the default e(ic) 8 refit (w2e_probes.log:249 lp_tight r(chi2) against :141
// lp_default). A fit given none has no key and adds nothing.
// cloglog's reply carries both keys too (cmds_mle.cpp cmd_cloglog; BUG-1547).
void pushFitAsisM4(std::vector<std::string>& args, const LastModel& lm) {
  if (!(lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "cloglog") ||
      !lm.result.has_value() || !lm.result->is_object())
    return;
  if (lm.result->value("asis", false)) args.push_back("--asis");
  const auto it = lm.result->find("ml_max_argv");
  if (it == lm.result->end() || !it->is_array()) return;
  for (const auto& f : *it)
    if (f.is_string()) args.push_back(f.get<std::string>());
}

bool postestFitLacksConstantM4(const LastModel& lm);  // defined with the refit guards below

// FU-385/FU-387: the `--model iv` 2SLS refit of lincom/test/nlcom after
// `ivregress 2sls` (postestIvRefitServedM4): --x carries the included exogenous
// regressors only (or the constant placeholder), --endog/--instruments the fit's
// own lists, and --small / --multiway-subset-factors the fit's variance convention
// (runIvRegress; the reply keeps `small` true only after `small`).
bool ivFitSmallM4(const LastModel& lm) {
  return lm.result.has_value() && lm.result->is_object() && lm.result->contains("small") &&
         (*lm.result)["small"].is_boolean() && (*lm.result)["small"].get<bool>();
}
bool ivRefitVerbM4(const std::string& verb) {
  return verb == "lincom" || verb == "test" || verb == "nlcom";
}

std::vector<std::string> lastModelBaseArgv(const std::string& cobraCmd, const LastModel& lm) {
  // W:P5 fit-sample fidelity (BUG-163): refit from the sample the model was
  // ACTUALLY fitted on. fitDataPath is runRegress's materialized if/in scratch
  // file; fitKeepif replays a --keepif that went straight to the engine. Both
  // empty => byte-identical to the historical full-dataPath call.
  const bool ivRefit = lm.cobraCmd == "iv" && ivRefitVerbM4(cobraCmd);
  std::vector<std::string> args = {cobraCmd, "--data",
                                    lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath,
                                    "--y", lm.y, "--x",
                                    ivRefit ? (lm.ivExog.empty() ? std::string("(intercept)")
                                                                 : joinCommaM4(lm.ivExog))
                                            : (lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x))};
  if (ivRefit) {
    args.insert(args.end(), {"--endog", joinCommaM4(lm.ivEndog), "--instruments",
                             joinCommaM4(lm.ivInstruments)});
    if (ivFitSmallM4(lm)) {
      args.push_back("--small");
      if (lm.vcov == "twoway" || lm.vcov == "multiway") args.push_back("--multiway-subset-factors");
    }
  }
  if (!lm.fitKeepif.empty()) {
    args.push_back("--keepif");
    args.push_back(lm.fitKeepif);
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }
  // BS-098/BS-013 (BETA A4): forward the FITTED model's vcov/cluster so
  // post-estimation recomputes SEs under the SAME vcov the original model
  // used, instead of cobra's test/lincom/nlcom CLI silently defaulting to
  // iid (the §1f probe: lincom SE 0.0543 iid vs 0.0044 cluster after a
  // `vce(cluster ...)` model — cmds_postest.cpp's cmd_lincom/cmd_test both
  // do `parse_vcov(opt(a, "--vcov", "iid"))`, i.e. iid whenever --vcov is
  // simply never sent, which is what happened for EVERY post-estimation
  // call before this fix). lm.vcov defaults to "iid" (session_state.h) and
  // that IS cobra's own default too, so this is a byte-identical no-op for
  // the (overwhelmingly common) unclustered/non-robust case — the ONLY
  // observable change is for a lastModel that actually used a non-default
  // vcov.
  args.push_back("--vcov");
  args.push_back(lm.vcov);
  if (!lm.cluster.empty()) {
    args.push_back("--cluster");
    args.push_back(joinCommaM4(lm.cluster));
  }
  // BUG-571: the fit's weight. Without it the refit was the UNWEIGHTED model at
  // rc 0 (w2h_postest_refit.log:383 R_pw_lincomx1 r(estimate) 2.381531911602316;
  // main answered 2.4882849226167476). Empty for an unweighted fit, so that argv
  // is byte-identical to before. postestRefitMatchesFitM4 keeps refusing the
  // weighted fits this refit cannot reproduce (ML estimators, svy:).
  if (!lm.fitWeight.empty()) {
    args.push_back("--weight");
    args.push_back(lm.fitWeight);
    if (!lm.fitWtype.empty()) {
      args.push_back("--wtype");
      args.push_back(lm.fitWtype);
    }
  }
  for (const auto& f : fitConventionFlagsM4(lm)) args.push_back(f);
  pushFitAsisM4(args, lm);
  // FU-131: the fit's `noconstant`. postest_fit_spec refits without the constant
  // (feols_fit's no_intercept; logit/probit drop load_glm's trailing one), so
  // lincom/test/testparm/nlcom answer for the fitted model (w2p_estim_ev.log:1294
  // G_nc_lincom_x1 2.356553697480766 = :1275 G_nc_fit _b[x1]). Absent for a fit
  // with a constant, whose argv stays byte-identical.
  if (postestFitLacksConstantM4(lm)) args.push_back("--no-intercept");
  return args;
}

// ---------------------------------------------------------------------------
// contract_A §31 — post-estimation by handle, without the re-fit.
//
// WHAT THIS COSTS TODAY. lastModelBaseArgv above rebuilds the ENTIRE model
// specification on every lincom/nlcom/test, and the engine answers it by
// fitting that model again. Measured 2026-07-21 on the real analysis: in ONE
// dependent-variable pass `lincom` ran 54 times for 22.61 s — 419 ms each, 17 %
// of that pass's 131 s, the largest single engine-side consumer. At the
// 160-depvar scale that is roughly an hour spent recomputing fits that already
// existed.
//
// WHAT IS AND IS NOT BEING PAID — measured, not assumed. These calls are
// ALREADY resident: the argv carries --data but no --out and no --keepif, so
// SessionState's intercept takes its `goResident` branch (session_state.cpp)
// and reissues them as a frame `run` with --data stripped. That same branch's
// own comment names `test`/`margins`/`lincom` as the post-estimation case it
// resyncs for. So the 164 MB re-read is NOT part of the 419 ms — only the
// resync after a `gen`/`replace` moved the head re-reads, and that is one call,
// not 54. The re-fit is the whole remaining term, and the only thing this
// change buys.
//
// WHY THE FIRST CALL STILL FITS. §31 mints only on lincom/nlcom/test, not on
// feols itself, so the first post-estimation call after each fit still fits
// once and publishes the result; the remaining N−1 are free. On the measured
// flow that is 1 fit instead of 54 per dependent variable.
//
// STORE PRESSURE, AND WHY EVICTION CANNOT BITE THE LIVE HANDLE. Because the
// fitting form already arrives as a frame `run`, the engine mints on EVERY
// post-estimation call PIET issues today — ~54 per depvar, ~8,600 across
// analysis.do, each storing a k×k covariance. This change is what stops that:
// only the first call per model still fits, so mint traffic drops with the
// re-fits (~54/depvar -> 1). The engine caps the store at 32 MODELS (not
// bytes), evicting in mint order. PIET holds at most ONE handle at a time — it
// lives on LastModel, and step (3) below overwrites it with each new mint — so
// the handle PIET is holding is always the MOST RECENTLY minted one, i.e. the
// last thing the cap would ever evict. Eviction would have to be preceded by 32
// further mints while PIET still held this one, which the single-handle
// invariant forbids. And if it somehow happened anyway, it is not a user-facing
// failure: exit 3, the handle is dropped, the full specification runs and
// answers correctly one re-fit slower, and re-mints for the next call.
//
// FEATURE-DETECTED, NEVER VERSION-GATED. See CobraSession::postestState() —
// the engine implementing §31 still advertises contract_a 1.19.0, and Art 7
// means a shipped PIET pins the ARCHIVED engine, so PIET must keep working
// against a binary with no `postest` op at all. Every failure path below lands
// on the unchanged full-specification call.

// Write `handle` onto the session's LastModel (empty clears it). Copy-modify-set
// through setLastModel is the only supported mutation: it also refreshes
// eval.lastModelResult, which the _b[]/_se[]/e() resolver reads. The equality
// guard keeps the common case (handle unchanged across a run of combinations)
// from re-assigning that JSON on every single call.
void setEstHandleM4(SessionState& session, const std::string& handle) {
  const auto& cur = session.lastModel();
  if (!cur.has_value() || cur->estHandle == handle) return;
  LastModel updated = *cur;
  updated.estHandle = handle;
  session.setLastModel(std::move(updated));
}

// BUG-203. Defined below, next to the estimator-membership sets it reads (they
// belong beside runLincom/runNlcom/runTestCmd's guard, not here); forward-
// declared because the fallback argv is assembled in this function.
std::string postestModelFlagM4(const LastModel& lm);

// Run ONE post-estimation combination, preferring the §31 handle path.
//   `verb`  — "lincom" | "nlcom" | "test"
//   `combo` — the combination-only flags (--exp/--expr/--joint/--constraint,
//             --tidy). Must contain NOTHING that specifies the model: on the
//             handle path the stored model IS the specification, and §31
//             rejects an argv carrying both rather than letting one silently
//             win.
// Returns the raw cobra result JSON, and throws CobraError exactly where
// runCobraJsonM4 does — so all three call sites keep their existing catch.
ojson runPostEstimationM4(const std::string& verb, const std::vector<std::string>& combo,
                          SessionState& session, const LastModel& lm) {
  CobraSession* cs = activeCobraSession();
  const std::string handle = lm.estHandle;

  // Built up front, while `lm` is unambiguously live. setEstHandleM4 below
  // writes through setLastModel, and `lm` is a reference INTO the session's own
  // LastModel — reading it across that write is a lifetime question not worth
  // having. A dozen small string copies on the fast path does not buy back the
  // subtlety of avoiding them.
  std::vector<std::string> fallbackArgs = lastModelBaseArgv(verb, lm);
  // BUG-203: name the ESTIMATOR on the full-specification path, so the engine
  // refits with it instead of with feols_fit(). Empty for the least-squares
  // family, which keeps its argv byte-for-byte. NOT added to `combo`: the
  // handle path passes combo verbatim and §31 rejects an argv that carries both
  // a handle and a model specification — and it does not need the flag anyway,
  // because the stored fit already IS the maximum-likelihood one (cobra's
  // PostestModel carries its own `asymptotic` marker for exactly this reason).
  {
    const std::string modelFlag = postestModelFlagM4(lm);
    if (!modelFlag.empty()) {
      fallbackArgs.push_back("--model");
      fallbackArgs.push_back(modelFlag);
    }
  }
  fallbackArgs.insert(fallbackArgs.end(), combo.begin(), combo.end());
  // BUG-369: the fallback REFITS from the original specification, so an `L.x`
  // regressor needs the same tsset context the original fit was given
  // (buildEstimationArgvM4's injectTsContext at :793). Without it, a model that
  // fitted perfectly well could not be post-estimated: `regress fatal beertax
  // L.beertax` then `test L.beertax` came back ts_no_tsset — the fit knew about
  // the time variable and its own `test` did not.
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.push_back(lm.y);
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    bool needTs = translate::hasTsToken(tsTokens);
    // The restriction itself can name one too (`test L.x`, `testparm L.x`), and
    // it arrives as a `--joint`/`--constraint` VALUE rather than a bare token.
    if (!needTs)
      for (const auto& c : combo)
        if (translate::exprHasTsToken(c)) { needTs = true; break; }
    if (needTs) fallbackArgs = translate::injectTsContext(fallbackArgs, session);
  }

  // ---- (1) handle path: no data read, no re-fit -------------------------
  if (!handle.empty() && cs != nullptr && cs->postestState() != CobraSession::Postest::No) {
    std::vector<std::string> argv = {verb};
    argv.insert(argv.end(), combo.begin(), combo.end());
    // Mirror runCobraJsonM4's choke point so stdout has the SAME shape on both
    // paths (§31 guarantees the estimator numbers are byte-equal; provenance is
    // the documented carve-out, and this is the flag that governs it).
    if (!piet::settings::load().provenanceHash) argv.push_back("--no-provenance");
    // No out-param: nothing here needs the echoed handle or `fits` (the gate
    // leg reads those through lastEst()); the numbers come from stdout.
    CobraRunResult r = cs->framePostest(handle, argv, nullptr);
    if (r.ok) {
      try {
        return ojson::parse(r.stdout_text);
      } catch (const nlohmann::json::exception&) {
        // An exit-0 postest with unparseable stdout is not a shape §31 permits.
        // Treat it as a miss and re-run the full specification rather than
        // surfacing a synthesized failure for something the slow path can
        // answer correctly.
      }
    }
    // ANY miss falls through: exit 3 (handle expired / never minted / evicted,
    // or the E6 unconsumed-flag net), exit 2 (engine predates §31), a dead
    // worker. §31 is explicit that the client must never assume, reuse or
    // synthesize an answer on a stale handle — re-fitting is the only correct
    // response, and dropping the handle first keeps a permanently-dead one from
    // costing a failed round trip on every subsequent call.
    setEstHandleM4(session, "");
  }

  // ---- (2) full-specification fallback — today's path, byte-for-byte -----
  // fallbackArgs came from lastModelBaseArgv, which forwards the FITTED model's
  // --vcov/--cluster; that is what keeps BS-098/BS-013 alive on this path (see
  // its comment above). Nothing here is conditional on §31 — an engine without
  // it reaches this line on every single call, exactly as today.
  const long long seqBefore = (cs != nullptr) ? cs->responseSeq() : 0;
  ojson raw = runCobraJsonM4(fallbackArgs);  // throws CobraError -> caller's catch

  // ---- (3) retain whatever handle that fit just minted -------------------
  // Re-fetched, not reused: the call above may be the FIRST thing on this
  // thread to reach the worker at all, in which case `cs` was null on entry and
  // the session that just serviced it is only discoverable now.
  //
  // Guarded on responseSeq() having MOVED, because the call may equally have
  // bypassed the worker entirely (serve disabled, §25-only engine, one-shot
  // fallback) — and then lastEst() still describes some EARLIER response.
  // Storing that onto the model just fitted is exactly the wrong-model hazard,
  // so an unmoved sequence must read as "no mint", never as "reuse the last
  // one". A null-on-entry cs contributes seqBefore == 0, which is safe: any
  // session that parsed a response has responseSeq() >= 1.
  CobraSession* csAfter = activeCobraSession();
  if (csAfter != nullptr && csAfter->responseSeq() != seqBefore) {
    const CobraEstMeta& est = csAfter->lastEst();
    if (est.valid) setEstHandleM4(session, est.handle);
  }
  return raw;
}

// BUG-203 — WHICH ESTIMATOR PRODUCED THE MODEL IS PART OF THE QUESTION.
//
// cobra's `lincom`/`nlcom`/`test` CLIs take a model SPECIFICATION (--y/--x/
// --fe/--vcov) and no estimator at all, so cmds_postest.cpp answers every one
// of them with `feols_fit()`. runLincom/runNlcom/runTestCmd checked only that a
// lastModel EXISTS, never which estimator produced it, so `logit y x1 x2` ->
// `lincom x1` silently reported the LINEAR-PROBABILITY-MODEL coefficient as the
// logit one. The knowledge was already in this file (runIvRegress's own
// comment: cobra's post-estimation CLIs "always re-fit as plain OLS") — it had
// just never been followed to the user-visible surface.
//
// Measured against StataNow 19.5 (n=120, seed 203203, `logit ybin x1 x2` then
// `lincom x1`): Stata 1.087742905079 (se .266346342116), PIET 0.221521751160
// (se .046231897247) — and `poisson ybin x1 x2` then `lincom x1` returned that
// SAME 0.221521751160 to every digit, which is the proof the estimator was not
// read at all rather than merely mishandled. 0.221521751160 is exactly what
// Stata's own `regress ybin x1 x2` -> `lincom x1` prints.
//
// kPostestOlsCmds is exactly the set of lm.cobraCmd values for which the
// engine's OLS refit IS the fitted model's own coefficients — the same
// membership argument runMargins' kAtSafeCmds makes further down (plain
// regress/feols, including reghdfe and xtreg's ,fe path, all of which already
// share these cobraCmd spellings per runXtreg/handlers.cpp::runRegress).
//
// kPostestGlmCmds is the set cobra's `--model` refit covers: the same
// logit/probit/poisson family cmd_margins' GLM branch already fits through
// load_glm + logit_fit/probit_fit/fepois_fit. For those PIET forwards
// `--model <estimator>` and the engine answers off the REAL maximum-likelihood
// fit, referencing the combination to the normal (and `test` to chi2) the way
// Stata does.
//
// Everything else honest-rejects: `nbreg`, `tobit`/`heckman`/`qreg`/`mediate`
// (non-OLS point estimates), `newey`/`prais` (HAC/AR1 transforms),
// `iv`/`liml`/`reg3` (2SLS/GMM/SUR), `xtreg`'s random-effects path, `fepois`/
// `ppmlhdfe`/`xtpois`/`xtlogit` (the engine's ML refit has no absorbed-effects
// path), `csdid` — none of these estimators' fitted coefficients equal a plain
// OLS refit of y on x, and none has a post-estimation refit in the engine yet.
// Honest refusal beats a silent wrong answer (the same doctrine BS-023/BS-052
// and runMargins' own kAtSafeCmds reject already apply on the neighbouring
// `margins, at()` surface).
const std::set<std::string>& postestOlsCmdsM4() {
  static const std::set<std::string> kPostestOlsCmds = {"feols", "ols", "regress", "reg", "r"};
  return kPostestOlsCmds;
}
// BUG-1547: cloglog joined the engine's ML refits (9e2e97c8: predict pr/xb, the
// margins AME and at() branches under OIM/robust/cluster/pweight, lincom/nlcom/test
// off cloglog_ml), matching StataBE 19.5 within 2e-15 (scratch-laneA/agents/eng-ml
// x21.log, x22.log).
const std::set<std::string>& postestGlmCmdsM4() {
  static const std::set<std::string> kPostestGlmCmds = {"logit", "probit", "poisson", "cloglog"};
  return kPostestGlmCmds;
}

// The `--model` value to forward for this lastModel, or empty for the
// least-squares family (which forwards nothing and keeps its historical argv
// byte-for-byte). Callers must first have passed postestEstimatorOkM4.
std::string postestModelFlagM4(const LastModel& lm) {
  if (lm.cobraCmd == "iv") return "iv";  // FU-385: the 2SLS refit (postestIvRefitServedM4)
  return postestGlmCmdsM4().count(lm.cobraCmd) ? lm.cobraCmd : std::string();
}

// FU-385/FU-387: lincom/test/testparm/nlcom after `ivregress 2sls` are served by the
// engine's `--model iv` refit (cmds_postest.cpp postest_fit_spec), which runs the
// fit's own iv2sls_fit under its --vcov/--cluster/--small and multiway convention:
// w2s_multiway_class.log:840 mw_iv_c2_lincom r(se) .0503059336995026 = _se[x1] :816,
// r(df) `.` (no `small`: the normal reference). Only that shape: ivreghdfe/ivreg2
// (absorbed effects, their own small-sample rules), gmm (its own weight matrix) and
// liml (cobraCmd liml) keep the refusal below.
bool postestIvRefitServedM4(const std::string& verb, const LastModel& lm) {
  if (lm.cobraCmd != "iv" || lm.stataCmd != "ivregress" || !lm.fe.empty()) return false;
  if (verb != "lincom" && verb != "test" && verb != "testparm" && verb != "nlcom") return false;
  return lm.result.has_value() && lm.result->is_object() &&
         lm.result->value("estimator", std::string()) == "iv2sls";
}

// Does the engine's post-estimation refit answer for the FITTED model?
// test/testparm/lincom/nlcom refit through lastModelBaseArgv, which carries the
// fit's weight (BUG-571) and its clustered small-sample convention
// (fitConventionFlagsM4: BUG-568 areg, FU-365 multiway) into cmds_postest.cpp
// postest_fit_spec; the §31 handle is minted by that same refit, so it carries
// them too. `margins, at()` (runMargins' own argv, cmd_margins' --at branches)
// carries only --multiway-subset-factors: it reads --weight only to mark it
// consumed and has no areg dof, so a weighted or areg-cluster fit still refuses
// there by name, as predict does for weights (BUG-519). Returns true when the
// refit answers for the fitted model; returns false and fills `reject` otherwise.
// A least-squares / logit / probit / poisson fit made WITHOUT a constant
// (`noconstant`, buildEstimationArgvM4's --no-intercept) leaves no intercept row
// in the cached coefficient table. LastModel records no noconstant flag, so this
// is the record: every engine post-estimation refit that would otherwise append
// the intercept (lastModelBaseArgv, runMargins, runPredict) forwards
// --no-intercept when it is true (FU-131), and the engine refits the fitted
// model (w2p_estim_ev.log G_nc_* / G_pnc_*). Before FU-131 those verbs refused
// here (BUG-603): the refit answered the WITH-constant model at rc 0
// (w2b_estim.log:732 probit_noconstant _b[x1] .4484790120538529 against :762
// probit_ctl 1.355178). Absorbed-FE fits are excluded: their tables carry no
// intercept row by design and have their own refit guards.
bool postestFitLacksConstantM4(const LastModel& lm) {
  if (!lm.fe.empty() || !lm.result.has_value()) return false;
  if (!postestGlmCmdsM4().count(lm.cobraCmd) && !postestOlsCmdsM4().count(lm.cobraCmd)) return false;
  const auto& r = *lm.result;
  const char* key = r.is_object() && r.contains("coefs") ? "coefs" : "coeftable";
  if (!r.is_object() || !r.contains(key) || !r[key].is_array() || r[key].empty()) {
    return false;
  }
  for (const auto& row : r[key]) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) return false;
    const std::string n = row["name"].get<std::string>();
    if (n == "(intercept)" || n == "_cons" || n == "cons") return false;
  }
  return true;
}

// The refusal for a no-constant fit whose engine refit cannot drop the constant:
// the poisson refit (cmds_postest.cpp refuses --no-intercept with --model
// poisson; PIET also refuses `poisson ..., noconstant` at the fit, so this is
// the defensive half). By name, rc 198, never the with-constant model's number.
ojson postestNoConstantRejectM4(const std::string& verb, const LastModel& lm) {
  return engineErrorBlockM4(
      verb + " after `" + lm.cobraCmd +
          " ..., noconstant` is not supported in this version of PIET -- the engine's "
          "post-estimation re-fit of this model always includes a constant, so it would "
          "answer for a different model than the one fitted. Refusing rather than reporting "
          "a number that is not the one asked for.",
      "unsupported_option");
}

}  // namespace

// Declared in estimation_m4_internal.h: boottest (estimation_m4b.cpp) asks too.
// FU-475's refit half. A fit whose regressors carry a time-series operator and
// whose `if` the engine applied as --keepif (runRegress's TS route, and the iv,
// xtreg, newey and prais routes, which always send --keepif) kept the rows
// whose lag reads a row outside the restriction (w2_esample.log:751-775 C6,
// e(N) 40). A refit that replays lm.fitKeepif as a PHYSICAL pre-filter loses
// those lags: a smaller sample at rc 0. Measured on main 2026-10-13 with the
// w2_esample.do data: `xtreg y L.x1 x2 if t>=3, fe` fits _b[L.x1] -.7953699
// (N 40) and `lincom L.x1` answered -.8123398. The engine refits that apply the
// restriction after the operators (plan_keepif) are served: test/lincom/nlcom
// (cmds_postest.cpp postest_refit), the linear margins refit (cmd_margins) and
// predict (cmd_predict --fit-keepif, with --fit-data once the head moved).
// Refused by name for the rest: boottest (applyIfAndInFilter), the ML refits,
// the native predict statistics and predict after absorbed effects.
std::optional<ojson> postestTsKeepifRefusalM4(const std::string& verb, const LastModel& lm) {
  if (lm.fitKeepif.empty()) return std::nullopt;
  std::vector<std::string> terms = lm.x;
  terms.insert(terms.end(), lm.fe.begin(), lm.fe.end());
  terms.push_back(lm.y);  // an operated depvar too (BUG-1277: `regress D.y x if t>=3`)
  if (!translate::hasTsToken(terms)) return std::nullopt;
  return engineErrorBlockM4(
      verb + " after a fit with time-series operators and an `if` restriction is not "
          "supported in this version of PIET -- the engine's post-estimation re-fit applies the "
          "restriction before the operators read the full series, so it would answer for a "
          "smaller sample than the one fitted. Refusing rather than reporting a number that is "
          "not the one asked for.",
      "unsupported_option");
}

namespace postest_detail {

// FU-131: true when `lm` is a no-constant fit the engine's refit cannot reproduce
// (only the poisson refit; feols/logit/probit take --no-intercept).
bool postestNoConstantUnrefittableM4(const LastModel& lm) {
  return lm.cobraCmd == "poisson" && postestFitLacksConstantM4(lm);
}

bool postestRefitMatchesFitM4(const std::string& verb, const LastModel& lm, ojson& reject) {
  // The verbs whose refit is lastModelBaseArgv's (weight + convention flags).
  const bool fullRefit = verb == "lincom" || verb == "test" || verb == "testparm" || verb == "nlcom";
  // FU-475: those four replay lm.fitKeepif as the engine's --keepif on the fit's
  // full frame, and cmds_postest.cpp's refit (postest_refit) applies it as a row
  // mask after the time-series operators, as the fit did -- served. So does the
  // linear refit of margins (cmd_margins' --at/--marginlist path, plan_keepif
  // likewise) after a least-squares fit. Every other verb's refit still filters
  // before the operators and refuses by name.
  const bool linearMargins =
      verb.rfind("margins", 0) == 0 && postestOlsCmdsM4().count(lm.cobraCmd) > 0;
  if (!fullRefit && !linearMargins) {
    if (auto refused = postestTsKeepifRefusalM4(verb, lm)) {
      reject = *refused;
      return false;
    }
  }
  //  - A weighted fit: measured on main 2026-09-28, `regress y x1 x2 [pweight=w]`
  //    (w2_pweight.do fixture) fits _b[x1] 2.38153191 and `lincom x1` answered
  //    2.48828492 -- the unweighted coefficient; `test x1` F 210.02 against the
  //    fit's (_b/_se)^2 135.33. Served since the refit carries the weight, for
  //    the least-squares family; still refused for `margins, at()` and for an
  //    estimator whose refit is not feols (the ML refit takes no weight).
  //  - A `svy:` fit (cached as a pweighted feols, runSvy): its linearized VCE
  //    has no refit at all, and the weight alone would answer under HC1.
  //  - areg with a cluster VCE: its SE charges an FE nested in the cluster
  //    (BUG-568). Served for the full refit (--keep-nested-fe-dof); `margins,
  //    at()` would apply reghdfe's exemption, so it refuses for any cluster
  //    (whether an FE is nested in the cluster is a property of the data).
  //  - regress with a two-way cluster VCE (BUG-850): the fit runs Stata's
  //    built-in multiway convention (--multiway-subset-factors, BUG-848;
  //    w2l_vce_shape_det.log:258 regress_vce_c2_se .0643525900770342).
  //    lastModelBaseArgv forwards the flag (fitConventionFlagsM4), so the full
  //    refit and `margins, at()` answer under it (FU-385; :263 lincom
  //    .0643525900770342) and nothing here refuses.
  // No-constant least-squares fits, including wildboot, carry --no-intercept.
  // The Poisson refit still has no constant-free implementation and refuses.
  if (postestNoConstantUnrefittableM4(lm)) {
    reject = postestNoConstantRejectM4(verb, lm);
    return false;
  }
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("svy")) {
    reject = engineErrorBlockM4(
        verb + " after `svy:` is not supported in this version of PIET -- the engine's "
            "post-estimation re-fit has no survey-design input, so it would answer under a "
            "different variance than the linearized one fitted. Refusing rather than reporting "
            "a number that is not the one asked for.",
        "unsupported_option");
    return false;
  }
  const bool weightedWildboot = verb == "boottest" && postestOlsCmdsM4().count(lm.cobraCmd) && lm.fe.empty();
  if (!lm.fitWeight.empty() && !weightedWildboot && (!fullRefit || !postestOlsCmdsM4().count(lm.cobraCmd))) {
    reject = engineErrorBlockM4(
        verb + " after a weighted fit ([" +
            (lm.fitWtype.empty() ? std::string("weight") : lm.fitWtype) + "=" + lm.fitWeight +
            "]) is not supported in this version of PIET -- the engine's post-estimation "
            "re-fit has no weight input, so it would answer for the unweighted model. "
            "Refusing rather than reporting a number that is not the one asked for.",
        "unsupported_option");
    return false;
  }
  // boottest is exempt (FU-732): its p is a ratio of t statistics that share one
  // scale, so the clustered dof never moves it, and runBoottest takes the t it
  // reports from the areg table's own standard error (boottestRefitMatchesFitM4).
  if (!fullRefit && verb != "boottest" && lm.stataCmd == "areg" &&
      (!lm.cluster.empty() || lm.vcov == "cluster" || lm.vcov == "twoway" ||
       lm.vcov == "multiway")) {
    reject = engineErrorBlockM4(
        verb + " after `areg ..., vce(cluster)` is not supported in this version of PIET -- "
            "the engine's post-estimation re-fit applies the reghdfe/xtreg,fe clustered "
            "degrees of freedom (an absorbed effect nested in the cluster is exempt), not "
            "areg's, so its standard errors would differ from the areg table. Refusing "
            "rather than reporting a number that is not the one asked for.",
        "unsupported_option");
    return false;
  }
  return true;
}

// Gate a post-estimation verb on the estimator that produced lastModel.
// Returns true when the call may proceed; returns false and fills `reject` with
// the honest-refusal block otherwise.
bool postestEstimatorOkM4(const std::string& verb, const LastModel& lm, ojson& reject) {
  if (!postestRefitMatchesFitM4(verb, lm, reject)) return false;
  if (postestOlsCmdsM4().count(lm.cobraCmd)) return true;
  if (postestIvRefitServedM4(verb, lm)) return true;
  if (postestGlmCmdsM4().count(lm.cobraCmd)) {
    // The engine's ML refit is the plain OIM fit: no absorbed effects, no
    // alternative VCE (exactly what cmd_margins' GLM branch supports). A
    // lastModel carrying either would be answered under a DIFFERENT
    // specification than it was fitted under — the same silent-wrong shape this
    // whole fix removes — so it refuses rather than forwarding a flag the
    // engine would have to ignore. cobra's own postest_fit_spec makes the
    // identical check, so a direct CLI caller cannot slip past this either.
    if (!lm.fe.empty() || !lm.cluster.empty() || (!lm.vcov.empty() && lm.vcov != "iid")) {
      reject = engineErrorBlockM4(
          verb + " is not supported after `" + lm.cobraCmd +
              "` with absorbed fixed effects or a non-default vce() in this version of PIET — "
              "the engine's maximum-likelihood post-estimation refit covers only the plain "
              "(OIM) fit. Refusing rather than answering under a specification the model was "
              "not fitted under.",
          "unsupported_option");
      return false;
    }
    return true;
  }
  reject = engineErrorBlockM4(
      verb + " is not supported after `" + lm.cobraCmd +
          "` in this version of PIET — the engine's post-estimation path re-fits the model as "
          "plain OLS from the stored dependent and independent variables, so the answer would "
          "be a least-squares one reported as if it came from the `" +
          lm.cobraCmd +
          "` fit. Refusing rather than reporting a number that is not the one asked for.",
      "unsupported_option");
  return false;
}

}  // namespace

// boottest refits the stored least-squares model: wildboot accepts the fit's
// no-intercept flag and unabsorbed aw/fw/iw/pw weights. Weighted absorbed fits
// remain named refusals; none is silently reduced to OLS. The IV WRE (ivregress
// 2sls/liml, one-absorb ivreghdfe) and the unweighted, unabsorbed multiway
// bootstrap do not come through this guard: runBoottest serves them with the
// engine's direct-refit kernel (wildboot --wre / --cluster a,b), and any other
// IV fit is still refused here.
// The helper's areg-with-cluster rule does NOT apply to boottest (FU-732):
// wildboot's t0/se come from cluster_ssf (src/inference/wildboot.cpp), the
// reghdfe/xtreg,fe convention that exempts an FE nested in the cluster, while
// areg's clustered table (BUG-568) does not -- but cluster_ssf is one constant
// for t0 and every bootstrap t, so the symmetric p does not depend on it
// (w2_esample_tsrefit.log:336 W1 p .3125, the engine's 128-draw p), and
// runBoottest reports areg's t from the fit's own standard error, not t0
// (E2_areg_id_st t 1.156417206472481 at :1535 is areg's, not reghdfe's
// 1.264649918737085 at :1668; W1_st :342). Absorbed-FE fits stay served:
// cluster_ssf is the same dof as the reghdfe/xtreg,fe fit they follow.
bool boottestFitLacksConstantM4(const LastModel& lm) { return postestFitLacksConstantM4(lm); }

bool boottestRefitMatchesFitM4(const LastModel& lm, ojson& reject) {
  if (!postestOlsCmdsM4().count(lm.cobraCmd)) {
    std::string kind = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
    if (lm.cobraCmd == "iv" || lm.cobraCmd == "liml") kind = "ivregress/ivreg2/ivreghdfe (" + kind + ")";
    reject = engineErrorBlockM4(
        "boottest after `" + kind +
            "` is not supported in this version of PIET -- the engine's wild bootstrap "
            "re-fits the model as unweighted least squares of the dependent variable on the "
            "stored regressors (with a constant), so after an instrumental-variables, "
            "maximum-likelihood or other non-least-squares fit it would bootstrap a "
            "different model than the one fitted. Refusing rather than reporting a t and p "
            "that are not the ones asked for.",
        "unsupported_option");
    return false;
  }
  return postestRefitMatchesFitM4("boottest", lm, reject);
}


}  // namespace piet::handlers
