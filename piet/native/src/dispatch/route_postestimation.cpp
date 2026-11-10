// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "results_support.h"
#include "text_support.h"
#include "pipeline.h"
#include "options.h"
#include "handler_declarations.h"
#include "piet/handlers/estimates_tables.h"
#include "piet/handlers/postestimation.h"
#include "piet/handlers/results.h"
namespace piet::dispatch_detail {
bool route_postestimation(std::string& cmd,
    std::string& cmdTyped,
    eval::ParsedCommand& parsed,
    SessionState& session,
    ojson& blocks) {


  if (cmd == "test") {
    // BS-097 / BUG-333: every option is either read here or refused by name before
    // the handler runs. `accumulate` is IMPLEMENTED (w2c_postest arm
    // test_2c_accumulate) as session state owned by runTestCmd: the previous
    // test's restriction set on the same estimation is kept after each successful
    // test, replaced by a plain `test`, extended by `test ..., accumulate`.
    // `notest`/`mtest`/`coef` stay honest unimplemented-rejects (rc 198).
    std::vector<std::pair<std::string, eval::OptValue>> testOpts = parsed.opts;
    eval::canonicalizeOptionKeys(testOpts, {"accumulate", "notest", "mtest", "coef"});
    if (auto err = eval::rejectUnknownOptions(testOpts, {"accumulate"},
                                              {"notest", "mtest", "coef"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (auto gone = refuseIfFitDataGone("test", session)) {
      blocks.push_back(std::move(*gone));
      return true;
    }
    bool accumulate = false;
    for (const auto& [k, v] : testOpts)
      if (k == "accumulate" && (!v.isBool || v.boolValue)) accumulate = true;
    ojson testBlock = ojson(runPostestKeepingE(  // round 3: e() is left alone
        session, [&] {
          return handlers::runTestCmd(joinVarlist(parsed.varlist), session, accumulate);
        }));
    // `test` is r-class: the r() it writes below replaces all of r(), r(table)
    // included (w2c_postest arm rtable_replaced_by_test), at dispatch()'s exit
    // (handlers::replaceStaleRClass). A failing test writes nothing and leaves r().
    // W:P5 — `test` is r-class (captureRFromBlock Branch 3), same wiring the
    // lincom arm below has had since FIX-RESTIMATE. Without this,
    // `test 1.treated` then `display r(p)` errored with "r(p) is not set".
    eval::captureRFromBlock(testBlock, session.eval);
    if (testBlock.value("type", std::string()) != "error") {
      // BUG-373: Stata publishes r(drop) — whether any constraint was DROPPED.
      // The engine drops exactly one kind: a restriction on omitted coefficients
      // only (`test 0.c` names the base level, i.e. `0 = 0`), which it reports as
      // `dropped`; a redundant restriction still surfaces as an error.
      const bool anyDropped = testBlock.contains("dropped") && testBlock["dropped"].is_number() &&
                              testBlock["dropped"].get<double>() > 0;
      session.eval.rValues.set("drop", anyDropped ? 1.0 : 0.0);
      // The names Stata posts (test's help; RULING.md D.3.9): the F form
      // when the block carries a p_F (captureRFromBlock's own test), else the chi2
      // form; r(dropped_1) when a constraint was dropped.
      // A null F and p_F is the least-squares form on a zero R V R' (an exact
      // `regress`: Stata posts r(F) = r(p) = missing), still the F form.
      const bool fForm = testBlock.contains("p_F") &&
                         (!testBlock["p_F"].is_null() ||
                          (testBlock.contains("F") && testBlock["F"].is_null()));
      std::vector<std::string> stataNames =
          fForm ? std::vector<std::string>{"F", "p", "df", "df_r", "drop"}
                : std::vector<std::string>{"chi2", "p", "df", "drop"};
      // r(dropped_#), one per dropped constraint (captureRFromBlock), BUG-1276.
      const int nDropped = anyDropped ? testBlock["dropped"].get<int>() : 0;
      for (int k = 1; k <= nDropped; ++k) stataNames.push_back("dropped_" + std::to_string(k));
      eval::declareRPosted(session.eval, "test", std::move(stataNames));
    }
    blocks.push_back(std::move(testBlock));
    return true;
  }


  if (cmd == "testparm") {
    // BUG-367: `testparm x1 x2` / `testparm i.year` / `testparm x*`. Stata's own
    // options (equal, equation(), nosvyadjust, ...) are NOT implemented, so they
    // are refused by name rather than silently dropped — the same discipline the
    // `test` arm above adopted under BUG-333.
    if (auto err = eval::rejectUnknownOptions(parsed.opts, {},
                                              {"equal", "equation", "nosvyadjust", "df"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (auto gone = refuseIfFitDataGone("testparm", session)) {
      blocks.push_back(std::move(*gone));
      return true;
    }
    ojson tpBlock = ojson(runPostestKeepingE(  // round 3: e() is left alone
        session, [&] { return handlers::runTestparm(parsed.varlist, session); }));
    // r-class, exactly as `test` is (W:P5 captureRFromBlock Branch 3) — a
    // `testparm` followed by `display r(p)` has to see the same r() the test
    // arm fills, or the two spellings of one command would disagree.
    eval::captureRFromBlock(tpBlock, session.eval);
    if (tpBlock.value("type", std::string()) != "error") {
      // r(drop) exactly as the test arm posts it: "test and testparm store" the
      // same results (test's help); pinned 0 at w2h_postest_refit.log:173.
      const bool anyDropped = tpBlock.contains("dropped") && tpBlock["dropped"].is_number() &&
                              tpBlock["dropped"].get<double>() > 0;
      session.eval.rValues.set("drop", anyDropped ? 1.0 : 0.0);
      // The test set (RULING.md D.3.10).
      const bool fForm = tpBlock.contains("p_F") &&
                         (!tpBlock["p_F"].is_null() ||
                          (tpBlock.contains("F") && tpBlock["F"].is_null()));
      std::vector<std::string> stataNames =
          fForm ? std::vector<std::string>{"F", "p", "df", "df_r", "drop"}
                : std::vector<std::string>{"chi2", "p", "df", "drop"};
      const int nDropped = anyDropped ? tpBlock["dropped"].get<int>() : 0;  // as the test arm
      for (int k = 1; k <= nDropped; ++k) stataNames.push_back("dropped_" + std::to_string(k));
      eval::declareRPosted(session.eval, "testparm", std::move(stataNames));
    }
    blocks.push_back(std::move(tpBlock));
    return true;
  }


  if (cmd == "lincom") {
    // BUG-333: level() and the eform family are now IMPLEMENTED (engine --level
    // landed this wave; eform is a display metric applied to the block). The
    // BS-097 note below described the pre-fix state, when they were silently
    // discarded and then honestly rejected.
    if (auto err = eval::rejectUnknownOptions(
            parsed.opts, {"level", "eform", "or", "hr", "irr", "rrr", "shr", "coef"}, {"post"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // FIX-RESTIMATE: lincom is r-class — populate r(estimate)/r(se)/r(t)/r(p)/
    // r(lb)/r(ub)/r(df) from its coeftable (captureRFromBlock Branch 2). This
    // was wired only for summarize before, so `lincom` after any estimation
    // (reghdfe/regress/…) left r() empty and every downstream `r(estimate)`
    // read errored. Error blocks (no prior estimation) don't match Branch 2.
    if (auto gone = refuseIfFitDataGone("lincom", session)) {
      blocks.push_back(std::move(*gone));
      return true;
    }
    // Without level(), lincom's interval is at `set level` (w2o_rnames.log:111
    // N03: set level 90 -> r(level) 90 with the 90% r(lb)/r(ub)).
    auto lincomOpts = parsed.opts;
    {
      bool hasLevel = false;
      for (const auto& [ok, ov] : lincomOpts)
        if (ok == "level") hasLevel = true;
      if (!hasLevel && session.level != 95) {
        eval::OptValue lv;
        lv.isBool = false;
        lv.strValue = std::to_string(session.level);
        lincomOpts.emplace_back("level", lv);
      }
    }
    ojson lincomBlock = ojson(runPostestKeepingE(session, [&] {  // round 3: e() is left alone
      return handlers::runLincom(joinVarlist(parsed.varlist), session, lincomOpts);
    }));
    eval::captureRFromBlock(lincomBlock, session.eval);
    if (lincomBlock.value("type", std::string()) != "error") {
      // r(level) (lincom's help; pinned 95 at w2h_postest_refit.log:191;
      // RULING.md D.3.7): level()'s value, the nlcom arm's own reading; without
      // level() only at 95, because runLincom does not forward `set level`, so
      // its interval is at 95 and posting c(level) would contradict r(lb)/r(ub).
      // Otherwise nothing is posted and the read refuses (FU-c).
      std::optional<double> lcLevel;
      bool levelGiven = false;
      for (const auto& [ok, ov] : parsed.opts)
        if (ok == "level" && !ov.isBool) {
          levelGiven = true;
          lcLevel = std::strtod(ov.strValue.c_str(), nullptr);
        }
      if (!levelGiven) lcLevel = static_cast<double>(session.level);
      if (lcLevel.has_value()) session.eval.rValues.set("level", *lcLevel);
      // The names Stata posts (lincom's stored results): r(t) after a t-reference fit
      // (the block's df is positive), else r(z); and r(df) only when the reference
      // has degrees of freedom (a normal-reference lincom has none),
      // so after a normal-reference combination r(df) is outside the set and reads
      // `.` (w2s_multiway_class.log:840 mw_iv_c2_lincom, w2o_rnames.log:125 N04).
      const bool tRef = lincomBlock.contains("df") && lincomBlock["df"].is_number() &&
                        lincomBlock["df"].get<double>() > 0;
      // The coefficient table's statistic is captured as r(t) (captureRFromBlock
      // Branch 2); under a normal reference lincom.ado posts it as r(z).
      if (!tRef && session.eval.rValues.contains("t") && !session.eval.rValues.contains("z")) {
        eval::OrderedFloatDict renamed;
        for (const auto& [k, v] : session.eval.rValues.items()) renamed.set(k == "t" ? "z" : k, v);
        session.eval.rValues = renamed;
      }
      std::vector<std::string> lincomNames = {"estimate", "se", tRef ? "t" : "z", "p", "lb", "ub",
                                              "level"};
      if (tRef) lincomNames.insert(lincomNames.begin() + 2, "df");
      eval::declareRPosted(session.eval, "lincom", std::move(lincomNames));
    }
    blocks.push_back(std::move(lincomBlock));
    return true;
  }


  if (cmd == "nlcom") {
    // BUG-333: level() implemented (engine --level landed this wave).
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNlcom.known, kVocabNlcom.unimplemented)) {
      blocks.push_back(ojson(*err));
      return true;
    }
    // FIX-RESTIMATE: nlcom is r-class too (same captureRFromBlock Branch 2).
    if (auto gone = refuseIfFitDataGone("nlcom", session)) {
      blocks.push_back(std::move(*gone));
      return true;
    }
    // round 3: without `post` nlcom leaves e() alone; `post` installs a new e()
    // (nlcom.sthlp), so only then is it not wrapped. Read as runNlcom reads it.
    bool nlcomPost = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "post" && (!v.isBool || v.boolValue)) nlcomPost = true;
    }
    auto nlcomRun = [&] {
      return handlers::runNlcom(joinVarlist(parsed.varlist), session, parsed.opts);
    };
    ojson nlcomBlock = ojson(nlcomPost ? nlcomRun() : runPostestKeepingE(session, nlcomRun));
    eval::captureRFromBlock(nlcomBlock, session.eval);
    // BUG-373: captureRFromBlock fills the estimate/se/t/p family; r(N) is the
    // fit's own observation count and was not among them.
    if (nlcomBlock.value("type", std::string()) != "error" && nlcomBlock.contains("n_obs") &&
        nlcomBlock["n_obs"].is_number())
      session.eval.rValues.set("N", nlcomBlock["n_obs"].get<double>());
    // w2c_postest family 5: nlcom is r-class and posts r(b) (1 x k, the
    // combinations) and r(table) (9 x k, normal-referenced) — arms nlcom_ratio /
    // nlcom_label_only read r(b)[1,1], nlcom_multi_nopost_rtable reads a 9 x 3
    // r(table). Replaces the r-matrices wholesale, as any r-class command does.
    if (nlcomBlock.value("type", std::string()) != "error") {
      session.eval.rMatrices.clear();
      eval::EvalMatrix rb;
      // Stata names the one row `c1` (StataBE 19.5, BUG-1504: rownames r(b)
      // `c1` for `nlcom _b[x1]/_b[x2]`, a named and a two-term call alike).
      rb.rownames = {"c1"};
      rb.data.emplace_back();
      if (nlcomBlock.contains("coeftable") && nlcomBlock["coeftable"].is_array()) {
        for (const auto& row : nlcomBlock["coeftable"]) {
          rb.colnames.push_back(row.value("name", std::string()));
          rb.data[0].push_back((row.contains("b") && row["b"].is_number())
                                   ? row["b"].get<double>()
                                   : std::numeric_limits<double>::quiet_NaN());
        }
      }
      // FU-410: r(V), the combinations' delta-method covariance G V G' (the
      // engine's, runNlcom's `nlcom_V`), named like r(b). w2p_estim_ev.log:1502
      // G_c_nlcom el(r(V),1,1) .0129450049474129 = the table se squared.
      if (nlcomBlock.contains("nlcom_V") && nlcomBlock["nlcom_V"].is_array() &&
          nlcomBlock["nlcom_V"].size() == rb.colnames.size()) {
        eval::EvalMatrix rv;
        rv.rownames = rb.colnames;
        rv.colnames = rb.colnames;
        bool shapeOk = true;
        for (const auto& vrow : nlcomBlock["nlcom_V"]) {
          if (!vrow.is_array() || vrow.size() != rb.colnames.size()) {
            shapeOk = false;
            break;
          }
          std::vector<double> r;
          for (const auto& x : vrow)
            r.push_back(x.is_number() ? x.get<double>() : std::numeric_limits<double>::quiet_NaN());
          rv.data.push_back(std::move(r));
        }
        if (shapeOk) session.eval.rMatrices.emplace_back("V", std::move(rv));
      }
      session.eval.rMatrices.emplace_back("b", std::move(rb));
      // level(#) was validated by the engine run above; absent = `set level`.
      double nlLevel = static_cast<double>(session.level);
      for (const auto& [ok, ov] : parsed.opts)
        if (ok == "level" && !ov.isBool) nlLevel = std::strtod(ov.strValue.c_str(), nullptr);
      handlers::postRTableFromBlock(session, nlcomBlock, nlLevel);
      // The names Stata posts (nlcom's help; measured): r(N), r(b),
      // r(V), r(table), r(level); r(df_r) only under df(), and none of lincom's
      // estimate/se family (w2o_rnames.log:340 N20 `60|.|.|.|95|`).
      for (const char* k : {"estimate", "se", "t", "z", "p", "lb", "ub", "df", "df_r"})
        session.eval.rValues.erase(k);
      session.eval.rValues.set("level", nlLevel);
      eval::declareRPosted(session.eval, "nlcom", {"N", "b", "V", "table", "level"});
    }
    if (nlcomBlock.is_object()) nlcomBlock.erase("nlcom_V");
    blocks.push_back(std::move(nlcomBlock));
    return true;
  }


  if (cmd == "margins") {
    // X2-GLM: after a glm the engine's margins would answer a DIFFERENT model --
    // runMargins sends no --model for cobraCmd "glm", and cmd_margins then
    // defaults to a logit AME. The glm's own delta-method margins answer instead.
    if (session.lastModel().has_value() && session.lastModel()->cobraCmd == "glm") {
      ojson r_gm = handlers::runGlmMargins(parsed, session);
      if (r_gm.is_array()) {
        for (const auto& b : r_gm) blocks.push_back(b);
      } else if (!r_gm.is_null()) {
        blocks.push_back(r_gm);
      }
      return true;
    }
    // qreg2 (estimation/qreg2_estimation.cpp): no engine refit exists for it; its linear-index
    // dy/dx comes from the posted e(b)/e(V) (w2j_e3_qreg2.log M01/M08).
    if (session.lastModel().has_value() && session.lastModel()->cobraCmd == "qreg2") {
      ojson r_qm = handlers::runQreg2Margins(parsed, session);
      if (r_qm.is_array()) {
        for (const auto& b : r_qm) blocks.push_back(b);
      } else if (!r_qm.is_null()) {
        blocks.push_back(r_qm);
      }
      return true;
    }
    // Stata's margins takes `if` to choose the rows it averages over;
    // runMargins never read it (runGlmMargins above refuses it itself), so
    // every form answered for the full e(sample) at rc 0 (`margins if t==1`
    // gave the all-rows 12.673333, where the t==1 rows average 11.913333).
    // Refused by name until a path restricts the
    // averaging sample -- forwarding it as --keepif would refit on the subset,
    // a different model. The registry row's allowIf=false is descriptive only
    // (no enforcement site reads it), so the refusal lives here; `in` and
    // weights are already refused by the registry's allowInRange/WeightPolicy
    // gates before this arm.
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
      blocks.push_back(ojson(eval::errorBlock(
          "unsupported_option",
          "margins with an if qualifier (averaging over a subset of the estimation sample) is "
          "not implemented in this version of PIET; nothing was computed.",
          std::nullopt, std::nullopt, std::nullopt, "r(198)")));
      return true;
    }
    // FU-94: the marginlist is the positional terms. Passing the options alone
    // dropped it, so `margins t` answered what bare `margins` answers, at rc 0.
    blocks.push_back(ojson(handlers::runMargins(parsed.varlist, parsed.opts, session)));
    return true;
  }


  if (cmd == "lassocoef") {
    blocks.push_back(handlers::runLassoCoef(parsed, session));
    return true;
  }


  if (cmd == "predict") {
    // Only cached lasso predictions implement in-range filtering. Preserve
    // the tokenizer's previous explicit refusal for every generic path.
    if (parsed.inRange.has_value() &&
        !(session.lastModel().has_value() && session.lastModel()->stataCmd == "lasso")) {
      blocks.push_back(ojson(eval::unsupportedOption("in " + *parsed.inRange,
          "`" + cmdTyped + "` does not support the `in` row-range qualifier in this engine. "
          "Use `if` to subset rows by a condition (maps to --keepif).")));
      return true;
    }
    // BUG-517: the `if` qualifier reaches the handler (it was parsed and then
    // dropped here, so `predict p if x` filled every row). c() refs resolve the
    // way gen's `if` does; est refs were already resolved at the parse point.
    std::optional<std::string> predictIf = parsed.ifExpr;
    if (predictIf.has_value()) predictIf = substituteCRefs(*predictIf, session);
    ojson predictBlk;
    if (session.lastModel().has_value() && session.lastModel()->stataCmd == "lasso") {
      auto lassoParsed = parsed;
      lassoParsed.ifExpr = predictIf;
      predictBlk = handlers::runLassoPredict(lassoParsed, session);
    } else predictBlk = handlers::runPredict(parsed.varlist, parsed.opts, session, predictIf);
    // A refused predict leaves r() alone (StataBE: r(N) 38 survives `predict pd, d`
    // r(198)). The w2b_estim.log:370 arm reads r(sum) `.` because its `drop pv`
    // replaced r() (BUG-1417), which an earlier rule here ("C11") misread as the
    // refused predict's doing.
    blocks.push_back(std::move(predictBlk));
    return true;
  }


  if (cmd == "estat") {
    std::string sub = parsed.varlist.empty() ? std::string() : lower(parsed.varlist[0]);
    std::vector<std::string> after(parsed.varlist.empty() ? parsed.varlist.begin()
                                                            : parsed.varlist.begin() + 1,
                                    parsed.varlist.end());
    // round 3: estat leaves e() alone unless `post` installs a new one (read as
    // runEstat reads it), so only the post form is not wrapped.
    bool estatPost = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "post") estatPost = true;
    }
    auto estatRun = [&] {
      return handlers::runEstat(sub, after, parsed.opts, parsed.ifExpr, session);
    };
    ojson estatBlk = ojson(estatPost ? estatRun() : runPostestKeepingE(session, estatRun));
    // BUG-373: `estat vif` is r-class in Stata — r(name_K)/r(vif_K), one pair per
    // regressor, 1-based in coeftable order. The values were computed and printed
    // and never published.
    if (sub == "vif" && estatBlk.value("type", std::string()) != "error" &&
        estatBlk.contains("coeftable") && estatBlk["coeftable"].is_array()) {
      int k = 0;
      std::vector<std::string> vifNames;
      for (const auto& row : estatBlk["coeftable"]) {
        ++k;
        const std::string nm = row.value("name", std::string());
        if (!nm.empty()) session.eval.rStrings.set("name_" + std::to_string(k), nm);
        if (row.contains("b") && row["b"].is_number())
          session.eval.rValues.set("vif_" + std::to_string(k), row["b"].get<double>());
        vifNames.push_back("name_" + std::to_string(k));
        vifNames.push_back("vif_" + std::to_string(k));
      }
      // BUG-1496: those pairs are vif's whole r() (StataBE 19.5, rlife/ts1.do: `:
      // r(scalars)` `vif_2 vif_1`, `: r(macros)` `name_2 name_1`).
      eval::declareRPosted(session.eval, "estat", std::move(vifNames));
    }
    blocks.push_back(std::move(estatBlk));
    return true;
  }

  if (cmd == "erepost") {
    // round 3: repost changes b/V on the SAME model; every other e() key stays.
    // BUG-1202: a fit that posted e(b) as a matrix (glm, oaxaca, teffects) has
    // that matrix reposted too, so it is the one store the restore keeps.
    std::optional<eval::EvalMatrix> repostedB;
    ojson res = runPostestKeepingE(session, [&] {
      ojson r = handlers::runErepost(parsed, session);
      if (const eval::EvalMatrix* b = session.eval.findClassMatrix('e', "b")) repostedB = *b;
      return r;
    });
    if (repostedB.has_value()) {
      for (auto& p : session.eval.eMatrices)
        if (p.first == "b") p.second = *repostedB;
    }
    pushEnvResult(blocks, std::move(res));
    return true;
  }
  return false;
}

}
