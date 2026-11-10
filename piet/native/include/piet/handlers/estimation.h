// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: model fits and the statistical commands routed with them.
// Implemented under piet/native/src/estimation/; a few live beside related code in
// src/postestimation/ (suest, boottest, sdtest, hausman, chow), src/datamgmt/ (xi,
// swindex), src/descriptive/ (centile), src/time/ (tsfilter hp) and src/graph/ (binscatter).
#ifndef PIET_NATIVE_HANDLERS_ESTIMATION_H
#define PIET_NATIVE_HANDLERS_ESTIMATION_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// Bounded, expression-compiled generic lf likelihoods; no evaluator execution.
nlohmann::ordered_json runMl(const eval::ParsedCommand& parsed, SessionState& session);

// `regress`/`reg`/`feols`/`reghdfe` y x1 x2 [, robust|cluster(v)|noconstant|
// level(#)|absorb(...)] — C++ port of the classify()->build_estimation_argv()
// ->cobra feols->_cobra_to_estimation() pipeline, IV-free vertical slice
// (plain OLS/feols/HDFE with iid, hc1, or cluster vcov). M3a: TS-context
// injection (translate.cpp) wired in. The TODO(M3b) that sat here said IV/xtreg
// routing was still out of scope; both have their own registry rows and handlers
// (estimation/instrumental_variables.cpp, estimation/panel_estimation.cpp), so the marker was retired 2026-08-18 (INSPECT-2608
// census). THIS function stays the IV-free route — that part was never stale.
// `stataCmd` is the ORIGINAL Stata command word (defaults to "regress").
// It is display-label-only: it never changes the cobra invocation (still
// always "feols", since cobra has no separate reghdfe estimator — reghdfe
// IS feols --fe) nor LastModel.cobraCmd (re-estimation/eststo/estat replay
// must keep invoking cobra's real "feols" binary). It only lets the
// resulting block's "estimator" field read "reghdfe" when the user typed
// `reghdfe`, matching Stata's own output label.
// `dataOverride` (P4 wave): when non-null, fit against THIS hbin path instead of
// the session's current dataset, and skip the internal `if`/`in` materialization
// — the caller has already produced a slice with those qualifiers applied. The
// only caller is the `by:`/`bysort:` group driver, which must run one fit per
// contiguous group; passing the group's own slice is what makes each fit see its
// group's sample and nothing else. nullptr (the default) is the ordinary path,
// byte-unchanged.
// `residMutationOut` (W2 estimopts, BUG-539): reghdfe's `residuals[(newvar)]`
// adds a column to the live frame AFTER the fit. The handler still returns the
// one estimation block; the data_mutation announcing that column is written
// here for the caller to push as its own block. nullptr means the caller
// cannot carry a second block (the by: driver) and residuals() is refused.
nlohmann::ordered_json runRegress(const eval::ParsedCommand& parsed, SessionState& session,
                                   const std::string& stataCmd = "regress",
                                   const std::string* dataOverride = nullptr,
                                   nlohmann::ordered_json* residMutationOut = nullptr);

