// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: time series estimation.
#include "selftest_support.h"
#include "selftest_cases.h"


// --- BETA campaign Phase D batch D4/D5 (time-series + post-estimation/
// diag/misc wirings): BS-077/078/079/080/081/085/021/082/086/083/084/087.
// Oracle: internal capture
// d45-oracle-stata.log (StataBE 19.5 on piet/packaging/sample/
// fatalities.csv, `gen high=(beertax>.5)` + `encode state, gen(snum)` +
// `gen t=_n` + `tsset t`; sections D4a-D4f, D5a-D5g). --------------------

void testD4BS077Newey() {
  std::printf("[selftest] --- D4a/BS-077 newey (Newey-West HAC) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d4_bs077").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen t = _n", session);
  (void)piet::dispatch("tsset t", session);

  // D4a: `newey income beertax spirits, lag(2)`. Oracle: beertax
  // b=-1697.15 se=353.9058, spirits b=1384.099 se=292.4321, _cons
  // b=12323.98 se=533.9508.
  {
    auto blocks = piet::dispatch("newey income beertax spirits, lag(2)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4a: `newey income beertax spirits, lag(2)` returns an estimation block");
    if (est != nullptr) {
      check(est->value("estimator", std::string()) == "newey", "D4a: estimator label is \"newey\"");
      auto b = coefFieldD1(*est, "beertax", "b");
      check(b.has_value() && std::abs(*b - (-1697.15)) < 0.01,
            "D4a oracle: beertax b ~= -1697.15, got " +
                (b.has_value() ? std::to_string(*b) : "MISSING"));
      auto se = coefFieldD1(*est, "beertax", "se");
      check(se.has_value() && std::abs(*se - 353.9058) < 0.01,
            "D4a oracle: beertax se ~= 353.9058, got " +
                (se.has_value() ? std::to_string(*se) : "MISSING"));
      auto bSpirits = coefFieldD1(*est, "spirits", "b");
      check(bSpirits.has_value() && std::abs(*bSpirits - 1384.099) < 0.01,
            "D4a oracle: spirits b ~= 1384.099, got " +
                (bSpirits.has_value() ? std::to_string(*bSpirits) : "MISSING"));
      auto bCons = coefFieldD1(*est, "(intercept)", "b");
      check(bCons.has_value() && std::abs(*bCons - 12323.98) < 0.01,
            "D4a oracle: _cons b ~= 12323.98, got " +
                (bCons.has_value() ? std::to_string(*bCons) : "MISSING"));
    }
  }

  // lag() is REQUIRED (real Stata errors "option lag() required" if omitted).
  (void)piet::dispatch("capture newey income beertax spirits", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D4/BS-077: `newey ...` without lag() honestly rejects");

  // Requires a prior tsset.
  piet::SessionState session2(workdir + "_notsset");
  (void)piet::dispatch("sysuse fatalities, clear", session2);
  (void)piet::dispatch("capture newey income beertax, lag(2)", session2);
  check(session2.eval.locals.get("_rc", "") != "0",
        "D4/BS-077: `newey` without a prior `tsset` honestly rejects");

  fs::remove_all(workdir, ec);
}


void testD4BS078Prais() {
  std::printf("[selftest] --- D4b/BS-078 prais (Prais-Winsten/Cochrane-Orcutt AR(1)) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d4_bs078").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen t = _n", session);
  (void)piet::dispatch("tsset t", session);

  // D4b twostep: beertax b=-1997.243 se=281.7461, rho=.8034461.
  {
    auto blocks = piet::dispatch("prais income beertax, twostep", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4b: `prais income beertax, twostep` returns an estimation block");
    if (est != nullptr) {
      auto b = coefFieldD1(*est, "beertax", "b");
      check(b.has_value() && std::abs(*b - (-1997.243)) < 0.01,
            "D4b twostep oracle: beertax b ~= -1997.243, got " +
                (b.has_value() ? std::to_string(*b) : "MISSING"));
      auto se = coefFieldD1(*est, "beertax", "se");
      check(se.has_value() && std::abs(*se - 281.7461) < 0.01,
            "D4b twostep oracle: beertax se ~= 281.7461, got " +
                (se.has_value() ? std::to_string(*se) : "MISSING"));
      double rho = est->value("rho", -999.0);
      check(std::abs(rho - 0.8034461) < 0.0001,
            "D4b twostep oracle: rho ~= .8034461, got " + std::to_string(rho));
    }
  }

  // D4b corc: beertax b=-1972.609 se=284.8613, rho=.8038026.
  {
    auto blocks = piet::dispatch("prais income beertax, corc", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4b: `prais income beertax, corc` returns an estimation block");
    if (est != nullptr) {
      auto b = coefFieldD1(*est, "beertax", "b");
      check(b.has_value() && std::abs(*b - (-1972.609)) < 0.01,
            "D4b corc oracle: beertax b ~= -1972.609, got " +
                (b.has_value() ? std::to_string(*b) : "MISSING"));
      double rho = est->value("rho", -999.0);
      check(std::abs(rho - 0.8038026) < 0.0001,
            "D4b corc oracle: rho ~= .8038026, got " + std::to_string(rho));
    }
  }

  // corc and twostep may not be combined.
  (void)piet::dispatch("capture prais income beertax, corc twostep", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D4/BS-078: `prais ..., corc twostep` honestly rejects (mutually exclusive)");

  fs::remove_all(workdir, ec);
}


void testD4BS079DfullerPperron() {
  std::printf("[selftest] --- D4c-d/BS-079 dfuller/pperron (unit-root tests) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d4_bs079").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen t = _n", session);
  (void)piet::dispatch("tsset t", session);

  // D4c bare: Zt=-6.004, cv 1/5/10 = -3.453/-2.877/-2.570.
  {
    auto blocks = piet::dispatch("dfuller income", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4c: `dfuller income` returns an estimation block");
    if (est != nullptr) {
      double zt = est->value("Zt", -999.0);
      check(std::abs(zt - (-6.004257793732445)) < 0.001,
            "D4c oracle: Zt ~= -6.004, got " + std::to_string(zt));
      double cv1 = est->value("cv_1", -999.0);
      check(std::abs(cv1 - (-3.4532)) < 0.001, "D4c oracle: cv_1 ~= -3.4532, got " +
                                                    std::to_string(cv1));
      double cv5 = est->value("cv_5", -999.0);
      check(std::abs(cv5 - (-2.8766)) < 0.001, "D4c oracle: cv_5 ~= -2.8766, got " +
                                                    std::to_string(cv5));
    }
  }

  // D4c lags(2) trend: Zt=-5.873, cv_1=-3.98668.
  {
    auto blocks = piet::dispatch("dfuller income, lags(2) trend", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4c: `dfuller income, lags(2) trend` returns an estimation block");
    if (est != nullptr) {
      double zt = est->value("Zt", -999.0);
      check(std::abs(zt - (-5.873384850758245)) < 0.001,
            "D4c lags(2) trend oracle: Zt ~= -5.873, got " + std::to_string(zt));
      double cv1 = est->value("cv_1", -999.0);
      check(std::abs(cv1 - (-3.98668)) < 0.001,
            "D4c lags(2) trend oracle: cv_1 ~= -3.98668, got " + std::to_string(cv1));
    }
  }

  // pperron has no `drift` form in real Stata.
  (void)piet::dispatch("capture pperron income, drift", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D4/BS-079: `pperron ..., drift` honestly rejects (no drift() form in real pperron)");

  // D4d bare pperron: Zrho=-65.302, Zt=-6.041.
  {
    auto blocks = piet::dispatch("pperron income", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4d: `pperron income` returns an estimation block");
    if (est != nullptr) {
      double zrho = est->value("Zrho", -999.0);
      check(std::abs(zrho - (-65.30208678978194)) < 0.001,
            "D4d oracle: Zrho ~= -65.302, got " + std::to_string(zrho));
      double zt = est->value("Zt", -999.0);
      check(std::abs(zt - (-6.041425741988765)) < 0.001,
            "D4d oracle: Zt ~= -6.041, got " + std::to_string(zt));
    }
  }

  fs::remove_all(workdir, ec);
}


void testD4BS080Wntestq() {
  std::printf("[selftest] --- D4e/BS-080 wntestq (portmanteau white-noise test) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d4_bs080").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // D4e bare: Q=839.9262, df=40 (real Stata's default lag rule,
  // min(n/2-2,40), reproduced automatically by the engine — PIET forwards
  // no --lags at all here).
  {
    auto blocks = piet::dispatch("wntestq income", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4e: `wntestq income` returns an estimation block");
    if (est != nullptr) {
      double q = est->value("Q", -999.0);
      check(std::abs(q - 839.9261650984547) < 0.001,
            "D4e oracle: Q ~= 839.9262, got " + std::to_string(q));
      int df = est->value("df", -1);
      check(df == 40, "D4e oracle: df == 40 (Stata's own default lag rule), got " +
                           std::to_string(df));
    }
  }

  // D4e lags(10): Q=557.2013, df=10.
  {
    auto blocks = piet::dispatch("wntestq income, lags(10)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D4e: `wntestq income, lags(10)` returns an estimation block");
    if (est != nullptr) {
      double q = est->value("Q", -999.0);
      check(std::abs(q - 557.2013247767943) < 0.001,
            "D4e oracle: Q ~= 557.2013, got " + std::to_string(q));
      int df = est->value("df", -1);
      check(df == 10, "D4e oracle: df == 10, got " + std::to_string(df));
    }
  }

  fs::remove_all(workdir, ec);
}


void testD4BS081Tsfill() {
  std::printf("[selftest] --- D4f/BS-081 tsfill (fill panel/ts gaps) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d4_bs081").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen t = _n", session);
  (void)piet::dispatch("tsset t", session);

  // tsfill with no prior tsset -> honest reject.
  piet::SessionState session0(workdir + "_notsset");
  (void)piet::dispatch("sysuse fatalities, clear", session0);
  (void)piet::dispatch("capture tsfill", session0);
  check(session0.eval.locals.get("_rc", "") != "0",
        "D4/BS-081: `tsfill` without a prior `tsset` honestly rejects");

  // BUG-897: `full` is served (cobra tsfill --full) -- toward StataBE,
  // w2w_tsset_tsfill.log:1233 tsfill_full_tsonly rc 0 (and w2d_uncovered.log:276).
  // A time series with no gaps has nothing to fill, so the row count stays.
  {
    fs::create_directories(workdir + "_full", ec);
    piet::SessionState sessionF(workdir + "_full");
    (void)piet::dispatch("sysuse fatalities, clear", sessionF);
    (void)piet::dispatch("gen t = _n", sessionF);
    (void)piet::dispatch("tsset t", sessionF);
    (void)piet::dispatch("capture tsfill, full", sessionF);
    piet::Datastore* dsF = sessionF.datastore();
    check(sessionF.eval.locals.get("_rc", "") == "0" && dsF != nullptr && dsF->nRows() == 336,
          "D4/BS-081 -> BUG-897: `tsfill, full` is served (w2w_tsset_tsfill.log:1233 rc 0)");
    fs::remove_all(workdir + "_full", ec);
  }

  // D4f (adapted): written when mod() was engine-unimplemented (BS-094 —
  // since LANDED; the historical claim here was stale, corrected 2026-08-31),
  // so the leg uses a contiguous interior gap — same gap-fill semantics. The oracle's
  // own no-extrapolation nuance (dropping t=336 → tsfill restores 335, not
  // 336) was live-verified directly against the engine binary during
  // implementation and is documented in runTsfill's comments.
  (void)piet::dispatch("drop if t >= 100 & t <= 106", session);
  {
    auto blocksN = piet::dispatch("count", session);
    // 336 - 7 = 329 survivors before the fill.
    bool saw329 = false;
    for (const auto& b : blocksN) {
      if (b.is_object() && b.value("type", std::string()) == "display" &&
          b.value("text", std::string()).find("329") != std::string::npos) {
        saw329 = true;
      }
    }
    check(saw329, "D4f setup: dropping t in [100,106] leaves 329 rows");
  }
  {
    auto blocks = piet::dispatch("tsfill", session);
    const auto* mut = firstBlockOfTypeD1(blocks, "data_mutation");
    check(mut != nullptr, "D4f: `tsfill` returns a data_mutation block");
    if (mut != nullptr) {
      int nRows = mut->value("n_rows_after", -1);
      check(nRows == 336,
            "D4f: tsfill restores the full interior gap (329 -> 336; endpoints survived), got " +
                std::to_string(nRows));
    }
  }

  fs::remove_all(workdir, ec);
}


void testG_BS088TsOperatorInjectionLock() {
  std::printf("[selftest] --- BS-088 lock (Phase G): L./F./D./S. time-series operators through "
              "translate.cpp's ts-context injection ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs088").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen t = _n", session);
  (void)piet::dispatch("tsset t", session);

  // BS-088 (ADJUDICATED, Secretary 2026-07-12): tsops has no standalone
  // user-facing verb — PIET reaches L./F./D./S. resolution via translate.cpp's
  // hasTsToken/injectTsContext, which fires for any estimator y/x token
  // (estimation/linear_estimation.cpp::runRegress, estimation/instrumental_variables.cpp's IV family). This locks THAT
  // mechanism, with both an L. and a D. token in the SAME estimation to prove
  // the injection doesn't special-case a single letter.
  //
  // BUG-369 (2026-08-30) CORRECTS THIS COMMENT. It used to end "— NOT
  // `gen`/`replace`, which route through a differently-scoped cobra `eval --gen`
  // call with no ts-context injection at all", stated as scope. It was really a
  // GAP: `regress y L.x` worked and `gen ly = L.x` did not, for the same token
  // on the same data. gen/replace/keep-if now inject the same context (see
  // testG_BUG369TsInExpressionLock below), so the sentence is corrected rather
  // than left standing as a description of intended behaviour.
  auto blocks = piet::dispatch("regress income L.beertax D.spirits", session);
  const auto* est = firstBlockOfTypeD1(blocks, "estimation");
  check(est != nullptr,
        "BS-088: `regress income L.beertax D.spirits` after `tsset t` returns an estimation "
        "block (both L. and D. resolve through the SAME translate.cpp injection path)");

  fs::remove_all(workdir, ec);
}
