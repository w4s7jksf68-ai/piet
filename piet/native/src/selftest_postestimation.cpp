// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: postestimation.
#include "selftest_support.h"
#include "selftest_cases.h"


void testSuccessfulReg3TestLincomResetRc() {
  std::printf("[selftest] --- successful reg3/test/lincom reset stale _rc ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_success_rc").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto hasError = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    }
    return false;
  };

  piet::SessionState session(workdir);
  auto opened = piet::dispatch("sysuse fatalities, clear", session);
  check(!hasError(opened), "stale _rc setup: `sysuse fatalities, clear` succeeds");

  session.eval.locals.set("_rc", "198");
  auto reg3 = piet::dispatch("reg3 (income beertax spirits) (emppop unemp income)", session);
  check(!hasError(reg3), "stale _rc: target `reg3` succeeds");
  check(session.eval.locals.get("_rc", "") == "0",
        "stale _rc: successful `reg3` resets seeded _rc=198 to 0");

  auto fitted = piet::dispatch("regress income beertax spirits", session);
  check(!hasError(fitted), "stale _rc setup: `regress income beertax spirits` succeeds");

  // BUG-1509 re-pin: test.ado runs no internal capture, so StataBE 19.5 leaves
  // `_rc` as it was (scratch-laneA/agents/file-new-1/i4b.do `RS2|test x1|111`,
  // rlife/side.do `PO|test x1|rc=111`).
  session.eval.locals.set("_rc", "1");
  auto test = piet::dispatch("test beertax spirits", session);
  check(!hasError(test), "stale _rc: target `test` succeeds");
  check(session.eval.locals.get("_rc", "") == "1",
        "stale _rc: successful `test` leaves a seeded _rc=1 alone (BUG-1509)");

  session.eval.locals.set("_rc", "1");
  auto lincom = piet::dispatch("lincom beertax - spirits", session);
  check(!hasError(lincom), "stale _rc: target `lincom` succeeds");
  check(session.eval.locals.get("_rc", "") == "0",
        "stale _rc: successful `lincom` resets seeded _rc=1 to 0");

  fs::remove_all(workdir, ec);
}


// --- O. BETA Phase A4 — context spine (BS-013/046/047/048/091/098/100/101) -
// One leg per mechanism, mirroring the A1-A3 phases' "focused leg per named
// defect" convention. None of these are exercised by the frozen kitchensink
// corpus (verified at implementation time: debug_all.do's ONE post-
// estimation section uses a plain, unclustered `regress` and is truncated by
// an earlier, unrelated `predict ck, cooksd` MUST-ERROR probe before it ever
// reaches test/lincom/nlcom/estat/margins; it also has zero capture/_rc,
// zero c(...) tokens, and zero `set level|clevel|seed|graphics` lines) — so
// these selftest legs are the ONLY exercise of this phase's fix, in-process.

void testBS013VceInheritancePostEstimation() {
  std::printf("[selftest] --- BS-013/BS-098 lastModel vcov/cluster inheritance: "
              "test/lincom/nlcom no longer silently revert to iid ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs013").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  auto firstBlockOfType = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == type) return b;
    }
    return nlohmann::json();
  };
  auto coefSe = [](const nlohmann::json& est, const std::string& field,
                   const std::string& name) -> double {
    for (const auto& row : est.value(field, nlohmann::json::array())) {
      if (row.value("name", std::string()) == name || row.value("exp", std::string()) == name) {
        return row.value("se", -1.0);
      }
    }
    return -1.0;
  };

  double seClusterFromRegress = -1.0, seClusterFromLincom = -1.0, seIidFromLincom = -1.0;

  // --- clustered lastModel: test/lincom/nlcom must inherit vcov=cluster +
  // cluster(year), not silently revert to iid (the exact §1f finding).
  {
    std::string wd = workdir + "/clustered";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as mission 3 / BUG A / BUG-039
      // / BUG-055 / BUG-056 above (FIX-SELFTEST-ISOBUILD-CRASH): without
      // fatalities.csv, `est` below would be a null "error" block and
      // coefSe()'s unguarded .value() would crash. Skip the whole clustered
      // leg honestly (seClusterFromRegress/seClusterFromLincom stay at their
      // -1.0 default, so the headline §1f comparison below FAILs honestly
      // rather than crashing).
      skip("BS-013 clustered leg", "`sysuse fatalities` setup returned no data");
    } else {
      auto jReg = runCmd(session, "regress fatal beertax, cluster(year)");
      auto est = firstBlockOfType(jReg, "estimation");
      seClusterFromRegress = coefSe(est, "coeftable", "beertax");
      check(seClusterFromRegress > 0,
            "BS-013 setup: `regress fatal beertax, cluster(year)` reports a beertax SE");

      auto jLincom = runCmd(session, "lincom beertax");
      auto lincomEst = firstBlockOfType(jLincom, "estimation");
      check(!lincomEst.is_null(), "BS-013: `lincom beertax` after a clustered regress succeeds");
      seClusterFromLincom = coefSe(lincomEst, "coefs", "beertax");
      check(lincomEst.value("vcov_type", std::string()).rfind("cluster", 0) == 0,
            "BS-013: lincom's OWN reported vcov_type is 'cluster' — before this fix "
            "lastModelBaseArgv never forwarded --vcov/--cluster, so this silently read 'iid'");
      check(seClusterFromLincom > 0 &&
                std::abs(seClusterFromLincom - seClusterFromRegress) < 1e-6,
            "BS-013: `lincom beertax`'s SE matches the clustered regress's OWN beertax SE exactly "
            "(a single-variable lincom is mathematically identical to reading _se[beertax])");

      auto jTest = runCmd(session, "test beertax");
      auto testEst = firstBlockOfType(jTest, "estimation");
      check(!testEst.is_null(), "BS-013: `test beertax` after a clustered regress succeeds");
      check(testEst.value("vcov_type", std::string()).rfind("cluster", 0) == 0,
            "BS-013: test's OWN reported vcov_type is 'cluster' too (same shared "
            "lastModelBaseArgv fix)");

      auto jNlcom = runCmd(session, "nlcom 2*_b[beertax]");
      auto nlcomEst = firstBlockOfType(jNlcom, "estimation");
      check(!nlcomEst.is_null(), "BS-013: `nlcom 2*_b[beertax]` after a clustered regress succeeds");
      check(nlcomEst.value("vcov_type", std::string()).rfind("cluster", 0) == 0,
            "BS-013: nlcom's OWN reported vcov_type is 'cluster' too (runNlcom now shares "
            "lastModelBaseArgv instead of hand-rolling {data,y,x,fe} without vcov/cluster)");
    }
  }

  // --- plain (iid) lastModel: lincom/test correctly STAY iid — the fix must
  // not force cluster onto a model that never used it.
  {
    std::string wd = workdir + "/iid";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as the clustered leg above
      // (FIX-SELFTEST-ISOBUILD-CRASH). seIidFromLincom stays at its -1.0
      // default, so the headline §1f comparison below FAILs honestly.
      skip("BS-013 iid regression-guard leg", "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "regress fatal beertax");

      auto jLincom = runCmd(session, "lincom beertax");
      auto lincomEst = firstBlockOfType(jLincom, "estimation");
      seIidFromLincom = coefSe(lincomEst, "coefs", "beertax");
      check(lincomEst.value("vcov_type", std::string()) == "iid",
            "BS-013 regression guard: lincom after a PLAIN regress stays iid (the fix does not "
            "force cluster everywhere)");

      auto jTest = runCmd(session, "test beertax");
      auto testEst = firstBlockOfType(jTest, "estimation");
      check(testEst.value("vcov_type", std::string()) == "iid",
            "BS-013 regression guard: test after a PLAIN regress stays iid too");
    }
  }

  // --- the headline §1f assertion: the SAME single-variable lincom's SE is
  // CLEARLY different depending on whether the underlying model was clustered.
  check(seClusterFromLincom > 0 && seIidFromLincom > 0,
        "BS-013: both lincom SEs were captured (setup sanity)");
  check(std::abs(seClusterFromLincom - seIidFromLincom) > 1e-4,
        "BS-013 (audit sec1f probe shape): lincom's cluster-inherited SE (" +
            std::to_string(seClusterFromLincom) + ") is CLEARLY DISTINCT from the iid SE (" +
            std::to_string(seIidFromLincom) +
            ") for the exact same beertax coefficient — before this fix both were silently "
            "identical (lincom always read iid regardless of the fitted model's own vcov)");

  fs::remove_all(workdir, ec);
}