// Package-007 lane LB. `leebounds depvar treatvar [if] [in] [, select() level()
// vce(analytic)]` (SSC, Harald Tauchmann) -- Lee (2009) treatment-effect bounds,
// e(b) striped treatvar:lower treatvar:upper. Returns an ARRAY of blocks.
nlohmann::ordered_json runLeebounds(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle H (ESTIM, 2026-09-05) -------------------------------
// E1 aliases. `areg` IS regress with an absorb(), so it delegates rather than
// re-deriving; absorb() missing is r(198), not a silent plain regression.
nlohmann::ordered_json runAreg(const eval::ParsedCommand& parsed, SessionState& session);
// `ivreg2`/`xtivreg2` -> runIvRegress. `stataCmd` is the spelling the user
// typed, which is what the estimator label and the community gate both read.
nlohmann::ordered_json runIvreg2(const eval::ParsedCommand& parsed, SessionState& session,
                                  const std::string& stataCmd);
// E2, computed PIET-side with no engine call.
nlohmann::ordered_json runSpearman(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSktest(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSignrank(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runCentile(const eval::ParsedCommand& parsed, SessionState& session);
// E2, engine-backed. Each ASKS the engine whether its subcommand exists and
// refuses by name when it does not, so a PIET built against an older cobra says
// so instead of producing a wrong number.
nlohmann::ordered_json runTsfilterHp(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLpoly(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLowess(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runPca(const eval::ParsedCommand& parsed, SessionState& session);
// AER scope-2 B2: `oaxaca ..., by(g) pooled` (Jann), twofold with robust suest V.
nlohmann::ordered_json runOaxaca(const eval::ParsedCommand& parsed, SessionState& session);
// X2-GLM (estimation_glm.cpp): `glm` fitted in-process through the engine's
// cecon::glm_fit, Stata's default ML lifecycle (oracle w2_glm_lifecycle). Returns
// an ARRAY of blocks (weight note, iteration log, header, estimation, warning);
// the caller pushes each element. runGlmMargins answers `margins, dydx() [post]`
// when the model in memory is a glm -- the engine's margins cannot (it would
// answer a logit AME), so the dispatcher routes here instead.
nlohmann::ordered_json runGlm(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runGlmMargins(const eval::ParsedCommand& parsed, SessionState& session);
// E3 scaffold stubs (handlers_e3_<cmd>.cpp): each returns an ARRAY holding the
// block `refused:e3_deferred` (rc 199) until its native task replaces the body.
// runLasso answers both `lasso` and `rlasso` (it reads the typed word from
// parsed.cmd). `honestdid` has no handler: its refusal stays in the dispatcher.
nlohmann::ordered_json runSwindex(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runQreg2(const eval::ParsedCommand& parsed, SessionState& session);
// `margins, dydx(varlist) [post]` while the model in memory is a qreg2 (its
// linear-index dy/dx from the posted e(b)/e(V); qreg2_estimation.cpp).
nlohmann::ordered_json runQreg2Margins(const eval::ParsedCommand& parsed, SessionState& session);
// Stata `qreg` with its default vce(iid) (fitted/hsheather), fitted natively with
// the exact LP vertex (qreg2_estimation.cpp). A null reply means "not this
// path's form" (vce(robust), weights, other options): runQreg answers those.
nlohmann::ordered_json runQregNative(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLasso(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLassoPredict(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runLassoCoef(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSuest(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runRitest(const eval::ParsedCommand& parsed, SessionState& session);
// `ritest` from its raw line (dispatcher step 0a-ter): REST is the text after the
// command word, unexpanded -- the exp_list's e()/r()/_b[] are per replicate.
nlohmann::ordered_json runRitestLine(const std::string& rest, SessionState& session);
nlohmann::ordered_json runAcreg(const eval::ParsedCommand& parsed, SessionState& session);
// glm's e(rank): rowsof(V) - diag0cnt(invsym(V)) on the k x k row-major V,
// with invsym's measured drop rule (comment at the definition).
int64_t glmVceRank(const std::vector<double>& V, size_t k);
// The e() entries glm posts that no other estimator clears (e(b) e(V) e(ilog)
// e(gradient), e(varfunct) e(linkt) e(opt) e(technique) e(a)). The dispatcher
// snapshots them while the model in memory is a glm (or its posted margins)
// and, once a command replaces that model, drops each entry that is STILL
// exactly what glm posted -- so `glm` then `regress` then `matrix V = e(V)`
// cannot read the glm's V. An entry the new command wrote itself differs and
// is kept.
struct GlmEStores {
  std::vector<std::pair<std::string, eval::EvalMatrix>> matrices;
  std::vector<std::pair<std::string, std::string>> strings;
};
std::optional<GlmEStores> glmOwnedEStores(const SessionState& session);
void dropStaleGlmEStores(SessionState& session, const GlmEStores& before);
// AER scope-2 B2-4: `teffects nnmatch (ovar mvarlist) (tvar) [if] [in], [gen() ...]`
// (StataCorp [CAUSAL]). `sub` is the sub-command word the dispatcher stripped.
nlohmann::ordered_json runTeffects(const std::string& sub, const eval::ParsedCommand& parsed,
                                   SessionState& session);
// `xi [, prefix() noomit] i.a [i.b*i.c ...]` -- ALWAYS materializes the _I*
// columns; there is no pass-through form.
nlohmann::ordered_json runXi(const eval::ParsedCommand& parsed, SessionState& session);
// The `xi: <command>` prefix form. Materializes exactly as runXi does, then
// hands back the wrapped command with each i. term replaced by the columns it
// produced (`rewritten`) plus the mapping lines to print (`displayText`); the
// caller re-dispatches. Returns a non-null error block on refusal.
nlohmann::ordered_json runXiPrefix(const std::vector<std::string>& terms,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::string& wrapped, SessionState& session,
                                    std::string* rewritten, std::string* displayText);

// `xtreg Y X..., fe|re [vce(...)]` — fe routes to feols with --fe=panelvar
// (build_estimation_argv("feols", extra={"fe":panelvar})); re routes to
// cobra xtreg with --ivar=panelvar (no vcov support). Requires a prior
// `xtset ivar [tvar]`.
nlohmann::ordered_json runXtreg(const eval::ParsedCommand& parsed, SessionState& session);

// `logit`/`probit` Y X... [, vce(robust)] — C++ port of the _DIRECT_MAP path
// through build_estimation_argv (GLM --vce swap applied).
// --- P7 wave: vector autoregression -----------------------------------------
// One engine verb backs all three; vargranger and irf create re-render the fit.
nlohmann::ordered_json runVar(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runVargranger(SessionState& session);
nlohmann::ordered_json runIrf(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: multiple imputation -------------------------------------------
// The mlong rows are written by the engine (--out-data); SessionState keeps only
// the declaration, so `mi impute` can reject on un-set or unregistered data.
nlohmann::ordered_json runMi(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: dynamic panel -------------------------------------------------
nlohmann::ordered_json runXtabond(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: nonlinear -----------------------------------------------------
// The parenthesised expression goes to the engine VERBATIM off the raw line —
// PIET must not grow a second parser for it.
nlohmann::ordered_json runNlGmm(const std::string& cobraCmd, const std::string& rawLine,
                                 const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: multilevel ----------------------------------------------------
// Random INTERCEPT only; random slopes and a second `||` level reject.
nlohmann::ordered_json runMixed(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: survey design -------------------------------------------------
// The design is session state; `svy:` rewrites the inner command into the
// ordinary engine call plus --svy/--psu/--strata. The inner-command whitelist
// fails CLOSED — an unrecognised inner command rejects rather than running
// unweighted.
nlohmann::ordered_json runSvyset(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSvyPrefix(const std::string& innerRaw, SessionState& session);

// --- P7 wave: IV maximum likelihood -----------------------------------------
// Shares ivregress's `(endog = instruments)` varlist parser rather than growing
// a second one.
nlohmann::ordered_json runIvMl(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                                SessionState& session);

// --- P7 wave: survival ------------------------------------------------------
// stset is an engine verb that materializes _t0/_t/_d/_st as real columns;
// SessionState keeps only the declaration, which is what lets stcox/streg reject
// honestly on un-stset data and cluster vce(robust) on the subject id.
nlohmann::ordered_json runStset(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runStcox(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runStreg(const eval::ParsedCommand& parsed, SessionState& session);

// --- P7 wave: ordered / multinomial / zero-inflated / fractional MLE ---------
// These take the engine's ML-family `--vce` spelling (not `--vcov`) and each
// honest-rejects the option shapes its estimator does not implement.
nlohmann::ordered_json runOrdered(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                                  SessionState& session);
nlohmann::ordered_json runMlogit(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runZeroInflated(const std::string& cobraCmd,
                                       const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runFracreg(const eval::ParsedCommand& parsed, SessionState& session);

nlohmann::ordered_json runLogitProbit(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                                       SessionState& session);

// `csdid`/`drdid` Y X..., ivar(ID) time(T) gvar(G) [method() comparison()
// pscoretrim() window() cluster()] — C++ port of build_csdid_argv +
// _csdid_to_estimation("simple") (the initial estimation block; estat
// event/calendar/group/pretrend read the cached result back via runEstat).
nlohmann::ordered_json runCsdid(const eval::ParsedCommand& parsed, SessionState& session);

// `ivregress 2sls|gmm|liml depvar [exog...] (endog = instruments) [if] [,
// vce(robust|cluster C)]` / `ivreghdfe depvar [exog...] (endog=instr),
// absorb(F) [vce(...)]` / the `reghdfe depvar (endog=instr), absorb(F)`
// paren form (dispatcher.cpp routes all three here) — BS-063/BS-064 (BETA
// D1). `method` is "2sls"|"gmm"|"liml" (ivregress's own sub-command word;
// ivreghdfe/the reghdfe-paren form always pass "2sls" — neither has a gmm/
// liml sub-variant). `allowAbsorb` gates whether absorb() is read as --fe:
// true for ivreghdfe/reghdfe-paren, false for plain ivregress (which has no
// absorb() option in real Stata — an unknown-option reject otherwise).
// `stataCmd` is the literal Stata command word actually typed ("ivregress"/
// "ivreghdfe"/"reghdfe") — display-label-only, drives the block's
// "estimator" field ("ivregress 2sls"/"ivregress gmm"/"ivregress liml"/
// "ivreghdfe").
// `forceSmall` (B4 P0-C) asks for cobra's `--small` t/F small-sample correction
// independently of `allowAbsorb`. `ivreg2 ..., small` is exactly that request,
// and before this argument existed the caller parsed the flag and discarded it,
// so the option was certified and dropped. `allowAbsorb` still implies --small
// on its own — ivreghdfe reports the absorbed correction by default — so this
// only ever ADDS the flag, never removes it.
// 2026-09-09 (BUG-474): `allowAbsorb` used to mean TWO things — "this spelling accepts
// absorb()" AND "this spelling reports small-sample SEs" — and the second meaning was
// invisible at the call sites. Flipping plain `ivregress` to accept absorb() therefore
// silently moved its standard errors off Stata's ASYMPTOTIC default (D1a beertax se
// 13643.678807996115 -> 13684.467176). The selftest caught it; the fix is to say the two
// things separately. `smallDefault` is the spelling's own convention (ivreghdfe reports the
// absorbed correction by default, plain ivregress does not); `forceSmall` is a user request
// arriving as `ivreg2 ..., small`. Either is sufficient, so the two are OR-ed.
nlohmann::ordered_json runIvRegress(const std::string& method, const eval::ParsedCommand& parsed,
                                     bool allowAbsorb, const std::string& stataCmd,
                                     SessionState& session, bool smallDefault, bool forceSmall);
// NEITHER small flag has a default, deliberately. When `smallDefault` carried one, the ivreg2
// caller inherited it and `ivreg2` bare moved from se[mpg] 65.331433 to 66.232599 — the same
// class of silent value change this split exists to prevent, one layer down. An omitted
// argument must be a compile error, not a different number.

// `reg3 (y1 x1 x2) (y2 x3 y1) [(y3 ...) ...] [, sure]` — BS-065 (BETA D1).
// Stata's own default is 3SLS (`sure` opts into SUR); cobra's reg3 CLI
// default is the OPPOSITE (SUR unless --method 3sls), so this always sends
// --method explicitly rather than relying on either side's default.
// `sureg (y1 x1 x2) (y2 x3)` is the same SUR fit (stataCmd "sureg": SUR always,
// e(cmd) `sureg`; sureg.ado's default is two-step FGLS, no dfk/small).
nlohmann::ordered_json runReg3(const eval::ParsedCommand& parsed, SessionState& session,
                               const std::string& stataCmd = "reg3");

// `boottest paramname [, reps(#) seed(#) weights(rademacher|webb) null(#)]`
// — wild cluster bootstrap post-estimation (BS-066), requires a preceding
// vce(cluster ...) fit in session.lastModel() (honest reject otherwise).
nlohmann::ordered_json runBoottest(const eval::ParsedCommand& parsed, SessionState& session);

// `tobit depvar indepvars [if], ll[(#)] ul[(#)]` — BS-069 (BETA D2). At
// least one of ll()/ul() required; a bare ll/ul (no numeric value) is
// PIET-computed from the depvar's own observed min/max (cobra's tobit CLI
// has no bare-flag equivalent). `level()` is never forwarded (cobra tobit
// has no --level surface at all — honest reject).
nlohmann::ordered_json runTobit(const eval::ParsedCommand& parsed, SessionState& session);

// `heckman depvar indepvars [if], select(selvar = selvars) [twostep]` —
// BS-070 (BETA D2). cobra's heckman implements ONLY the two-step Heckit
// estimator (its bare/no-flag default already computes it — the CLI's own
// `--twostep` flag is NEVER forwarded, it triggers a cobra stdout/exit-code
// masking bug); Stata's default (no `twostep`, full MLE) has no engine
// equivalent — honest reject.
nlohmann::ordered_json runHeckman(const eval::ParsedCommand& parsed, SessionState& session);

// `qreg depvar indepvars [if] [, quantile(#) level(#)]` — BS-071 (BETA D2).
// quantile() accepts a (0,1) fraction or a (1,100) percentage (normalized to
// cobra's --tau); unlike tobit/ivregress/reg3, qreg's `level()` DOES forward
// (live-verified: cobra qreg's --level genuinely changes the CI/SE/t/p).
nlohmann::ordered_json runQreg(const eval::ParsedCommand& parsed, SessionState& session);

// `mediate (Y [controls]) (M [controls]) (T, continuous(#level0 #level1))`
// — BS-072 (BETA D2), Stata 18 syntax. Only the continuous-treatment,
// two-explicit-level form is in beta scope (the oracle's sole working
// invocation); every other treatment shape honest-rejects naming this form.
// ENGINE-PARITY, NOT STATA-PARITY: cobra's `mediate` computes a plain
// linear (no treatment-mediator interaction) per-unit decomposition, while
// Stata's continuous(#L0 #L1) fits a richer interaction model evaluated at
// the specific contrast — verified these are NOT numerically equivalent
// (see the .cpp function-header comment for the full reconstruction/
// disclosure). Point estimates below are the ENGINE's own values, not a
// Stata-matching result.
nlohmann::ordered_json runMediate(const eval::ParsedCommand& parsed, SessionState& session);

// `poisson`/`ppmlhdfe`/`fepois` Y X... [if] [, vce(robust|cluster C)
// offset(O) absorb(F)]` — BS-073 (BETA D3 batch). `stataCmd` is the literal
// command word typed ("poisson"/"ppmlhdfe"/"fepois" — all three route
// through cobra's `fepois`; D0 finding: WITHOUT --fe it reproduces Stata's
// bare `poisson` exactly). absorb() only allowed for ppmlhdfe/fepois (real
// Stata's `poisson` has no absorb() option) -> --fe. offset() is a VERIFIED
// literal passthrough (cobra's --offset expects the same already-logged
// exposure variable Stata's own offset() does — no ln() transform needed).
// ppmlhdfe/fepois (fe form) DEFAULT to vce(robust) when the user gives no
// vce() at all, matching real ppmlhdfe's own default (live-verified against
// the D3d oracle, which never types vce() yet the Stata table header reads
// "Robust std. err."). See runPoisson's header comment (estimation/count_estimation.cpp)
// for the ENGINE FINDING disclosure (HC1 small-sample-correction gap).
nlohmann::ordered_json runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                                   SessionState& session);

// `nbreg` Y X... [if] [, vce(robust|cluster C) offset(O) noconstant]` —
// BS-074 (BETA D3 batch), NB2 ("dispersion(mean)", Stata's default). See
// runNbreg's header comment (estimation/count_estimation.cpp) for the ENGINE BLOCKER
// disclosure: cobra's nbreg fails to converge on data whose fitted
// intercept sits away from zero, including the D3e oracle's own data — a
// live-verified engine-side gap, not a wiring gap.
nlohmann::ordered_json runNbreg(const eval::ParsedCommand& parsed, SessionState& session);

// `xtpoisson`/`xtlogit` Y X..., fe [vce(robust)]` — BS-075 (BETA D3 batch).
// `,fe` ONLY: the engine has no random-effects or population-averaged
// xtpoisson/xtlogit estimator, so Stata's own DEFAULT (bare / `,re` / `,pa`
// — real Stata's default for both commands IS random-effects) honest-
// rejects naming `,fe` as the only supported form. Requires a prior
// `xtset ivar [tvar]` (same panelvar spine xtreg's `,fe` router reads).
// vce(cluster ...)/cluster() honest-rejects for both (live-verified silent
// no-op on the engine's own CLI — see the .cpp header comment); vce(robust)
// genuinely changes the SE for both (not a masking no-op). xtpoisson's
// offset() forwards (xtpois's own CLI has --offset); xtlogit's does not.
nlohmann::ordered_json runXtPoisson(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runXtLogitFe(const eval::ParsedCommand& parsed, SessionState& session);

// `drdid` Y X..., ivar(ID) time(T) treatment(D) dripw` — BS-076 (BETA D3
// batch). The classic two-period/two-group doubly-robust DiD, mirroring
// ONLY the tests/validate/drdid2x2.do fixture's exact invocation surface
// (the sole oracle-covered form; other method()s and the bare/no-`dripw`
// form honest-reject). Routes through the SAME engine csdid --method dripw
// machinery `csdid`/`estat` already use:
// zero cobra-side csdid code touched, only a PIET-side synthesized `gvar`
// column (treatment()!=0 -> gvar=max(time), the post period; ==0 -> gvar=0,
// CS21's never-treated sentinel) built via cobra's GENERIC `eval --gen`
// data-prep verb. Restricted to exactly 2 distinct time() values (the
// command's own "2x2" design, matching real drdid.ado's same restriction).
// See runDrdid's header comment (estimation/did_two_period.cpp) for the live-verified
// oracle match (ATT/SE to ~1e-13 relative).
nlohmann::ordered_json runDrdid(const eval::ParsedCommand& parsed, SessionState& session);

// -- BETA campaign Phase D batch D4/D5: time-series + post-estimation/diag/
// misc wirings (BS-077/078/079/080/081/085/021/082/086/083/084/087). Oracle:
// internal capture d45-oracle-stata.log
// (StataBE 19.5 on piet/packaging/sample/fatalities.csv, `gen high=(beertax
// >.5)` + `encode state, gen(snum)` + `gen t=_n` + `tsset t`; sections
// D4a-D4f, D5a-D5g). See each handler's own .cpp header comment
// (in the estimation/ and postestimation/ handler files) for the full oracle-match / engine-gap disclosure.

// `newey depvar indepvars [if], lag(#)` — BS-077. lag() is REQUIRED (matches
// real Stata, which errors "option lag() required" if omitted); the time
// variable is read from the session's own `tsset timevar` spine.
nlohmann::ordered_json runNewey(const eval::ParsedCommand& parsed, SessionState& session);

// `prais depvar indepvars [if] [, corc twostep rhotype(regress|tscorr)]` —
// BS-078. Requires a prior `tsset timevar`.
nlohmann::ordered_json runPrais(const eval::ParsedCommand& parsed, SessionState& session);

// `dfuller varname [if] [, lags(#) noconstant|drift|trend]` / `pperron
// varname [if] [, lags(#) noconstant|trend]` — BS-079. `cobraCmd` selects
// which engine CLI ("dfuller"/"pperron"); pperron has no `drift` form in
// real Stata (honest-rejected there, unlike dfuller). Requires a prior
// `tsset timevar`.
nlohmann::ordered_json runDfullerPperron(const std::string& cobraCmd,
                                          const eval::ParsedCommand& parsed, SessionState& session);

// `wntestq varname [if] [, lags(#)]` — BS-080. No timevar requirement (the
// engine's own CLI has no --timevar flag — rows are read in on-disk order).
// The engine reproduces Stata's own default lag rule (min(n/2-2,40))
// automatically when lags() is omitted — PIET never computes it itself.
nlohmann::ordered_json runWntestq(const eval::ParsedCommand& parsed, SessionState& session);

// `sdtest varname [if], by(groupvar)` (two-sample variance-ratio test) /
// `sdtest varname [if] = #` (one-sample, against a fixed SD) — BS-085.
// Exactly one of by()/`= #` is required. The `= #` form has no oracle
// coverage this batch — wired but not Stata-pinned.
nlohmann::ordered_json runSdtest(const std::string& varName,
                                  const std::optional<std::string>& againstVal,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::optional<std::string>& ifExpr, SessionState& session);

// `hausman NAME1 NAME2` — BS-083. Validates both eststo/estimates-store
// names exist and are the `xtreg ,fe`/`xtreg ,re` pair of the SAME spec
// (same depvar, same regressor set, current xtset panelvar matches the fe
// store's own absorbed variable), then calls cobra's own `hausman --y --x
// --ivar` directly (the engine recomputes BOTH sides from raw data itself).
// See the .cpp header comment for the eststo-snapshot limitations this
// works around (no stored ivar/dataPath — uses the CURRENT xtset context
// and currently-loaded dataset, a disclosed workflow assumption).
nlohmann::ordered_json runHausman(const std::string& name1, const std::string& name2,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// `chow depvar indepvars [if], by(groupvar)` — BS-084. A PIET-ONLY
// convenience wrapper (real Stata has NO `chow` command at all — the
// oracle's own D5f section computes it by hand: pooled vs split-sample SSR
// F-test); routes to cobra's own `chow --by` CLI, which computes the
// IDENTICAL classic homoskedastic F test as its "classic" block.
nlohmann::ordered_json runChow(const eval::ParsedCommand& parsed, SessionState& session);

// -- Arc F: the seven QE / RD commands (estimation/regression_discontinuity.cpp, graph/binscatter.cpp, estimation/eventstudyinteract.cpp, estimation/did_imputation.cpp, estimation/synthetic_did.cpp) ---------------------
// Each parses its Stata line, calls the already-merged cobra verb, and renders
// the table Stata prints. All seven return an ARRAY of blocks (one `display`
// per printed line, then ONE structured `qe_result` block) — the
// shape of the resampling prefixes (estimation/bootstrap_prefix.cpp), NOT the single-block shape runCsdid/runMixed
// use — so the dispatcher call sites splice rather than push_back. binscatter
// is the deliberate exception that prints zero lines: real Stata prints none,
// so its array carries the structured block only.
nlohmann::ordered_json runRdrobust(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runRddensity(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runRdplot(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runBinscatter(const eval::ParsedCommand& parsed, SessionState& session);

nlohmann::ordered_json runEventstudyinteract(const eval::ParsedCommand& parsed,
                                              SessionState& session);
nlohmann::ordered_json runDidImputation(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runSdid(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_ESTIMATION_H
