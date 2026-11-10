// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: model diagnostics.
#include "selftest_support.h"
#include "selftest_cases.h"


void testBS013EstatDiagFeHonestReject() {
  std::printf("[selftest] --- BS-013 estat diagnostic family: honest reject after an "
              "FE model instead of a silently-wrong plain-OLS number ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bs013_estat_fe").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  // `absorb(year)` (reghdfe-style; year is confirmed numeric — see
  // testClusterVceAndUnknownOptionFixes's own comment on why `year`, not the
  // string/factor-coded `state`, is used as a plain numeric column elsewhere
  // in this dataset) sets LastModel.fe WITHOUT needing xtset at all.
  // BUG-473 (User ruling 2026-09-09: match Stata): plain `regress` now
  // honestly rejects `absorb()` (r(198), matches StataBE —
  // tests/eval/golden/aer_b4/b5_absorb_class.log:32-36). `areg` is the
  // command that HAS this option and sets LastModel.fe the same way
  // (runAreg -> runRegress("areg")) — swap the command word; the setup's
  // subject (an FE model in LastModel) is unchanged.
  auto blocks1 = piet::dispatch("areg fatal beertax, absorb(year)", session);
  bool regOk = true;
  for (const auto& b : blocks1) {
    if (b.value("type", "") == "error") regOk = false;
  }
  check(regOk, "BS-013 setup: `areg fatal beertax, absorb(year)` succeeds");
  check(session.lastModel().has_value() && !session.lastModel()->fe.empty(),
        "BS-013 setup: LastModel.fe is non-empty after absorb(year)");

  // estat hettest with NO explicit varlist relies on the lastModel fallback —
  // cobra's hettest/ovtest/vif/dwatson/bgodfrey/durbinalt/archlm CLI has no
  // --fe flag at all (verified against apps/cmds/cmds_diag.cpp), so silently
  // running plain OLS here would answer a DIFFERENT question than the one
  // just estimated. Must honest-reject, not silently succeed.
  auto blocks2 = piet::dispatch("estat hettest", session);
  bool sawError = false;
  std::string errMsg;
  for (const auto& b : blocks2) {
    if (b.value("type", "") == "error") {
      sawError = true;
      errMsg = b.value("message", std::string());
    }
  }
  check(sawError,
        "BS-013: `estat hettest` (no varlist) after an FE model is an HONEST REJECT, not a "
        "silently-wrong plain-OLS Breusch-Pagan number");
  // BUG-1222: the reject is Stata's own -- estat hettest is regress's, and after
  // areg StataBE 19.5 answers r(321) "estat hettest not valid" (scratch-laneA/
  // agents/N1/b2.do), with or without a varlist.
  check(errMsg == "estat hettest not valid",
        "BS-013/BUG-1222: the reject is Stata's `estat hettest not valid` — got: " + errMsg);

  // BUG-1222: an EXPLICIT varlist is hettest's z-list, not a model of its own,
  // and StataBE refuses it after areg too (r(321)); it used to test a plain-OLS
  // refit at rc 0.
  auto blocks3 = piet::dispatch("estat hettest fatal beertax", session);
  std::string explicitMsg;
  for (const auto& b : blocks3) {
    if (b.value("type", "") == "error") explicitMsg = b.value("message", std::string());
  }
  check(explicitMsg == "estat hettest not valid",
        "BS-013/BUG-1222: `estat hettest` WITH a varlist after areg is r(321) `estat hettest "
        "not valid` too — got: " + explicitMsg);

  fs::remove_all(workdir, ec);
}