// FIX-RESTIMATE: two coupled parity gaps that broke a real User script
// (analysis.do's `capture lincom ... / if !_rc & !missing(r(estimate)) ...`
// idiom, skipping whole run_one_depvar groups):
//   half 1 — an UNSET r() inside an if/while CONDITION must evaluate to Stata
//            missing (`.`), NOT raise "r(name) is not set". (The command-line
//            path — `gen z = r(mean)` — keeps its honest error; goldens pin it.)
//   half 2 — `lincom`/`nlcom` are r-class: they must populate r(estimate)/
//            r(se)/… . captureRFromBlock had the logic (Branch 2) but was wired
//            only after `summarize`, so r() stayed empty after every lincom.
void testFIXRestimateUnsetRAndLincomFill() {
  std::printf("[selftest] --- FIX-RESTIMATE unset-r-in-condition = missing + lincom r-class fill ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_restimate").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // --- half 1: the guard idiom survives an unset r() after a failed capture.
  {
    std::string wd = workdir + "/half1";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    // Fit a model, then capture a lincom of a coefficient that does NOT exist:
    // _rc != 0 and r() is left unset — exactly analysis.do's shape.
    std::string src =
        "sysuse fatalities, clear\n"
        "regress fatal beertax\n"
        "capture lincom nosuchcoef_zzz\n"
        "if !_rc & !missing(r(estimate)) & !missing(r(se)) {\n"
        "    gen restimate_should_not_exist = 1\n"
        "}\n"
        "gen restimate_reached_end = 1\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!result.stoppedAtReason.has_value(),
          "FIX-RESTIMATE half1: `if !_rc & !missing(r(estimate))` after a failed `capture lincom` "
          "does NOT error on the unset r() (Stata evaluates it as missing; pre-fix engine_error "
          "halted the do-file here, skipping the whole downstream group)");
    check(session.hasData() && session.datastore()->has("restimate_reached_end"),
          "FIX-RESTIMATE half1: execution reached the line AFTER the guard — the do-file did not halt");
    check(session.hasData() && !session.datastore()->has("restimate_should_not_exist"),
          "FIX-RESTIMATE half1: the guarded body did NOT run — the guard is correctly False "
          "(_rc!=0 and r(estimate) missing), matching Stata's non-short-circuit `&`");
  }

  // --- half 2: lincom after a reghdfe-style FE model fills r(estimate)/r(se)
  // and the guard then runs the post body.
  {
    std::string wd = workdir + "/half2";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    std::string src =
        "sysuse fatalities, clear\n"
        // BUG-473 (User ruling 2026-09-09: match Stata): plain `regress`
        // now honestly rejects `absorb()` (r(198), matches StataBE —
        // tests/eval/golden/aer_b4/b5_absorb_class.log:32-36). `areg` is
        // the command that HAS this option and gives the same numbers by
        // construction (runAreg -> runRegress("areg") through the same FE
        // code path) — swap the command word, not the test's subject.
        "areg fatal beertax, absorb(year)\n"  // reghdfe-style FE (absorb)
        "lincom beertax\n"
        "if !_rc & !missing(r(estimate)) & !missing(r(se)) {\n"
        "    gen restimate_posted = 1\n"
        "}\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!result.stoppedAtReason.has_value(),
          "FIX-RESTIMATE half2: reghdfe-style regress + lincom + guarded post runs cleanly");
    const double* est = session.eval.rValues.find("estimate");
    const double* se = session.eval.rValues.find("se");
    check(est != nullptr && se != nullptr,
          "FIX-RESTIMATE half2: `lincom beertax` populates r(estimate) AND r(se) — "
          "captureRFromBlock was wired only for summarize before, so lincom left r() empty and "
          "every downstream `r(estimate)` read errored");
    check(est != nullptr && std::isfinite(*est) && se != nullptr && std::isfinite(*se) && *se > 0,
          "FIX-RESTIMATE half2: the captured r(estimate)/r(se) are finite (a single-variable "
          "lincom equals _b[beertax]/_se[beertax])");
    check(session.hasData() && session.datastore()->has("restimate_posted"),
          "FIX-RESTIMATE half2: the guard was TRUE (r() non-missing) so the post body ran — "
          "the full `capture lincom / if !missing(r()) { post }` idiom now works end-to-end");
  }

  fs::remove_all(workdir, ec);
}


