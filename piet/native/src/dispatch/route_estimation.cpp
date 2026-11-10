// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "graphics_support.h"
#include "results_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
namespace piet::dispatch_detail {
bool route_estimation(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks,
    const std::string& rawCmd,
    const std::string& graphLineTyped) {


  // BUG-105 (WS-K): `r` is GONE from this arm and from the regress registry
  // row. Measured live: `r price mpg` and `re price mpg` are r(199) in real
  // Stata — PIET was fabricating an abbreviation, which is the same defect
  // class as omitting one. `reg`/`regr`/`regre`/`regres` are the measured set
  // and now arrive here already normalised to "regress"; the literals kept
  // below are the two DISTINCT command names this one arm serves.
  if (cmd == "regress" || cmd == "feols" || cmd == "reghdfe") {
    // BS-063 (BETA D1): a varlist token containing '(' means an inline
    // `(endog = instruments)` IV block — real Stata syntax for THIS still
    // splits in two:
    //   - `reghdfe y (d=z), absorb(F)` IS the ivreghdfe shape (the task's
    //     own framing; cobra has no separate ivreghdfe estimator either —
    //     it is `cobra iv --fe`, same as ivreghdfe itself routes to below)
    //     -> route to the real IV handler, labelled "ivreghdfe" either way
    //     the user spelled the command.
    //   - `regress`/`reg`/`r`/`feols` with the SAME inline-paren shape is
    //     the OLD pre-`ivregress` Stata IV syntax, long retired in modern
    //     Stata in favor of `ivregress`/`ivreg2` — the D1 oracle doesn't
    //     probe this reject directly, so this keeps the pre-existing
    //     honest reject, just re-pointing the message at the real command
    //     (`ivregress`) instead of implying regress/feols will ever grow
    //     inline IV support of their own.
    bool hasIvBlock = std::any_of(parsed.varlist.begin(), parsed.varlist.end(),
                                  [](const std::string& t) { return t.find('(') != std::string::npos; });
    if (hasIvBlock) {
      if (cmd == "reghdfe") {
        // reghdfe 6.x has no IV syntax: its varlist parse stops at the `(`
        // with "parentheses unbalanced" r(132), leaving e() alone
        // (w2_esample.log:967-975 D6_reghdfe_iv_aw_cluster `132|57|57|`).
        // The IV fit is `ivreghdfe`, routed below.
        blocks.push_back(ojson(eval::errorBlock("syntax_error", "parentheses unbalanced",
                                                std::nullopt, std::nullopt, std::nullopt,
                                                "r(132)")));
        return true;
      }
      blocks.push_back(ojson(eval::unsupportedOption(
          "regress (IV block)",
          "Old-style inline IV syntax `regress y (endog=instruments)` is not supported by "
          "`regress`/`feols` in this version of PIET. Use `ivregress 2sls y (endog=instruments)` "
          "instead.")));
      return true;
    }
    // M3a: TS-context injection lands in handlers::runRegress itself
    // (translate.cpp's hasTsToken/injectTsContext, MI-083 CONTRACT-engine
    // §16) — an L./F./D./S. varlist token now injects --xtset/--tsset from
    // session.panelvar/timevar (set by the xtset/tsset handlers below)
    // before cobra sees it; with no TS context set, cobra's own
    // `ts_no_tsset` engine error surfaces, re-wrapped with the `[code]`
    // prefix runRegress's catch clause adds (matches the 2 Eval
    // M2-ratification carve-out cells byte-for-byte — see handlers.cpp).
    // `reghdfe` IS `feols --fe` under the hood (cobra has no separate
    // reghdfe estimator — verified bit-for-bit against the Stata
    // oracle); it is routed through the exact same runRegress path as
    // regress/reg/r/feols, with only the display "estimator" label
    // differing (handlers.cpp's stataCmd param).
    // BUG-539 (W2 estimopts): `reghdfe ..., residuals[(v)]` also creates a
    // column. The handler returns the estimation block alone and hands the
    // data_mutation back through this out-parameter, so it lands as a block
    // of its own (a handler returning an array is the ARRAY-vs-BLOCK abort).
    ojson residMutation;
    blocks.push_back(ojson(handlers::runRegress(parsed, session, cmd, nullptr, &residMutation)));
    if (residMutation.is_object() && !residMutation.empty()) blocks.push_back(residMutation);
    return true;
  }


  // ── AER-SCOPE1 bundle H (ESTIM, 2026-09-05) ────────────────────────────────
  // E1: aliases onto estimators PIET already has.
  if (cmd == "areg") {
    blocks.push_back(ojson(handlers::runAreg(parsed, session)));
    return true;
  }

  if (cmd == "ivreg2" || cmd == "xtivreg2") {
    blocks.push_back(ojson(handlers::runIvreg2(parsed, session, cmd)));
    return true;
  }

  // E2 computed PIET-side.
  if (cmd == "spearman") {
    blocks.push_back(ojson(handlers::runSpearman(parsed, session)));
    return true;
  }

  if (cmd == "sktest") {
    ojson skBlock = ojson(handlers::runSktest(parsed, session));
    // BUG-1490: Stata's r() after sktest, the last variable's scalars and r(table)
    // plus the hidden P_* scalars and r(Utest) (StataBE 19.5, rlife/side2.do
    // `: r(scalars)` `N p_skew p_kurt chi2 p_chi2 P_chi2 P_kurt P_skew`,
    // `: r(matrices)` `table Utest`); the hidden four refuse by name (unposted).
    if (skBlock.value("type", std::string()) != "error")
      eval::declareRPosted(session.eval, "sktest",
                           {"N", "p_skew", "p_kurt", "chi2", "p_chi2", "P_chi2", "P_kurt",
                            "P_skew", "table", "Utest"});
    blocks.push_back(std::move(skBlock));
    return true;
  }

  if (cmd == "signrank") {
    blocks.push_back(ojson(handlers::runSignrank(parsed, session)));
    return true;
  }

  if (cmd == "centile") {
    blocks.push_back(ojson(handlers::runCentile(parsed, session)));
    return true;
  }

  // `tsfilter` takes a REQUIRED filter word (hp, bw, bk, cf) as its first
  // token. Only `hp` is in scope; the others are named as unknown SUBcommands
  // rather than as an unknown command, because the head verb does exist.
  if (cmd == "tsfilter") {
    const std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    if (sub != "hp") {
      blocks.push_back(ojson(eval::unsupportedCommand(
          "tsfilter " + (sub.empty() ? std::string("<none>") : sub), std::nullopt, std::nullopt,
          "piet_gap", eval::UnsupportedCommandKind::kUnknownSubcommand)));
      return true;
    }
    ojson r_tsfilter = handlers::runTsfilterHp(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_tsfilter.is_array()) {
      for (const auto& b : r_tsfilter) blocks.push_back(b);
    } else if (!r_tsfilter.is_null()) {
      blocks.push_back(r_tsfilter);
    }
    return true;
  }

  if (cmd == "lpoly" || cmd == "lowess") {
    const bool isLpoly = (cmd == "lpoly");
    ojson r_smooth =
        isLpoly ? handlers::runLpoly(parsed, session) : handlers::runLowess(parsed, session);
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"]. The
    // drawn form's graph block is one element of the array: it gets the
    // accepted-not-applied note and becomes the graph in memory (BS-011).
    auto finish = [&](ojson b) {
      graphAcceptedNotApplied(b, cmd, parsed.opts, isLpoly ? kLpolyApplied : kLowessApplied);
      registerGraphOutput(b, session, parsed.opts, graphLineTyped);
      blocks.push_back(std::move(b));
    };
    if (r_smooth.is_array()) {
      for (const auto& b : r_smooth) finish(b);
    } else if (!r_smooth.is_null()) {
      finish(r_smooth);
    }
    return true;
  }

  if (cmd == "pca") {
    // BUG-733: pca posts its e() before setLastModel (runFitWritingEFirst).
    ojson r_pca = runFitWritingEFirst(session, [&] { return handlers::runPca(parsed, session); });
    // A handler here returns EITHER one object, an EMPTY array (silent
    // success), or an array of blocks. Pushing an array where an object belongs
    // is json type_error.306 at the first consumer that reads ["type"].
    if (r_pca.is_array()) {
      for (const auto& b : r_pca) blocks.push_back(b);
    } else if (!r_pca.is_null()) {
      blocks.push_back(r_pca);
    }
    return true;
  }

  if (cmd == "oaxaca") {
    // AER scope-2 B2 (2026-09-08): left the E3 refusal list above once the
    // pooled twofold inference was verified against live Stata. Same
    // array-or-object shape rule as the H1 arms.
    ojson r_ox =  // BUG-733: e() before setLastModel (runFitWritingEFirst)
        runFitWritingEFirst(session, [&] { return handlers::runOaxaca(parsed, session); });
    if (r_ox.is_array()) {
      for (const auto& b : r_ox) blocks.push_back(b);
    } else if (!r_ox.is_null()) {
      blocks.push_back(r_ox);
    }
    return true;
  }

  if (cmd == "xi") {
    ojson xr = handlers::runXi(parsed, session);
    if (xr.is_array())
      for (const auto& b : xr) blocks.push_back(b);
    else if (!xr.is_null())
      blocks.push_back(xr);
    return true;
  }

  // E3: registered so the NAME resolves and the refusal carries a named cause
  // (`refused:e3_deferred`) that the route drift guard can tell apart from an
  // unrecognised token. The nearest native command for each lives on its
  // registry row's handlerTag, which is where a reader looks for it.
  if (cmd == "teffects") {
    // AER scope-2 B2-4 (2026-09-09): left the E3 refusal list below once
    // `nnmatch`'s match-set rule was measured (and the REPORT's first rule
    // REFUTED) against live Stata. The sub-command word is REQUIRED Stata
    // syntax and lands as parsed.varlist[0], the same shape `ivregress 2sls`
    // handles further down; every OTHER teffects estimator keeps its own named
    // refusal rather than sharing one cause.
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `teffects` requires an estimator: nnmatch, ra, ipw, ipwra, aipw or "
          "psmatch",
          "syntax_error")));
      return true;
    }
    eval::ParsedCommand rest = parsed;
    const std::string sub = lower(rest.varlist[0]);
    rest.varlist.erase(rest.varlist.begin());
    ojson r_te =  // BUG-733: e() before setLastModel (runFitWritingEFirst)
        runFitWritingEFirst(session, [&] { return handlers::runTeffects(sub, rest, session); });
    if (r_te.is_array()) {
      for (const auto& b : r_te) blocks.push_back(b);
    } else if (!r_te.is_null()) {
      blocks.push_back(r_te);
    }
    return true;
  }

  // X2-GLM: `glm` left E3. runGlm returns an ARRAY of blocks (weight note,
  // iteration log, header, estimation block, convergence warning) -- pushed one
  // by one, never as a single array-valued block (the type_error.306 class).
  if (cmd == "glm") {
    ojson r_glm = handlers::runGlm(parsed, session);
    if (r_glm.is_array()) {
      for (const auto& b : r_glm) blocks.push_back(b);
    } else if (!r_glm.is_null()) {
      blocks.push_back(r_glm);
    }
    return true;
  }

  // E3 scaffold: each name routes to its own stub TU (handlers_e3_<cmd>.cpp),
  // which returns an ARRAY pushed element by element (never as one block --
  // the type_error.306 class). `honestdid` has no TU (source/solver question
  // open) and keeps the literal refusal below.
  {
    ojson r_e3;
    bool e3Routed = true;
    if (cmd == "swindex") r_e3 = handlers::runSwindex(parsed, session);
    else if (cmd == "qreg2") r_e3 = handlers::runQreg2(parsed, session);
    else if (cmd == "lasso" || cmd == "rlasso") r_e3 = handlers::runLasso(parsed, session);
    else if (cmd == "suest") r_e3 = handlers::runSuest(parsed, session);
    else if (cmd == "ritest") r_e3 = handlers::runRitest(parsed, session);
    else if (cmd == "acreg") r_e3 = handlers::runAcreg(parsed, session);
    else if (cmd == "leebounds") r_e3 = handlers::runLeebounds(parsed, session);
    else e3Routed = false;
    if (e3Routed) {
      if (r_e3.is_array()) {
        for (const auto& b : r_e3) blocks.push_back(b);
      } else if (!r_e3.is_null()) {
        blocks.push_back(r_e3);
      }
      return true;
    }
  }

  if (cmd == "honestdid") {
    blocks.push_back(ojson(eval::refusedCommand(cmdTyped, "e3_deferred")));
    return true;
  }


  if (cmd == "ivregress") {
    // BS-063: `ivregress 2sls|gmm|liml depvar [exog...] (endog =
    // instruments) [if] [, vce(...)]` — the estimator word ("2sls"/"gmm"/
    // "liml") is REQUIRED Stata syntax, landing as parsed.varlist[0] (the
    // tokenizer has no concept of a command's own required sub-command
    // token — same shape `estat SUBCMD` already handles elsewhere in this
    // file).
    if (parsed.varlist.empty()) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `ivregress` requires an estimator: 2sls, gmm, or liml", "syntax_error")));
      return true;
    }
    std::string method = lower(parsed.varlist[0]);
    if (method != "2sls" && method != "gmm" && method != "liml") {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `ivregress " + parsed.varlist[0] + "` is not a recognized estimator "
          "(supported: 2sls, gmm, liml)",
          "syntax_error")));
      return true;
    }
    eval::ParsedCommand rest = parsed;
    rest.varlist.erase(rest.varlist.begin());
    // BUG-474: `ivregress ..., absorb()` is rc 0 in Stata's OWN base
    // ivregress.ado (`which ivregress` -> StataNow's ado/base/i/
    // ivregress.ado, tests/eval/golden/aer_b4/b5_which.log — not a
    // third-party rule), measured
    // tests/eval/golden/aer_b4/b5_absorb_class.log:38-62
    // (`B5REC|ivregress|absorb|0|`). This is the mirror of BUG-473
    // (plain `regress` must REJECT absorb()) — the two point in opposite
    // directions through the same runIvRegress `allowAbsorb` gate, so only
    // THIS call site (the plain `ivregress` command) flips; `ivreg`
    // (pre-Stata-11 spelling, below) is untouched — not measured.
    // BUG-1227: `first`'s report is a display block of its own, ahead of the
    // estimation block, as Stata prints the first stages above the IV table.
    ojson firstReport;
    ojson ivBlock =
        ojson(handlers::runIvRegress(method, rest, /*allowAbsorb=*/true, "ivregress", session,
                                     /*smallDefault=*/false, /*forceSmall=*/false, &firstReport));
    if (firstReport.is_object() && !firstReport.empty()) blocks.push_back(firstReport);
    blocks.push_back(std::move(ivBlock));
    return true;
  }


  if (cmd == "ivreg") {
    // AER-SCOPE1 bundle A: `ivreg y x (d = z)` is Stata's PRE-11 spelling of
    // 2SLS. Unlike `ivregress` it carries no estimator word -- the estimator
    // IS 2SLS -- so the varlist arrives without the sub-command token that arm
    // strips, and the whole difference between the two commands is that one
    // token. Routed to the same handler with the method supplied here rather
    // than a second copy of the IV path that could drift from it.
    blocks.push_back(
        ojson(handlers::runIvRegress("2sls", parsed, /*allowAbsorb=*/false, "ivregress", session,
                                     /*smallDefault=*/false, /*forceSmall=*/false)));
    return true;
  }


  if (cmd == "ivreghdfe") {
    // BS-063: `ivreghdfe depvar [exog...] (endog=instr), absorb(F)
    // [vce(...)]` — real Stata's ivreghdfe has no 2sls/gmm/liml sub-command
    // word of its own (always the 2SLS-family estimator); `absorb()` is
    // required in practice (that's the entire point of the command over
    // plain `ivregress`) but left to the handler's own absorb() reader to
    // enforce/ignore, matching how `reghdfe` itself doesn't hard-require
    // `absorb()` at the dispatch layer either.
    blocks.push_back(
        ojson(handlers::runIvRegress("2sls", parsed, /*allowAbsorb=*/true, "ivreghdfe", session,
                                     /*smallDefault=*/true, /*forceSmall=*/false)));
    return true;
  }


  if (cmd == "reg3") {
    // BS-065: `reg3 (y1 x1 x2) (y2 x3 y1) [, sure]` — multiple parenthesised
    // equation groups directly in the varlist (no leading positional
    // depvar/exog before the first paren, unlike ivregress); runReg3 does
    // its own paren-group parsing from parsed.varlist.
    ojson reg3Block = ojson(handlers::runReg3(parsed, session));
    blocks.push_back(std::move(reg3Block));
    return true;
  }


  if (cmd == "sureg") {
    // sureg is `reg3 <eqns>, sure` with e(cmd)
    // `sureg`: the same handler, SUR always (w2d_uncovered.log:1073).
    ojson suregBlock = ojson(handlers::runReg3(parsed, session, "sureg"));
    blocks.push_back(std::move(suregBlock));
    return true;
  }


  if (cmd == "boottest") {
    // BS-066: post-estimation wild cluster bootstrap — reconstructs the
    // cobra wildboot call from session.lastModel() (requires a preceding
    // vce(cluster ...) fit; runBoottest honest-rejects otherwise).
    // BUG-1175: boottest's printed report is a display block of its own, ahead of
    // the estimation block.
    ojson report;
    ojson bootBlock = ojson(handlers::runBoottest(parsed, session, &report));
    if (report.is_object() && !report.empty()) blocks.push_back(report);
    blocks.push_back(std::move(bootBlock));
    return true;
  }


  if (cmd == "var") {
    blocks.push_back(ojson(handlers::runVar(parsed, session)));
    return true;
  }


  if (cmd == "vargranger") {
    blocks.push_back(ojson(handlers::runVargranger(session)));
    return true;
  }


  if (cmd == "irf") {
    blocks.push_back(ojson(handlers::runIrf(parsed, session)));
    return true;
  }


  if (cmd == "mi") {
    blocks.push_back(ojson(handlers::runMi(parsed, session)));
    return true;
  }


  if (cmd == "xtabond") {
    blocks.push_back(ojson(handlers::runXtabond(parsed, session)));
    return true;
  }


  if (cmd == "ml") {
    blocks.push_back(ojson(handlers::runMl(parsed, session)));
    return true;
  }


  if (cmd == "nl" || cmd == "gmm") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runNlGmm reads instruments/onestep; vce/robust stay known so nl's OWN precise reject fires);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNlGmm.known, kVocabNlGmm.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    ojson r = ojson(handlers::runNlGmm(cmd, rawCmd, parsed, session));
    if (r.is_array()) {
      for (auto& b : r) blocks.push_back(std::move(b));  // a weighted nl's note, then its table
    } else {
      blocks.push_back(std::move(r));
    }
    return true;
  }


  if (cmd == "mixed") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runMixed -> buildP7MleArgv: vce/vcov/robust/cluster);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMixed.known, kVocabMixed.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(ojson(handlers::runMixed(parsed, session)));
    return true;
  }


  if (cmd == "svyset") {
    blocks.push_back(ojson(handlers::runSvyset(parsed, session)));
    return true;
  }


  if (cmd == "ivprobit" || cmd == "ivtobit") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runIvMl reads ll/ul explicitly, then vce/vcov/robust/cluster);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabIvMl.known, kVocabIvMl.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(ojson(handlers::runIvMl(cmd, parsed, session)));
    return true;
  }


  // ---- P7 wave: survival ----
  if (cmd == "stset") {
    blocks.push_back(ojson(handlers::runStset(parsed, session)));
    return true;
  }


  if (cmd == "stcox") {
    blocks.push_back(ojson(handlers::runStcox(parsed, session)));
    return true;
  }


  if (cmd == "streg") {
    blocks.push_back(ojson(handlers::runStreg(parsed, session)));
    return true;
  }


  // ---- P7 wave: ordered / multinomial / zero-inflated / fractional ----
  if (cmd == "ologit" || cmd == "oprobit") {
    blocks.push_back(ojson(handlers::runOrdered(cmd, parsed, session)));
    return true;
  }


  if (cmd == "mlogit") {
    blocks.push_back(ojson(handlers::runMlogit(parsed, session)));
    installMlEsampleAfterFit(blocks.back(), session);  // BUG-1214
    return true;
  }


  if (cmd == "zip" || cmd == "zinb") {
    blocks.push_back(ojson(handlers::runZeroInflated(cmd, parsed, session)));
    return true;
  }


  if (cmd == "fracreg") {
    // BUG-336: this handler consumed its real options but never rejected UNKNOWN
    // ones, so a typo was accepted and the model ran WITHOUT it. The known set is
    // MEASURED from the handler body (runFracreg -> buildP7MleArgv: same four);
    // an EMPTY set here eats the options the handler honours -- that first attempt
    // broke 14 freeze fixtures, and the corpus was never the problem.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFracreg.known, kVocabFracreg.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    blocks.push_back(ojson(handlers::runFracreg(parsed, session)));
    return true;
  }


  if (cmd == "tobit") {
    // BS-069 (BETA D2): `tobit depvar indepvars [if], ll[(#)] ul[(#)]` —
    // runTobit computes a bare ll/ul (no numeric value) from the depvar's
    // own observed min/max itself (cobra's tobit CLI has no bare-flag
    // equivalent); level() always honest-rejects (no --level surface at
    // all on cobra's tobit).
    blocks.push_back(ojson(handlers::runTobit(parsed, session)));
    installMlEsampleAfterFit(blocks.back(), session);  // BUG-1214
    return true;
  }


  if (cmd == "heckman") {
    // BS-070 (BETA D2): `heckman depvar indepvars [if], select(selvar =
    // selvars) [twostep]` — runHeckman NEVER forwards --twostep to the
    // engine (cobra's own --twostep flag triggers a stdout/exit-code
    // masking bug; the bare/no-flag default already computes the two-step
    // result); Stata's default (no `twostep`, full MLE) honest-rejects.
    blocks.push_back(ojson(handlers::runHeckman(parsed, session)));
    return true;
  }


  if (cmd == "qreg") {
    // BS-071 (BETA D2): `qreg depvar indepvars [if] [, quantile(#)
    // level(#)]` — runQreg normalizes quantile()'s fraction-or-percent
    // syntax to cobra's --tau; level() DOES forward here (live-verified:
    // cobra qreg's --level genuinely changes the CI/SE/t/p, unlike
    // tobit/ivregress/reg3's level(), which never forwards).
    // The default vce(iid) is fitted natively (runQregNative: the exact LP vertex,
    // Stata's fitted/hsheather sparsity, e(b)/e(V) with the base/omitted factor
    // columns, w2j_e3_qreg2.log A01/F00); a null reply is a form it does not take.
    {
      ojson r_qn = handlers::runQregNative(parsed, session);
      if (r_qn.is_array()) {
        for (const auto& b : r_qn) blocks.push_back(b);
        return true;
      }
    }
    blocks.push_back(ojson(handlers::runQreg(parsed, session)));
    return true;
  }


  if (cmd == "mediate") {
    // BS-072 (BETA D2): Stata 18 `mediate (Y) (M) (T, continuous(#L0 #L1))`
    // — runMediate parses the three parenthesised blocks itself (depth-
    // aware, since the treatment block nests continuous(...) inside its
    // own parens); only the continuous-treatment, two-explicit-level form
    // is in beta scope. ENGINE-PARITY not Stata-parity — see runMediate's
    // own header comment (estimation_m4.cpp) for the full disclosure.
    blocks.push_back(ojson(handlers::runMediate(parsed, session)));
    return true;
  }


  if (cmd == "xtreg") {
    blocks.push_back(ojson(handlers::runXtreg(parsed, session)));
    return true;
  }


  if (cmd == "logit" || cmd == "logistic" || cmd == "probit" || cmd == "cloglog") {
    std::string cobraCmd = (cmd == "probit" || cmd == "cloglog") ? cmd : "logit";
    ojson block = ojson(handlers::runLogitProbit(cobraCmd, parsed, session));
    installMlEsampleAfterFit(block, session);
    // BS-022 (BETA Phase C batch C1): `logistic` previously returned the
    // IDENTICAL block as `logit` (estimator "logit", b = log-odds) — real
    // Stata's `logistic` runs the SAME engine call as `logit` but displays
    // odds ratios: OR = exp(b), SE_OR = OR * se(b) (delta method), CI =
    // exp(ci of b); z/p/t are invariant under this monotonic reparam and
    // stay untouched. Only the DISPLAY coeftable is transformed — `coefs`
    // (feeds e(b)/postestimation, e.g. a later `margins`/`lincom`) is left
    // in the log-odds scale untouched, matching Stata's own internal
    // storage (its display-only OR transform never touches e(b)).
    // NOTE: ci_lo/ci_hi (and p) are currently null in this engine's logit
    // coeftable rows (a separate, pre-existing gap outside this batch's
    // scope) — the exp() transform below is a no-op on a null field, so
    // this stays forward-compatible once that gap is closed.
    // NOTE (frontend, out of scope — piet/frontend/** is forbidden for this
    // batch): console.js's formatOneTable() derives the coefficient
    // column's header purely from `block.estimator === 'margins' ? 'dy/dx'
    // : 'Coefficient'` (no other estimator branch) — with no companion
    // frontend change the column header will still literally read
    // "Coefficient" even though the values underneath are now correct odds
    // ratios. estimatorTitle() already has a `logistic` -> "Logistic
    // regression" row (identical to logit's, matching real Stata, which
    // titles both commands the same), so the header banner itself needs no
    // change.
    // BUG-333: `logit ..., or` reports odds ratios. Stata's `logistic` IS
    // `logit, or`, so the transform this file already performs for `logistic`
    // below is exactly the one `or` needs — generalised here instead of copied.
    // e(b) stays in log-odds for both (Stata does the same: `or` is a display
    // metric, `_b[]` is unchanged), which is why only `coeftable` is touched.
    bool wantOr = false;
    for (const auto& [ok, ov] : parsed.opts)
      if (ok == "or" && ov.isBool && ov.boolValue) wantOr = true;
    if ((cmd == "logistic" || wantOr) && block.value("type", "") == "estimation") {
      if (cmd == "logistic") block["estimator"] = "logistic";
      // A metric marker the renderer can key on without knowing the command —
      // stcox/streg already set this key (applyHazardRatioTableM4).
      block["coef_metric"] = "odds_ratio";
      if (block.contains("coeftable") && block["coeftable"].is_array()) {
        for (auto& row : block["coeftable"]) {
          if (!row.contains("b") || row["b"].is_null()) continue;
          double b = row["b"].get<double>();
          double orv = std::exp(b);
          row["b"] = orv;
          if (row.contains("se") && row["se"].is_number()) {
            row["se"] = orv * row["se"].get<double>();
          }
          if (row.contains("ci_lo") && row["ci_lo"].is_number()) {
            row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
          }
          if (row.contains("ci_hi") && row["ci_hi"].is_number()) {
            row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
          }
          // t/p intentionally unchanged (invariant under a monotonic exp() reparam).
        }
      }
    }
    blocks.push_back(block);
    return true;
  }


  if (cmd == "poisson" || cmd == "ppmlhdfe" || cmd == "fepois") {
    // BS-073 (BETA D3a): bare `poisson` -> cobra fepois WITHOUT --fe (D0
    // finding: reproduces Stata's poisson exactly); `ppmlhdfe`/cobra's own
    // `fepois` command name (BOTH accepted as typeable command words here,
    // mirroring the feols/reghdfe precedent above of accepting the engine's
    // own internal estimator name as an additional PIET spelling) allow
    // absorb() -> --fe. Real Stata's plain `poisson` has NO absorb() option
    // — runPoisson honest-rejects it there.
    // BUG-1251: ppmlhdfe d's new column is announced in a data_mutation block of
    // its own, after the estimation block (the reghdfe residuals() order).
    ojson dMutation;
    blocks.push_back(ojson(handlers::runPoisson(cmd, parsed, session, &dMutation)));
    if (dMutation.is_object() && !dMutation.empty()) blocks.push_back(dMutation);
    return true;
  }


  if (cmd == "nbreg") {
    // BS-074 (BETA D3b): negative-binomial (NB2) regression -> cobra nbreg.
    // NOTE: the engine has a live-verified convergence gap on data whose
    // fitted intercept sits away from zero (including the D3e oracle's own
    // fatalities/cnt data) — see runNbreg's header comment (estimation_m4.cpp)
    // for the full disclosure; this is an engine-side limitation, not a
    // wiring gap.
    blocks.push_back(ojson(handlers::runNbreg(parsed, session)));
    installMlEsampleAfterFit(blocks.back(), session);  // BUG-1214
    return true;
  }


  if (cmd == "xtpoisson") {
    // BS-075 (BETA D3c): conditional fixed-effects Poisson MLE, `,fe` ONLY
    // (the engine has no RE/PA xtpoisson — Stata's own default IS
    // random-effects, so the bare/`,re`/`,pa` forms all honest-reject).
    blocks.push_back(ojson(handlers::runXtPoisson(parsed, session)));
    return true;
  }


  if (cmd == "xtlogit") {
    // BS-075 (BETA D3c): conditional fixed-effects logit MLE, `,fe` ONLY
    // (same RE/PA-absence rationale as xtpoisson above).
    blocks.push_back(ojson(handlers::runXtLogitFe(parsed, session)));
    return true;
  }


  if (cmd == "csdid") {
    blocks.push_back(ojson(handlers::runCsdid(parsed, session)));
    return true;
  }


  // ---- Arc F (P3 wave): the seven QE/RD commands. Unlike every neighbouring
  // estimator arm, these handlers return an ARRAY (one `display` block per
  // printed line + one structured block last — the handlers_resample.cpp
  // shape), because the tables are rendered as TEXT here rather than being
  // handed to the frontend as a coeftable. So the arms splice; they must not
  // push_back, which would nest an array inside the block stream. ----
  if (cmd == "rdrobust" || cmd == "rddensity" || cmd == "rdplot" || cmd == "binscatter" ||
      cmd == "eventstudyinteract" || cmd == "did_imputation" || cmd == "sdid") {
    ojson res;
    if (cmd == "rdrobust") res = handlers::runRdrobust(parsed, session);
    else if (cmd == "rddensity") res = handlers::runRddensity(parsed, session);
    else if (cmd == "rdplot") res = handlers::runRdplot(parsed, session);
    else if (cmd == "binscatter") res = handlers::runBinscatter(parsed, session);
    else if (cmd == "eventstudyinteract") res = handlers::runEventstudyinteract(parsed, session);
    else if (cmd == "did_imputation") res = handlers::runDidImputation(parsed, session);
    else res = handlers::runSdid(parsed, session);
    if (cmd == "binscatter") {
      // W4: binscatter's own net (handlers_qe.cpp) has already run and now
      // ACCEPTS the graph drawing keys (kVocabBinscatterGraph unions them in).
      // Accepting them without saying so would be the silent drop this wave
      // exists to close, so the block carries the note.
      if (res.is_array()) {
        for (auto& b : res) graphAcceptedNotApplied(b, "binscatter", parsed.opts, kBinscatterApplied);
      } else {
        graphAcceptedNotApplied(res, "binscatter", parsed.opts, kBinscatterApplied);
      }
    }
    if (res.is_array()) {
      for (const auto& b : res) blocks.push_back(b);
    } else {
      blocks.push_back(res);
    }
    return true;
  }


  if (cmd == "drdid") {
    // BS-076 (BETA D3d): the classic 2-period doubly-robust DiD -> the SAME
    // engine csdid machinery as `csdid` above, ONE (g,t) cell — now its OWN
    // handler (previously shared csdid's arm above and unconditionally
    // rejected on missing gvar(), since real drdid syntax has no gvar()
    // option at all). See runDrdid's header comment (estimation_m4.cpp) for
    // how the wiring leaves the estimator unmodified and the live-verified
    // oracle match.
    blocks.push_back(ojson(handlers::runDrdid(parsed, session)));
    return true;
  }


  // ------------------------------------------------------------------
  // BETA campaign Phase D batch D4 (time-series wirings): BS-077 newey,
  // BS-078 prais, BS-079 dfuller/pperron, BS-080 wntestq, BS-081 tsfill.
  // Oracle: internal capture
  // d45-oracle-stata.log, sections D4a-D4f.

  if (cmd == "newey") {
    blocks.push_back(ojson(handlers::runNewey(parsed, session)));
    return true;
  }


  if (cmd == "prais") {
    blocks.push_back(ojson(handlers::runPrais(parsed, session)));
    return true;
  }


  // BUG-373: these three computed their statistics and then published NOTHING.
  // A capture gate cannot see that — it compares what a command PRINTS, and the
  // printing was always right. The 2026-08-31 declared-returns sweep is what
  // measured it, against real StataBE: our dfuller numbers match Stata to every
  // digit it reports, and `display r(p)` still errored.
  if (cmd == "dfuller" || cmd == "pperron") {
    ojson blk = ojson(handlers::runDfullerPperron(cmd, parsed, session));
    if (blk.value("type", std::string()) != "error") {
      auto put = [&](const char* rname, const char* key) {
        if (blk.contains(key) && blk[key].is_number())
          session.eval.rValues.set(rname, blk[key].get<double>());
      };
      put("N", "n_obs");
      put("lags", "lags");
      put("Zt", "Zt");
      put("p", "p");
      put("Zrho", "Zrho");   // pperron only; absent on dfuller, so not set there
      // pperron names its lag count `newey_west_lags` in the block and Stata
      // calls it r(lags); and Stata publishes the p-value under BOTH r(p) and
      // r(pval) for this command. Measured, not assumed — the first wiring pass
      // used the block's own key names and the re-sweep caught both misses.
      put("lags", "newey_west_lags");
      put("pval", "p");
      // Stata publishes the critical values under BOTH spellings.
      for (const auto& pr : {std::make_pair("cv_1", "cv_1"), std::make_pair("cv_5", "cv_5"),
                             std::make_pair("cv_10", "cv_10"), std::make_pair("cv1", "cv_1"),
                             std::make_pair("cv5", "cv_5"), std::make_pair("cv10", "cv_10")})
        put(pr.first, pr.second);
    }
    blocks.push_back(std::move(blk));
    return true;
  }


  if (cmd == "wntestq") {
    ojson blk = ojson(handlers::runWntestq(parsed, session));
    if (blk.value("type", std::string()) != "error") {
      if (blk.contains("df") && blk["df"].is_number())
        session.eval.rValues.set("df", blk["df"].get<double>());
      if (blk.contains("p") && blk["p"].is_number())
        session.eval.rValues.set("p", blk["p"].get<double>());
      // Stata calls the Q statistic r(stat).
      if (blk.contains("Q") && blk["Q"].is_number())
        session.eval.rValues.set("stat", blk["Q"].get<double>());
    }
    blocks.push_back(std::move(blk));
    return true;
  }


  // ------------------------------------------------------------------
  // BETA campaign Phase D batch D5 (post-estimation/diag/misc wirings):
  // BS-085 sdtest, BS-083 hausman, BS-084 chow, BS-087(part) mkspline.
  // (BS-021 durbinalt fix + BS-082 estat endogenous/overid/firststage live
  // inside runEstat, estimation_m4.cpp — no separate dispatch arm needed,
  // both ride the existing `estat` arm below. BS-086 margins ,at() fix
  // lives inside runMargins, same file — no dispatch change needed either.)

  if (cmd == "sdtest") {
    // Real Stata's `sdtest varname [if], by(groupvar)` / `sdtest varname
    // [if] = #` — the `= #` comparison value rides the varlist as a bare
    // `=` token (same reconstruct-then-split idiom `egen`'s arm above uses
    // for `o = fn(expr)`), NOT a parsed.opts entry.
    std::string raw = joinVarlist(parsed.varlist);
    size_t eqPos = raw.find('=');
    std::string varName = trimWs(eqPos == std::string::npos ? raw : raw.substr(0, eqPos));
    std::optional<std::string> againstVal;
    if (eqPos != std::string::npos) againstVal = trimWs(raw.substr(eqPos + 1));
    blocks.push_back(
        ojson(handlers::runSdtest(varName, againstVal, parsed.opts, parsed.ifExpr, session)));
    return true;
  }


  if (cmd == "hausman") {
    if (parsed.varlist.size() != 2) {
      blocks.push_back(ojson(eval::engineError(
          "syntax_error: `hausman` requires exactly two stored estimation names, e.g. `hausman "
          "A B`",
          "syntax_error")));
      return true;
    }
    blocks.push_back(
        ojson(handlers::runHausman(parsed.varlist[0], parsed.varlist[1], parsed.opts, session)));
    return true;
  }


  if (cmd == "chow") {
    blocks.push_back(ojson(handlers::runChow(parsed, session)));
    return true;
  }
  return false;
}

}
