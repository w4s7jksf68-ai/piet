// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/result_order.h"  // orderRResults (test lists r() as Stata does)
#include "results_support.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
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
    // mtest(bonferroni|holm|sidak): each constraint tested on its own as well,
    // its p-value adjusted for the multiple tests (bank 336b8488 bonferroni,
    // 698c716b holm, d3b1f63e sidak, all after regress). The bare / noadjust
    // form and the chi2 form keep the named refusal (no measured layout).
    std::string mtMethod;
    bool bareMtest = false;  // `mtest` alone: Stata's noadjust method
    {
      std::vector<std::pair<std::string, eval::OptValue>> rest;
      for (const auto& kv : testOpts) {
        if (kv.first == "mtest" && !kv.second.isBool) {
          const std::string m = lower(strutil::trim(kv.second.strValue));
          auto pre = [&m](const char* full) {
            const std::string f(full);
            return !m.empty() && m.size() <= f.size() && f.compare(0, m.size(), m) == 0;
          };
          if (pre("bonferroni")) mtMethod = "bonferroni";
          else if (pre("holm")) mtMethod = "holm";
          else if (pre("sidak")) mtMethod = "sidak";
          else rest.push_back(kv);
          if (!mtMethod.empty()) continue;
        }
        if (kv.first == "mtest" && kv.second.isBool && kv.second.boolValue) bareMtest = true;
        rest.push_back(kv);
      }
      testOpts = std::move(rest);
    }
    // notest: the constraints are formed (and kept for a later accumulate)
    // and listed, nothing is tested and r() is left empty (bank 37fd4049
    // `test mpg, notest` after regress: "( 1)  mpg = 0", no r()).
    bool notest = false;
    for (auto it = testOpts.begin(); it != testOpts.end();)
      if (it->first == "notest" && it->second.isBool && it->second.boolValue) {
        notest = true;
        it = testOpts.erase(it);
      } else {
        ++it;
      }
    // A multiple test is refused with notest, then with accumulate, before
    // anything is computed (r(198) "options mtest and notest may not be
    // combined", "... mtest and accumulate ...").
    if (!mtMethod.empty() || bareMtest) {
      bool accumulateGiven = false;
      for (const auto& [k, v] : testOpts)
        if (k == "accumulate" && (!v.isBool || v.boolValue)) accumulateGiven = true;
      const char* clash = notest ? "options mtest and notest may not be combined"
                          : accumulateGiven ? "options mtest and accumulate may not be combined"
                                            : nullptr;
      if (clash != nullptr) {
        blocks.push_back(ojson(eval::errorBlock("syntax_error", clash, std::nullopt, std::nullopt,
                                                std::nullopt, "r(198)")));
        return true;
      }
    }
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
    if (notest && testBlock.value("type", std::string()) != "error") {
      ojson listed;
      listed["type"] = "estimation";
      listed["estimator"] = "test";
      listed["notest"] = true;
      listed["restrictions"] = testBlock.value("restrictions", ojson::array());
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "test", {});
      blocks.push_back(std::move(listed));
      return true;
    }
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
      // Stata lists them `drop df_r F df p` (vs-stata truth bank); the chi2 form
      // takes the same places.
      eval::orderRResults(session.eval, {"drop", "df_r", "F", "chi2", "df", "p"});
      if (!mtMethod.empty()) {
        // Each restriction alone, then the joint test once more so the
        // accumulated set and r() are the joint test's.
        const ojson restr = testBlock.value("restrictions", ojson::array());
        if (!fForm || anyDropped || !restr.is_array() || restr.empty()) {
          blocks.push_back(ojson(eval::engineError(
              "Option `mtest` is a valid Stata option that this version of PIET does not yet implement "
              "for this test (the chi2 form or a dropped constraint); nothing was computed.",
              "unsupported_option")));
          return true;
        }
        std::vector<std::array<double, 3>> singles;  // F, df, p
        for (const auto& r : restr) {
          const ojson one = ojson(runPostestKeepingE(session, [&] {
            return handlers::runTestCmd("(" + r.get<std::string>() + ")", session, false);
          }));
          auto num = [&one](const char* k) {
            return one.contains(k) && one[k].is_number() ? one[k].get<double>()
                                                         : std::numeric_limits<double>::quiet_NaN();
          };
          if (one.value("type", std::string()) == "error" || !std::isfinite(num("p_F"))) {
            blocks.push_back(one);
            return true;
          }
          singles.push_back({num("F"), num("df1"), num("p_F")});
        }
        testBlock = ojson(runPostestKeepingE(session, [&] {
          return handlers::runTestCmd(joinVarlist(parsed.varlist), session, accumulate);
        }));
        eval::captureRFromBlock(testBlock, session.eval);
        session.eval.rValues.set("drop", 0.0);
        const size_t q = singles.size();
        std::vector<double> adj(q);
        if (mtMethod == "bonferroni") {
          for (size_t i = 0; i < q; ++i) adj[i] = std::min(1.0, singles[i][2] * static_cast<double>(q));
        } else if (mtMethod == "sidak") {
          for (size_t i = 0; i < q; ++i)
            adj[i] = std::min(1.0, 1.0 - std::pow(1.0 - singles[i][2], static_cast<double>(q)));
        } else {  // holm: step-down over the ordered p-values
          std::vector<size_t> ord(q);
          for (size_t i = 0; i < q; ++i) ord[i] = i;
          std::stable_sort(ord.begin(), ord.end(),
                           [&](size_t a, size_t b) { return singles[a][2] < singles[b][2]; });
          double run = 0;
          for (size_t j = 0; j < q; ++j) {
            run = std::max(run, std::min(1.0, static_cast<double>(q - j) * singles[ord[j]][2]));
            adj[ord[j]] = run;
          }
        }
        eval::EvalMatrix mt;
        mt.colnames = {"F", "df", "p", "adjustp"};
        ojson rows = ojson::array();
        for (size_t i = 0; i < q; ++i) {
          mt.rownames.push_back("r" + std::to_string(i + 1));
          mt.data.push_back({singles[i][0], singles[i][1], singles[i][2], adj[i]});
          rows.push_back(ojson{{"F", singles[i][0]}, {"df", singles[i][1]}, {"p", singles[i][2]}, {"adjustp", adj[i]}});
        }
        for (auto it = session.eval.rMatrices.begin(); it != session.eval.rMatrices.end();)
          it = it->first == "mtest" ? session.eval.rMatrices.erase(it) : it + 1;
        session.eval.rMatrices.emplace_back("mtest", std::move(mt));
        session.eval.rStrings.set("mtmethod", mtMethod);
        stataNames.push_back("mtest");
        stataNames.push_back("mtmethod");
        testBlock["mtest"] = rows;
        testBlock["mtmethod"] = mtMethod;
        eval::orderRResults(session.eval, {"drop", "df_r", "F", "chi2", "df", "p"});
      }
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
    bool tpEqual = false;
    std::vector<std::pair<std::string, eval::OptValue>> tpRest;
    for (const auto& kv : parsed.opts) {
      if (kv.first == "equal" && kv.second.isBool) tpEqual = true;
      else tpRest.push_back(kv);
    }
    if (auto err = eval::rejectUnknownOptions(tpRest, {},
                                              {"equal", "equation", "nosvyadjust", "df"})) {
      blocks.push_back(ojson(*err));
      return true;
    }
    if (auto gone = refuseIfFitDataGone("testparm", session)) {
      blocks.push_back(std::move(*gone));
      return true;
    }
    ojson tpBlock = ojson(runPostestKeepingE(  // round 3: e() is left alone
        session, [&] { return handlers::runTestparm(parsed.varlist, session, tpEqual); }));
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
      // Listed as test lists them (vs-stata bank 0ae98070 `testparm mpg weight`:
      // return list r(drop) r(df_r) r(F) r(df) r(p)).
      eval::orderRResults(session.eval, {"drop", "df_r", "F", "chi2", "df", "p"});
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
    // margins' vce(): delta (the default), unconditional, svy -- checked in
    // Stata's order once estimates exist. vce(delta) is the variance every path
    // here computes, so it is read and dropped. vce(unconditional) is refused
    // with r(322) after a fit whose e(vce) is not robust or cluster ("allowed
    // only after estimation with robust, semirobust, or linearized standard
    // errors"; bank c5655ba7 after regress, 697bbe16 after logit), and after
    // areg / ivreg / xtgee / xtreg / ivregress or a fit marked nolinearize
    // ("<cmd> is not supported by margins with the vce(unconditional) option";
    // bank 9351b85d ivregress robust, 1f63e93d xtreg fe cluster). The
    // linearized variance itself is not implemented: a fit that qualifies keeps
    // the refusal below. vce(svy) without svy estimation results is r(322).
    if (session.lastModel().has_value()) {
      for (auto it = parsed.opts.begin(); it != parsed.opts.end(); ++it) {
        if (it->first != "vce" || it->second.isBool) continue;
        std::string w = lower(strutil::trim(it->second.strValue));
        const std::string kUncond = "unconditional";
        auto err322 = [&](const std::string& msg) {
          blocks.push_back(ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt,
                                                  std::nullopt, "r(322)")));
          return true;
        };
        if (w == "delta") {
          parsed.opts.erase(it);
          break;
        }
        const bool svyFit = session.eval.eStrings.get("prefix") == "svy";
        if (w == "svy" && !svyFit)
          return err322("option vce(svy) is allowed only with survey estimation results");
        const bool uncond = w.size() >= 6 && w.size() <= kUncond.size() && kUncond.compare(0, w.size(), w) == 0;
        // After svy estimation vce(svy) / vce(unconditional) take the survey
        // route: a fit whose e(vce) is not linearized is refused (r(322));
        // otherwise Stata computes the survey variance of the margins, which
        // margins after `svy:` does not do here (the same refusal as plain
        // margins after svy).
        if ((uncond || w == "svy") && svyFit) {
          if (session.eval.eStrings.get("vce") != "linearized")
            return err322("option vce(unconditional) is allowed only after estimation with linearized "
                          "standard errors");
          blocks.push_back(ojson(eval::engineError(
              "margins after `svy:` is not supported in this version of PIET -- the survey (linearized) "
              "variance of the margins is not implemented; nothing was computed.",
              "unsupported_option")));
          return true;
        }
        if (uncond) {
          const std::string mvce = session.eval.eStrings.get("vce");
          if (mvce != "robust" && mvce != "cluster")
            return err322("option vce(unconditional) is allowed only after estimation with robust, "
                          "semirobust, or linearized standard errors");
          std::string ecmd = session.eval.eStrings.get("cmd");
          static const std::set<std::string> kNoLin = {"areg", "ivreg", "xtgee", "xtreg", "ivregress"};
          const std::string mprop = " " + session.eval.eStrings.get("marginsprop") + " ";
          if (kNoLin.count(ecmd) || mprop.find(" nolinearize ") != std::string::npos)
            return err322(ecmd + " is not supported by margins with the vce(unconditional) option");
          if (ecmd == "regress" && !session.eval.eStrings.get("absvar").empty())
            return err322("regress with option absorb() is not supported by margins with the "
                          "vce(unconditional) option");
          blocks.push_back(ojson(eval::engineError(
              "Option `vce(unconditional)` is a valid Stata option that this version of PIET does not "
              "yet implement (the linearized variance of the margins); nothing was computed.",
              "unsupported_option")));
          return true;
        }
        break;
      }
    }
    // predict(): a statistic the fit lists in e(marginsnotok) is r(322)
    // "predict option <stat> not appropriate with margins" (bank d66f4c7b
    // `margins, predict(xbu)` after xtreg, fe). An entry's capitals are its
    // shortest abbreviation (SCore: sc), an all-lowercase entry is a full word.
    if (session.lastModel().has_value()) {
      const std::string notok = session.eval.eStrings.get("marginsnotok");
      for (const auto& [k, v] : parsed.opts) {
        if (k != "predict" || v.isBool || notok.empty()) continue;
        std::string inner = v.strValue;
        for (char& c : inner)
          if (c == '(' || c == ')' || c == ',') c = ' ';
        std::istringstream words(inner);
        std::string w;
        while (words >> w) {
          const std::string lw = lower(w);
          std::istringstream entries(notok);
          std::string e;
          bool hit = false;
          while (!hit && entries >> e) {
            size_t minLen = 0;
            while (minLen < e.size() && std::isupper(static_cast<unsigned char>(e[minLen]))) ++minLen;
            if (minLen == 0) minLen = e.size();
            const std::string le = lower(e);
            hit = lw.size() >= minLen && lw.size() <= le.size() && le.compare(0, lw.size(), lw) == 0;
          }
          if (hit) {
            blocks.push_back(ojson(eval::errorBlock("syntax_error",
                                                    "predict option " + w + " not appropriate with margins",
                                                    std::nullopt, std::nullopt, std::nullopt, "r(322)")));
            return true;
          }
        }
      }
    }
    // X2-GLM: after a glm the engine's margins would answer a DIFFERENT model --
    // runMargins sends no --model for cobraCmd "glm", and cmd_margins then
    // defaults to a logit AME. The glm's own delta-method margins answer instead.
    if (session.lastModel().has_value() && session.lastModel()->cobraCmd == "glm") {
      handlers::captureGlmMarginsFit(session);
      ojson r_gm = handlers::runGlmMargins(parsed, session);
      handlers::clearGlmMarginsFit();
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
    } else {
      // The options as typed (before the shared prefix/alias resolution): after
      // glm `p` is pearson and `r` response, which the shared resolution would
      // read as logit's pr and regress's residuals.
      std::vector<std::pair<std::string, eval::OptValue>> typedOpts;
      bool haveTyped = false;
      const long long comma = eval::findUnquotedComma(parsed.raw);
      if (comma >= 0) {
        try {
          typedOpts = eval::parseOpts(parsed.raw.substr(static_cast<size_t>(comma) + 1));
          haveTyped = true;
        } catch (const std::exception&) {
          typedOpts.clear();
        }
      } else {
        haveTyped = true;
      }
      predictBlk = handlers::runPredict(parsed.varlist, parsed.opts, session, predictIf,
                                        haveTyped ? &typedOpts : nullptr);
    }
    // A refused predict leaves r() alone (StataBE: r(N) 38 survives `predict pd, d`
    // r(198)). The w2b_estim.log:370 arm reads r(sum) `.` because its `drop pv`
    // replaced r() (BUG-1417), which an earlier rule here ("C11") misread as the
    // refused predict's doing.
    if (predictBlk.is_array()) {  // a note line ahead of the mutation (glm's "option mu assumed")
      for (auto& b : predictBlk) blocks.push_back(std::move(b));
      return true;
    }
    // The r() a served statistic leaves behind, as Stata's predict programs
    // do (an r-class step inside them replaces r()): after areg, d and xbd
    // expand the depvar (r(varlist) = e(depvar); bank 015491bd, 6706ee1f) and
    // residuals/scores generate scores (r(scorevars) = the new variable; bank
    // 569e9877); after xtreg, fe or re, u / xbu / e read the depvar through
    // tsrevar when the panel has a time variable (r(varlist) = e(depvar); bank
    // 9f2402e4, 5e8ea380); score after logit, probit, regress,
    // tobit, ologit, mlogit returns r(scorevars) (bank 98f5e1c0).
    const bool predictFailed = predictBlk.is_object() && predictBlk.value("type", std::string()) == "error";
    if (!predictFailed && session.lastModel().has_value()) {
      const LastModel& lm = *session.lastModel();
      const std::string est = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
      std::vector<std::string> newVars;
      for (const auto& t : parsed.varlist) {
        static const std::set<std::string> kTypes = {"byte", "int", "long", "float", "double"};
        if (kTypes.count(lower(t)) || (t.size() > 3 && lower(t).compare(0, 3, "str") == 0)) continue;
        newVars.push_back(t);
      }
      std::string stat;  // the one flag option as typed; "\x01" when several
      {
        const long long comma = eval::findUnquotedComma(parsed.raw);
        std::vector<std::pair<std::string, eval::OptValue>> typed;
        if (comma >= 0) {
          try {
            typed = eval::parseOpts(parsed.raw.substr(static_cast<size_t>(comma) + 1));
          } catch (const std::exception&) {
            typed.clear();
          }
        }
        for (const auto& [k, v] : typed)
          if (v.isBool && v.boolValue) stat = stat.empty() ? lower(k) : std::string("\x01");
      }
      auto abbrevOf = [&stat](const std::string& full, size_t minLen) {
        return stat.size() >= minLen && stat.size() <= full.size() && full.compare(0, stat.size(), stat) == 0;
      };
      const std::string depvar = lm.depvar;
      const bool plainDep = !depvar.empty() && depvar.find_first_of(".#() ") == std::string::npos;
      const bool oneNew = newVars.size() == 1 && newVars[0].find_first_of("*-") == std::string::npos;
      std::optional<std::pair<std::string, std::string>> leftover;
      if (est == "areg") {
        // one absorbed variable (several take the MAP path, which does not
        // expand the depvar)
        if ((stat == "d" || stat == "xbd") && plainDep && lm.fe.size() == 1)
          leftover = std::make_pair(std::string("varlist"), depvar);
        else if ((abbrevOf("residuals", 1) || abbrevOf("scores", 2) || abbrevOf("score", 2)) && oneNew)
          leftover = std::make_pair(std::string("scorevars"), newVars[0]);
      } else if (est == "xtreg") {
        const std::string model = session.eval.eStrings.get("model");
        if ((model == "fe" || model == "re") && (stat == "u" || stat == "xbu" || stat == "e") && plainDep &&
            session.timevar.has_value() && !session.timevar->empty())
          leftover = std::make_pair(std::string("varlist"), depvar);
      } else if (est == "logit" || est == "probit" || est == "regress" || est == "tobit" ||
                 est == "ologit" || est == "mlogit") {
        if ((abbrevOf("scores", 2) || abbrevOf("score", 2)) && oneNew)
          leftover = std::make_pair(std::string("scorevars"), newVars[0]);
      }
      if (leftover.has_value()) {
        eval::replaceRClass(session.eval);
        session.eval.rStrings.set(leftover->first, leftover->second);
        eval::declareRPosted(session.eval, "predict", {leftover->first});
      }
    }
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
      // vif.ado keeps each VIF in a `gen`-ed variable (storage type c(type), float by
      // default) and, unless the regressors carry factor-variable operators or
      // `nosort` is given, lists them by `gsort -vif name`; r(name_#)/r(vif_#) are
      // read off that order (bank 4e66fae1 after `regress price mpg weight length`:
      // vif_1 3.8638598918914795 = float(3.86386000511...) for weight, the largest).
      bool fvops = false, nosort = false;
      for (const auto& [ok, ov] : parsed.opts)
        if (ok == "nosort") nosort = true;
      for (const auto& row : estatBlk["coeftable"])
        if (row.value("name", std::string()).find_first_of(".#") != std::string::npos) fvops = true;
      const bool asFloat = session.defaultStorageType != "double";
      for (auto& row : estatBlk["coeftable"])
        if (asFloat && row.contains("b") && row["b"].is_number())
          row["b"] = static_cast<double>(static_cast<float>(row["b"].get<double>()));
      if (!fvops && !nosort) {
        std::vector<ojson> rows(estatBlk["coeftable"].begin(), estatBlk["coeftable"].end());
        std::stable_sort(rows.begin(), rows.end(), [](const ojson& a, const ojson& b) {
          const double va = a.contains("b") && a["b"].is_number() ? a["b"].get<double>() : -HUGE_VAL;
          const double vb = b.contains("b") && b["b"].is_number() ? b["b"].get<double>() : -HUGE_VAL;
          if (va != vb) return va > vb;
          return a.value("name", std::string()) < b.value("name", std::string());
        });
        estatBlk["coeftable"] = rows;
      }
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
    // `erepost ..., rename` keeps e(beta) as a matrix of its own (the columns it
    // had), and `V = M` reposts e(V) the same way as e(b): those stores are kept too.
    std::vector<std::pair<std::string, eval::EvalMatrix>> reposted;
    // properties(str) is the one e() macro erepost writes (ereturn post's own).
    std::optional<std::string> repostedProps, repostedCmd;
    ojson res = runPostestKeepingE(session, [&] {
      ojson r = handlers::runErepost(parsed, session);
      for (const auto& [k, v] : parsed.opts) {
        if (k == "properties") {
          if (const std::string* pr = session.eval.eStrings.find("properties")) repostedProps = *pr;
        }
        if (k == "cmd") {
          if (const std::string* cm = session.eval.eStrings.find("cmd")) repostedCmd = *cm;
        }
      }
      for (const char* nm : {"b", "V", "beta"}) {
        if (const eval::EvalMatrix* m = session.eval.findClassMatrix('e', nm)) reposted.emplace_back(nm, *m);
      }
      return r;
    });
    for (const auto& [nm, m] : reposted) {
      bool put = false;
      for (auto& p : session.eval.eMatrices) {
        if (p.first == nm) {
          p.second = m;
          put = true;
        }
      }
      if (!put) session.eval.eMatrices.emplace_back(nm, m);
    }
    if (repostedProps.has_value()) session.eval.setEString("properties", *repostedProps);
    if (repostedCmd.has_value()) session.eval.setEString("cmd", *repostedCmd);
    pushEnvResult(blocks, std::move(res));
    return true;
  }
  return false;
}

}