void testBUG155PredictOnlyRequestedColumn() {
  std::printf("[selftest] --- BUG-155 `predict, xb` creates ONLY the requested column ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug155").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  if (!session.hasData()) {
    // BUG-1939: with no rows the sentinel read below walked off an empty
    // column (segfault in a public clone whose sysuse could not find the
    // sample); a setup miss is a declared skip, never a crash.
    skip("BUG-155 (all legs)", "`sysuse fatalities` setup returned no data");
    fs::remove_all(workdir, ec);
    return;
  }

  // Arm 1 (THE RED ARM — this is the pre-fix defect verbatim): a user variable
  // that happens to be called `resid` must survive `predict <name>, xb`. Pre-fix,
  // the engine appended BOTH columns under their default names on every call, so
  // this sentinel was silently replaced by the regression's residuals.
  (void)piet::dispatch("generate double resid = 99999", session);
  (void)piet::dispatch("regress fatal beertax", session);
  (void)piet::dispatch("predict double yhat, xb", session);
  piet::Datastore* ds = session.datastore();
  check(ds != nullptr && ds->has("yhat"), "BUG-155: the requested column `yhat` is created");
  bool sentinelIntact = false;
  if (ds != nullptr && ds->has("resid")) {
    const double* vals = ds->col("resid").f64();
    sentinelIntact = vals != nullptr && ds->nRows() > 0 && vals[0] == 99999.0;
  }
  check(sentinelIntact,
        "BUG-155: a pre-existing user variable named `resid` is NOT overwritten by "
        "`predict, xb` (was: silently replaced by the regression residuals — user data "
        "destroyed by a command that Stata says touches only the named newvar)");

  // Arm 2: the unrequested column is not conjured into existence either.
  (void)piet::dispatch("drop resid", session);
  (void)piet::dispatch("regress fatal beertax", session);
  (void)piet::dispatch("predict double yhat2, xb", session);
  piet::Datastore* ds2 = session.datastore();
  check(ds2 != nullptr && ds2->has("yhat2") && !ds2->has("resid"),
        "BUG-155: `predict, xb` appends no second `resid` column of its own");

  // Arm 3: the residual side still works when it IS what was asked for.
  (void)piet::dispatch("predict double e1, resid", session);
  piet::Datastore* ds3 = session.datastore();
  check(ds3 != nullptr && ds3->has("e1") && !ds3->has("xb"),
        "BUG-155: `predict, resid` appends only the named residual column (no stray `xb`)");

  fs::remove_all(workdir, ec);
}


