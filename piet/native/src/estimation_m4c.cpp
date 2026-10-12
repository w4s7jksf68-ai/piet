// R2 (giant-file split, 2026-09-01): continuation TU of estimation_m4.cpp —
// a MOVE-ONLY cut at pre-existing section banners. Preamble = the original
// file head verbatim minus its option-vocab anon block (single registration
// stays in the first TU); shared helpers come from estimation_m4_internal.h.
// This TU carries -ffp-contract=off like its parent (CMake) — FMA fusion
// here would drift golden numerics.
// M4a estimation + diagnostic surface — C++ port of
// piet/backend/piet/router/translate.py's build_estimation_argv/
// build_csdid_argv/build_corr_argv/build_ttest_argv routing plus
// dispatcher.py's _cobra_to_estimation / _csdid_to_estimation / _run_predict
// / the test_cmd/lincom_cmd/nlcom_cmd/margins_cmd post-estimation handlers
// and the hettest/ovtest/vif "estat" diagnostic rewrite. See
// piet/native/include/piet/handlers.h for the per-handler oracle-source
// mapping (ROADMAP M4a item 2).
//
// Scope: xtreg (fe/re), logit, probit, csdid (+ estat simple/event/
// calendar/group/pretrend), tabstat, correlate/pwcorr/ttest, predict,
// test, lincom, nlcom, margins, estat hettest/ovtest/vif, ivregress/
// ivreghdfe/reg3/boottest (BETA D1), tobit/heckman/qreg/mediate (BETA D2,
// BS-069/070/071/072).
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.

#include "estimation_m4_internal.h"

