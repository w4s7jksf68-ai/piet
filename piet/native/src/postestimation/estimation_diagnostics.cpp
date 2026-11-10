// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Estimation diagnostics.
#include "piet/handlers/estimates_tables.h"
#include "piet/handlers/postestimation.h"
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
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation_internal.h"
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
    if (auto err = eval::rejectUnknownOptions(
            opts, diagCmdPeek == "durbinalt" ? kDurbinaltKnownOpts
                                             : std::unordered_set<std::string>{})) {
      return *err;
    }
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
    ojson diagBlock = cobraToEstimationM4(raw, diagCmd, &session);
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
  if (sub.empty()) {
    return engineErrorBlockM4(
        "estat (no subcommand) — try `estat simple`, `estat event`, "
        "`estat pretrend`, `estat calendar`, or `estat group` after csdid.",
        "unsupported_command");
  }
  const auto& lmOpt = session.lastModel();
  if (sub == "ic" && session.eval.eStrings.get("cmd") == "ml") {
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
    // FU-437: a subcommand this build does not serve here is r(198), never the
    // default engine_error's rc 1 (`regress` then `estat ic` printed 1).
    return engineErrorBlockM4(
        "estat: no csdid result in memory. Run a `csdid ...` command first "
        "(estat simple/event/pretrend/calendar/group operate on the last csdid).",
        "unsupported_option");
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
  if (sub == "simple" || sub == "event") {
    const std::optional<CsdidPostingM4> P =
        csdidPostingM4(raw, sub == "simple" ? std::string("agg_simple") : std::string("estat_event"));
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
        eval::EvalMatrix rb;
        rb.data.push_back(P->b);
        rb.rownames = {"y1"};
        rb.colnames = P->names;
        eval::EvalMatrix rV;
        for (size_t i = 0; i < K; ++i)
          rV.data.emplace_back(P->V.begin() + static_cast<std::ptrdiff_t>(i * K),
                               P->V.begin() + static_cast<std::ptrdiff_t>((i + 1) * K));
        rV.rownames = P->names;
        rV.colnames = P->names;
        session.eval.rMatrices.emplace_back("table", std::move(tbl));
        session.eval.rMatrices.emplace_back("b", std::move(rb));
        session.eval.rMatrices.emplace_back("V", std::move(rV));
        session.eval.rStrings.set("agg", sub);
        eval::declareRPosted(session.eval, "estat", {"table", "b", "V", "agg"});
      }

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