void testD5BS086MarginsAt() {
  std::printf("[selftest] --- D5d/BS-086 margins at()/dydx() context forward ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d5_bs086").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);
  (void)piet::dispatch("logit high income unemp", session);

  // D5d dydx(income): dydx=-.0001192 se=.0000112 (this path was ALREADY
  // correct pre-batch — cobra margins --dydx --model logit genuinely
  // applies the logistic link; pinned here as a regression guard).
  {
    auto blocks = piet::dispatch("margins, dydx(income)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5d: `margins, dydx(income)` after logit returns an estimation block");
    if (est != nullptr) {
      auto dydx = coefFieldD1(*est, "income", "b");
      check(dydx.has_value() && std::abs(*dydx - (-0.0001192)) < 0.000001,
            "D5d oracle: dydx(income) ~= -.0001192, got " +
                (dydx.has_value() ? std::to_string(*dydx) : "MISSING"));
      auto se = coefFieldD1(*est, "income", "se");
      check(se.has_value() && std::abs(*se - 0.0000112) < 0.000001,
            "D5d oracle: se ~= .0000112, got " + (se.has_value() ? std::to_string(*se) : "MISSING"));
    }
  }

  // D5d/BS-086, SUPERSEDED BY THE BUG-188 FIX (ONESHOT-PREP9, 2026-08-06).
  //
  // This leg used to pin the HONEST REFUSAL: the plain-`at()` branch rejected
  // after any non-OLS lastModel (kAtSafeCmds gate) because the engine was
  // believed to re-fit plain OLS regardless of --model, which would have
  // returned 0.41098... against Stata's logistic-link 0.4074234 — a ~1% gap.
  // The refusal was the right call WHILE it was true.
  //
  // What the fix changed: the branch now forwards `--model` for the
  // kDydxAtModels family, exactly as the neighbouring dydx()+at() branch had
  // always done. So the premise the refusal rested on is gone — the engine's
  // GLM at() site (cmds_postest.cpp) honours --model and applies the real link.
  //
  // The pin therefore INVERTS rather than disappears: it must now assert the
  // right ANSWER, using the very oracle the old comment recorded. That number
  // was captured from real Stata by a different investigation BEFORE this fix
  // existed, which makes it an independent check on the fix rather than a
  // restatement of it. Measured on the fixed build: 0.40742339233027786.
  // Deleting this leg instead of inverting it would have thrown away the one
  // in-tree witness that distinguishes "forwards --model" from "re-fits OLS".
  {
    auto blocks = piet::dispatch("margins, at(income=13000)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr,
          "D5d/BS-086: `margins, at(...)` after `logit` now ANSWERS (BUG-188 fixed; the "
          "kAtSafeCmds refusal is superseded by forwarding --model)");
    if (est != nullptr) {
      // NOT coefFieldD1, and NOT "coeftable": an at()-form margins block puts
      // its rows under a "margins" array, keyed by "at" (not "name") with the
      // value in "margin" (not "b"). coefFieldD1 returns nullopt on all three
      // counts, so the leg would read MISSING — a false RED that looks exactly
      // like a real regression. Verified against the live block shape:
      // keys = [type, estimator, depvar, n_obs, margins, df].
      std::optional<double> m;
      if (est->contains("margins") && (*est)["margins"].is_array()) {
        for (const auto& row : (*est)["margins"]) {
          if (row.value("at", std::string()) == "income=13000" &&
              row.contains("margin") && row["margin"].is_number()) {
            m = row["margin"].get<double>();
          }
        }
      }
      check(m.has_value() && std::abs(*m - 0.4074234) < 0.000001,
            "D5d/BS-086 oracle: margin ~= .4074234 (Stata's logistic link), NOT .41098 (the "
            "OLS-refit answer the old refusal existed to avoid); got " +
                (m.has_value() ? std::to_string(*m) : "MISSING"));
    }
  }

  // Regression guard: margins, at(...) after a plain OLS fit (feols/
  // regress) is genuinely safe (the engine's OLS refit reproduces the
  // model's own coefficients exactly) and must still WORK.
  (void)piet::dispatch("regress income beertax", session);
  {
    auto blocks = piet::dispatch("margins, at(beertax=0.5)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr,
          "D5d/BS-086: `margins, at(...)` after `regress` (kAtSafeCmds) still succeeds");
  }

  fs::remove_all(workdir, ec);
}