// R2: the option-vocab bindings, restored VERBATIM — registering the same
// token twice UNIONS the sets (errors.h, deliberate), and a set unioned with
// itself is itself, so the single-registry law holds across the three TUs.
namespace {
const piet::eval::OptVocab& kVocabTtest = piet::eval::registerOptVocab(
    {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse", "unpaired"},
     {}});
const piet::eval::OptVocab& kVocabCsdid = piet::eval::registerOptVocab(
    {"csdid"}, {{"ivar", "time", "gvar", "method", "comparison", "pscoretrim", "window", "cluster", "long2"},
     {}});
const piet::eval::OptVocab& kVocabReg3 = piet::eval::registerOptVocab(
    {"reg3"}, {{"sure", "3sls"},
     {}});
const piet::eval::OptVocab& kVocabBoottest = piet::eval::registerOptVocab(
    {"boottest"}, {{"reps", "seed", "weights", "null",
                                                          "cluster"},
     {}});
const piet::eval::OptVocab& kVocabTobit = piet::eval::registerOptVocab(
    {"tobit"}, {{"ll", "ul", "level", "vce", "robust"},
     {}});
const piet::eval::OptVocab& kVocabHeckman = piet::eval::registerOptVocab(
    {"heckman"}, {{"select", "twostep", "level"},
     {}});
const piet::eval::OptVocab& kVocabQreg = piet::eval::registerOptVocab(
    {"qreg"}, {{"quantile", "level", "vce"},
     {}});
const piet::eval::OptVocab& kVocabMediate = piet::eval::registerOptVocab(
    {"mediate"}, {{"reps", "seed", "level"},
     {}});
// BUG-263: mediate's treatment sub-block `(tvar, ...)` is a SECOND
// eval::parseOpts vector built inside runMediate itself (BUG-262's class), so
// the dispatcher's resolution point can never see its keys — the handler
// canonicalizes and rejects locally against this entry. Registered under its
// own token, NOT under "mediate": registerOptVocab UNIONs per token and
// kVocabMediate above binds the "mediate" slot by reference, so adding
// `continuous` there would make the TOP-LEVEL net accept-and-silently-ignore
// a trailing `, continuous(...)` — BUG-231's exact shape. The sub-block
// grammar is closed at exactly one option ([CAUSAL] mediate, Syntax:
// `(tvar [, continuous(numlist)])`), so the unimplemented set is empty by
// measurement, not omission; the "real Stata shape PIET doesn't implement"
// honesty lives in the handler's own numlist-arity message (a >2-value
// continuous() numlist is real Stata, and gets the two-level-only reject).
const piet::eval::OptVocab& kVocabMediateTreat = piet::eval::registerOptVocab(
    {"mediate:tvar"}, {{"continuous"},
     {}});
const piet::eval::OptVocab& kVocabDrdid = piet::eval::registerOptVocab(
    {"drdid"}, {{"ivar", "time",     "treatment", "dripw",
                                                          "reg",  "ipw",      "std_ipw",   "trad"},
     {}});
// Item 4 (User ruling 2026-09-09, BUG-467's sibling surface): `post` REPLACES
// e() with the margin (Cells `B4REC|margins|dydx_post|0|`,
// `B4VAL|ecmd|margins|b_mpg|-1.8c19072612ae4X+005`, oracle_r.log). `margins`
// is in kOptMinComplete (opt_minimums_bug264.inc), so the key arrives
// exactOnly -- adding it to `known` accepts ONLY the exact spelling `post`;
// no abbreviation surface widens. THREE byte-identical copies (this file,
// estimation_m4.cpp, estimation_m4b.cpp) -- registerOptVocab unions
// registrations for the same command, so all three must move together or
// two would silently drift (IDIOMS Duplication).
const piet::eval::OptVocab& kVocabMargins = piet::eval::registerOptVocab(
    {"margins"}, {{"at", "dydx", "post"},
     {}});
const piet::eval::OptVocab& kVocabEstatDurbinalt = piet::eval::registerOptVocab(
    {"estat"}, {{"small", "lags"},
     {}});
const piet::eval::OptVocab& kVocabEstatEvent = piet::eval::registerOptVocab(
    {"estat"}, {{"window", "post"},
     {}});
const piet::eval::OptVocab& kVocabNewey = piet::eval::registerOptVocab(
    {"newey"}, {{"lag"},
     {}});
const piet::eval::OptVocab& kVocabPrais = piet::eval::registerOptVocab(
    {"prais"}, {{"corc", "twostep", "rhotype"},
     {}});
const piet::eval::OptVocab& kVocabWntestq = piet::eval::registerOptVocab(
    {"wntestq"}, {{"lags"},
     {}});
const piet::eval::OptVocab& kVocabSdtest = piet::eval::registerOptVocab(
    {"sdtest"}, {{"by", "level"},
     {}});
const piet::eval::OptVocab& kVocabHausman = piet::eval::registerOptVocab(
    {"hausman"}, {{},
     {"constant", "sigmamore", "sigmaless", "force", "alleqs",
      "skipeqs",  "equations", "df",        "tconsistent", "tefficient"}});
const piet::eval::OptVocab& kVocabChow = piet::eval::registerOptVocab(
    {"chow"}, {{"by"},
     {}});
const piet::eval::OptVocab& kVocabMkspline = piet::eval::registerOptVocab(
    {"mkspline"}, {{"cubic", "nknots", "knots",
                                                          "displayknots"},
     {}});
const piet::eval::OptVocab& kVocabIvRegress = piet::eval::registerOptVocab(
    {"ivregress", "ivreg", "ivreghdfe", "reghdfe"}, {{"vce", "vcov", "robust", "cluster",
      "gmm",
      "absorb", "keepsingletons", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabPoisson = piet::eval::registerOptVocab(
    {"poisson", "ppmlhdfe", "fepois"}, {{"vce", "vcov", "robust", "cluster", "absorb",
      "offset", "exposure", "noconstant", "nocons", "level"},
     {}});
const piet::eval::OptVocab& kVocabNbreg = piet::eval::registerOptVocab(
    {"nbreg"}, {{"vce", "vcov", "robust", "cluster", "noconstant",
      "nocons", "offset", "exposure", "dispersion", "level"},
     {}});
// The two conditionally-built nets: the base literal lives here (one copy), and
// the registered vocabulary is base + the command-conditional key, so the
// dispatcher can canonicalize `offs`/`dri` while the handler's own net keeps
// deciding whether the canonical spelling is allowed for the command at hand.
const std::unordered_set<std::string> kXtPoissonLogitBase = {
    "fe", "re", "pa", "vce", "vcov", "robust", "cluster", "noconstant", "nocons", "level"};
const piet::eval::OptVocab& kVocabXtPoissonLogit = piet::eval::registerOptVocab(
    {"xtpoisson", "xtlogit"},
    {[] { auto s = kXtPoissonLogitBase; s.insert("offset"); return s; }(), {}});
const std::unordered_set<std::string> kDfullerPperronBase = {
    "lags", "noconstant", "nocons", "trend"};
const piet::eval::OptVocab& kVocabDfullerPperron = piet::eval::registerOptVocab(
    {"dfuller", "pperron"},
    {[] { auto s = kDfullerPperronBase; s.insert("drift"); return s; }(), {}});
}  // namespace

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;

// ===========================================================================
// test / lincom / nlcom (post-estimation, session.lastModel()-driven)

namespace {

ojson noPreviousEstimation() {
  return engineErrorBlockM4("No previous estimation result. Run a regression first.");
}

std::vector<std::string> lastModelBaseArgv(const std::string& cobraCmd, const LastModel& lm) {
  // W:P5 fit-sample fidelity (BUG-163): refit from the sample the model was
  // ACTUALLY fitted on. fitDataPath is runRegress's materialized if/in scratch
  // file; fitKeepif replays a --keepif that went straight to the engine. Both
  // empty => byte-identical to the historical full-dataPath call.
  std::vector<std::string> args = {cobraCmd, "--data",
                                    lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath,
                                    "--y", lm.y, "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x)};
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
const std::set<std::string>& postestGlmCmdsM4() {
  static const std::set<std::string> kPostestGlmCmds = {"logit", "probit", "poisson"};
  return kPostestGlmCmds;
}

// The `--model` value to forward for this lastModel, or empty for the
// least-squares family (which forwards nothing and keeps its historical argv
// byte-for-byte). Callers must first have passed postestEstimatorOkM4.
std::string postestModelFlagM4(const LastModel& lm) {
  return postestGlmCmdsM4().count(lm.cobraCmd) ? lm.cobraCmd : std::string();
}

// The engine's post-estimation refits -- test/lincom/nlcom (built by
// lastModelBaseArgv) and `margins, at()` (runMargins' own argv) -- take
// neither a weight nor areg's clustered dof (options_table.cpp rows;
// cmds_postest.cpp postest_fit_spec fits build_model + feols_fit with neither,
// and cmd_margins' --at branches read --weight only to mark it consumed). Two
// fitted models therefore refit as a DIFFERENT model at rc 0 -- the §31 handle
// path included, since its handle is minted by that same refit. Both refuse by
// name until the engine's post-estimation refit carries them, as predict
// already does for weights (BUG-519). Returns true when the refit answers for
// the fitted model; returns false and fills `reject` otherwise.
// A least-squares / logit / probit / poisson fit made WITHOUT a constant
// (`noconstant`, buildEstimationArgvM4's --no-intercept) leaves no intercept row
// in the cached coefficient table. LastModel records no noconstant flag, and
// every engine post-estimation refit (lastModelBaseArgv, runMargins, runPredict:
// load_glm / feols_fit) appends the intercept, so test/lincom/nlcom/predict/
// margins would answer from the WITH-constant model at rc 0 (w2b_estim.log:732
// probit_noconstant _b[x1] .4484790120538529 against :762 probit_ctl 1.355178).
// Absorbed-FE fits are excluded: their tables carry no intercept row by design
// and have their own refit guards.
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

ojson postestNoConstantRejectM4(const std::string& verb, const LastModel& lm) {
  return engineErrorBlockM4(
      verb + " after `" + lm.cobraCmd +
          " ..., noconstant` is not supported in this version of PIET -- the engine's "
          "post-estimation re-fit always includes a constant, so it would answer for a "
          "different model than the one fitted. Refusing rather than reporting a number "
          "that is not the one asked for.",
      "unsupported_option");
}

bool postestRefitMatchesFitM4(const std::string& verb, const LastModel& lm, ojson& reject) {
  //  - A weighted fit: measured on main 2026-09-28, `regress y x1 x2 [pweight=w]`
  //    (w2_pweight.do fixture) fits _b[x1] 2.38153191 and `lincom x1` answered
  //    2.48828492 -- the unweighted coefficient; `test x1` F 210.02 against the
  //    fit's (_b/_se)^2 135.33.
  //  - areg with a cluster VCE: its SE charges an FE nested in the cluster
  //    (BUG-568, w2b_estim.log areg_a_cl se .0722686736) and the refit applies
  //    the reghdfe/xtreg,fe exemption (.0663182832), so `test`/`lincom` would
  //    disagree with the table they follow. Keyed on lm.stataCmd (the only
  //    record of the areg word on LastModel); refused for any cluster, because
  //    whether an FE is nested in the cluster is a property of the data, not of
  //    the names.
  if (postestFitLacksConstantM4(lm)) {
    reject = postestNoConstantRejectM4(verb, lm);
    return false;
  }
  if (!lm.fitWeight.empty()) {
    reject = engineErrorBlockM4(
        verb + " after a weighted fit ([" +
            (lm.fitWtype.empty() ? std::string("weight") : lm.fitWtype) + "=" + lm.fitWeight +
            "]) is not supported in this version of PIET -- the engine's post-estimation "
            "re-fit has no weight input, so it would answer for the unweighted model. "
            "Refusing rather than reporting a number that is not the one asked for.",
        "unsupported_option");
    return false;
  }
  if (lm.stataCmd == "areg" &&
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

namespace {

// Stata-style glob for a coefficient NAME: `*` any run, `?` one char. Iterative
// with a backtrack cursor (no recursion, so a pathological pattern cannot blow
// the stack the way a naive `*` recursion does).
bool globMatchM4(const std::string& pat, const std::string& str) {
  size_t p = 0, s = 0, star = std::string::npos, mark = 0;
  while (s < str.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
      ++p;
      ++s;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = s;
    } else if (star != std::string::npos) {
      p = star + 1;
      s = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

// Does coefficient `name` belong to variable `var`? Two shapes, because that is
// how our coeftable actually names things (measured, not assumed): a continuous
// regressor is its own name (`beertax`), and a factor level is `<level>.<var>`
// (`1983.dyear`) — the same spelling Stata uses.
bool coefBelongsToVarM4(const std::string& name, const std::string& var) {
  if (name == var) return true;
  if (name.size() <= var.size() + 1) return false;
  if (name.compare(name.size() - var.size(), var.size(), var) != 0) return false;
  if (name[name.size() - var.size() - 1] != '.') return false;
  // Everything before the dot must be the level value (digits, or a negative /
  // decimal level), never another variable's name.
  for (size_t k = 0; k + var.size() + 1 < name.size(); ++k) {
    char c = name[k];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.') return false;
  }
  return true;
}

// The VARIABLE a coefficient came from: `beertax` -> "beertax",
// `1983.dyear` -> "dyear". Stata's `testparm x*` globs VARIABLE names and then
// tests every coefficient those variables produced, so the glob has to be
// matched here and not against the printed coefficient label.
std::string coefSourceVarM4(const std::string& name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return name;
  for (size_t k = 0; k < dot; ++k) {
    char c = name[k];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.') return name;
  }
  return name.substr(dot + 1);
}

bool isInterceptNameM4(const std::string& n) {
  return n == "(intercept)" || n == "_cons" || n == "cons";
}

}  // namespace

ojson runTestparm(const std::vector<std::string>& varlist, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  {  // BUG-203 — the same estimator gate `test` carries; testparm IS `test`.
    ojson reject;
    if (!postestEstimatorOkM4("testparm", lm, reject)) return reject;
  }
  if (varlist.empty())
    return engineErrorBlockM4("testparm: varlist required", "syntax_error");

  // Coefficient names come from the stored fit, in coeftable order, so the
  // constraint list reads in the order the user saw the model printed.
  std::vector<std::string> coefs;
  if (lm.result.has_value() && lm.result->contains("coeftable") &&
      (*lm.result)["coeftable"].is_array()) {
    for (const auto& row : (*lm.result)["coeftable"]) {
      std::string nm = row.value("name", std::string());
      if (!nm.empty()) coefs.push_back(nm);
    }
  }
  if (coefs.empty())
    return engineErrorBlockM4(
        "testparm: the last estimation exposes no coefficient names to expand against",
        "unimplemented");

  std::vector<std::string> picked;
  auto addOnce = [&picked](const std::string& n) {
    for (const auto& q : picked)
      if (q == n) return;
    picked.push_back(n);
  };

  for (const std::string& rawTok : varlist) {
    std::string tok = trimM4(rawTok);
    if (tok.empty()) continue;
    // `i.`/`c.`/`ib#.`/`o.` prefixes name the SOURCE variable; the coefficients
    // they produce are what gets tested, so strip the prefix and match the rest.
    std::string bare = tok;
    for (const char* pfx : {"i.", "c.", "o.", "ibn."}) {
      size_t plen = std::strlen(pfx);
      if (bare.size() > plen && bare.compare(0, plen, pfx) == 0) {
        bare = bare.substr(plen);
        break;
      }
    }
    const bool isGlob = bare.find('*') != std::string::npos || bare.find('?') != std::string::npos;
    size_t before = picked.size();
    for (const std::string& c : coefs) {
      // The intercept is never swept in by a pattern — Stata's testparm tests
      // the named parameters, and `_cons` has to be asked for by name.
      if (isGlob) {
        // Match the source variable (`dyear*` -> every `NNNN.dyear`), and also the
        // printed name so an explicit pattern like `198*.dyear` still works.
        if (!isInterceptNameM4(c) &&
            (globMatchM4(bare, coefSourceVarM4(c)) || globMatchM4(bare, c)))
          addOnce(c);
      } else if (coefBelongsToVarM4(c, bare)) {
        addOnce(c);
      }
    }
    if (picked.size() == before)
      return engineErrorBlockM4(
          "testparm: `" + tok + "' matches no coefficient in the last estimation",
          "bad_input");
  }

  // Delegate. Everything above is name resolution; the arithmetic, the vcov, the
  // r-class fill and the estimator gate all stay in the one place that owns them.
  std::string joined;
  for (const std::string& c : picked) {
    if (!joined.empty()) joined += ' ';
    joined += c;
  }
  return runTestCmd(joined, session);
}

ojson runTestCmd(const std::string& exprIn, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  // BUG-203: refuse before any parsing — the OLS-refit answer is not this
  // model's answer for any estimator outside kPostestOlsCmds.
  {
    ojson reject;
    if (!postestEstimatorOkM4("test", lm, reject)) return reject;
  }

  std::string expr = trimM4(exprIn);
  // Combination-only flags. On §31's handle path the stored fit IS the model,
  // so nothing accumulated here may name it; runPostEstimationM4 prepends
  // lastModelBaseArgv only when it falls back to the full specification.
  std::vector<std::string> combo;

  // Strip parens: "(mpg) (weight)" -> "mpg weight".
  std::string flat;
  for (char c : expr) {
    if (c == '(' || c == ')') {
      flat += ' ';
    } else {
      flat += c;
    }
  }
  flat = trimM4(flat);
  // Collapse whitespace around =+-*/ operators (BUG-015).
  {
    std::string collapsed;
    for (size_t i = 0; i < flat.size(); ++i) {
      char c = flat[i];
      if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/') {
        while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
        collapsed += c;
        while (i + 1 < flat.size() && flat[i + 1] == ' ') ++i;
      } else {
        collapsed += c;
      }
    }
    flat = collapsed;
  }
  // Split on commas/spaces.
  std::vector<std::string> tokens;
  {
    std::string cur;
    for (char c : flat) {
      if (c == ',' || c == ' ' || c == '\t') {
        if (!cur.empty()) tokens.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) tokens.push_back(cur);
  }

  auto isJointToken = [](const std::string& tok) -> bool {
    size_t eq = tok.find('=');
    if (eq == std::string::npos) return true;
    std::string rhs = trimM4(tok.substr(eq + 1));
    try {
      size_t pos;
      double v = std::stod(rhs, &pos);
      return pos == rhs.size() && v == 0.0;
    } catch (...) {
      return false;
    }
  };
  auto jointName = [](const std::string& tok) -> std::string {
    size_t eq = tok.find('=');
    if (eq == std::string::npos) return tok;
    return trimM4(tok.substr(0, eq));
  };

  bool hasEq = expr.find('=') != std::string::npos;
  if (hasEq) {
    std::vector<std::string> constraints, jointNames;
    for (const auto& tok : tokens) {
      if (isJointToken(tok)) {
        jointNames.push_back(jointName(tok));
      } else {
        constraints.push_back(tok);
      }
    }
    if (!constraints.empty() && jointNames.empty()) {
      for (const auto& c : constraints) {
        combo.push_back("--constraint");
        combo.push_back(c);
      }
    } else if (!jointNames.empty() && constraints.empty()) {
      combo.push_back("--joint");
      combo.push_back(joinCommaM4(jointNames));
    } else {
      if (!jointNames.empty()) {
        combo.push_back("--joint");
        combo.push_back(joinCommaM4(jointNames));
      }
      for (const auto& c : constraints) {
        combo.push_back("--constraint");
        combo.push_back(c);
      }
    }
  } else {
    if (!tokens.empty()) {
      combo.push_back("--joint");
      combo.push_back(joinCommaM4(tokens));
    } else {
      combo.push_back("--joint");
      combo.push_back(expr);
    }
  }

  combo.push_back("--tidy");
  ojson raw;
  try {
    raw = runPostEstimationM4("test", combo, session, lm);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  return cobraToEstimationM4(raw, "test", &session);
}

// BUG-333: lincom/nlcom now take the parsed option vector. `level(#)` forwards
// to cobra's new --level (engine half landed this wave); lincom's eform family
// (eform/or/hr/irr/rrr/shr) is a DISPLAY metric applied to the returned block,
// exactly as `logit, or` is — Stata computes the linear combination and its CI
// in the linear metric and exponentiates for display, which is why the SE uses
// the delta factor exp(b) and t/p are left alone.
namespace {
const char* eformLabelM4(const std::string& k) {
  if (k == "or") return "odds_ratio";
  if (k == "hr") return "hazard_ratio";
  if (k == "irr") return "incidence_rate_ratio";
  if (k == "rrr") return "relative_risk_ratio";
  if (k == "shr") return "subhazard_ratio";
  return "exp_b";
}
void applyEformM4(ojson& block, const char* metric) {
  if (block.value("type", "") == "error") return;
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  for (auto& row : block["coeftable"]) {
    if (!row.contains("b") || row["b"].is_null()) continue;
    const double e = std::exp(row["b"].get<double>());
    row["b"] = e;
    if (row.contains("se") && row["se"].is_number()) row["se"] = e * row["se"].get<double>();
    if (row.contains("ci_lo") && row["ci_lo"].is_number())
      row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
    if (row.contains("ci_hi") && row["ci_hi"].is_number())
      row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
  }
  block["coef_metric"] = metric;
}

// Items 4/5 (User ruling 2026-09-09): `margins ..., post` and
// `nlcom ..., post` REPLACE the posted estimation results wholesale --
// e(cmd) becomes "margins"/"nlcom" and _b[x] becomes the margin/combination
// afterwards. Same shape as eststo_esttab.cpp's runEstimatesUse (the
// canonical "replace e() wholesale" precedent) and the SAME order its own
// comment insists on: setLastModel() (republishes e(cmd)/e(depvar)/e(vce)
// and re-points eval.lastModelResult) BEFORE lastEstBlock, then
// estAddedClear() (the estadd bag belongs to the estimate being replaced).
// y/x/fe are left EMPTY on the new LastModel — postestEstimatorOkM4 (below)
// refuses every later lincom/test/predict verb BY NAME for a cobraCmd it
// does not recognize (neither postestOlsCmdsM4() nor postestGlmCmdsM4()
// list "margins"/"nlcom"), which turns a post-post replay into an honest
// named refusal instead of a silent wrong number computed from stale
// y/x/fe — leaving them stale would be exactly the silent-wrong shape this
// codebase's Doctrine gate ① forbids.
//
// KNOWN OPEN GAP (BUG-467, filed 2026-09-09, OPEN): setLastModel does NOT
// clear session.eval.eScalars/eStrings, so an e-scalar/e-string posted by
// an EARLIER model (oaxaca/pca/teffects/estadd) still answers after this
// publish. This helper does not fix that — it is a NEW CALLER of
// setLastModel, one more surface the open bug reaches, not a new instance
// of the bug itself.
void postEstimationBlockM4(SessionState& session, const ojson& block, const std::string& cmdName,
                           const std::string& depvar, const std::string& dataPath) {
  LastModel lm2;
  lm2.cobraCmd = cmdName;
  lm2.stataCmd = cmdName;
  lm2.depvar = depvar;
  lm2.dataPath = dataPath;
  lm2.result = block;
  session.setLastModel(lm2);
  session.lastEstBlock = block;
  session.estAddedClear();
}

// AER-B4 margins-dydx-OLS (2026-09-09) — `margins, dydx(x)` AFTER A PLAIN OLS FIT.
//
// Oracle: tests/eval/golden/aer_b4/milfpins_oracle_r.do:75-79 (log lines
// 199-224) fits `quietly regress price mpg weight` and asks for
// `margins, dydx(mpg) post`. Stata answers rc 0 with
//   dy/dx -49.51222  std. err. 86.15604  t -0.57  P>|t| 0.567
//   [95% conf. interval] -221.3025 .. 122.278   Number of obs = 74
// and every one of those columns is IDENTICAL to the `mpg` row of that
// regression's own coefficient table (measured on this build:
// b -49.51222066635501, se 86.1560388671846, t -0.5746807921692114,
// p 0.5673237265003643, ci -221.302482965428 .. 122.27804163271796). That is
// not a coincidence: a linear model's dy/dx IS its coefficient — no sample
// average enters the derivative, so the AME does not depend on the data at
// all, and the delta-method variance of a single coefficient is that
// coefficient's own variance. Stata's header says so too ("Model VCE: OLS",
// "Expression: Linear prediction, predict()"), and the t/CI it prints are the
// fit's own t-distribution ones, NOT the normal-approximation pair
// cobraToEstimationM4's BUG-006 AME remap builds for the GLM family.
//
// Why it is NOT enough to drop feols/regress from runMargins' kLinearCmds:
// cmd_margins (apps/cmds/cmds_postest.cpp:929) reads
// `std::string model = opt(a, "--model", "logit");` and its AME path computes
// the logit/probit/poisson family only — with no --model flag a continuous
// outcome like `price` would be pushed through a LOGISTIC AME, a silent wrong
// number strictly worse than the honest reject it replaced (Doctrine gate ①).
// So the answer is built HERE, from the fitted model's own already-published
// coefficient table (LastModel::result — the same payload macros.cpp's
// _b[]/_se[] resolver reads), and the engine is not called at all.
//
// Two shapes stay REFUSED, because for them the coefficient is NOT the
// marginal effect:
//   * an INTERACTED regressor. Measured on this build, `regress price
//     c.mpg##c.mpg weight` names its rows "(intercept)", "mpg", "mpg#mpg",
//     "weight", and `regress price c.mpg#c.weight mpg weight` names one
//     "mpg#weight". Wherever the requested variable appears inside a `#` term
//     the chain rule puts a sample average into dy/dx (AME = b_x +
//     b_{x#z}*mean(z)), so the bare coefficient is wrong. `dydx(*)` refuses
//     when ANY row is interacted, since it asks for every covariate.
//   * a requested name no coefficient belongs to — an empty margins table is
//     a silent no-op, so say which name found nothing.
// A FACTOR regressor IS answered: a `<level>.<var>` row carries the predicted
// difference between that level and the base level, which for a linear model
// is exactly that level's coefficient and its own SE. `<level>.<var>` is the
// spelling coefBelongsToVarM4 (above) and runMargins' own dydx row filter
// already key on, so one predicate covers both.
//
// Fills `out` with a payload shaped like cobra's own `margins --tidy` wire
// (estimator/n_obs/coeftable/coefs, no depvar — measured against the engine's
// GLM AME output) so the caller's shared tail — marginsplot caching, the dydx
// row filter, `post` — runs over it unchanged. Returns false and fills
// `reject` with the honest-refusal block instead when it cannot.
bool buildLinearAmeRawM4(const LastModel& lm, const std::vector<std::string>& dydxVars,
                         ojson& out, ojson& reject) {
  if (!lm.result.has_value() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array() || (*lm.result)["coeftable"].empty()) {
    reject = engineErrorBlockM4(
        "margins, dydx(...): the fitted model's coefficient table is not in memory, so its "
        "marginal effects cannot be read from it.",
        "unsupported_option");
    return false;
  }
  const ojson& ct = (*lm.result)["coeftable"];

  auto rowName = [](const ojson& row) {
    return (row.contains("name") && row["name"].is_string()) ? row["name"].get<std::string>()
                                                             : std::string();
  };
  // Does `var` appear as one component of some `#` interaction row?
  auto isInteracted = [&ct, &rowName](const std::string& var) {
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.find('#') == std::string::npos) continue;
      size_t start = 0;
      for (;;) {
        const size_t hash = name.find('#', start);
        const std::string part =
            (hash == std::string::npos) ? name.substr(start) : name.substr(start, hash - start);
        if (coefBelongsToVarM4(part, var)) return true;
        if (hash == std::string::npos) break;
        start = hash + 1;
      }
    }
    return false;
  };
  // `what` names the request (a variable, or "*" for every covariate); `who`
  // names what is actually interacted, so the `*` form reads as a sentence.
  auto interactionReject = [](const std::string& what, const std::string& who) {
    return engineErrorBlockM4(
        "margins, dydx(" + what + ") after a linear model is not supported while " + who +
            " enters an interaction: the marginal effect is then the coefficient plus the "
            "interacted terms weighted by the sample, not the coefficient itself. Fit without "
            "the interaction, or use `margins, at(...)` at the point of interest.",
        "unsupported_option");
  };

  ojson coeftable = ojson::array();
  ojson coefs = ojson::array();
  auto take = [&coeftable, &coefs, &rowName](const ojson& row) {
    coeftable.push_back(row);
    ojson c;
    c["name"] = rowName(row);
    // Mirrors cobra's own AME wire shape, which marginsplot (graph_m4.cpp)
    // reads by `dydx`/`se` — the coeftable copy above is what the block and
    // `post` read.
    c["dydx"] = row.contains("b") ? row["b"] : ojson(nullptr);
    c["se"] = row.contains("se") ? row["se"] : ojson(nullptr);
    c["t"] = row.contains("t") ? row["t"] : ojson(nullptr);
    coefs.push_back(c);
  };

  if (dydxVars.empty()) {
    // `dydx(*)` (and `dydx()` with only a `*` token) — every covariate, which
    // is also what Stata prints, and never the intercept.
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.empty() || isInterceptNameM4(name)) continue;
      if (name.find('#') != std::string::npos) {
        reject = interactionReject("*", "`" + name + "`");
        return false;
      }
      take(row);
    }
  } else {
    for (const auto& var : dydxVars) {
      if (isInteracted(var)) {
        reject = interactionReject(var, var);
        return false;
      }
    }
    // Iterated over the MODEL's row order (not the request's), matching the
    // order the engine's GLM AME path returns its rows in.
    std::vector<bool> matched(dydxVars.size(), false);
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.empty() || isInterceptNameM4(name)) continue;
      for (size_t i = 0; i < dydxVars.size(); ++i) {
        if (!coefBelongsToVarM4(name, dydxVars[i])) continue;
        matched[i] = true;
        take(row);
        break;
      }
    }
    for (size_t i = 0; i < dydxVars.size(); ++i) {
      if (!matched[i]) {
        reject = engineErrorBlockM4(
            "margins, dydx(" + dydxVars[i] + "): `" + dydxVars[i] +
                "` is not a covariate of the last estimation.",
            "unsupported_option");
        return false;
      }
    }
  }
  if (coeftable.empty()) {
    reject = engineErrorBlockM4(
        "margins, dydx(...): the last estimation has no covariate to take a marginal effect of.",
        "unsupported_option");
    return false;
  }

  out = ojson::object();
  out["estimator"] = "margins";
  if (lm.result->contains("n_obs")) out["n_obs"] = (*lm.result)["n_obs"];
  out["coeftable"] = coeftable;
  out["coefs"] = coefs;
  return true;
}
}  // namespace

ojson runLincom(const std::string& expr, SessionState& session,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  {  // BUG-203 — see postestEstimatorOkM4.
    ojson reject;
    if (!postestEstimatorOkM4("lincom", lm, reject)) return reject;
  }
  ojson raw;
  try {
    std::vector<std::string> argv = {"--exp", expr, "--tidy"};
    for (const auto& [k, v] : opts)
      if (k == "level" && !v.isBool) { argv.push_back("--level"); argv.push_back(v.strValue); break; }
    raw = runPostEstimationM4("lincom", argv, session, lm);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson blk = cobraToEstimationM4(raw, "lincom", &session);
  // Two eform-family options at once is r(198) in Stata (measured: `lincom unemp,
  // or hr` -> 198, while each alone is rc 0 — including after `regress`, where the
  // exponentiated OLS coefficient is meaningless but Stata still prints it, so we
  // do not second-guess the estimator either).
  std::string metricKey;
  int eformCount = 0;
  for (const auto& [k, v] : opts)
    if ((k == "eform" || k == "or" || k == "hr" || k == "irr" || k == "rrr" || k == "shr") &&
        v.isBool && v.boolValue) {
      ++eformCount;
      if (metricKey.empty()) metricKey = k;
    }
  if (eformCount > 1)
    return engineErrorBlockM4("option `" + metricKey + "` may not be combined with another "
                              "eform option", "unsupported_option");
  if (eformCount == 1) applyEformM4(blk, eformLabelM4(metricKey));
  return blk;
}

ojson runNlcom(const std::string& exprIn, SessionState& session,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  {  // BUG-203 — see postestEstimatorOkM4.
    ojson reject;
    if (!postestEstimatorOkM4("nlcom", lm, reject)) return reject;
  }

  std::string raw = trimM4(exprIn);
  // Item 5 defect (a), User ruling 2026-09-09: Stata's `nlcom (name: expr)`
  // wraps the whole label:expr pair in ONE balanced paren pair — `(` is not
  // `\w`, so the label regex below never matched it and the entire
  // parenthesised string shipped as the expression body (measured: `nlcom
  // (r: _b[mpg]/_b[weight])` -> rc 1, "bare identifier 'r'"). Strip exactly
  // one OUTER balanced pair, and ONLY when it wraps the WHOLE string — a
  // leading paren that is really expression grouping (`nlcom
  // (_b[x1]+_b[x2])/2`) does not end the string in `)`, so it is left
  // alone by construction; a leading paren that DOES wrap the whole string
  // with no label (`nlcom (_b[x1]+_b[x2])`) is a no-op strip (pure
  // grouping, same value either way). freeze/nlcom.json's bare, unlabelled
  // `nlcom _b[x1]/_b[x2]` has no leading paren at all and is untouched.
  if (raw.size() >= 2 && raw.front() == '(' && raw.back() == ')') {
    int depth = 0;
    bool wholeStringWrap = true;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '(') {
        ++depth;
      } else if (raw[i] == ')') {
        --depth;
        if (depth == 0 && i + 1 != raw.size()) {
          wholeStringWrap = false;
          break;
        }
      }
    }
    if (wholeStringWrap && depth == 0) raw = trimM4(raw.substr(1, raw.size() - 2));
  }
  std::string label = "_nl_1";
  std::string body = raw;
  std::smatch m;
  std::regex lblRe(R"(^(\w+)\s*:\s*(.+)$)");
  if (std::regex_match(raw, m, lblRe)) {
    label = m[1].str();
    body = trimM4(m[2].str());
  }

  // Item 5 defect (b), User ruling 2026-09-09: `post` moved from
  // `unimplemented` to `known` in kVocabNlcom (dispatcher.cpp) — read it
  // here the same way `level` is read just below.
  bool postOpt = false;
  for (const auto& [k, v] : opts) {
    if (k == "post" && (!v.isBool || v.boolValue)) postOpt = true;
  }

  // BS-098/BS-013 (BETA A4): shares lastModelBaseArgv with test/lincom
  // (above, now via runPostEstimationM4's fallback) — nlcom ALSO inherits
  // vcov/cluster, not just fe (previously this hand-rolled the same
  // {data,y,x,fe} skeleton WITHOUT vcov/cluster, duplicating and diverging
  // from lastModelBaseArgv). On the §31 handle path the same guarantee holds
  // structurally instead: the stored model carries its vcov as fitted, and a
  // postest passes no --vcov at all, so there is no default to fall back to.
  ojson rawJson;
  try {
    std::vector<std::string> nargv = {"--expr", label + "=" + body, "--tidy"};
    for (const auto& [k, v] : opts)
      if (k == "level" && !v.isBool) { nargv.push_back("--level"); nargv.push_back(v.strValue); break; }
    rawJson = runPostEstimationM4("nlcom", nargv, session, lm);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson blk = cobraToEstimationM4(rawJson, "nlcom", &session);
  // dispatcher.cpp's captureRFromBlock (Branch 2) still runs on the RETURNED
  // block regardless of postOpt — Stata's nlcom is BOTH r-class (always)
  // and, under `post`, ALSO e-class. This only adds the e() publish.
  if (postOpt) {
    postEstimationBlockM4(session, blk, "nlcom", lm.depvar, lm.dataPath);
  }
  return blk;
}

// ===========================================================================
// margins

ojson runMargins(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;

  // BUG-163: margins computes over e(sample) (oracle: N=4,282 after a fit
  // with `if t>=3`), so replay the fitted sample exactly as
  // lastModelBaseArgv does. cmd_margins consumes --keepif (cmds_postest).
  std::vector<std::string> args = {"margins", "--data",
                                    lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath,
                                    "--y", lm.y, "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x)};
  if (!lm.fitKeepif.empty()) {
    args.push_back("--keepif");
    args.push_back(lm.fitKeepif);
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }

  std::optional<std::string> atVal;
  std::optional<std::string> dydxVal;
  bool postOpt = false;
  for (const auto& [k, v] : opts) {
    if (k == "at" && !v.isBool) atVal = v.strValue;
    if (k == "dydx" && !v.isBool) dydxVal = v.strValue;
    // Item 4 (User ruling 2026-09-09): `post` REPLACES e() with the margin
    // (Cells `B4REC|margins|dydx_post|0|`,
    // `B4VAL|ecmd|margins|b_mpg|-1.8c19072612ae4X+005`). Read here, applied
    // at both success return points below via postEstimationBlockM4.
    if (k == "post" && (!v.isBool || v.boolValue)) postOpt = true;
  }

  // BS-097: `at`/`dydx`/`post` are the ONLY margins options this handler
  // reads (B6 audit: margins had ZERO unknown-option net; `post` joined
  // 2026-09-09). --fe/--vcov/--cluster (BS-086, BETA D5 batch) are
  // forwarded from session.lastModel() itself, not from a new opts-derived
  // key here, so they don't collide with this net. Any other leftover key
  // (e.g. `level()`, or a typo) reads as an honest reject instead of a
  // silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kMarginsKnownOpts = kVocabMargins.known;
  if (auto err = eval::rejectUnknownOptions(opts, kMarginsKnownOpts)) {
    return *err;
  }

  // A no-constant logit/probit/poisson: every margins form below refits through
  // load_glm, which appends the constant (see postestFitLacksConstantM4). The
  // linear family answers dydx from the coefficient table and at() is gated by
  // postestRefitMatchesFitM4, so only the GLM family is refused here.
  if (postestGlmCmdsM4().count(lm.cobraCmd) && postestFitLacksConstantM4(lm)) {
    return postestNoConstantRejectM4("margins", lm);
  }

  std::vector<std::string> dydxVars;
  // AER-B4 margins-dydx-OLS: set by the linear-AME branch below, which answers from the fitted
  // model's own coefficient table instead of calling the engine. When it holds
  // a value the shared tail skips runCobraJsonM4 and runs on this payload.
  std::optional<ojson> linearAmeRaw;
  if (atVal.has_value() && !atVal->empty()) {
    // BUG-571 sibling: every at() form below is answered by an engine refit
    // (the linear --at branch refits feols from y/x/fe; the GLM at() and
    // dydx()+at() branches refit from load_glm), and none of them carries the
    // fit's weight or areg's clustered dof. Measured on main 2026-09-29
    // (BUG-571 probe frame): after `regress y x1 x2 [aweight=w]`,
    // `margins, at(x1=1)` answered 3.3492323 at rc 0 -- the unweighted fit's
    // _b[_cons]+_b[x1]+_b[x2]*mean(x2) -- where the weighted fit's is
    // 3.5853964; after `areg ..., absorb(id) vce(cluster id)` its SE is the
    // reghdfe fit's, byte for byte. The same refusal as test/lincom/nlcom.
    {
      ojson reject;
      if (!postestRefitMatchesFitM4("margins, at()", lm, reject)) return reject;
    }
    // BS-086 (BETA D5 batch) ENGINE FINDING: cobra's `margins --at`
    // (predictive margins) ALWAYS internally RE-ESTIMATES a plain OLS
    // regression from the model's own --y/--x/--fe, regardless of any
    // --model flag — live-verified DIRECTLY against the compiled engine:
    // `--model logit`, `--model probit`, `--model poisson`, and even a
    // BOGUS --model value all produce the IDENTICAL byte-for-byte output
    // in --at mode as no --model flag at all, and that shared output
    // matches a from-scratch plain-OLS refit of y~x to 10+ significant
    // digits. This is NOT a bug this batch introduced — the pre-existing
    // `--at` branch never forwarded --model at all either — but it means
    // `margins, at(...)` after ANY non-OLS estimator silently returns the
    // WRONG number: confirmed against the D5c/D5d oracle itself,
    // `margins, at(income=13000)` after `logit high income unemp` returns
    // this engine's 0.41098... vs Stata's real logistic-link 0.4074234 (a
    // ~1% relative gap, not rounding noise) — a "silently wrong" shape
    // this codebase's honest-reject doctrine forbids shipping (see e.g.
    // BS-023/BS-052). The SAME mechanism (raw --y/--x OLS refit) means
    // this is equally wrong after tobit/heckman/qreg/mediate (non-OLS
    // point estimates), newey/prais (HAC/AR1 transforms), iv/liml/reg3
    // (2SLS/GMM/SUR), and xtreg's random-effects GLS path (lm.cobraCmd
    // =="xtreg" — NOT the ,fe path, which routes through cobraCmd=="feols"
    // and is genuinely safe, see below) — none of these estimators'
    // actual fitted coefficients equal a plain OLS refit of y on x.
    // kAtSafeCmds is exactly the set of lm.cobraCmd values for which the
    // engine's OLS-refit IS the model's own true coefficients (plain
    // regress/feols, including reghdfe and xtreg's ,fe path, both of
    // which already share cobraCmd=="feols"/"ols"/"regress"/"reg"/"r" per
    // runXtreg/handlers.cpp::runRegress above) — everything else honest-
    // rejects rather than silently answer a different question than the
    // one the user's `at()` call asked.
    static const std::set<std::string> kAtSafeCmds = {"feols", "ols", "regress", "reg", "r"};
    // Hoisted out of the dydx+at() branch below so the plain at() branch
    // (BUG-188-nonOLS mirror, further down) can test the same set — both
    // branches route the identical GLM family to cobra's real fitted-model
    // margins machinery instead of the OLS-refit path.
    static const std::set<std::string> kDydxAtModels = {"logit", "probit", "poisson", "fepois",
                                                        "ppmlhdfe", "xtpois", "xtlogit"};
    // P7: `margins, dydx(...) at(...)` is a different request from the
    // predictive margin BS-086 describes above, and the engine gained a real
    // branch for it — the model is fitted once, then the AME is taken over a
    // design copy with the at() variables fixed counterfactually. That branch
    // reuses the ACTUAL fitted model, so the OLS-refit hazard the note above
    // documents does not apply to it, and the GLM estimators are supported.
    if (dydxVal.has_value() && !dydxVal->empty()) {
      if (!kDydxAtModels.count(lm.cobraCmd)) {
        return engineErrorBlockM4(
            "margins, dydx(...) at(...) is not supported after `" + lm.cobraCmd + "` in this "
            "version of PIET (the combined marginal-effects-at-a-point path covers the "
            "logit/probit/poisson family).",
            "unsupported_option");
      }
      for (const auto& spec : parseAtNumlistM4(*atVal)) {
        args.push_back("--at");
        args.push_back(spec);
      }
      args.push_back("--dydx");
      args.push_back(joinCommaM4(splitTokensM4(*dydxVal)));
      args.push_back("--model");
      args.push_back(lm.cobraCmd == "logit" ? "logit"
                                            : (lm.cobraCmd == "probit" ? "probit" : "poisson"));
      // The at()+dydx() branch is OIM by construction; the fit's own vce is not
      // forwarded, matching Stata's own "Model VCE: OIM" header on this form.
      // No --data here: this handler's shared tail runs against the RESIDENT
      // frame, which is the data source, and the serve path rejects an argv
      // that names --data or --out.
      std::vector<std::string> finalArgs = args;
      finalArgs.push_back("--tidy");
      ojson raw;
      try {
        raw = runCobraJsonM4(finalArgs);
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
      ojson block = cobraToEstimationM4(raw, "margins", &session);
      // session.lastMargins/lastMarginsDydx feed marginsplot — publish the
      // `post` e() AFTER them, so marginsplot keeps working post-`post`.
      session.lastMargins = raw;
      if (postOpt) postEstimationBlockM4(session, block, "margins", lm.depvar, lm.dataPath);
      return block;
    }
    // BUG-188-nonOLS: mirror of the dydx+at() branch above. `margins, at(...)`
    // with no dydx() is a predictive margin, and cobra's GLM predictive-margins
    // branch (apps/cmds/cmds_postest.cpp, `--at .. --model ..` with no --dydx)
    // already fits logit/probit/poisson for real and computes the average
    // adjusted prediction correctly (BUG-194's own oracle table exercises this
    // exact path) — the OLS-refit hazard the reject below exists for does not
    // apply to this family, so forward --model instead of rejecting.
    if (kDydxAtModels.count(lm.cobraCmd)) {
      args.push_back("--model");
      args.push_back(lm.cobraCmd == "logit" ? "logit"
                                            : (lm.cobraCmd == "probit" ? "probit" : "poisson"));
    } else if (!kAtSafeCmds.count(lm.cobraCmd)) {
      return engineErrorBlockM4(
          "margins, at(...) is not supported after `" + lm.cobraCmd + "` in this version of PIET "
          "— the engine's predictive-margins machinery always re-estimates a plain OLS regression "
          "from the model's y/x (and fe, if any) rather than reusing the model actually fit, and "
          "completely ignores any --model flag in this mode (live-verified) — silently wrong for "
          "any non-OLS estimator. Only `regress`/`feols`/`reghdfe`/`xtreg ,fe` are supported for "
          "margins, at(...) in this version of PIET.",
          "unsupported_option");
    }
    // BUG-188 (P2-PERF ARC-P). This split at() on whitespace and commas, which
    // is exactly the bug P7 already fixed for the dydx+at branch: it turns
    // `at(x1=(0 1))` into the two nonsense fragments `x1=(0` and `1)` and the
    // engine answers "'x1=(0' has a non-numeric value". The parser that
    // understands Stata's real at() grammar — including the parenthesised
    // numlist and the cartesian product across several variables — is
    // parseAtNumlistM4, sitting a few hundred lines up and used by the other
    // branch. Use it here too rather than keep a second, worse grammar.
    //
    // A bare single-value at() (`at(x1=3)`, `at(x1=3 x2=4)`) produces exactly
    // the same argv it did before, so the previously-working spellings are
    // untouched.
    for (const auto& spec : parseAtNumlistM4(*atVal)) {
      args.push_back("--at");
      args.push_back(spec);
    }
    // BS-086: forward the SAME vcov/cluster context the original fit used
    // (cobra margins' own usage banner: `predictive margins (linear): ...
    // [--vcov ..] [--cluster C]`) — safe here specifically because
    // kAtSafeCmds (above) is exactly the set of estimators for which the
    // engine's internal OLS refit reproduces the ORIGINAL fit's own
    // coefficients; the reported SE should likewise reflect whatever
    // vce() the original model specified rather than silently reverting
    // to iid.
    // Only for the OLS-refit family: cobra's GLM predictive-margins branch
    // (kDydxAtModels, just above) never reads --vcov/--cluster in --at mode
    // with no --dydx (matches the dydx+at() branch's own "no --vcov/--cluster
    // here" note) — forwarding them there would trip the E6 unconsumed-flag
    // net instead of silently doing the wrong thing, but it is still the
    // wrong argv to send.
    if (!kDydxAtModels.count(lm.cobraCmd)) {
      if (!lm.vcov.empty() && lm.vcov != "iid") {
        args.push_back("--vcov");
        args.push_back(lm.vcov);
      }
      if (!lm.cluster.empty()) {
        args.push_back("--cluster");
        args.push_back(joinCommaM4(lm.cluster));
      }
    }
  } else {
    // BS-063/BS-064 (BETA D1): "iv"/"reg3" were already anticipated here
    // (pre-dating this batch); "liml" is this batch's new lm.cobraCmd value
    // and needs the SAME linear-model dydx reject — without it, `ivregress
    // liml ...` followed by `margins, dydx(*)` would fall into the ELSE
    // branch below (kModelMap lookup misses "liml" too, so no --model flag
    // either) and silently run cobra's AME machinery over what is actually
    // a linear IV fit. "heckman"/"mediate" are this batch's (BETA D2, BS-070/
    // 072) additions: heckman's outcome equation is itself a plain linear
    // fit (dydx of a linear coefficient is a meaningless constant, same
    // rationale as tobit/qreg above), and margins has no coherent meaning
    // at all after `mediate` (lm.x is a best-effort {mediator, treatment}
    // placeholder, not a real regression spec) — both need the SAME reject
    // rather than falling to the ELSE branch's silent AME computation.
    // AER-B4 margins-dydx-OLS (2026-09-09) closes the half of this reject the oracle
    // contradicts. tests/eval/golden/aer_b4/milfpins_oracle_r.do:75-79 fits
    // `quietly regress price mpg weight` then `margins, dydx(mpg) post` and
    // Stata answers rc 0 — PIET refused the combination outright. The fix is
    // NOT to drop feols/regress from kLinearCmds and let this branch's engine
    // call handle them: cmd_margins (apps/cmds/cmds_postest.cpp:929) reads
    // `opt(a, "--model", "logit")` and its AME path computes the
    // logit/probit/poisson family only, so a continuous outcome like `price`
    // sent with no --model would be answered through a LOGISTIC AME — the
    // silent wrong number Doctrine gate ① forbids, strictly worse than the
    // honest reject. buildLinearAmeRawM4 (above) instead reads the answer off
    // the fitted model's own coefficient table, where a linear model's dy/dx
    // already sits, and never calls the engine.
    //
    // kLinearCmds therefore keeps all 15 names, and the OLS family is
    // diverted BEFORE it — only for a `dydx()` request. A `margins` with NO
    // dydx() after a linear model is a different question (the unconditional
    // predictive margin over the sample, which is not any coefficient), and
    // this engine still has no path to it, so it keeps refusing. Narrowing
    // kLinearCmds instead would have dropped that case through to the
    // kModelMap miss below and answered it as a logit.
    static const std::set<std::string> kLinearAmeCmds = {"feols", "ols", "regress", "reg", "r"};
    // Hoisted above the rejects (it used to sit after the kModelMap lookup) so
    // the linear-AME branch can see which variables were asked for. Parsing is
    // byte-for-byte the same; nothing between here and its old position reads
    // dydxVars.
    if (dydxVal.has_value() && !dydxVal->empty()) {
      std::string cur;
      for (char c : *dydxVal) {
        if (c == ' ' || c == ',' || c == '\t') {
          if (!cur.empty() && cur != "*") dydxVars.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty() && cur != "*") dydxVars.push_back(cur);
    }
    if (kLinearAmeCmds.count(lm.cobraCmd) && dydxVal.has_value() && !dydxVal->empty()) {
      ojson built;
      ojson reject;
      if (!buildLinearAmeRawM4(lm, dydxVars, built, reject)) return reject;
      linearAmeRaw = std::move(built);
    }
    // BUG-473-sibling (item 4, 2026-09-09): the remaining ten names have real
    // structural reasons — iv/liml/reg3's endogenous system, xtreg's
    // random-effects GLS path, and tobit/qreg/heckman/mediate's non-OLS or
    // non-coefficient marginal effects — none of which the coefficient table
    // above answers, and none of which the engine's GLM AME answers either.
    static const std::set<std::string> kLinearCmds = {
        "feols",  "ols",     "regress", "reg",    "r",      "iv",       "liml",
        "reg3",   "xtreg",   "newey",   "prais",  "tobit",  "qreg",     "heckman",
        "mediate",
    };
    if (!linearAmeRaw.has_value() && kLinearCmds.count(lm.cobraCmd)) {
      return engineErrorBlockM4(
          "margins: marginal effects (dydx) after a linear model are not supported. "
          "Use predictive margins, e.g. `margins, at(mpg=20)`.");
    }
    static const std::map<std::string, std::string> kModelMap = {
        {"logit", "logit"}, {"probit", "probit"}, {"fepois", "poisson"},
        {"poisson", "poisson"}, {"xtpois", "poisson"}, {"xtlogit", "logit"},
    };
    auto mit = kModelMap.find(lm.cobraCmd);
    if (mit != kModelMap.end()) {
      args.push_back("--model");
      args.push_back(mit->second);
    }
    // W:P5: `--dydx` now takes a VALUE on the engine's CLI (it selects which
    // variables get an effect, and drives the interaction chain rule and the
    // factor discrete-change contrast). It used to be pushed as a bare flag
    // with the selection applied only as a post-hoc row filter here — which
    // meant the next argv element (`--tidy`) was swallowed as its value.
    // Stata's `dydx(*)` means "every covariate", which is the engine's own
    // behaviour when no `--dydx` is given — so an empty selection sends no
    // flag rather than forwarding a literal `*` the engine would reject as
    // an unknown variable name.
    if (!dydxVars.empty()) {
      args.push_back("--dydx");
      args.push_back(joinCommaM4(dydxVars));
    }

    // W:P5: the AME must be computed under the SAME weights and VCE the
    // model was fitted with — Stata's margins reports `Model VCE: Robust`
    // after a clustered pweighted logit, and the delta-method SE is built
    // from that VCE. Forwarding these was the difference between
    // dy/dx x1 = .293147 (wrong) and .2717222 (Stata) on the P5 answerbook.
    if (!lm.fitWeight.empty()) {
      args.push_back("--weight");
      args.push_back(lm.fitWeight);
      if (!lm.fitWtype.empty()) {
        args.push_back("--wtype");
        args.push_back(lm.fitWtype);
      }
    }
    if (!lm.vcov.empty() && lm.vcov != "iid") {
      args.push_back("--vce");
      // lm.vcov carries the engine's linear spelling ("hc1"); the GLM
      // margins path names the same shape "robust", as cobra's own
      // logit/probit CLI does.
      args.push_back(lm.vcov == "hc1" ? std::string("robust") : lm.vcov);
    }
    if (!lm.cluster.empty()) {
      args.push_back("--cluster");
      args.push_back(joinCommaM4(lm.cluster));
    }
  }

  ojson raw;
  if (linearAmeRaw.has_value()) {
    // AER-B4 margins-dydx-OLS: the linear-AME branch already holds the whole answer, read off
    // the fitted model's own coefficient table — there is no engine call to
    // make, and `args` (built for one) is discarded. Everything below runs on
    // this payload unchanged: marginsplot caching, the dydx row filter, `post`.
    raw = std::move(*linearAmeRaw);
  } else {
    args.push_back("--tidy");
    try {
      raw = runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce));
    }
  }
  // 1.5.0 marginsplot (dispatcher.py's session.set_last_margins): cache the
  // RAW cobra JSON (not the formatted estimation block) + the requested
  // dydx var(s) so `marginsplot` can plot it without re-running cobra.
  session.lastMargins = raw;
  session.lastMarginsDydx = dydxVars;

  ojson est = cobraToEstimationM4(raw, "margins_cmd", &session);
  if (!dydxVars.empty() && est.contains("coeftable") && est["coeftable"].is_array()) {
    std::set<std::string> dydxSet(dydxVars.begin(), dydxVars.end());
    ojson filtered = ojson::array();
    for (const auto& row : est["coeftable"]) {
      if (!row.contains("name")) continue;
      const std::string name = row["name"].get<std::string>();
      // W:P5: a FACTOR variable's effect row is named `<level>.<var>`
      // (BUG-101), but the user asked for the bare `<var>` — `margins,
      // dydx(x1 treated)` must keep the `1.treated` row. Matching only the
      // literal name silently dropped every factor effect from the table
      // while the engine had computed it correctly.
      bool wanted = dydxSet.count(name) > 0;
      if (!wanted) {
        const size_t dot = name.find('.');
        if (dot != std::string::npos && dot + 1 < name.size()) {
          wanted = dydxSet.count(name.substr(dot + 1)) > 0;
        }
      }
      if (wanted) filtered.push_back(row);
    }
    est["coeftable"] = filtered;
  }
  // Item 4, `post`: published AFTER the dydxVars coeftable filter above, so
  // e(b)/_b[x] resolve from the FILTERED table (the margin rows the user
  // asked for), not from every row cobra returned. Also after
  // lastMargins/lastMarginsDydx (just above), so marginsplot keeps working.
  if (postOpt) postEstimationBlockM4(session, est, "margins", lm.depvar, lm.dataPath);
  return est;
}

// ===========================================================================
// predict

namespace {

std::string nextPathEst(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BUG-516: does every column the cached model READS still hold, in the LIVE
// frame, exactly what it held in the estimation-time frame, row for row?
//
// Only the absorbed-FE family asks. The engine's by-name prediction path
// (`--fit-data`) refuses `--fe` -- an absorbed effect cannot be evaluated on a
// row the fit never saw -- so once the head has moved, the one way left to
// predict on the live frame is to fit THERE. That is the fitted model if and
// only if the answer here is yes, and the answer is MEASURED, never assumed: a
// `replace x = ...`, a `drop if`, a `sort` between the fit and the predict each
// leave a frame that refits to different coefficients at rc 0. The ordinary
// history -- an earlier `predict`, a `generate` (w2_smalls.log section C is
// exactly that) -- only ADDS columns, and passes.
//
// The column set is "every identifier in y/x/fe that the estimation-time frame
// confirms is a column" -- column_fetch.h's predicate: never miss a real column.
// An operator prefix (`i`, `c`, `L2`) that happens to also be a column is
// compared too, which can only refuse more. A term that names NO known column
// (an abbreviation, a wildcard) cannot be verified, and refuses.
bool fitColumnsUnchangedM4(SessionState& session, const LastModel& lm, std::string& why) {
  Datastore* live = nullptr;
  std::unique_ptr<Datastore> fit;
  try {
    live = session.datastore();
    fit = std::make_unique<Datastore>(lm.dataPath);
  } catch (const std::exception& e) {
    why = std::string("the estimation-time data could not be read back (") + e.what() + ")";
    return false;
  } catch (...) {
    why = "the estimation-time data could not be read back";
    return false;
  }
  if (live == nullptr) {
    why = "no dataset is in memory";
    return false;
  }
  if (live->nRows() != fit->nRows()) {
    why = "the number of observations changed (" + std::to_string(fit->nRows()) +
          " at the fit, " + std::to_string(live->nRows()) + " now)";
    return false;
  }
  std::set<std::string> cols;
  std::vector<std::string> terms = lm.x;
  terms.push_back(lm.y);
  terms.insert(terms.end(), lm.fe.begin(), lm.fe.end());
  for (const std::string& term : terms) {
    if (term.empty()) continue;
    bool any = false;
    for (size_t i = 0; i < term.size();) {
      const unsigned char c = static_cast<unsigned char>(term[i]);
      if (!std::isalpha(c) && c != '_') {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < term.size() &&
             (std::isalnum(static_cast<unsigned char>(term[j])) || term[j] == '_')) {
        ++j;
      }
      const std::string tok = term.substr(i, j - i);
      if (fit->has(tok)) {
        cols.insert(tok);
        any = true;
      }
      i = j;
    }
    if (!any) {
      why = "`" + term + "` names no variable of the estimation-time data";
      return false;
    }
  }
  const size_t n = static_cast<size_t>(fit->nRows());
  for (const std::string& name : cols) {
    if (!live->has(name)) {
      why = "variable " + name + " is no longer in the data";
      return false;
    }
    const cecon::Column& a = fit->col(name);
    const cecon::Column& b = live->col(name);
    bool same = a.dtype == b.dtype;
    if (same && a.dtype == cecon::DType::F64) {
      const double* pa = a.f64();
      const double* pb = b.f64();
      same = pa != nullptr && pb != nullptr;
      // Missing == missing: NaN never compares equal to itself, and a column that
      // was only ever carried forward must not read as "changed".
      for (size_t r = 0; same && r < n; ++r) {
        same = pa[r] == pb[r] || (std::isnan(pa[r]) && std::isnan(pb[r]));
      }
    } else if (same) {
      const int32_t* ca = a.codes();
      const int32_t* cb = b.codes();
      same = ca != nullptr && cb != nullptr && a.levels == b.levels;
      for (size_t r = 0; same && r < n; ++r) same = ca[r] == cb[r];
    }
    if (!same) {
      why = "variable " + name + " changed";
      return false;
    }
  }
  return true;
}

// ===========================================================================
// W2B predict floors (BUG-596). The statistics `predict` answers after the four
// least-squares commands the AER corpus leans on, per their own predict
// programs as StataBE answered them (tests/eval/golden/aer_w2/w2b_estim.log,
// the predict_* arms; aer_b4/predict-e-xbd.log for the reghdfe `resid` rule,
// the xtreg,fe "not allowed" pair and the estimation-sample masking):
//
//   regress   xb residuals score stdp stdf stdr rstandard rstudent leverage|hat
//             (`e`, `d`: r(198) "option e not allowed")
//   areg      xb stdp dresiduals, and on e(sample) only: xbd d residuals score
//   xtreg,fe  xb stdp ue residuals (= ue), and on e(sample) only: xbu u e
//             (`xbd`, `d`: r(198))
//   reghdfe   xb stdp, and on e(sample) only: xbd d residuals; xbd/d need the
//             fit to have kept its residuals (`resid`), else r(9) (`e`: r(198))
//
// Definitions ([R] regress postestimation and [R] areg postestimation, Methods
// and formulas; [XT] xtreg postestimation): xb = x b INCLUDING _cons. With the
// within residual e = y - xb - d, the absorbed component is d = y - xb - e over
// e(sample) and xbd = y - e. stdp = sqrt(x V x'). Under vce(ols) h = x V x'/s^2,
// stdr = s sqrt(1-h), stdf = s sqrt(1+h), rstandard = e/stdr and rstudent =
// e/(s_(j) sqrt(1-h)), s_(j)^2 = (s^2 df_r - e^2/(1-h))/(df_r - 1).
//
// Measured on main before this (frozen binary, w2b_estim.do lines 1-17 + 50-108):
// every statistic above except xb/residuals refused, `r` refused, and xb after
// areg / xtreg,fe / reghdfe wrote the WITHIN prediction x~b -- r(sum) 1.8e-15,
// r(sd) 6.196 where the oracle's column sums to 760.4 with sd 6.324 -- at rc 0.
// That last one is the silent wrong answer this block exists for: an absorbed-FE
// fit's `predict p` is now x b + _cons, evaluated here, never the engine's
// demeaned index.
//
// Nothing below is filled that was not computed for THIS model: the design is
// read off e(b)'s own names (a name this reader cannot evaluate refuses), the
// estimation sample is the one the engine's refit reports (its within residual
// is missing off the sample, and the count must equal e(N)), and each quantity
// is checked against an independent one the engine produced before it is
// written (see computePredictNativeM4).

enum class PredictFamilyM4 { Other, Regress, Areg, XtregFe, Reghdfe };

PredictFamilyM4 predictFamilyM4(const LastModel& lm) {
  if (lm.fe.empty()) return lm.stataCmd == "regress" ? PredictFamilyM4::Regress : PredictFamilyM4::Other;
  if (lm.stataCmd == "areg") return PredictFamilyM4::Areg;
  if (lm.stataCmd == "reghdfe") return PredictFamilyM4::Reghdfe;
  // `xtreg, fe` leaves no Stata spelling on LastModel (its e(cmd) reads the
  // engine's "feols"). The one mark its route leaves is the N_g it copies into
  // the cached reply (runXtreg's fixed-effects arm, BUG-538's e() half) --
  // nothing else in the tree writes that key.
  if (lm.stataCmd.empty() && lm.cobraCmd == "feols" && lm.result.has_value() &&
      lm.result->is_object() && lm.result->contains("N_g")) {
    return PredictFamilyM4::XtregFe;
  }
  return PredictFamilyM4::Other;
}

std::string predictFamilyCmdM4(PredictFamilyM4 f, const LastModel& lm) {
  return f == PredictFamilyM4::XtregFe ? std::string("xtreg, fe") : lm.stataCmd;
}

// A typed spelling -> the statistic it names. The dispatcher's option vocabulary
// (dispatcher.cpp kVocabPredict) already canonicalizes the unique prefixes
// (`rsta`, `rstu`, `lev`, `sc`, `dr`) and the `re`/`res`/`resi` aliases; `r` is a
// prefix of four names there and arrives as typed. Each row is an oracle spelling
// (w2b_estim.do lines 52, 85, 96, 107).
std::string predictStatisticM4(const std::string& k) {
  if (k == "r" || k == "re" || k == "res" || k == "resi" || k == "resid" || k == "residuals") {
    return "residuals";
  }
  if (k == "sc" || k == "score") return "score";
  if (k == "rsta" || k == "rstandard") return "rstandard";
  if (k == "rstu" || k == "rstudent") return "rstudent";
  if (k == "lev" || k == "leverage" || k == "hat") return "hat";
  if (k == "dr" || k == "dresid" || k == "dresiduals") return "dresiduals";
  return k;
}

bool predictServesM4(PredictFamilyM4 f, const std::string& s) {
  static const std::set<std::string> kRegress = {"xb",   "residuals", "score",    "stdp",    "stdf",
                                                 "stdr", "rstandard", "rstudent", "hat"};
  static const std::set<std::string> kAreg = {"xb", "stdp", "dresiduals", "xbd", "d", "residuals",
                                              "score"};
  static const std::set<std::string> kXtregFe = {"xb", "stdp", "ue", "xbu", "u", "e", "residuals"};
  static const std::set<std::string> kReghdfe = {"xb", "stdp", "xbd", "d", "residuals"};
  switch (f) {
    case PredictFamilyM4::Regress: return kRegress.count(s) > 0;
    case PredictFamilyM4::Areg: return kAreg.count(s) > 0;
    case PredictFamilyM4::XtregFe: return kXtregFe.count(s) > 0;
    case PredictFamilyM4::Reghdfe: return kReghdfe.count(s) > 0;
    default: return false;
  }
}

// Stata's own refusal, r(198) "option X not allowed": w2b_estim.log predict_reg_e /
// predict_reg_d; predict-e-xbd.log B_stat_xbd / B_stat_d (xtreg,fe) and P_res_e
// (reghdfe).
bool predictNotAllowedM4(PredictFamilyM4 f, const std::string& s) {
  switch (f) {
    case PredictFamilyM4::Regress: return s == "e" || s == "d";
    case PredictFamilyM4::XtregFe: return s == "xbd" || s == "d";
    case PredictFamilyM4::Reghdfe: return s == "e";
    default: return false;
  }
}

// The statistics the engine's own predict already writes as Stata defines them,
// so they keep the historical argv byte for byte: `--xb` after regress, and the
// residual (`--resid`, the within residual under --fe, missing off e(sample)).
// "" = computed here.
std::string predictEngineServedM4(PredictFamilyM4 f, const std::string& s) {
  switch (f) {
    case PredictFamilyM4::Regress:
      if (s == "xb") return "xb";
      if (s == "residuals" || s == "score") return "resid";
      return "";
    case PredictFamilyM4::Areg: return (s == "residuals" || s == "score") ? "resid" : "";
    case PredictFamilyM4::XtregFe: return s == "e" ? "resid" : "";
    case PredictFamilyM4::Reghdfe: return s == "residuals" ? "resid" : "";
    default: return "";
  }
}

// The one statistic the options name, or the refusal block.
std::optional<ojson> resolvePredictStatM4(PredictFamilyM4 f, const LastModel& lm,
                                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                          std::string& stat) {
  stat.clear();
  for (const auto& [k, v] : opts) {
    if (k == "quietly" || k == "noisily") continue;
    const std::string s = predictStatisticM4(k);
    const bool bare = v.isBool && v.boolValue;
    if (bare && predictNotAllowedM4(f, s)) {
      return engineErrorBlockM4("option " + k + " not allowed", "syntax_error");
    }
    if (!bare || !predictServesM4(f, s)) {
      return engineErrorBlockM4("predict " + k + ": not supported after `" + predictFamilyCmdM4(f, lm) +
                                    "` in this version of PIET. Nothing was written to your data.",
                                "unsupported_option");
    }
    if (!stat.empty() && stat != s) {
      return engineErrorBlockM4("predict: only one statistic may be specified", "syntax_error");
    }
    stat = s;
  }
  if (stat.empty()) stat = "xb";  // Stata's default after all four
  return std::nullopt;
}

// One e(b) column of the design, as a product of live-frame columns. `level` < 0
// is the column's value (a plain or `c.` term; an `o.` omitted term, whose
// coefficient is 0); level >= 0 is the indicator `var == level` of a factor term
// (`2.t`, the base `1b.t`), missing where var is. `constant` is _cons.
struct DesignColM4 {
  bool constant = false;
  std::vector<std::pair<const double*, long long>> parts;
};

// Read e(b)'s names as design columns of the live frame. Accepts exactly the
// shapes above joined by `#`; anything else -- a time-series operator (`L.x`),
// a string column, a name that is not a column -- is not guessed at: `why` names
// it and the caller refuses.
bool buildDesignM4(const Datastore& ds, const std::vector<std::string>& names,
                   std::vector<DesignColM4>& cols, std::string& why) {
  cols.clear();
  for (const std::string& nm : names) {
    DesignColM4 c;
    if (nm == "_cons" || nm == "(intercept)") {
      c.constant = true;
      cols.push_back(std::move(c));
      continue;
    }
    size_t pos = 0;
    while (true) {
      const size_t hash = nm.find('#', pos);
      const std::string seg =
          nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      std::string var = seg;
      long long level = -1;
      const size_t dot = seg.find('.');
      if (dot != std::string::npos) {
        const std::string op = seg.substr(0, dot);
        var = seg.substr(dot + 1);
        if (op != "c" && op != "o") {
          size_t d = 0;
          while (d < op.size() && std::isdigit(static_cast<unsigned char>(op[d]))) ++d;
          const std::string suffix = op.substr(d);
          if (d == 0 || d > 15 ||
              !(suffix.empty() || suffix == "b" || suffix == "o" || suffix == "bn" || suffix == "n")) {
            why = "the coefficient `" + nm + "` is not a term this version of PIET can evaluate";
            return false;
          }
          level = std::stoll(op.substr(0, d));
        }
      }
      if (var.empty() || var.find('.') != std::string::npos || !ds.has(var) ||
          ds.col(var).dtype != cecon::DType::F64 || ds.col(var).f64() == nullptr) {
        why = "the coefficient `" + nm + "` does not name a numeric variable of the data in memory";
        return false;
      }
      c.parts.emplace_back(ds.col(var).f64(), level);
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
    cols.push_back(std::move(c));
  }
  return true;
}

double designValueM4(const DesignColM4& c, size_t r) {
  if (c.constant) return 1.0;
  double v = 1.0;
  for (const auto& [p, level] : c.parts) {
    const double x = p[r];
    if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();
    v *= (level < 0) ? x : (x == static_cast<double>(level) ? 1.0 : 0.0);
  }
  return v;
}

// e(V) over `names` (row-major), from the engine's own lincom: Var(b_j) and
// Var(b_j + b_l) in ONE call, V_jl = (Var(b_j + b_l) - V_jj - V_ll) / 2. It is the
// covariance every lincom/test answer already rests on (the refit forwards the
// fitted --vcov/--cluster, BS-098) -- and the only full one reachable: the fit's
// reply carries standard errors, not V, and `matrix V = e(V)` synthesizes a
// diagonal. The FULL-specification call (lastModelBaseArgv), deliberately not
// runPostEstimationM4: minting a §31 handle goes through setLastModel, which
// erases e(resid), and a later `predict, xbd` after `reghdfe ..., resid` would
// then refuse r(9) on a fit that kept its residuals. Its b and its standard errors
// must reproduce the fit's, or the V it carries belongs to some other fit (a refit
// on another sample, another VCE) and nothing is returned.
bool lincomVcovM4(const LastModel& lm, const std::vector<std::string>& names,
                  const std::vector<double>& b, const std::vector<double>& seFit,
                  std::vector<double>& V, std::string& why) {
  const size_t k = names.size();
  // k + k(k-1)/2 expressions ride on one argv; past this the list outgrows it.
  constexpr size_t kMaxTerms = 160;
  if (k == 0 || k > kMaxTerms) {
    why = "the model has " + std::to_string(k) + " coefficients (this version of PIET computes "
          "the prediction's variance for at most " + std::to_string(kMaxTerms) + ")";
    return false;
  }
  std::vector<std::string> spell(k);
  for (size_t j = 0; j < k; ++j) {
    spell[j] = names[j] == "(intercept)" ? std::string("_cons") : names[j];
    if (spell[j].find_first_of("+-* \t") != std::string::npos) {
      why = "the coefficient `" + names[j] + "` cannot be addressed by the engine's lincom";
      return false;
    }
  }
  std::vector<std::string> argv = lastModelBaseArgv("lincom", lm);
  for (size_t j = 0; j < k; ++j) argv.insert(argv.end(), {"--exp", spell[j]});
  for (size_t j = 0; j < k; ++j)
    for (size_t l = j + 1; l < k; ++l) argv.insert(argv.end(), {"--exp", spell[j] + " + " + spell[l]});
  argv.push_back("--tidy");
  const nlohmann::json raw = runCobraJsonM4(argv);  // CobraError -> the caller's catch
  const size_t want = k + k * (k - 1) / 2;
  if (!raw.is_object() || !raw.contains("coefs") || !raw["coefs"].is_array() ||
      raw["coefs"].size() != want) {
    why = "the engine's lincom returned an unexpected reply";
    return false;
  }
  const auto& co = raw["coefs"];
  auto num = [&co](size_t i, const char* key) {
    return (co[i].is_object() && co[i].contains(key) && co[i][key].is_number())
               ? co[i][key].get<double>()
               : std::numeric_limits<double>::quiet_NaN();
  };
  V.assign(k * k, 0.0);
  for (size_t j = 0; j < k; ++j) {
    const double bj = num(j, "b");
    const double se = num(j, "se");
    if (!std::isfinite(bj) || !std::isfinite(se) || !std::isfinite(seFit[j]) ||
        std::fabs(bj - b[j]) > 1e-9 * std::max({1.0, std::fabs(b[j]), se}) ||
        std::fabs(se - seFit[j]) > 1e-9 * seFit[j]) {
      why = "the engine's post-estimation refit did not reproduce the fitted coefficient and "
            "standard error of `" + names[j] + "`";
      return false;
    }
    V[j * k + j] = se * se;
  }
  size_t idx = k;
  for (size_t j = 0; j < k; ++j) {
    for (size_t l = j + 1; l < k; ++l, ++idx) {
      const double se = num(idx, "se");
      if (!std::isfinite(se)) {
        why = "the engine's lincom returned no standard error for `" + spell[j] + " + " + spell[l] + "`";
        return false;
      }
      const double c = (se * se - V[j * k + j] - V[l * k + l]) / 2.0;
      V[j * k + l] = c;
      V[l * k + j] = c;
    }
  }
  return true;
}

double jsonNumM4(const ojson& o, const char* key) {
  return (o.is_object() && o.contains(key) && o[key].is_number())
             ? o[key].get<double>()
             : std::numeric_limits<double>::quiet_NaN();
}

// Compute statistic `stat` (one the engine does not write itself) over every row
// of the live frame. `engineArgs` is runPredict's replay argv up to and including
// --fe (no --xb/--resid/--out): it is re-run here as a ONE-SHOT engine call into a
// scratch file whose two columns this function reads -- the engine's xb (after
// regress, the exact linear predictor over the rows the engine evaluates) and its
// residual (under --fe the within residual, missing off e(sample)). One-shot, so
// the output is a file on disk whatever plane the session runs on; every input
// path must therefore be on disk too, and is checked.
//
// Cross-checks before anything is returned (each failure refuses, naming why):
//   regress  the xb evaluated here equals the engine's on every row it evaluates;
//   FE       the engine's refit sample has e(N) rows, xb and y exist on all of
//            them, and sum over e(sample) of d is 0 (it is, exactly, when _cons =
//            ybar - xbar b, the engine's own convention -- GAP B in feols.cpp);
//   stdp     lincom's b reproduces e(b) (lincomVcovM4); under FE additionally
//            se(_cons)^2 == s^2/N + xbar V xbar', the engine's own extended-inverse
//            identity, which also proves the sample means used below.
std::optional<ojson> computePredictNativeM4(SessionState& session, const LastModel& lm,
                                            PredictFamilyM4 fam, const std::string& stat,
                                            std::vector<std::string> engineArgs,
                                            std::vector<double>& vals) {
  const std::string cmd = predictFamilyCmdM4(fam, lm);
  auto refuse = [&](const std::string& why) {
    return engineErrorBlockM4("predict, " + stat + " after `" + cmd +
                                  "` is not supported here in this version of PIET: " + why +
                                  ". Refusing rather than writing a number that was not computed "
                                  "for this model. Nothing was written to your data.",
                              "unsupported_option");
  };
  const bool fe = fam != PredictFamilyM4::Regress;
  const bool iid = lm.vcov == "iid" && lm.cluster.empty();

  // Stata: rstandard/rstudent/leverage/stdf/stdr are "not available if any vce()
  // other than vce(ols) was specified" ([R] regress postestimation). Under an
  // absorbed FE the prediction's variance needs _cons's covariances, which only
  // the iid algebra reaches from here (the engine's lincom cannot address the
  // reconstructed _cons row).
  if (!iid && (stat == "hat" || stat == "stdr" || stat == "stdf" || stat == "rstandard" ||
               stat == "rstudent")) {
    return engineErrorBlockM4("predict, " + stat + " is not available after a `regress` with a "
                              "robust or clustered VCE; Stata computes it only under vce(ols)",
                              "unsupported_option");
  }
  if (fe && stat == "stdp" && !iid) {
    return refuse("the standard error of the prediction after absorbed fixed effects is computed "
                  "only under the default (iid) VCE");
  }

  // reghdfe answers xbd/d only when the fit kept its residuals (reghdfe_p: r(9)),
  // and reads them from that variable (predict-e-xbd.log R_drop_xbd: dropped ->
  // r(111)). w2b_estim.log predict_reghdfe_xbd_noresid is the r(9) arm.
  if (fam == PredictFamilyM4::Reghdfe && (stat == "xbd" || stat == "d")) {
    const std::string* rv = session.eval.eStrings.find("resid");
    if (rv == nullptr || rv->empty()) {
      return ojson(eval::errorBlock(
          "engine_error", "you must add the resid option to reghdfe before running this prediction",
          std::nullopt, std::nullopt, std::nullopt, "r(9)"));
    }
    Datastore* live0 = nullptr;
    try {
      live0 = session.datastore();
    } catch (const std::exception& e) {
      return refuse(e.what());
    }
    if (live0 == nullptr || !live0->has(*rv)) {
      return ojson(eval::engineError("variable " + *rv + " not found"));
    }
  }

  // Coefficients: e(b) as the engine reported it for this fit.
  std::vector<std::string> names;
  std::vector<double> b, se;
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array()) {
    return refuse("the fitted coefficients are not available");
  }
  for (const auto& row : (*lm.result)["coeftable"]) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) {
      return refuse("the fitted coefficients are not available");
    }
    names.push_back(row["name"].get<std::string>());
    b.push_back(jsonNumM4(row, "b"));
    se.push_back(jsonNumM4(row, "se"));
    if (!std::isfinite(b.back())) return refuse("the coefficient `" + names.back() + "` is missing");
  }
  size_t consIdx = names.size();
  for (size_t j = 0; j < names.size(); ++j)
    if (names[j] == "_cons" || names[j] == "(intercept)") consIdx = j;
  if (fe && consIdx == names.size()) {
    return refuse("the fit reports no _cons, and xb after absorbed effects is x b + _cons");
  }
  const double sigma2 = [&] {
    const double s2 = jsonNumM4(*lm.result, "sigma2");
    if (std::isfinite(s2)) return s2;
    const double rm = jsonNumM4(*lm.result, "rmse");
    return rm * rm;
  }();
  const double nObs = jsonNumM4(*lm.result, "n_obs");
  const double dfR = jsonNumM4(*lm.result, "df_r");

  // The covariance, before any data is read (lincom never touches the session).
  const bool needV = stat == "stdp" || stat == "hat" || stat == "stdr" || stat == "stdf" ||
                     stat == "rstandard" || stat == "rstudent";
  std::vector<std::string> vNames;
  std::vector<double> vB, vSe, V;
  if (needV) {
    for (size_t j = 0; j < names.size(); ++j) {
      if (fe && j == consIdx) continue;  // the reconstructed _cons: identity below
      vNames.push_back(names[j]);
      vB.push_back(b[j]);
      vSe.push_back(se[j]);
    }
    std::string why;
    try {
      if (!lincomVcovM4(lm, vNames, vB, vSe, V, why)) return refuse(why);
    } catch (const CobraError& ce) {
      return refuse(cobraErrorStrM4(ce));
    }
  }
  if ((stat == "hat" || stat == "stdr" || stat == "stdf" || stat == "rstandard" ||
       stat == "rstudent") && !(std::isfinite(sigma2) && sigma2 > 0.0)) {
    return refuse("the fit reports no residual variance");
  }

  // The engine's refit, one-shot into a scratch file on disk.
  Datastore* live = nullptr;
  try {
    live = session.datastore();  // materializes a virtual head before anything reads it
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  for (size_t i = 0; i + 1 < engineArgs.size(); ++i) {
    if ((engineArgs[i] == "--data" || engineArgs[i] == "--fit-data") &&
        !cecon::native_exists(engineArgs[i + 1])) {
      return refuse("the data the model was fitted on are not available on disk");
    }
  }
  const std::string kXb = "__piet_predict_xb__";
  const std::string kE = "__piet_predict_e__";
  const std::string scratch = nextPathEst(session.workdir(), "predict_refit");
  engineArgs.insert(engineArgs.end(), {"--xb", kXb, "--resid", kE, "--out", scratch});
  const CobraRunResult rr = runCobraOneShot(engineArgs);
  if (!rr.ok || !cecon::native_exists(scratch)) {
    cecon::native_unlink(scratch);
    const std::string err = rr.stderr_text.empty() ? rr.error : rr.stderr_text;
    return refuse("the engine's refit failed (" + strutil::trim(err) + ")");
  }
  std::vector<double> engXb, engE;
  try {
    Datastore eng(scratch);
    if (eng.nRows() != live->nRows() || !eng.has(kXb) || !eng.has(kE) ||
        eng.col(kXb).f64() == nullptr || eng.col(kE).f64() == nullptr) {
      throw std::runtime_error("the engine's refit returned an unexpected frame");
    }
    const double* px = eng.col(kXb).f64();
    const double* pe = eng.col(kE).f64();
    engXb.assign(px, px + eng.nRows());
    engE.assign(pe, pe + eng.nRows());
  } catch (const std::exception& e) {
    cecon::native_unlink(scratch);
    return refuse(e.what());
  }
  cecon::native_unlink(scratch);

  // The live frame again: nothing above may have kept the pointer current.
  try {
    live = session.datastore();
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  if (live == nullptr) return ojson(eval::noDataLoaded());
  const size_t n = static_cast<size_t>(live->nRows());
  if (engXb.size() != n) return refuse("the engine's refit returned an unexpected frame");

  std::vector<DesignColM4> design, vDesign;
  {
    std::string why;
    if (!buildDesignM4(*live, names, design, why)) return refuse(why);
    if (needV && !buildDesignM4(*live, vNames, vDesign, why)) return refuse(why);
  }
  const double* y = nullptr;
  if (live->has(lm.y) && live->col(lm.y).dtype == cecon::DType::F64) y = live->col(lm.y).f64();
  const bool needY = fe || stat == "residuals" || stat == "rstandard" || stat == "rstudent";
  if (needY && y == nullptr) return refuse("the dependent variable `" + lm.y + "` is not a numeric variable");

  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> xb(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    double s = 0.0;
    bool ok = true;
    for (size_t j = 0; j < design.size() && ok; ++j) {
      const double v = designValueM4(design[j], r);
      if (std::isnan(v)) ok = false;
      else s += v * b[j];
    }
    if (ok) xb[r] = s;
  }

  // The within residual and the estimation sample (FE only).
  std::vector<double> d;
  std::vector<char> inSample;
  if (!fe) {
    // After regress the engine evaluates the fitted coefficients itself: the xb
    // built here from e(b)'s names must be its xb, row for row.
    for (size_t r = 0; r < n; ++r) {
      if (std::isnan(engXb[r])) continue;
      if (std::isnan(xb[r]) || std::fabs(xb[r] - engXb[r]) > 1e-9 * (1.0 + std::fabs(engXb[r]))) {
        return refuse("the design read from e(b) does not reproduce the engine's linear prediction");
      }
    }
  } else {
    inSample.assign(n, 0);
    d.assign(n, kNaN);
    size_t count = 0;
    double sumD = 0.0, scale = 0.0;
    for (size_t r = 0; r < n; ++r) {
      if (std::isnan(engE[r])) continue;
      if (y == nullptr || std::isnan(y[r]) || std::isnan(xb[r])) {
        return refuse("an estimation-sample row has no linear prediction");
      }
      inSample[r] = 1;
      ++count;
      d[r] = y[r] - xb[r] - engE[r];
      sumD += d[r];
      scale += std::fabs(y[r] - engE[r]) + std::fabs(xb[r]);
    }
    if (!std::isfinite(nObs) || static_cast<double>(count) != nObs) {
      return refuse("the engine's refit sample (" + std::to_string(count) +
                    " observations) is not the fitted one");
    }
    if (std::fabs(sumD) > 1e-9 * scale) {
      return refuse("the absorbed component does not sum to zero over the estimation sample, so "
                    "the design read from e(b) is not the fitted one");
    }
  }

  // x V x' (stdp; h = x V x'/s^2 under vce(ols)).
  std::vector<double> q;
  if (needV) {
    const size_t k = vDesign.size();
    std::vector<double> xbar(k, 0.0);
    if (fe) {
      // Frisch-Waugh: under iid the engine's extended inverse gives Cov(b, _cons)
      // = -V xbar' and Var(_cons) = s^2/N + xbar V xbar' (feols.cpp GAP B), so
      // x_c V_full x_c' = (x - xbar) V (x - xbar)' + s^2/N.
      size_t cnt = 0;
      for (size_t r = 0; r < n; ++r) {
        if (!inSample[r]) continue;
        ++cnt;
        for (size_t j = 0; j < k; ++j) xbar[j] += designValueM4(vDesign[j], r);
      }
      for (size_t j = 0; j < k; ++j) xbar[j] /= static_cast<double>(cnt);
      double quad = 0.0;
      for (size_t j = 0; j < k; ++j)
        for (size_t l = 0; l < k; ++l) quad += xbar[j] * V[j * k + l] * xbar[l];
      const double want = sigma2 / nObs + quad;
      const double got = se[consIdx] * se[consIdx];
      if (!(std::isfinite(want) && std::isfinite(got)) || std::fabs(want - got) > 1e-8 * std::fabs(got)) {
        return refuse("Var(_cons) does not satisfy the fit's own identity, so the estimation-sample "
                      "means are not the fitted ones");
      }
    }
    q.assign(n, kNaN);
    std::vector<double> x(k);
    for (size_t r = 0; r < n; ++r) {
      bool ok = true;
      for (size_t j = 0; j < k && ok; ++j) {
        x[j] = designValueM4(vDesign[j], r);
        if (std::isnan(x[j])) ok = false;
        else x[j] -= xbar[j];
      }
      if (!ok) continue;
      double s = 0.0;
      for (size_t j = 0; j < k; ++j) {
        double row = 0.0;
        for (size_t l = 0; l < k; ++l) row += V[j * k + l] * x[l];
        s += x[j] * row;
      }
      if (fe) s += sigma2 / nObs;
      q[r] = s;
    }
  }

  vals.assign(n, kNaN);
  for (size_t r = 0; r < n; ++r) {
    const double yr = y != nullptr ? y[r] : kNaN;
    const double res = yr - xb[r];  // NaN wherever either is
    if (stat == "xb") {
      vals[r] = xb[r];
    } else if (stat == "stdp") {
      vals[r] = std::isnan(q[r]) ? kNaN : std::sqrt(std::max(0.0, q[r]));
    } else if (stat == "dresiduals" || stat == "ue" ||
               (fam == PredictFamilyM4::XtregFe && stat == "residuals")) {
      vals[r] = res;
    } else if (stat == "xbd" || stat == "xbu") {
      vals[r] = inSample[r] ? xb[r] + d[r] : kNaN;
    } else if (stat == "d" || stat == "u") {
      vals[r] = inSample[r] ? d[r] : kNaN;
    } else if (q.size() == n) {
      // regress, vce(ols): h, stdr, stdf, rstandard, rstudent.
      if (std::isnan(q[r])) continue;
      const double h = q[r] / sigma2;
      const double s = std::sqrt(sigma2);
      if (stat == "hat") {
        vals[r] = h;
      } else if (stat == "stdf") {
        vals[r] = s * std::sqrt(1.0 + h);
      } else if (stat == "stdr") {
        vals[r] = h < 1.0 ? s * std::sqrt(1.0 - h) : kNaN;
      } else if (stat == "rstandard") {
        vals[r] = (h < 1.0 && !std::isnan(res)) ? res / (s * std::sqrt(1.0 - h)) : kNaN;
      } else if (stat == "rstudent") {
        if (h < 1.0 && !std::isnan(res) && dfR > 1.0) {
          const double sj2 = (sigma2 * dfR - res * res / (1.0 - h)) / (dfR - 1.0);
          vals[r] = sj2 > 0.0 ? res / (std::sqrt(sj2) * std::sqrt(1.0 - h)) : kNaN;
        }
      }
    }
  }
  return std::nullopt;
}

// Write `vals` as the new f64 column `name` of the live frame into `outPath`
// (`cobra setcol --new`, the payload shape estimWriteColumn uses: one value per
// row, an EMPTY line for missing).
std::optional<ojson> writePredictColumnM4(SessionState& session, const std::string& liveHead,
                                          const std::string& name, const std::vector<double>& vals,
                                          const std::string& outPath) {
  const std::string hb = mintScratchPath(session.workdir(), "predict_values");
  forgetScratchMint(hb);
  const std::string csv = hb.substr(0, hb.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return engineErrorBlockM4("predict: could not write the values payload");
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (double v : vals) {
      if (!std::isnan(v)) f << v;
      f << "\n";
    }
    if (!f) return engineErrorBlockM4("predict: could not write the values payload");
  }
  const CobraRunResult r = runCobra({"setcol", "--data", liveHead, "--var", name, "--values", csv,
                                     "--dtype", "f64", "--new", "--out", outPath});
  cecon::native_unlink(csv);
  if (!r.ok) {
    return engineErrorBlockM4("predict: could not create " + name + " (" +
                              strutil::trim(r.stderr_text) + ")");
  }
  // The engine's writer creates --out before it can detect a write failure (the
  // estimWriteColumn note): a missing file is a failed write, not a head.
  if (!cecon::native_exists(outPath)) {
    return engineErrorBlockM4("predict: failed writing output dataset (disk full or I/O error)");
  }
  return std::nullopt;
}

}  // namespace

ojson runPredict(const std::vector<std::string>& varlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session, const std::optional<std::string>& ifExpr) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  // BUG-204 (BUG-203's sibling, and the worse half): `predict` never read the
  // estimator either. The argv built below carries --data/--y/--x and no model,
  // and cobra's cmd_predict calls feols_fit() unconditionally, so `logit y x1 x2`
  // -> `predict phat` wrote the LINEAR-PROBABILITY fitted values into the user's
  // dataset under a name they believe holds Pr(y=1). Measured against StataNow
  // 19.5: min -0.06666 / max 1.20595 where Stata's own predict gives min 0.04791
  // / max 0.97712 — values outside [0,1] for a probability, and they PERSIST as
  // data, surviving save/export, which is why this outranks the lincom shape.
  //
  // Why no gate saw it: OLS fitted values average to ybar and so do logit
  // predicted probabilities, so the MEAN agrees to 15 digits (0.5633333...) and
  // every mean-based sanity check passed identically. Any pin on this path must
  // assert the RANGE, not central tendency.
  //
  // BUG-297 closes BUG-204's open half for logit/probit/poisson: cobra's
  // cmd_predict now HAS a `--model` GLM prediction path (the postest_fit_spec
  // dispatcher BUG-203 built — the same refit lincom/nlcom/test forward to),
  // emitting Stata's default statistic for the model: `pr` after logit/probit,
  // `n` after poisson, `xb` on request. So the refusal NARROWS to exactly the
  // estimators the engine still cannot re-fit (nbreg, tobit, heckman, qreg,
  // xtreg-re, fepois/ppmlhdfe/xtpois/xtlogit — no absorbed-effects ML refit —
  // newey, prais, iv/liml/reg3, csdid, ...), same membership as lincom/nlcom/
  // test's postestGlmCmdsM4 gate. Unlike those verbs, predict is a POINT
  // prediction — it reads only beta, which vce() never moves — so a
  // logit/probit/poisson fitted with vce(robust)/vce(cluster) still predicts
  // (the argv below carries no --vcov/--cluster and the engine refit is the
  // plain fit with the identical beta). Absorbed FE would move beta, so that
  // still refuses.
  const bool glmPredict = postestGlmCmdsM4().count(lm.cobraCmd) > 0;
  if (!postestOlsCmdsM4().count(lm.cobraCmd) && !glmPredict) {
    return engineErrorBlockM4(
        "predict is not supported after `" + lm.cobraCmd +
            "` in this version of PIET — the engine can compute predictions only for the "
            "least-squares family and for logit/probit/poisson (via a maximum-likelihood "
            "re-fit); `" + lm.cobraCmd +
            "` has neither, so the column it could write would not be this model's "
            "prediction. Refusing rather than writing a number that is not the one asked "
            "for into your data.",
        "unsupported_option");
  }
  if (postestFitLacksConstantM4(lm)) return postestNoConstantRejectM4("predict", lm);
  if (glmPredict && !lm.fe.empty()) {
    return engineErrorBlockM4(
        "predict is not supported after `" + lm.cobraCmd +
            "` with absorbed fixed effects in this version of PIET — the engine's "
            "maximum-likelihood re-fit has no absorbed-effects path, so its coefficients "
            "(and therefore its predictions) would differ from the model you fitted. "
            "Refusing rather than predicting from a different model.",
        "unsupported_option");
  }
  // BUG-519: predict after a WEIGHTED fit wrote the UNWEIGHTED model's
  // predictions at rc 0. The fit's [aw/fw/pw/iw=] lives in lm.fitWeight (margins
  // forwards it), but the engine's predict has no weight input at all --
  // cmd_predict's refit is `build_model(*fit_ds, y, xs, fes, {})` and its ML path
  // reads only --model/--fe/--vcov/--cluster -- so every branch below (head
  // unmoved, `--fit-data`, the absorbed-FE refit, the logit/probit/poisson ML
  // refit) re-estimated without the weight. Measured on main 2026-09-22:
  // `regress y x2 t [aw=w]` / `predict p` -> p differs from _b[_cons] + _b[x2]*x2
  // + _b[t]*t by up to 0.38 on 12 rows, rc 0; the same under [fw=]. Stata serves
  // it (aer_b4/predict-e-xbd.log J_build_areg: `predict double A_xb, xb` after
  // `areg ... [aw=wv]`, rc 0) -- this build cannot, so it refuses by name and
  // writes nothing. Ahead of the option parse: no statistic is served here.
  if (!lm.fitWeight.empty()) {
    return engineErrorBlockM4(
        "predict after a weighted fit ([" + (lm.fitWtype.empty() ? std::string("weight") : lm.fitWtype) +
            "=" + lm.fitWeight + "]) is not supported in this version of PIET -- the "
            "engine's prediction re-fits the model without the weights, so the column it "
            "could write would hold a different model's predictions. Refusing rather than "
            "writing a number that is not the one asked for into your data. Nothing was "
            "written to your data.",
        "unsupported_option");
  }

  static const std::set<std::string> kSupported = {"xb", "resid", "residuals", "fe"};
  static const std::set<std::string> kUnsupportedStats = {
      "cooksd", "leverage", "hat",     "stdp",     "stdf",   "stdr",  "rstandard",
      "rstudent", "dfbeta",  "pr",     "xbu",      "dresid", "anscombe",
      "deviance", "pearson", "response", "score",  "working",
  };
  bool wantXb = false, wantResid = false, wantMlDefault = false;
  const PredictFamilyM4 family = glmPredict ? PredictFamilyM4::Other : predictFamilyM4(lm);
  std::string predictStat;  // the statistic, for the four families above
  bool nativeStat = false;  // computed here rather than by the engine's predict
  if (glmPredict) {
    // The ML option surface, per Stata: bare `predict newvar` = the model's
    // default statistic (pr after logit/probit, n after poisson); the explicit
    // spelling of that default (`pr` / `n`) is accepted; `xb` is the linear
    // index. EVERYTHING else refuses BY NAME — including `resid`/`residuals`,
    // which after an ML fit is a DIFFERENT statistic in Stata (a Pearson
    // residual, not y - xb), so serving the OLS shape under that name would be
    // BUG-204's silent-wrong all over again.
    const std::string defaultStat = (lm.cobraCmd == "poisson") ? "n" : "pr";
    for (const auto& [k, v] : opts) {
      if (k == "quietly" || k == "noisily") continue;
      if (k == "xb" && v.isBool && v.boolValue) { wantXb = true; continue; }
      if (k == defaultStat && v.isBool && v.boolValue) { wantMlDefault = true; continue; }
      return engineErrorBlockM4(
          "predict " + k + ": not supported after `" + lm.cobraCmd +
          "` in this version of PIET (supported there: the default `" + defaultStat +
          "` statistic and `xb`).");
    }
    if (!wantXb) wantMlDefault = true;  // bare `predict newvar` = the default statistic
  } else if (family != PredictFamilyM4::Other) {
    // BUG-596: regress / areg / xtreg,fe / reghdfe answer their OWN statistic
    // vocabulary (the block above computePredictNativeM4). The engine keeps the
    // two it already writes as Stata defines them; everything else is computed
    // below from e(b), e(V) and the engine refit's estimation sample.
    if (auto rej = resolvePredictStatM4(family, lm, opts, predictStat)) return *rej;
    const std::string served = predictEngineServedM4(family, predictStat);
    wantXb = served == "xb";
    wantResid = served == "resid";
    nativeStat = served.empty();
  } else {
  for (const auto& [k, v] : opts) {
    if (kUnsupportedStats.count(k)) {
      // B4 P1: `unsupported_option` -> r(198). This refusal defaulted to
      // `engine_error`, i.e. rc 1 — a code no Stata command answers with, and
      // the one the standing rule reserves for nothing at all. An option this
      // build will not take is r(198), whatever the reason.
      return engineErrorBlockM4("predict " + k +
                                ": not supported (PIET supports xb/resid/fe). The " + k +
                                " statistic requires influence diagnostics not yet implemented.",
                                "unsupported_option");
    }
    // Audit fix: this check used to also require `v.isBool && v.boolValue`,
    // so it only ever caught unknown BOOLEAN flags — an unknown VALUED
    // option (e.g. a typo'd `predict yhat, level(95)`, where `level` isn't
    // a real predict() option and v.isBool is false) matched neither this
    // branch nor kUnsupportedStats above, and fell through completely
    // silently (defaulting to plain `xb`) instead of naming the bad
    // option. Any key not in kSupported/kUnsupportedStats/quietly/noisily
    // is unrecognized regardless of whether it was written as a bare flag
    // or a valued option.
    if (!kSupported.count(k) && k != "quietly" && k != "noisily") {
      // B4 P1: same rc correction as the branch above — see its comment.
      return engineErrorBlockM4("predict " + k +
                                ": not supported (PIET supports xb/resid/fe). The " + k +
                                " statistic requires influence diagnostics not yet implemented.",
                                "unsupported_option");
    }
  }

  for (const auto& [k, v] : opts) {
    if (k == "xb" && v.isBool && v.boolValue) wantXb = true;
    if ((k == "resid" || k == "residuals") && v.isBool && v.boolValue) wantResid = true;
  }
  if (!wantXb && !wantResid) wantXb = true;
  }

  // BUG-127: `predict <type> newvar` used to take the storage-type token AS
  // the new variable's name and silently discard the real name (`predict
  // double yh, xb` created a column literally named `double`; `yh` never
  // existed — gate-① silent-wrong). Recognize an optional leading
  // storage-type token the way gen/egen's stripStorageType does: `double`
  // is accepted and stripped (PIET stores every numeric column as
  // f64 = double anyway); any NARROWER numeric type is an honest reject —
  // silently storing wider than Stata would is a ① divergence.
  std::vector<std::string> nameToks = varlist;
  bool declaredDouble = false;  // BUG-598: recorded on the new column below
  if (nameToks.size() >= 2) {
    std::string t = lowerM4(nameToks[0]);
    if (t == "double") {
      nameToks.erase(nameToks.begin());
      declaredDouble = true;
    } else if (t == "float" && family == PredictFamilyM4::Reghdfe) {
      // reghdfe's predict writes double whatever type it is given:
      // predict-e-xbd.log P_res_xb/xbd/d/residuals (`predict float Q_<stat>`)
      // are rc 0 and type_xbd reads `double`. The BUG-598 rule below records it.
      nameToks.erase(nameToks.begin());
    } else if (t == "float" || t == "byte" || t == "int" || t == "long") {
      // unsupported_option -> r(198): this refusal used to be engine_error, rc 1.
      return engineErrorBlockM4("predict: storage type " + t +
                                    " not supported — PIET stores f64/double",
                                "unsupported_option");
    }
  }
  std::string newvar = nameToks.empty() ? std::string() : nameToks[0];
  if (newvar.empty()) {
    return engineErrorBlockM4("predict: a new variable name is required, e.g. `predict yhat`.",
                              "syntax_error");
  }
  // BUG-597: `predict z` with a variable z already in the data REPLACED it at
  // rc 0 -- the engine's predict writes its column by name, and set_f64_column
  // overwrites (measured on main: `generate double z = 7` / `predict z, xb`
  // left z holding the fitted values, rc 0). Stata's predict creates a NEW
  // variable and answers r(110) "variable z already defined", writing nothing.
  // Card first, datastore as the fallback (residentReadCard's contract), so a
  // dirty virtual head is not snapshotted just to answer a name question.
  {
    bool exists = false;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const VarInfo& vi : *card) {
        if (vi.name == newvar) exists = true;
      }
    } else {
      Datastore* cur = nullptr;
      try {
        cur = session.datastore();
      } catch (const std::exception& e) {
        return engineErrorBlockM4(std::string("predict: ") + e.what());
      }
      exists = cur != nullptr && cur->has(newvar);
    }
    if (exists) return ojson(eval::engineError("variable " + newvar + " already defined"));
  }

  // BUG-516: predict computes on the LIVE frame and ADDS one column to it.
  //
  // `--data` used to be lm.dataPath -- the frame as it stood when the model was
  // FIT -- and the engine's output (that frame + newvar) was then adopted as the
  // session head. The first predict after a fit hid it (the head still WAS that
  // frame). Every later one silently REPLACED the user's data with the
  // estimation-time frame: `regress y x2 t` / `predict r1, residuals` /
  // `generate foo = 1` / `predict p2, xb` left a dataset with neither r1 nor
  // foo, at rc 0 (StataBE, w2_smalls.log section C: predict_twice rc 0, r1 still
  // summarizes over N 12). Rows dropped since the fit came BACK, a `replace` was
  // undone, a sort was reverted -- all the same adoption.
  //
  // So `--data` is the live head, always. What the fit needs rides separately:
  //  * head unmoved (the first predict): the historical argv, byte for byte.
  //  * head moved, no absorbed FE: the estimation-time sample goes in as
  //    `--fit-data`, the split BUG-163 built -- the engine fits THERE and
  //    evaluates the named coefficients over every live row, which is Stata's
  //    rule (current rows, current x values, the fitted coefficients).
  //  * head moved, absorbed FE: `--fit-data` refuses `--fe`, so the live frame
  //    is refit -- after fitColumnsUnchangedM4 has PROVED that is the same fit.
  //
  // BUG-163 (W:P5), unchanged: `--data` is the FULL dataset -- Stata's `predict`
  // writes a value for every row whose covariates are non-missing, not just the
  // rows the model was fitted on -- while the coefficients must come from the
  // FITTED sample (`--fit-data`/`--fit-keepif`).
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::string liveHead = *session.currentHbin();
  const bool headMoved = liveHead != lm.dataPath;
  std::vector<std::string> args = {"predict", "--data", liveHead, "--y", lm.y, "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x)};
  if (!headMoved) {
    if (!lm.fitDataPath.empty()) {
      args.push_back("--fit-data");
      args.push_back(lm.fitDataPath);
    } else if (!lm.fitKeepif.empty()) {
      args.push_back("--fit-keepif");
      args.push_back(lm.fitKeepif);
    }
  } else {
    std::string fitFile = lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath;
    {
      // The refit reads the estimation-time sample AS A FILE. BUG-518: a
      // working frame that a resident mutation advanced before anything wrote
      // it to disk had none (`regress` / `generate` / `predict`: the fit-time
      // head only ever existed in the worker) -- SessionState::setLastModel now
      // pins that frame to disk at the fit, so this branch is the remainder:
      // a snapshot that failed (BUG-384's WARN), or an `estimates restore` of a
      // model whose scratch file reclaimSupersededScratch has since released
      // (exclusion (7) protects only the LIVE model's paths). Named here,
      // because the alternative -- fitting whatever the live frame now holds --
      // is a different model at rc 0.
      std::error_code fec;
      if (!std::filesystem::exists(cecon::to_native_path(fitFile), fec)) {
        return engineErrorBlockM4(
            "predict: the data `" + (lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd) +
                "` was fitted on are no longer available to re-fit from in this version of "
                "PIET (the dataset changed before that working frame was written to disk). "
                "Re-run the estimation command, then predict. Nothing was written to your "
                "data.",
            "unsupported_option");
      }
    }
    const bool restrictedFit = !lm.fitDataPath.empty() || !lm.fitKeepif.empty();
    if (!lm.fe.empty() && !restrictedFit) {
      std::string why;
      if (!fitColumnsUnchangedM4(session, lm, why)) {
        return engineErrorBlockM4(
            "predict after a model with absorbed fixed effects is not supported once the "
                "model's variables or the observations have changed since the fit, in this "
                "version of PIET (" + why + "). The engine predicts an absorbed-effects "
                "model by re-fitting it, and re-fitting the changed data would be a "
                "different model. Re-run the estimation command, then predict. Nothing was "
                "written to your data.",
            "unsupported_option");
      }
      // Proven identical: fitting the live frame IS the fitted model.
    } else {
      // (A restricted fit WITH absorbed FE lands here too and is refused by the
      // engine's own `--fit-data`+`--fe` message, exactly as before the head moved.)
      if (!lm.fitKeepif.empty()) {
        // `--fit-keepif` filters `--data`, which is no longer the frame the fit
        // saw -- materialize the fitted sample instead (boottest's BUG-163 shape).
        try {
          auto ffr = piet::handlers::applyIfAndInFilter(session, fitFile, lm.fitKeepif,
                                                        std::nullopt);
          if (ffr.ok) fitFile = ffr.dataPath;
        } catch (const CobraError& ce) {
          return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
        }
      }
      args.push_back("--fit-data");
      args.push_back(fitFile);
    }
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }
  if (glmPredict) {
    // BUG-297: forward the estimator; the engine re-fits through
    // postest_fit_spec and writes Stata's default statistic (--pred) or the
    // linear index (--xb). wantResid can never be true here (refused above).
    args.push_back("--model");
    args.push_back(lm.cobraCmd);
    if (wantMlDefault) {
      args.push_back("--pred");
      args.push_back(newvar);
    } else {
      args.push_back("--xb");
      args.push_back(newvar);
    }
  } else if (nativeStat) {
    // BUG-596: `args` stays the bare replay (through --fe); the statistic is
    // computed below and the engine's refit only supplies its checks.
  } else if (wantXb) {
    args.push_back("--xb");
    args.push_back(newvar);
  } else {
    args.push_back("--resid");
    args.push_back(newvar);
  }

  std::string outPath = nextPathEst(session.workdir(), "predict");
  if (nativeStat) {
    std::vector<double> vals;
    if (auto rej = computePredictNativeM4(session, lm, family, predictStat, args, vals)) return *rej;
    if (auto rej = writePredictColumnM4(session, liveHead, newvar, vals, outPath)) return *rej;
  } else {
    args.push_back("--out");
    args.push_back(outPath);

    try {
      runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }

  // BUG-517: `predict newvar if exp` fills ONLY the rows that satisfy exp; the
  // rest are missing (StataBE, aer_b4/predict-e-xbd.log C_if_half: `predict
  // double P_e_half if half==1, e` -> "(135 missing values generated)", rc 0).
  // The qualifier used to be accepted and DROPPED -- runPredict never saw it --
  // so every row was filled at rc 0. Masked here, on the engine's output and
  // BEFORE it is adopted, so a bad expression leaves the user's data untouched
  // (Stata creates no variable on that error either). Same cond() fold `generate
  // ... if` uses (handlers.cpp runGen), so the two agree on a missing condition.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    const std::string maskExpr = newvar + "=cond((" + *ifExpr + ")," + newvar + ",.)";
    const std::string maskedPath = nextPathEst(session.workdir(), "predict_if");
    try {
      runCobraJsonM4(translate::injectTsContextForExpr(
          {"eval", "--data", outPath, "--gen", maskExpr, "--out", maskedPath}, maskExpr,
          session));
    } catch (const CobraError& ce) {
      cecon::native_unlink(outPath);
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    cecon::native_unlink(outPath);  // never a head: nothing else can name it
    outPath = maskedPath;
  }

  session.load(outPath);
  session.invalidateDatastore();
  // BUG-598: the new column's storage type, as `: type`/describe and the .dta
  // writer read it (dispatcher.cpp recordDeclaredStataType is the generate twin).
  // `predict double` declares double -- `: type` answered float (w2b_estim.log
  // predict_double: double). reghdfe's predict writes double whatever it is told
  // (predict-e-xbd.log type_xbd: `predict float Q_xbd, xbd` -> double). Otherwise
  // Stata's own default, float, on the implicit plane generate uses (BUG-295) --
  // for the estimators whose predict this file knows to take that default.
  if (declaredDouble || family == PredictFamilyM4::Reghdfe || lm.stataCmd == "reghdfe") {
    session.implicitFloatVars.erase(newvar);
    session.varMeta[newvar].stataType = "double";
  } else if (family != PredictFamilyM4::Other || glmPredict) {
    session.implicitFloatVars.insert(newvar);
  }
  Datastore* ds = session.datastore();

  // C1-CARD-REBUILD-REDUCE A11 sibling: `predict` (engine `predict --xb/--resid`)
  // appends only `newvar` and byte-preserves every other column, so seed the
  // unchanged columns' missing-counts (action "gen" pure-ADD, newVars={newvar})
  // BEFORE the varlist rebuild below skips each column's O(nrows) nMissing scan.
  // Byte-identical vs PIET_NO_NMISS_CACHE=1.
  session.seedNMissForPureAdd(ds, "gen", {newvar}, /*droppedVars=*/{}, /*rewrittenVars=*/{});

  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "gen";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(ds);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = ds ? ds->nRows() : 0;
  out["new_vars"] = ojson::array({newvar});
  return out;
}

// ===========================================================================
// estat (hettest/ovtest/vif/dwatson/bgodfrey/archlm/durbinalt diagnostic
// rewrite; endogenous/overid/firststage IV diagnostics (BS-082, BETA D5
// batch); simple/event/calendar/group/pretrend csdid aggregate readback)

// `estat event, post` — mirror dispatcher.py's WS-D: post e(b)/e(V) from
// cobra's `estat_event` block (Stata Pre_avg/Post_avg/Tm#/Tp# names, full E×E
// covariance) into session.eval.eMatrices so `matrix b = e(b)` / `matrix V =
// e(V)` read the posted event-study coefficients. Any later estimation
// overwrites them (matched by name).
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

ojson runEstat(const std::string& subcmdIn, const std::vector<std::string>& rawVarlistAfterSub,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               const std::optional<std::string>& ifExpr, SessionState& session) {
  std::string sub = lowerM4(subcmdIn);

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
    if (auto err = eval::rejectUnknownOptions(
            opts, diagCmdPeek == "durbinalt" ? kDurbinaltKnownOpts
                                             : std::unordered_set<std::string>{})) {
      return *err;
    }
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    std::string diagCmd = diagIt->second;
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
    // BS-021 (BETA D5 batch): durbinalt's own small/lags() options -> cobra
    // durbinalt's --small/--lags. Oracle D5b: bare = chi2 form (no --small,
    // no --lags — matches the engine's own bare default of lag order 1);
    // `, small lags(2)` = F form with lag order 2 (engine's `--lags "2"`
    // means "test lag orders 1 through 2", i.e. Stata's own lags(#) max-
    // order semantics — live-verified NOT a literal single-lag selector).
    if (diagCmd == "durbinalt") {
      bool small = false;
      std::optional<std::string> lagsVal;
      for (const auto& [k, v] : opts) {
        if (k == "small" && v.isBool && v.boolValue) small = true;
        if (k == "lags" && !v.isBool) lagsVal = v.strValue;
      }
      if (small) args.push_back("--small");
      if (lagsVal.has_value() && !trimM4(*lagsVal).empty()) {
        args.push_back("--lags");
        args.push_back(trimM4(*lagsVal));
      }
    }
    if (ifExpr.has_value()) {
      args.push_back("--keepif");
      args.push_back(*ifExpr);
    }
    args.push_back("--data");
    args.push_back(*session.currentHbin());
    args.push_back("--tidy");
    ojson raw;
    try {
      raw = runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    return cobraToEstimationM4(raw, diagCmd, &session);
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
  if (sub.empty()) {
    return engineErrorBlockM4(
        "estat (no subcommand) — try `estat simple`, `estat event`, "
        "`estat pretrend`, `estat calendar`, or `estat group` after csdid.",
        "unsupported_command");
  }
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value() || lmOpt->cobraCmd != "csdid") {
    return engineErrorBlockM4(
        "estat: no csdid result in memory. Run a `csdid ...` command first "
        "(estat simple/event/pretrend/calendar/group operate on the last csdid).");
  }
  static const std::set<std::string> kValid = {"simple", "event", "calendar", "group", "pretrend"};
  if (!kValid.count(sub)) {
    return engineErrorBlockM4("estat " + sub +
                              ": not supported. After csdid, use one of: "
                              "simple, event, pretrend, calendar, group.");
  }

  // BS-097: only `estat event` reads any options (window()/post below); the
  // other 4 csdid sub-forms (simple/calendar/group/pretrend) read none —
  // branch-scoped by `sub` for the same reason as the diagnostic branch
  // above (event's known set must not leak into the other four, and vice
  // versa).
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kEstatEventKnownOpts = kVocabEstatEvent.known;
  if (auto err = eval::rejectUnknownOptions(opts, sub == "event"
                                                        ? kEstatEventKnownOpts
                                                        : std::unordered_set<std::string>{})) {
    return *err;
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
        argv.push_back("--window");
        argv.push_back(wantWin);
        argv.push_back("--tidy");
        try {
          raw = runCobraJsonM4(argv);
          LastModel updated = lm;
          updated.result = raw;
          session.setLastModel(updated);
        } catch (const CobraError& ce) {
          return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
        }
      }
    }
  }

  if (sub == "event") {
    bool doPost = false;
    for (const auto& [k, v] : opts) {
      if (k == "post") doPost = true;
    }
    if (doPost) postCsdidEventEMatrices(session, raw);
  }

  return csdidToEstimationM4(raw, sub);
}

// ===========================================================================
// BETA campaign Phase D batch D4/D5: time-series + post-estimation/diag/misc
// wirings (BS-077/078/079/080/081/085/021/082/086/083/084/087). Oracle:
// milf-coord/plans/BETA-20260712-campaign/probes/d45-oracle-stata.log
// (StataBE 19.5 on piet/packaging/sample/fatalities.csv, `gen high=(beertax
// >.5)` + `encode state, gen(snum)` + `gen t=_n` + `tsset t`; sections
// D4a-D4f, D5a-D5g). durbinalt's fix (BS-021) and the endogenous/overid/
// firststage IV diagnostics (BS-082) live inside runEstat above; the
// margins ,at() linear-only gate (BS-086) lives inside runMargins above.
// tsfill (BS-081) lives in handlers_data_verbs.cpp (a data-mutation verb,
// alongside gsort/order/recode/etc — this file's tsfill-adjacent handlers
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
// Real Stata's newey also has t()/force/level() options; none have oracle
// coverage this batch and are left as honest rejects via the unknown-
// option net below.
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
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lag" && !v.isBool) lagVal = v.strValue;
  }
  if (!lagVal.has_value() || trimM4(*lagVal).empty()) {
    return engineErrorBlockM4("syntax_error: option lag() required for `newey`", "syntax_error");
  }

  std::vector<std::string> argv = {"newey",  "--y",  y,      "--x",       joinCommaM4(xs),
                                    "--timevar", *session.timevar, "--lag", trimM4(*lagVal)};
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

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
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
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
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

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
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
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
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
ojson runSdtest(const std::string& varName, const std::optional<std::string>& againstVal,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::optional<std::string>& ifExpr, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varName.empty()) {
    return engineErrorBlockM4("syntax_error: `sdtest` requires a variable", "syntax_error");
  }

  std::optional<std::string> byVal;
  for (const auto& [k, v] : opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
  }
  bool hasAgainst = againstVal.has_value() && !trimM4(*againstVal).empty();
  if (byVal.has_value() == hasAgainst) {
    return engineErrorBlockM4(
        "sdtest: specify exactly one of `by(groupvar)` or `= #` (two-sample vs one-sample form).",
        "syntax_error");
  }

  std::vector<std::string> argv = {"sdtest", "--var", varName};
  if (byVal.has_value()) {
    argv.push_back("--by");
    argv.push_back(*byVal);
  } else {
    argv.push_back("--against");
    argv.push_back(trimM4(*againstVal));
  }
  if (ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*ifExpr);
  }
  std::optional<std::string> levelVal;
  for (const auto& [k, v] : opts) {
    if (k == "level" && !v.isBool) levelVal = v.strValue;
  }
  if (levelVal.has_value() && !trimM4(*levelVal).empty()) {
    argv.push_back("--level");
    argv.push_back(trimM4(*levelVal));
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabSdtest.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
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

  ojson block = cobraToEstimationM4(raw, "sdtest", &session);
  block["estimator"] = "sdtest";
  static const char* kSdtestFields[] = {
      "F", "df1", "df2", "p_lower", "p_upper", "mode", "sd", "sd_1", "sd_2",
      "groups", "combined", "level", "chi2", "sd0", "se", "ci_lo", "ci_hi", "var",
  };
  for (const char* k : kSdtestFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  return block;
}

// hausman (BS-083): `hausman NAME1 NAME2` — the classic Stata idiom
// `xtreg,fe` + `estimates store A` + `xtreg,re` + `estimates store B` +
// `hausman A B`. Validates both eststo/estimates-store names exist AND are
// the fe/re pair of the SAME spec (same depvar, same regressor SET; one fe
// one re), then calls cobra's OWN `hausman --y --x --ivar` directly (the
// engine recomputes BOTH sides itself from raw data — it does NOT consume
// the two stored coefficient tables at all, unlike real Stata's own
// hausman, which diffs two ALREADY-FITTED e(b)/e(V)). Oracle D5e: chi2=2.62,
// p=0.2697, per-coef b_fe/b_re/diff (beertax diff=376.6603, unemp
// diff=-.6916189) — byte-for-byte engine match.
//
// DISCLOSED LIMITATION: eststo's own snapshot (eststo_esttab.cpp's
// makeSnapshot) captures only {coeftable, n_obs, r2, depvar, estimator, fe}
// — no x-regressor list, dataPath, or ivar field of its own. x is
// reconstructed from the stored coeftable's own row names (dropping
// "_cons") — lossless for THIS purpose (set membership, not display
// order). depvar is a SECOND, independent gap: live-verified `cobra xtreg
// --ivar ...` (the RE path)'s own raw JSON carries NO "depvar" field at all
// (unlike `cobra feols --fe ...`, which does), so the RE snapshot's depvar
// is ALWAYS empty — the depvar cross-check below only fires when the RE
// side actually has one (never today), leaning on the regressor-SET check
// as the real "same specification" guard in the meantime. The FE store is
// identified by estimator=="feols" AND a non-null
// single-key "fe" (absorbed) object (xtreg,fe routes through
// cobraToEstimationM4(raw,"feols") with --fe=panelvar, which populates
// raw["absorbed"]={panelvar:k}; a PLAIN `feols`/`regress` fit has no
// absorb() and so no "fe" object at all here — this is a real, non-spurious
// signal). The RE store is identified by estimator=="xtreg_re" (cobra's own
// raw "estimator" string for the --ivar RE path, live-verified — NOT
// "xtreg", which is only the block's display default). Neither eststo's
// snapshot NOR runXtreg's own cacheLastModelM4 records the RE fit's ivar
// anywhere at all — this handler uses the CURRENT session.panelvar (the
// active `xtset` context) as the shared ivar for BOTH sides, and requires
// it to equal the FE store's own absorbed-variable name (its one source of
// ground truth), i.e. hausman assumes the user has not re-`xtset`'d to a
// different panel variable since fitting the RE side. Similarly there is no
// stored dataPath in the snapshot either — this uses session.currentHbin()
// (the CURRENTLY loaded dataset), i.e. hausman also assumes the same
// dataset is still loaded. Both are disclosed, reasonable-workflow
// assumptions (matching the D5e oracle's own single-tsset, single-dataset
// session), not silent wrong answers.
ojson runHausman(const std::string& name1, const std::string& name2,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  // BS-097: real Stata `hausman` takes constant/sigmamore/sigmaless/force/
  // alleqs/skipeqs()/equations()/df()/tconsistent()/tefficient() ([R]
  // hausman, r.pdf p.1091-1092) — none implemented by this handler, it
  // always recomputes both sides from raw data via cobra's own --tidy
  // hausman, which has no such knobs — every one of them now reads as an
  // honest named reject instead of a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kHausmanKnownOpts = kVocabHausman.known;
  // BUG-105 secondary ⑤: the options above are documented Stata syntax, not
  // typos, so they get the "not yet implemented" wording; anything else
  // (a genuine bogus token) keeps the plain "not allowed" reject.
  const std::unordered_set<std::string>& kHausmanKnownButUnimplemented = kVocabHausman.unimplemented;
  if (auto err =
          eval::rejectUnknownOptions(opts, kHausmanKnownOpts, kHausmanKnownButUnimplemented)) {
    return *err;
  }
  const ojson* s1 = session.estimatesGet(name1);
  const ojson* s2 = session.estimatesGet(name2);
  if (s1 == nullptr) {
    return engineErrorBlockM4(
        "hausman: no stored estimation result named `" + name1 + "`. Run `eststo " + name1 +
        "` (or `estimates store " + name1 + "`) after fitting the model.",
        "unsupported_option");
  }
  if (s2 == nullptr) {
    return engineErrorBlockM4(
        "hausman: no stored estimation result named `" + name2 + "`. Run `eststo " + name2 +
        "` (or `estimates store " + name2 + "`) after fitting the model.",
        "unsupported_option");
  }

  auto xset = [](const ojson& snap) {
    std::set<std::string> xs;
    if (snap.contains("coeftable") && snap["coeftable"].is_array()) {
      for (const auto& row : snap["coeftable"]) {
        std::string nm = row.value("name", std::string());
        if (nm != "_cons" && !nm.empty()) xs.insert(nm);
      }
    }
    return xs;
  };
  auto isFeSnap = [](const ojson& snap) {
    std::string estimator = snap.value("estimator", std::string());
    return (estimator == "feols" || estimator == "reghdfe") && snap.contains("fe") &&
           snap["fe"].is_object() && snap["fe"].size() == 1;
  };
  auto isReSnap = [](const ojson& snap) {
    return snap.value("estimator", std::string()) == "xtreg_re";
  };

  const ojson *feSnap = nullptr, *reSnap = nullptr;
  std::string feName, reName;
  if (isFeSnap(*s1) && isReSnap(*s2)) {
    feSnap = s1;
    reSnap = s2;
    feName = name1;
    reName = name2;
  } else if (isFeSnap(*s2) && isReSnap(*s1)) {
    feSnap = s2;
    reSnap = s1;
    feName = name2;
    reName = name1;
  } else {
    return engineErrorBlockM4(
        "hausman: `" + name1 + "` and `" + name2 + "` must be one `xtreg ,fe` result and one "
        "`xtreg ,re` result of the SAME specification (e.g. `xtreg y x, fe` / `eststo A` / "
        "`xtreg y x, re` / `eststo B` / `hausman A B`) — got estimator=`" +
        s1->value("estimator", std::string()) + "` and estimator=`" +
        s2->value("estimator", std::string()) + "`.",
        "unsupported_option");
  }

  // Depvar cross-check is BEST-EFFORT only: live-verified the engine's own
  // `cobra xtreg --ivar ...` (RE) raw JSON has NO "depvar" field at all
  // (unlike `cobra feols --fe ...`, which does) — cobraToEstimationM4
  // therefore always stores an EMPTY depvar for the RE side, and eststo's
  // snapshot inherits that emptiness. Comparing an always-empty RE depvar
  // against the FE side's real one would reject EVERY legitimate fe/re
  // pair, so this only runs the check when the RE snapshot actually HAS a
  // non-empty depvar (inert today, activates automatically if a future
  // engine build starts populating it) — the regressor-SET check right
  // below is the real safety net for "same specification" in the meantime.
  std::string reDepvar = reSnap->value("depvar", std::string());
  if (!reDepvar.empty() && feSnap->value("depvar", std::string()) != reDepvar) {
    return engineErrorBlockM4(
        "hausman: `" + feName + "` and `" + reName + "` have different dependent variables (`" +
        feSnap->value("depvar", std::string()) + "` vs `" + reDepvar +
        "`) — hausman requires the fe/re pair of the SAME specification.",
        "unsupported_option");
  }
  std::set<std::string> xsFe = xset(*feSnap), xsRe = xset(*reSnap);
  if (xsFe != xsRe) {
    return engineErrorBlockM4(
        "hausman: `" + feName + "` and `" + reName + "` fit different regressor lists — hausman "
        "requires the fe/re pair of the SAME specification.",
        "unsupported_option");
  }

  std::string ivar;
  for (const auto& [k, v] : (*feSnap)["fe"].items()) {
    (void)v;
    ivar = k;  // single-key object, validated by isFeSnap() above.
  }
  if (!session.panelvar.has_value() || session.panelvar->empty() || *session.panelvar != ivar) {
    return engineErrorBlockM4(
        "hausman: run `xtset " + ivar + " ...` (the SAME panel variable `" + feName +
        "` was fit with) before calling `hausman` — the current xtset context (" +
        session.panelvar.value_or(std::string("<none>")) + ") does not match.",
        "unsupported_option");
  }
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  std::vector<std::string> xs(xsFe.begin(), xsFe.end());
  std::vector<std::string> argv = {"hausman",       "--y",     feSnap->value("depvar", std::string()),
                                    "--x",           joinCommaM4(xs), "--ivar", ivar,
                                    "--data",        *session.currentHbin(), "--tidy"};
  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "hausman";
  block["depvar"] = feSnap->value("depvar", std::string());
  block["n_obs"] = raw.value("n_obs", 0);
  static const char* kHausmanFields[] = {"chi2", "p", "dof", "coefs"};
  for (const char* k : kHausmanFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  return block;
}

// chow (BS-084): `chow depvar indepvars [if], by(groupvar)` — a PIET-ONLY
// convenience wrapper; real Stata has NO `chow` command at all (the
// oracle's own D5f section computes the classic Chow F test by hand: fit
// pooled + both subsamples, chowF = ((rss_pooled - rss_0 - rss_1)/k) /
// ((rss_0+rss_1)/(n-2k)) — see d45-oracle-stata.do's own comment "Chow test
// income~beertax split by high: pooled + subsamples"). cobra's own `chow`
// CLI computes the IDENTICAL classic homoskedastic SSR-based F test as its
// "classic" block (live-verified: engine classic.F=28.07417107825576,
// df1=2, df2=332 — byte-for-byte the oracle's own manually-computed
// chowF=28.074171, df1=2, df2=332) — plus a bonus heteroskedasticity-robust
// Wald variant (raw.robust, no oracle coverage, not selftest-pinned) this
// handler also surfaces. vce()/cluster() are NOT forwarded this batch (no
// oracle coverage for cobra chow's own --vcov/--cluster surface) — left as
// honest rejects via the unknown-option net.
ojson runChow(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `chow` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> byVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
  }
  if (!byVal.has_value() || trimM4(*byVal).empty()) {
    return engineErrorBlockM4("syntax_error: `chow` requires by(groupvar)", "syntax_error");
  }

  std::vector<std::string> argv = {"chow", "--y", y, "--x", joinCommaM4(xs), "--by",
                                    trimM4(*byVal)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabChow.known;
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

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "chow";
  block["depvar"] = y;
  block["n_obs"] = raw.value("n_obs", 0);
  static const char* kChowFields[] = {"G", "by", "group_sizes", "classic", "robust"};
  for (const char* k : kChowFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  block["note"] =
      "Chow test (real Stata has no built-in `chow` command — this is a PIET convenience wrapper "
      "reproducing the classic manual F-test: pooled vs split-sample SSR).";
  return block;
}

// mkspline (BS-087, part): `mkspline stub = varname [if], cubic [nknots(#) |
// knots(v1,v2,...)]` -> cobra eval --mkspline "stub=var, cubic[, nknots(N)|,
// knots(...)]" --out new.hbin (a data-mutation verb, like egen — adds
// stub1..stubK-1 columns). Oracle D5g: nknots(4) gives 3 columns
// (inc_sp1/2/3, pinned means/sds) — see the fix disclosure below;
// only the CUBIC form is implemented. Real Stata's LINEAR/piecewise-linear
// default (mkspline with NO `cubic` keyword) has NO engine equivalent —
// live-verified the engine's `eval --mkspline` op ALWAYS computes a
// restricted cubic spline basis regardless of whether `cubic` was typed in
// the spec string (omitting it does NOT fall back to a linear basis, it
// silently produces the SAME cubic one). Rather than silently answer
// linear-mkspline with a cubic result, PIET requires `cubic` to be typed
// and has no linear mkspline of its own.
//
// ENGINE FIX (cobra 0.25.0): `eval --mkspline`'s own `nknots(#)` clause
// now works. LOWERCASE `nknots(N)` — the engine's own documented spelling,
// per `cobra eval`'s usage banner — places knots at Harrell's percentile
// method (e.g. nknots(4) -> the 5/35/65/95 percentiles of the source
// variable) and returns the correct N-1 basis columns, verified live
// against StataBE (byte-for-byte knot positions and basis-column summary
// stats). The old failure mode was a paren_arg substring collision at the
// CLI arg-parsing layer, present through 0.24.0: LOWERCASE `nknots(N)`
// hard-errored "mkspline: need at least 3 knots" for EVERY N tried from 1
// to 10 (the value never actually parsed), while UPPERCASE `NKNOTS(N)` was
// silently ACCEPTED but the value N was DISCARDED — it always fell back to
// the same hardcoded 5-knot/4-column default regardless of N. PIET forwards
// `nknots(N)` in LOWERCASE, per the engine's own documented spelling — the
// D5g oracle's own exact invocation (`nknots(4)`) now succeeds cleanly and
// is oracle-pinned in D5g's own selftest leg (selftest.cpp). The explicit
// `knots(v1,v2,...)` form (caller-supplied exact knot positions, bypassing
// nknots() entirely) also works mechanically (live-verified) and is
// independently wired/checked in its own selftest leg (a fresh stub name,
// since it now runs alongside the fixed nknots() leg rather than as its
// sole proof of mechanical wiring).
ojson runMkspline(const std::string& stubIn, const std::string& srcVarIn,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   const std::optional<std::string>& ifExpr, SessionState& session) {
  // C4-L4B (FIX-C4L4B-RESIDUALS ①): entry validation reads a NAME and the
  // column NAME SET and nothing else, so the resident card answers both (the
  // D1 rule) and the datastore opens LAZILY. Escape (PIET_NO_RESIDENT_READ)
  // keeps the eager entry — and the whole stateless tail below — VERBATIM, so
  // the floor RED arm reproduces the pre-C4 path with one env.
  if (piet::residentReadDisabled()) {
    if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
      return eval::noDataLoaded();
    }
  } else if (!session.hasData()) {
    return eval::noDataLoaded();
  }
  std::string stub = trimM4(stubIn);
  std::string srcVar = trimM4(srcVarIn);
  if (stub.empty() || stub.find_first_of(" \t") != std::string::npos) {
    return engineErrorBlockM4(
        "syntax_error: `mkspline` requires a single STUB name on the left of `=` (the multi-"
        "newvar form `mkspline v1 v2 v3 = x` is not supported in this version of PIET).",
        "syntax_error");
  }
  if (srcVar.empty() || srcVar.find_first_of(" \t") != std::string::npos) {
    return engineErrorBlockM4("syntax_error: `mkspline` requires a single source variable",
                              "syntax_error");
  }
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto cardHas = [&](const std::string& name) -> bool {
    for (const auto& vi : *card) {
      if (vi.name == name) return true;
    }
    return false;
  };
  if (card != nullptr ? !cardHas(srcVar) : !ensureDs()->has(srcVar)) {
    return engineErrorBlockM4("Variable '" + srcVar + "' not found.");
  }
  if (ifExpr.has_value()) {
    return engineErrorBlockM4(
        "mkspline: `if` is not supported in this version of PIET (live-verified: the engine's "
        "`eval --keepif` DROPS non-matching rows from the WHOLE dataset rather than leaving the "
        "new spline variables missing for them the way real Stata's if-qualified generate-family "
        "commands do — forwarding it would silently shrink the dataset).",
        "unsupported_option");
  }

  bool cubic = false;
  for (const auto& [k, v] : opts) {
    if (k == "cubic" && v.isBool && v.boolValue) cubic = true;
  }
  if (!cubic) {
    return engineErrorBlockM4(
        "mkspline: only the `cubic` (restricted cubic spline) form is supported in this version "
        "of PIET — the engine's eval --mkspline op has no linear/piecewise-linear basis (it "
        "always computes a cubic basis regardless of the `cubic` keyword), so PIET requires "
        "`cubic` to be typed explicitly rather than silently answer the linear form with a cubic "
        "result.",
        "unsupported_option");
  }
  for (const auto& [k, v] : opts) {
    if (k == "displayknots" && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `displayknots` is a valid Stata option that this version of PIET does not yet "
          "implement for `mkspline` (no engine CLI equivalent — knot positions are never "
          "surfaced back to PIET).",
          "unsupported_option");
    }
  }

  std::optional<std::string> nknotsVal, knotsVal;
  for (const auto& [k, v] : opts) {
    if (k == "nknots" && !v.isBool) nknotsVal = v.strValue;
    if (k == "knots" && !v.isBool) knotsVal = v.strValue;
  }
  if (nknotsVal.has_value() && knotsVal.has_value()) {
    return engineErrorBlockM4("mkspline: nknots() and knots() may not be combined.",
                              "syntax_error");
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabMkspline.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
  }

  std::string mkArg = stub + "=" + srcVar + ", cubic";
  if (nknotsVal.has_value() && !trimM4(*nknotsVal).empty()) {
    mkArg += ", nknots(" + trimM4(*nknotsVal) + ")";
  } else if (knotsVal.has_value() && !trimM4(*knotsVal).empty()) {
    mkArg += ", knots(" + trimM4(*knotsVal) + ")";
  }

  std::set<std::string> beforeSet;
  if (card != nullptr) {
    for (const auto& vi : *card) beforeSet.insert(vi.name);
  } else {
    std::vector<std::string> before = ensureDs()->names();
    beforeSet.insert(before.begin(), before.end());
  }

  std::vector<std::string> argv = {"eval", "--data", *session.currentHbin(), "--mkspline", mkArg};
  std::string outPath = nextPathEst(session.workdir(), "mkspline");
  argv.push_back("--out");
  argv.push_back(outPath);

  {
    // C4-L4B: `eval --mkspline` is a single TERMINAL mutation — its `--out`
    // becomes the head and is never read back as a file — so it takes the
    // resident frame plane, the same opt-in `recode`'s no-`if` arm has held
    // since MAXPERF Stage 2. Escape: pre-C4 stateless routing, verbatim.
    std::optional<SessionState::ResidentEligible> mksplineResident;
    if (!piet::residentReadDisabled()) mksplineResident.emplace(session);
    try {
      runCobraJsonM4(argv);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
  }

  session.load(outPath);
  // C4-L4B resident tail: fold the frame delta into the shadow card instead of
  // invalidate+reopen. mkspline is the one verb in this class that cannot NAME
  // its new columns up front — the engine reports the generated basis names on
  // stderr only (apps/cmds/cmds_data.cpp:144), never in the JSON PIET reads —
  // so `new_vars` is diffed off the freshly FOLDED card and appended last,
  // which is exactly where the stateless block below has always put it
  // (ordered_json preserves insertion order; the two blocks are key-for-key
  // identical). The delta is consumed here, so a fold that DECLINES falls
  // through to the stateless rebuild below with nothing left pending — the
  // same landing spot mutationResult uses for its own nullopt.
  if (!piet::residentReadDisabled()) {
    if (auto folded = session.tryResidentMutationCard("mkspline", {}, {}, {})) {
      if (const std::vector<VarInfo>* after = session.residentReadCard()) {
        ojson out = std::move(*folded);
        ojson newVars = ojson::array();
        for (const auto& vi : *after) {
          if (!beforeSet.count(vi.name)) newVars.push_back(vi.name);
        }
        out["new_vars"] = newVars;
        return out;
      }
    }
  }
  session.invalidateDatastore();
  Datastore* dsAfter = session.datastore();

  std::vector<std::string> newVarsVec;
  if (dsAfter != nullptr) {
    for (const auto& n : dsAfter->names()) {
      if (!beforeSet.count(n)) newVarsVec.push_back(n);
    }
  }
  ojson newVars = ojson::array();
  for (const auto& n : newVarsVec) newVars.push_back(n);

  // C1-CARD-REBUILD-REDUCE A11 sibling: `mkspline` (engine `eval --mkspline`)
  // appends only the new basis columns (newVarsVec = names present after but not
  // before) and byte-preserves every other column, so seed the unchanged
  // columns' missing-counts (kPureAdd branch 1, action "mkspline") BEFORE the
  // varlist rebuild below skips each column's O(nrows) nMissing scan. Byte-
  // identical vs PIET_NO_NMISS_CACHE=1.
  session.seedNMissForPureAdd(dsAfter, "mkspline", newVarsVec, /*droppedVars=*/{},
                             /*rewrittenVars=*/{});

  ojson out;
  out["type"] = "data_mutation";
  out["action"] = "mkspline";
  ojson varlistAfter = ojson::array();
  auto vlist = session.varlistWithMeta(dsAfter);
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  session.updateNMissCache(vlist);  // C2-F13a: keep the cache current for the next mutation
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = dsAfter ? dsAfter->nRows() : 0;
  out["new_vars"] = newVars;
  return out;
}


}  // namespace piet::handlers