void testD5BS085Sdtest() {
  std::printf("[selftest] --- D5a/BS-085 sdtest (variance-ratio test) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d5_bs085").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  // D5a: `sdtest income, by(high)`. Oracle: F=1.3992 (ratio sd(0)/sd(1)),
  // df1=229 df2=105, Pr(F<f)=.9742 (p_lower), Pr(F>f)=.0258 (p_upper),
  // 2*Pr(F>f)=.0516 (two-sided p).
  auto blocks = piet::dispatch("sdtest income, by(high)", session);
  const auto* est = firstBlockOfTypeD1(blocks, "estimation");
  check(est != nullptr, "D5a: `sdtest income, by(high)` returns an estimation block");
  if (est != nullptr) {
    double f = est->value("F", -999.0);
    check(std::abs(f - 1.3991627276773468) < 0.001, "D5a oracle: F ~= 1.3992, got " +
                                                          std::to_string(f));
    int df1 = est->value("df1", -1);
    int df2 = est->value("df2", -1);
    check(df1 == 229 && df2 == 105,
          "D5a oracle: df1=229 df2=105, got df1=" + std::to_string(df1) +
              " df2=" + std::to_string(df2));
    double pLower = est->value("p_lower", -999.0);
    check(std::abs(pLower - 0.974206786526249) < 0.0005,
          "D5a oracle: p_lower ~= .9742, got " + std::to_string(pLower));
    double pUpper = est->value("p_upper", -999.0);
    check(std::abs(pUpper - 0.025793213473751755) < 0.0005,
          "D5a oracle: p_upper ~= .0258, got " + std::to_string(pUpper));
    double p = est->value("p", -999.0);
    check(std::abs(p - 0.05158642694750351) < 0.0005,
          "D5a oracle: two-sided p ~= .0516, got " + std::to_string(p));
  }

  // Exactly one of by()/`= #` is required.
  (void)piet::dispatch("capture sdtest income", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D5/BS-085: `sdtest income` with NEITHER by() nor `= #` honestly rejects");

  // `sdtest income = 2000` (one-sample form): wiring-only smoke test — no
  // oracle coverage this batch, just proves the --against argv/JSON round-
  // trip works (mirrors runNbreg's synthetic-dataset convergence precedent
  // for an unverified-but-mechanically-working path).
  {
    auto blocks = piet::dispatch("sdtest income = 2000", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5/BS-085: `sdtest income = 2000` (one-sample form) returns an "
                          "estimation block (wiring-only, not oracle-pinned)");
    if (est != nullptr) {
      check(est->contains("chi2"), "D5/BS-085: one-sample sdtest block carries a chi2 field");
    }
  }

  fs::remove_all(workdir, ec);
}


void testD5BS021Durbinalt() {
  std::printf("[selftest] --- D5b/BS-021 estat durbinalt fix (was mis-aliased to bgodfrey) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d5_bs021").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("regress income beertax spirits", session);

  // D5b bare: chi2=372.934, df=1 (lastModel fallback — no explicit varlist).
  {
    auto blocks = piet::dispatch("estat durbinalt", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5b: `estat durbinalt` (bare) returns an estimation block");
    if (est != nullptr) {
      nlohmann::ordered_json chi2Arr = est->value("chi2", nlohmann::ordered_json::array());
      double chi2 = chi2Arr.is_array() && !chi2Arr.empty() ? chi2Arr[0].get<double>() : -999.0;
      check(std::abs(chi2 - 372.93353101543727) < 0.001,
            "D5b oracle: chi2 ~= 372.934 (correct Durbin's alternative test — NOT bgodfrey's own "
            "different chi2), got " + std::to_string(chi2));
      nlohmann::ordered_json dfArr = est->value("df", nlohmann::ordered_json::array());
      int df = dfArr.is_array() && !dfArr.empty() ? dfArr[0].get<int>() : -1;
      check(df == 1, "D5b oracle: df == 1, got " + std::to_string(df));
    }
  }

  // D5b `, small lags(2)`: F=189.005, df=(2,331).
  {
    auto blocks = piet::dispatch("estat durbinalt, small lags(2)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5b: `estat durbinalt, small lags(2)` returns an estimation block");
    if (est != nullptr) {
      nlohmann::ordered_json fArr = est->value("F", nlohmann::ordered_json::array());
      double f = fArr.is_array() && !fArr.empty() ? fArr[0].get<double>() : -999.0;
      check(std::abs(f - 189.00471663831613) < 0.001,
            "D5b oracle: F ~= 189.005, got " + std::to_string(f));
    }
  }

  fs::remove_all(workdir, ec);
}


void testD5BS082IvDiag() {
  std::printf("[selftest] --- D5c/BS-082 estat endogenous/overid/firststage (IV diagnostics) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d5_bs082").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("ivregress 2sls income emppop (beertax = spirits unemp)", session);

  // BETA Phase G REWIRE: `cobra iv-diag` (a SEPARATE cobra CLI command from
  // `cobra iv`, apps/cmds/cmds_diag.cpp::cmd_ivdiag) genuinely computes
  // Durbin/Wu-Hausman/Sargan/Basmann — the D5 batch's honest-reject below
  // was right that `cobra iv --tidy`'s OWN payload has none of these, but
  // wrong that the engine has no such test at all. Cross-check pattern
  // (same style as testBS090InRangeExpansion/testBS092BysortSummarize):
  // call `cobra iv-diag` DIRECTLY with the same data/spec the ivregress
  // command above used, and confirm `estat endogenous`/`estat overid`
  // return the IDENTICAL numbers — proving the wiring reconstructs the
  // right --endog/--x/--instruments from LastModel, not just that SOME
  // number came back.
  nlohmann::json directDiag;
  bool haveDirect = false;
  // e(sample) T2: the fit leaves the head on its mask-install frame, which on
  // the resident plane exists only in the worker; this direct call reads the
  // head as a FILE, so it is written first (a no-op on a clean head).
  session.ensureMaterializedHead("selftest-d5c");
  if (session.currentHbin().has_value()) {
    try {
      directDiag = piet::runCobraJson({"iv-diag", "--y", "income", "--endog", "beertax", "--x",
                                        "emppop", "--instruments", "spirits,unemp", "--data",
                                        *session.currentHbin()});
      haveDirect = true;
    } catch (const piet::CobraError&) {
      haveDirect = false;
    }
  }
  check(haveDirect, "D5c/BS-082 setup: direct `cobra iv-diag` cross-check call succeeds");

  // estat endogenous: Durbin score chi2 + Wu-Hausman F, now wired (was an
  // honest reject pre-Phase-G).
  {
    auto blocks = piet::dispatch("estat endogenous", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5c/BS-082: `estat endogenous` returns an estimation block");
    if (est != nullptr && haveDirect && directDiag.contains("endogeneity")) {
      const auto& e = directDiag["endogeneity"];
      check(std::abs(est->value("durbin_chi2", -999.0) - e.value("durbin", -998.0)) < 1e-9,
            "D5c/BS-082: estat endogenous durbin_chi2 matches the direct `cobra iv-diag` call");
      check(std::abs(est->value("wu_hausman_F", -999.0) - e.value("wu_hausman", -998.0)) < 1e-9,
            "D5c/BS-082: estat endogenous wu_hausman_F matches the direct `cobra iv-diag` call");
    }
  }

  // estat overid: 2 instruments (spirits, unemp) > 1 endog (beertax) ->
  // overidentified -> a real Sargan/Basmann stat, not the just-identified
  // note (was an honest reject pre-Phase-G).
  {
    auto blocks = piet::dispatch("estat overid", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5c/BS-082: `estat overid` returns an estimation block");
    if (est != nullptr) {
      check(!est->contains("note"),
            "D5c/BS-082: this model is overidentified (2 instruments > 1 endog) — a real Sargan/"
            "Basmann stat, not the just-identified note");
      if (haveDirect && directDiag.contains("overid") && !directDiag["overid"].contains("note")) {
        const auto& o = directDiag["overid"];
        check(std::abs(est->value("sargan_chi2", -999.0) - o.value("sargan", -998.0)) < 1e-9,
              "D5c/BS-082: estat overid sargan_chi2 matches the direct `cobra iv-diag` call");
        check(std::abs(est->value("basmann_chi2", -999.0) - o.value("basmann", -998.0)) < 1e-9,
              "D5c/BS-082: estat overid basmann_chi2 matches the direct `cobra iv-diag` call");
      }
    }
  }

  // estat firststage: F(2,332)=2.56678 for beertax, oracle-matched (UNCHANGED).
  {
    auto blocks = piet::dispatch("estat firststage", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5c: `estat firststage` returns an estimation block");
    if (est != nullptr && est->contains("firststage") && (*est)["firststage"].is_array()) {
      double f = -999.0;
      for (const auto& row : (*est)["firststage"]) {
        if (row.value("name", std::string()) == "beertax") f = row.value("F", -999.0);
      }
      check(std::abs(f - 2.5667784083942093) < 0.001,
            "D5c oracle: firststage F(beertax) ~= 2.56678, got " + std::to_string(f));
    } else {
      check(false, "D5c: `estat firststage` block has a \"firststage\" array");
    }
  }

  fs::remove_all(workdir, ec);
}


void testD5BS083Hausman() {
  std::printf("[selftest] --- D5e/BS-083 hausman (xtreg fe/re specification test) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d5_bs083").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("encode state, gen(snum)", session);
  (void)piet::dispatch("xtset snum year", session);
  (void)piet::dispatch("xtreg income beertax unemp, fe", session);
  (void)piet::dispatch("eststo feM", session);
  (void)piet::dispatch("xtreg income beertax unemp, re", session);
  (void)piet::dispatch("eststo reM", session);

  // D5e: chi2=2.62, p=0.2697, beertax diff=376.6603, unemp diff=-.6916189.
  {
    auto blocks = piet::dispatch("hausman feM reM", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D5e: `hausman feM reM` returns an estimation block");
    if (est != nullptr) {
      double chi2 = est->value("chi2", -999.0);
      check(std::abs(chi2 - 2.6205813255838897) < 0.001,
            "D5e oracle: chi2 ~= 2.62, got " + std::to_string(chi2));
      double p = est->value("p", -999.0);
      check(std::abs(p - 0.26974164113052557) < 0.001,
            "D5e oracle: p ~= .2697, got " + std::to_string(p));
      double diffBeertax = -999.0, diffUnemp = -999.0;
      if (est->contains("coefs") && (*est)["coefs"].is_array()) {
        for (const auto& row : (*est)["coefs"]) {
          if (row.value("name", std::string()) == "beertax") diffBeertax = row.value("diff", -999.0);
          if (row.value("name", std::string()) == "unemp") diffUnemp = row.value("diff", -999.0);
        }
      }
      check(std::abs(diffBeertax - 376.6602970137676) < 0.01,
            "D5e oracle: beertax diff ~= 376.6603, got " + std::to_string(diffBeertax));
      check(std::abs(diffUnemp - (-0.691618945067205)) < 0.01,
            "D5e oracle: unemp diff ~= -.6916189, got " + std::to_string(diffUnemp));
    }
  }

  // Missing store name.
  (void)piet::dispatch("capture hausman feM bogusName", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D5/BS-083: `hausman feM bogusName` (missing store) honestly rejects");

  // Two of the SAME type (both fe) — not a valid fe/re pair.
  (void)piet::dispatch("xtreg income beertax unemp, fe", session);
  (void)piet::dispatch("eststo feM2", session);
  (void)piet::dispatch("capture hausman feM feM2", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D5/BS-083: `hausman feM feM2` (two fe stores, not an fe/re pair) honestly rejects");

  fs::remove_all(workdir, ec);
}


void testD5BS084Chow() {
  std::printf("[selftest] --- D5f/BS-084 chow (PIET convenience wrapper) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d5_bs084").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  // D5f: manually-computed oracle chowF=28.074171, df1=2, df2=332
  // (real Stata has no `chow` command — see runChow's header comment).
  auto blocks = piet::dispatch("chow income beertax, by(high)", session);
  const auto* est = firstBlockOfTypeD1(blocks, "estimation");
  check(est != nullptr, "D5f: `chow income beertax, by(high)` returns an estimation block");
  if (est != nullptr && est->contains("classic")) {
    double f = (*est)["classic"].value("F", -999.0);
    check(std::abs(f - 28.07417107825576) < 0.001,
          "D5f oracle: chowF ~= 28.074171, got " + std::to_string(f));
    int df1 = (*est)["classic"].value("df1", -1);
    int df2 = (*est)["classic"].value("df2", -1);
    check(df1 == 2 && df2 == 332, "D5f oracle: df1=2 df2=332, got df1=" + std::to_string(df1) +
                                       " df2=" + std::to_string(df2));
  } else {
    check(false, "D5f: `chow` block has a \"classic\" sub-object");
  }

  // by() is required.
  (void)piet::dispatch("capture chow income beertax", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D5/BS-084: `chow income beertax` without by() honestly rejects");

  fs::remove_all(workdir, ec);
}


void testD5BS087Mkspline() {
  std::printf("[selftest] --- D5g/BS-087(part) mkspline (cubic restricted spline) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d5_bs087").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // nknots() ENGINE FIX (see runMkspline's header comment, estimation_m4.cpp):
  // cobra eval --mkspline's own nknots(#) clause was broken through 0.24.0
  // (a paren_arg substring collision), fixed in 0.25.0 — the D5g oracle's
  // own exact invocation (`nknots(4)`) now SUCCEEDS cleanly instead of
  // surfacing an engine_error.
  {
    auto blocks =
        piet::dispatch("capture noisily mkspline inc_sp = income, cubic nknots(4)", session);
    check(session.eval.locals.get("_rc", "") == "0",
          "D5g/BS-087: `mkspline inc_sp = income, cubic nknots(4)` (the D5g oracle's own "
          "invocation) now SUCCEEDS (cobra 0.25.0 fix), got _rc=" +
              session.eval.locals.get("_rc", "<unset>"));
    const auto* mut = firstBlockOfTypeD1(blocks, "data_mutation");
    check(mut != nullptr, "D5g: `mkspline inc_sp = ..., cubic nknots(4)` returns a data_mutation "
                          "block");
    if (mut != nullptr && mut->contains("new_vars")) {
      check((*mut)["new_vars"].size() == 3,
            "D5g oracle: nknots(4) -> 3 spline basis columns, got " +
                std::to_string((*mut)["new_vars"].size()));
    }
  }

  // Wiring proof: the explicit knots() form independently exercises the
  // mkspline plumbing under a FRESH stub name (`ek_sp`, not `inc_sp` —
  // now that nknots() above also succeeds and already created the
  // inc_sp1..3 columns, reusing that stub here would collide with them
  // instead of proving this leg's OWN new_vars wiring).
  {
    auto blocks =
        piet::dispatch("mkspline ek_sp = income, cubic knots(10000,12000,14000,16000)", session);
    const auto* mut = firstBlockOfTypeD1(blocks, "data_mutation");
    check(mut != nullptr,
          "D5g: `mkspline ek_sp = ..., cubic knots(...)` (explicit knots, fresh stub) returns a "
          "data_mutation block");
    if (mut != nullptr && mut->contains("new_vars")) {
      check((*mut)["new_vars"].size() == 3,
            "D5g: 4 explicit knots -> 3 spline basis columns, got " +
                std::to_string((*mut)["new_vars"].size()));
    }
  }

  // Linear (no `cubic`) honestly rejects — the engine has no linear basis
  // (it would silently compute a cubic one instead).
  (void)piet::dispatch("capture mkspline lin_sp = income", session);
  check(session.eval.locals.get("_rc", "") != "0",
        "D5/BS-087: `mkspline stub = var` (no `cubic`) honestly rejects (engine has no linear "
        "spline — always computes cubic regardless of the keyword)");

  fs::remove_all(workdir, ec);
}