// BUG-203: `lincom`/`nlcom`/`test` must read WHICH ESTIMATOR produced lastModel.
//
// Before this fix runLincom/runNlcom/runTestCmd checked only that a lastModel
// existed and forwarded the stored specification to cobra's lincom/nlcom/test
// CLIs, which had no `--model` flag — so cmds_postest.cpp answered every call
// with feols_fit() and the user got a linear-probability-model combination
// presented as the maximum-likelihood fit's result, silently.
//
// The single sharpest discriminator is in this arm's first block and needs no
// oracle to be decisive: `logit` and `poisson` on the SAME y and the SAME x
// have the same OLS refit, so the broken engine answered `lincom` identically
// after both (measured pre-fix: 0.221521751160 for logit, probit AND poisson).
// Two different ML estimators cannot honestly produce the same combination, so
// "these two answers differ" is a property only a fix that reads the estimator
// can satisfy. The numeric parity against real Stata is pinned separately, in
// tests/unit/test_lincom.cpp's test_lincom_ml_refit_bug203 (StataNow 19.5
// capture); this leg pins the WIRING — that PIET forwards the estimator at all.
//
// The identity legs are the second discriminator: a single-variable `lincom x1`
// is mathematically identical to reading _b[x1]/_se[x1] off the fit's own
// coeftable, so comparing the two catches a refit that used any other model
// without needing an external number (the same argument BS-013's leg makes for
// the clustered vcov).
void testBUG203PostestReadsEstimator() {
  std::printf("[selftest] --- BUG-203 lincom/nlcom/test read the ESTIMATOR, not just "
              "the specification ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug203").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto firstEst = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "estimation") return b;
    }
    return nlohmann::ordered_json();
  };
  auto rowValue = [](const nlohmann::ordered_json& est, const char* table, const std::string& name,
                     const char* field) -> double {
    if (!est.contains(table) || !est[table].is_array()) return std::nan("");
    for (const auto& row : est[table]) {
      if (row.value("name", std::string()) == name || row.value("exp", std::string()) == name) {
        if (row.contains(field) && row[field].is_number()) return row[field].get<double>();
      }
    }
    return std::nan("");
  };
  auto errorText = [](const nlohmann::ordered_json& blocks) -> std::string {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return b.value("message", std::string());
      }
    }
    return std::string();
  };

  (void)piet::dispatch("sysuse fatalities, clear", session);
  if (!session.hasData()) {
    // Same isolated/subset-build fixture gap the BS-013 legs above handle:
    // without fatalities.csv there is nothing to fit, and every check below
    // would compare NaNs. Skip honestly rather than fail on the environment.
    skip("BUG-203 postest estimator gate", "`sysuse fatalities` setup returned no data");
    fs::remove_all(workdir, ec);
    return;
  }
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  // ---- the discriminator: same y, same x, two different ML estimators ----
  //
  // ENVIRONMENT vs REGRESSION, and the discrimination is exact. The layer-2 legs
  // need an ENGINE that has `--model` on lincom/nlcom/test. A bare
  // `piet_native --selftest` resolves cobra to the Art 7 pinned reference
  // (cobra_runner.cpp's kPinnedReference), which predates that flag, so the
  // engine arm must be DECLARED (COBRA_BIN=<this tree>/build/cobra) for them to
  // mean anything. Two failure shapes have to be told apart rather than lumped:
  //   - PIET returns its OWN honest refusal ("not supported after `logit`")
  //     => a PIET-side regression: logit is supposed to be forwarded. FAIL.
  //   - PIET returns some other engine error, or no estimation block
  //     => the wired engine has no `--model`. Environment. SKIP the numeric
  //        legs, exactly as the §31 postest gate above skips on a `postest`-less
  //        engine.
  // The PIET-side legs below (the qreg refusal, the predict refusal, the
  // regress guards) need no engine feature at all and always run, so layer 1
  // stays gated even against the archive pin.
  double logitLincom = std::nan(""), poisLincom = std::nan("");
  bool engineHasModelFlag = false;
  {
    auto logitEst = firstEst(piet::dispatch("logit high income unemp", session));
    check(!logitEst.is_null(), "BUG-203 setup: `logit high income unemp` returns an estimation "
                               "block");
    const double bLogit = rowValue(logitEst, "coeftable", "income", "b");
    const double seLogit = rowValue(logitEst, "coeftable", "income", "se");

    auto lincomBlocks = piet::dispatch("lincom income", session);
    auto lincomEst = firstEst(lincomBlocks);
    const std::string lincomErr = errorText(lincomBlocks);
    check(lincomErr.find("not supported after") == std::string::npos,
          "BUG-203: `lincom` after `logit` is NOT PIET's own honest refusal — logit is in the "
          "forwarded set, so seeing the refusal here would be a PIET-side regression");
    engineHasModelFlag = !lincomEst.is_null();
    if (!engineHasModelFlag) {
      skip("BUG-203 layer-2 numeric legs",
           "the wired cobra has no `--model` on lincom/nlcom/test (declare the engine arm: "
           "COBRA_BIN=<tree>/build/cobra) — engine error: " + lincomErr);
    } else {
      logitLincom = rowValue(lincomEst, "coefs", "income", "b");
      const double seLincom = rowValue(lincomEst, "coefs", "income", "se");
      check(std::isfinite(logitLincom) && std::isfinite(bLogit) &&
                std::fabs(logitLincom - bLogit) <= 1e-9 * (std::fabs(bLogit) + 1e-30),
            "BUG-203: `lincom income` after `logit` equals the logit fit's OWN income "
            "coefficient — a single-variable lincom IS _b[income], so any other refit shows up "
            "here (pre-fix it was the plain-OLS one)");
      check(std::isfinite(seLincom) && std::isfinite(seLogit) &&
                std::fabs(seLincom - seLogit) <= 1e-9 * (std::fabs(seLogit) + 1e-30),
            "BUG-203: and its SE equals the logit fit's own income SE (the ML information "
            "matrix, not an OLS residual-variance one)");
      check(lincomEst.value("vcov_type", std::string()) == "oim",
            "BUG-203: lincom's own reported vcov_type after `logit` is 'oim' — pre-fix it read "
            "'iid', the tell that an OLS refit had answered");

      auto testEst = firstEst(piet::dispatch("test income", session));
      check(!testEst.is_null(), "BUG-203: `test income` after `logit` returns a result");
      check(!testEst.is_null() && testEst.contains("chi2") && !testEst.contains("F"),
            "BUG-203: `test` after an ML fit reports chi2(J) and NO F — an ML fit has no "
            "residual dof for an F to be referenced against (Stata prints chi2 there)");

      // BUG-205(3): `kTestRMap` (eval/macros.cpp) used to map ONLY `p_F -> p`,
      // and the ML block above emits no `p_F` at all (correctly — there is no
      // F), so r(p) was left unset entirely after `test` on an ML fit. Stata's
      // own `test` is r(p) on BOTH the F and chi2 forms — there is no
      // `r(p_chi2)` in real Stata. The fix maps `p_chi2 -> p` too (only one of
      // the two source keys is ever present on a given block).
      const double* rP = session.eval.rValues.find("p");
      const double pChi2Wire = testEst.is_null() ? std::nan("") : testEst.value("p_chi2", std::nan(""));
      check(rP != nullptr, "BUG-205(3): r(p) IS SET after `test` on an ML fit (pre-fix: unset — "
                           "kTestRMap only mapped p_F, and the ML block emits no p_F)");
      check(rP != nullptr && std::isfinite(pChi2Wire) &&
                std::fabs(*rP - pChi2Wire) < 1e-12,
            "BUG-205(3): and r(p) equals the block's own chi2 p-value (Stata's `test` is r(p) on "
            "BOTH the F and chi2 forms — there is no r(p_chi2))");
      check(session.eval.rValues.find("chi2") != nullptr,
            "BUG-205(3): r(chi2) is also set (unaffected by this fix — already correct)");
      check(session.eval.rValues.find("df") != nullptr,
            "BUG-205(3): r(df) is also set (unaffected by this fix — already correct)");

      // BUG-338 (p5-wave regression, 2026-08-08): the first BUG-205(3) fix
      // mapped p_chi2 -> p UNCONDITIONALLY, on the premise that a test block
      // never carries both p_F and p_chi2. Measured false: `test` after a
      // clustered-VCE REGRESS emits both, and the chi2 mapping ran last, so
      // r(p) became the chi2 tail (9.7e-40) where Stata answers the F-based
      // p (6.3e-26). These legs pin the F-form: when the block carries an F,
      // r(p) must equal p_F and r(F)/r(df_r) must be set; r(chi2) must NOT be
      // (Stata's F-form test sets no r(chi2)).
      {
        auto ols = firstEst(piet::dispatch("regress income unemp beertax, cluster(state)", session));
        check(!ols.is_null(), "BUG-338 setup: clustered regress returns a fit");
        auto tb = firstEst(piet::dispatch("test unemp", session));
        check(!tb.is_null() && tb.contains("p_F") && tb.contains("p_chi2"),
              "BUG-338 premise: this test block carries BOTH p_F and p_chi2 (the shape the "
              "first fix said could not exist — if this leg ever fails, the engine stopped "
              "emitting both and the F-priority split can be simplified)");
        const double* rP2 = session.eval.rValues.find("p");
        const double pF = tb.is_null() ? std::nan("") : tb.value("p_F", std::nan(""));
        check(rP2 != nullptr && std::isfinite(pF) && std::fabs(*rP2 - pF) < 1e-15 + 1e-12 * std::fabs(pF),
              "BUG-338: r(p) after F-form test == the block's p_F, NOT its p_chi2");
        check(session.eval.rValues.find("F") != nullptr && session.eval.rValues.find("df_r") != nullptr,
              "BUG-338: r(F)/r(df_r) set on the F form");
        check(session.eval.rValues.find("chi2") == nullptr,
              "BUG-338: r(chi2) NOT set on the F form (Stata sets it only on the chi2 form)");
      }
    }
  }
  if (engineHasModelFlag) {
    auto poisEst = firstEst(piet::dispatch("poisson high income unemp", session));
    check(!poisEst.is_null(), "BUG-203 setup: `poisson high income unemp` returns an "
                              "estimation block");
    const double bPois = rowValue(poisEst, "coeftable", "income", "b");

    auto lincomEst = firstEst(piet::dispatch("lincom income", session));
    poisLincom = rowValue(lincomEst, "coefs", "income", "b");
    check(std::isfinite(poisLincom) && std::isfinite(bPois) &&
              std::fabs(poisLincom - bPois) <= 1e-8 * (std::fabs(bPois) + 1e-30),
          "BUG-203: `lincom income` after `poisson` equals the poisson fit's OWN income "
          "coefficient");
    check(std::isfinite(logitLincom) && std::isfinite(poisLincom) &&
              std::fabs(logitLincom - poisLincom) > 1e-6 * (std::fabs(logitLincom) + 1e-30),
          "BUG-203 HEADLINE: `lincom income` after `logit` and after `poisson` — same y, same "
          "x — now give DIFFERENT answers. Pre-fix they were identical to every digit, which "
          "is the proof the estimator was not being read at all");
  }

  // ---- BUG-204: `predict` after an estimator with NO engine prediction path
  // must not write a column. This leg originally pinned the refusal after
  // `logit`; BUG-297 taught the engine the real logit/probit/poisson
  // predictions (range-pinned against a fresh Stata capture in
  // testBUG297PredictGlm), so the refusal — and this leg — moved to `qreg`,
  // which genuinely still has no re-fit. The invariant is unchanged: an error
  // block plus a written column would be the worst of both.
  {
    (void)piet::dispatch("qreg fatal beertax", session);
    auto blocks = piet::dispatch("predict phat203", session);
    const std::string msg = errorText(blocks);
    check(msg.find("not supported after") != std::string::npos &&
              msg.find("qreg") != std::string::npos,
          "BUG-204: `predict` after `qreg` is an honest refusal that NAMES the estimator");
    auto probe = piet::dispatch("summarize phat203", session);
    check(!errorText(probe).empty(),
          "BUG-204: and it wrote NO column — `summarize phat203` still errors, so the refused "
          "prediction never reached the dataset");
  }

  // ---- the estimators with no ML refit still honest-reject ---------------
  {
    (void)piet::dispatch("qreg fatal beertax", session);
    auto blocks = piet::dispatch("lincom beertax", session);
    const std::string msg = errorText(blocks);
    check(msg.find("not supported after") != std::string::npos &&
              msg.find("qreg") != std::string::npos,
          "BUG-203: `lincom` after an estimator with no ML refit (`qreg`) is an honest "
          "refusal that NAMES the estimator, not a silently-served OLS combination");
  }

  // ---- the OLS family is untouched --------------------------------------
  {
    auto regEst = firstEst(piet::dispatch("regress fatal beertax unemp", session));
    const double bReg = rowValue(regEst, "coeftable", "beertax", "b");
    auto lincomEst = firstEst(piet::dispatch("lincom beertax", session));
    const double bLincom = rowValue(lincomEst, "coefs", "beertax", "b");
    check(std::isfinite(bLincom) && std::isfinite(bReg) &&
              std::fabs(bLincom - bReg) <= 1e-12 * (std::fabs(bReg) + 1e-30),
          "BUG-203 regression guard: `lincom` after `regress` still answers from the "
          "least-squares fit, unchanged (no --model forwarded, historical argv)");
    check(!lincomEst.is_null() && lincomEst.value("vcov_type", std::string()) == "iid",
          "BUG-203 regression guard: and still reports vcov_type 'iid' there");
    auto predBlocks = piet::dispatch("predict yhat203", session);
    check(errorText(predBlocks).empty(),
          "BUG-204 regression guard: `predict` after `regress` still works — the refusal is "
          "scoped to estimators whose prediction the engine cannot compute");
  }

  fs::remove_all(workdir, ec);
}



// BUG-297 — BUG-204's open half, closed for logit/probit/poisson: `predict`
// after an ML fit now writes the model's OWN prediction (engine `predict
// --model` through BUG-203's postest_fit_spec dispatcher), and the honest
// refusal narrowed to the estimators that genuinely still have no engine
// prediction path.
//
// Oracle: StataNow 19.5, StataBE -b, judged by in-log WSW297_ markers (fresh
// capture 2026-08-07: `import delimited fatalities.csv, asdouble` -> `gen high
// = (beertax > .5)` -> fit -> `predict double ...`). PER BUG-204'S OWN
// BLINDNESS NOTE, THE PINS HERE ARE PER-OBSERVATION VALUES AND MIN/MAX RANGE,
// NEVER THE MEAN ALONE — the original defect's mean matched Stata to 15 digits
// (OLS fitted values and logit probabilities both average to ybar), which is
// exactly why every mean-based gate was blind to a column containing
// probabilities of -0.067 and 1.206.
//
// Tolerance 1e-6 (probit legs measure ~1e-13; logit/poisson legs measure
// 3e-10..8e-8 — the engine/Stata ML convergence floor, amplified in the
// distribution tails and by cancellation in eta; same grade family as the
// BUG-203 lincom pins at 1e-9/1e-8 on the raw coefficients). The defect this
// pins against sits 5+ orders above it: the OLS column's min/max were wrong by
// ~1e-1 ABSOLUTE and out of [0,1] entirely.
//
// ENVIRONMENT vs REGRESSION, same discriminator as testBUG203: the numeric
// legs need an engine with `--model` on predict, so a bare selftest against
// the pinned archive engine must SKIP them (engine error that is NOT PIET's
// own refusal), while PIET's own refusal appearing for logit is a FAIL — logit
// is in the forwarded set now. The refusal legs run against any engine.
void testBUG297PredictGlm() {
  std::printf("[selftest] --- BUG-297 predict after an ML fit writes the model's OWN "
              "prediction (range-pinned) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug297").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto errorText = [](const nlohmann::ordered_json& blocks) -> std::string {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        return b.value("message", std::string());
      }
    }
    return std::string();
  };
  // Read a freshly predicted f64 column straight out of the live Datastore —
  // the same bytes a save/export would persist, which is the surface BUG-204
  // was about (the wrong number OUTLIVED the session).
  auto colVals = [&session](const std::string& name, std::vector<double>& out) -> bool {
    piet::Datastore* ds = session.datastore();
    if (ds == nullptr || !ds->has(name)) return false;
    const double* v = ds->col(name).f64();
    if (v == nullptr) return false;
    out.assign(v, v + ds->nRows());
    return true;
  };
  auto rel = [](double got, double want) {
    return std::fabs(got - want) / (std::fabs(want) + 1e-30);
  };
  const double kTol = 1e-6;

  (void)piet::dispatch("sysuse fatalities, clear", session);
  if (!session.hasData()) {
    skip("BUG-297 predict GLM", "`sysuse fatalities` setup returned no data");
    fs::remove_all(workdir, ec);
    return;
  }
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  // ---- logit: bare `predict p` = pr, range-pinned --------------------------
  bool engineHasPredictModel = false;
  {
    (void)piet::dispatch("logit high income unemp", session);
    auto blocks = piet::dispatch("predict p", session);
    const std::string err = errorText(blocks);
    check(err.find("not supported after") == std::string::npos,
          "BUG-297: `predict` after `logit` is NOT PIET's own refusal any more — logit is in "
          "the forwarded set, so the refusal here would be a PIET-side regression");
    engineHasPredictModel = err.empty();
    if (!engineHasPredictModel) {
      skip("BUG-297 numeric legs",
           "the wired cobra has no `--model` on predict (declare the engine arm: "
           "COBRA_BIN=<tree>/build/cobra) — engine error: " + err);
    }
  }
  if (engineHasPredictModel) {
    std::vector<double> p;
    check(colVals("p", p) && p.size() == 336,
          "BUG-297: bare `predict p` after `logit` writes the column");
    if (p.size() == 336) {
      double mn = p[0], mx = p[0], sum = 0.0;
      for (double v : p) { mn = std::min(mn, v); mx = std::max(mx, v); sum += v; }
      check(mn >= 0.0 && mx <= 1.0,
            "BUG-297 THE RANGE PIN: every predicted probability is inside [0,1] — the OLS "
            "column this replaces ran -0.067..1.206 on this class of fit");
      check(rel(mn, 0.0042480614277093575) <= kTol && rel(mx, 0.8207965615283322469) <= kTol,
            "BUG-297: min/max match Stata's own `predict` (WSW297_LOGIT_PR_MIN/MAX)");
      check(rel(p[0], 0.2614877779953532211) <= kTol && rel(p[1], 0.2806424623828328024) <= kTol &&
                rel(p[167], 0.5401452564796866973) <= kTol &&
                rel(p[335], 0.4561970986456775345) <= kTol,
            "BUG-297: per-observation probabilities p[1],p[2],p[168],p[336] match Stata "
            "(never the mean alone — the mean was blind by construction)");
      check(rel(sum / 336.0, 0.3154761913283222752) <= kTol,
            "BUG-297: and the mean agrees too (necessary, never sufficient)");
    }
    // explicit `pr` = the same default
    (void)piet::dispatch("predict p2, pr", session);
    std::vector<double> p2;
    check(colVals("p2", p2) && !p2.empty() && rel(p2[0], 0.2614877779953532211) <= kTol,
          "BUG-297: explicit `predict p2, pr` after `logit` = the bare default");
    // xb variant = the linear index, NOT bounded to [0,1]
    (void)piet::dispatch("predict xb297, xb", session);
    std::vector<double> xb;
    check(colVals("xb297", xb) && xb.size() == 336,
          "BUG-297: `predict xb297, xb` after `logit` writes the linear index");
    if (xb.size() == 336) {
      double mn = xb[0], mx = xb[0];
      for (double v : xb) { mn = std::min(mn, v); mx = std::max(mx, v); }
      check(rel(xb[0], -1.038250107258576804) <= kTol &&
                rel(xb[167], 0.1609274322070710639) <= kTol &&
                rel(mn, -5.457035424687600056) <= kTol && rel(mx, 1.521753596979918299) <= kTol,
            "BUG-297: xb per-obs and min/max match Stata (and it is NOT the pr column: "
            "min -5.46)");
    }
    // ---- probit ------------------------------------------------------------
    (void)piet::dispatch("probit high income unemp", session);
    (void)piet::dispatch("predict pp297", session);
    std::vector<double> pp;
    check(colVals("pp297", pp) && pp.size() == 336,
          "BUG-297: bare `predict` after `probit` writes the column");
    if (pp.size() == 336) {
      double mn = pp[0], mx = pp[0];
      for (double v : pp) { mn = std::min(mn, v); mx = std::max(mx, v); }
      check(mn >= 0.0 && mx <= 1.0, "BUG-297: probit probabilities inside [0,1]");
      check(rel(pp[0], 0.2812836109189303935) <= kTol &&
                rel(pp[167], 0.5350157238644669411) <= kTol &&
                rel(mn, 0.0008714574064782166) <= kTol && rel(mx, 0.8056559811482841837) <= kTol,
            "BUG-297: probit pr per-obs + min/max match Stata — and DIFFER from the logit "
            "column's (p[1] .26149 vs .28128), so the estimator is READ, not defaulted");
    }
    // ---- poisson: bare `predict n` = predicted COUNT, not a probability ----
    (void)piet::dispatch("poisson fatal income unemp", session);
    (void)piet::dispatch("predict nh297", session);
    std::vector<double> nh;
    check(colVals("nh297", nh) && nh.size() == 336,
          "BUG-297: bare `predict` after `poisson` writes the column");
    if (nh.size() == 336) {
      double mn = nh[0], mx = nh[0];
      for (double v : nh) { mn = std::min(mn, v); mx = std::max(mx, v); }
      check(rel(nh[0], 1158.42589938783135) <= kTol &&
                rel(nh[167], 626.4122479435875448) <= kTol &&
                rel(nh[335], 668.3011722712437859) <= kTol &&
                rel(mn, 404.621874921810786) <= kTol && rel(mx, 2222.879761849161241) <= kTol,
            "BUG-297: poisson predicted counts (Stata's `n`) per-obs + min/max match Stata — "
            "counts in the hundreds, so an accidental pr/xb here cannot pass");
      check(mn >= 0.0, "BUG-297: predicted counts are non-negative (exp link)");
    }
    (void)piet::dispatch("predict nxb297, xb", session);
    std::vector<double> nxb;
    check(colVals("nxb297", nxb) && !nxb.empty() && rel(nxb[0], 7.054817379278471634) <= kTol,
          "BUG-297: `predict, xb` after `poisson` = log-scale linear index (Stata "
          "WSW297_POIS_XB_1)");
  }

  // ---- the refusal legs need no engine feature and always run --------------
  {
    (void)piet::dispatch("logit high income unemp", session);
    auto blocks = piet::dispatch("predict r297, residuals", session);
    const std::string msg = errorText(blocks);
    check(msg.find("residuals") != std::string::npos && msg.find("logit") != std::string::npos,
          "BUG-297: `predict, residuals` after `logit` refuses BY NAME — Stata's residual "
          "there is a Pearson residual, not y - xb, so serving y - xb would be BUG-204 again");
    auto probe = piet::dispatch("summarize r297", session);
    check(!errorText(probe).empty(),
          "BUG-297: and the refused residual wrote NO column");
    // BUG-1257: this arm pinned the refusal; Stata serves stdp after logit, and so
    // does PIET now (StataBE 19.5 on this data: s297[1] .41163095405962064,
    // [168] .16714780905833523, [336] .15383055321490155). It reads the engine's
    // `--model` refit, as the numeric legs above do.
    if (engineHasPredictModel) {
      auto blocks2 = piet::dispatch("predict double s297, stdp", session);
      std::vector<double> s297;
      check(errorText(blocks2).empty() && colVals("s297", s297) && s297.size() == 336 &&
                rel(s297[0], .41163095405962064) <= 1e-9 &&
                rel(s297[167], .16714780905833523) <= 1e-9 &&
                rel(s297[335], .15383055321490155) <= 1e-9,
            "BUG-1257: `predict, stdp` after `logit` is sqrt(x V x') per obs, as Stata's");
    }
  }
  {
    // An estimator with no engine prediction path keeps the honest refusal —
    // and the message now says exactly what IS covered.
    (void)piet::dispatch("qreg fatal beertax", session);
    auto blocks = piet::dispatch("predict q297", session);
    const std::string msg = errorText(blocks);
    check(msg.find("not supported after") != std::string::npos &&
              msg.find("qreg") != std::string::npos,
          "BUG-297: `predict` after `qreg` still refuses, naming the estimator");
    check(msg.find("logit/probit/poisson") != std::string::npos,
          "BUG-297: and the refusal names the models the engine DOES cover, so its premise "
          "cannot silently expire again");
    auto probe = piet::dispatch("summarize q297", session);
    check(!errorText(probe).empty(), "BUG-297: and it wrote NO column");
  }
  {
    // OLS predict untouched (byte-identity is pinned by the freeze fixture;
    // this is the live-wiring leg).
    (void)piet::dispatch("regress fatal beertax unemp", session);
    auto blocks = piet::dispatch("predict yh297", session);
    check(errorText(blocks).empty(), "BUG-297 regression guard: `predict` after `regress` "
                                     "still works, historical argv, no --model forwarded");
    auto blocksR = piet::dispatch("predict rr297, residuals", session);
    check(errorText(blocksR).empty(),
          "BUG-297 regression guard: `predict, residuals` after `regress` still works");
  }

  fs::remove_all(workdir, ec);
}
