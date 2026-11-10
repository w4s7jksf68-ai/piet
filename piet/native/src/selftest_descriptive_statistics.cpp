// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: descriptive statistics.
#include "selftest_support.h"
#include "selftest_cases.h"


// BUG-344: `summarize` never set r(sum) / r(sum_w).
//
// The r() mapping (captureRFromBlock, macros.cpp) had read both keys since it
// was written — the defect was upstream of it, in the two PRODUCERS: the engine
// published `sum` only under `detail` and `sum_w` only under [aw=], and PIET's
// no-cobra fallback summed the column to get the mean and then dropped the sum
// on the floor. So this test drives the whole user-visible path (`summarize`
// then `display r(sum)`) rather than the mapping in isolation: a mapping-only
// leg would have been GREEN for the entire time the bug was live.
//
// Stata posts r(sum) and r(sum_w) for EVERY summarize form — plain, detail, and
// meanonly — because they are properties of the sample, not of the display.
void testBug344SummarizeSum() {
  std::printf("[selftest] --- BUG-344 `summarize` sets r(sum) / r(sum_w) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug344").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto displayed = [](const auto& blocks) {
    std::string last;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        last = b.value("text", std::string());
    return last;
  };

  // x = 1..10 -> sum 55, sum_w 10 (Σw with w≡1 IS the observation count).
  for (const std::string& form : {std::string("summarize x"), std::string("summarize x, meanonly"),
                                  std::string("summarize x, detail")}) {
    piet::SessionState session(workdir);
    std::string src = "clear\nset obs 10\ngen double x = _n\n" + form +
                      "\ndisplay r(sum)\ndisplay r(sum_w)\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    int errs = 0;
    std::string sumTxt, sumwTxt;
    for (const auto& b : r.blocks) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) == "error") ++errs;
      if (b.value("type", std::string()) == "display") {
        if (sumTxt.empty()) sumTxt = b.value("text", std::string());
        else if (sumwTxt.empty()) sumwTxt = b.value("text", std::string());
      }
    }
    (void)displayed;
    check(errs == 0, "BUG-344 [" + form + "]: no error (r(sum) was `not set` before the fix)");
    check(sumTxt == "55", "BUG-344 [" + form + "]: r(sum) = 55 over x=1..10, got `" + sumTxt + "`");
    check(sumwTxt == "10",
          "BUG-344 [" + form + "]: r(sum_w) = 10 (=N unweighted), got `" + sumwTxt + "`");
  }

  // BUG-476 (User-ruled 2026-09-09): unlike r(sum)/r(sum_w) above, r(sd) is
  // NOT value-identical across forms — `meanonly` skips Stata's whole
  // variance/percentile pass, so r(sd) is left MISSING. Measured StataBE
  // 19.5 (tests/eval/golden/aer_b4/pins_oracle_r.log, batch R):
  // `B4VAL|mean|+1.81541bacf914cX+00c|sd_is_missing|+1.0000000000000X+3ff`
  // — mean published, sd is Stata's `.` (= 2^1023 = kStataMissingSentinel
  // in %21x). Checked at the C++ level (session.eval.rValues), not through
  // `display`'s text formatting, so this pins the VALUE the fix publishes
  // rather than a display-layer rendering detail.
  {
    piet::SessionState session(workdir);
    std::string src = "clear\nset obs 10\ngen double x = _n\nsummarize x, meanonly\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!r.stoppedAtReason.has_value(), "BUG-476: `summarize x, meanonly` runs cleanly");
    const double* mean = session.eval.rValues.find("mean");
    const double* sd = session.eval.rValues.find("sd");
    check(mean != nullptr && std::fabs(*mean - 5.5) < 1e-9,
          "BUG-476: r(mean) = 5.5 over x=1..10 under meanonly (still published) -- got " +
              (mean != nullptr ? std::to_string(*mean) : std::string("MISSING")));
    check(sd != nullptr && *sd >= piet::eval::kStataMissingSentinel,
          "BUG-476: r(sd) reads back as Stata MISSING after meanonly -- this build used to "
          "publish the true sd (3.0276503540974917) -- got " +
              (sd != nullptr ? std::to_string(*sd) : std::string("(key absent -- would honest-"
                                                                  "reject on a `display r(sd)` "
                                                                  "read, not silently miss)")));
    const double* var = session.eval.rValues.find("Var");
    const double* median = session.eval.rValues.find("median");
    check(var != nullptr && *var >= piet::eval::kStataMissingSentinel,
          "BUG-476: r(Var) (derived from sd) is ALSO missing under meanonly, not just r(sd)");
    // BUG-1013: summarize never posts r(median) -- Stata has no such result
    // (help summarize lists r(p50), `detail` only), so a read is `.` after every
    // form (w2as_summary_stats_family.log:407 sx_sum_plain_u, :509 sx_sum_d_u). The
    // PIET-only p50 alias this pin used to accept is gone; absent and missing both
    // read as `.`.
    check(median == nullptr || *median >= piet::eval::kStataMissingSentinel,
          "BUG-476/BUG-1013: r(median) is never a value after meanonly (Stata posts no "
          "r(median); w2as_summary_stats_family.log:407)");
  }
  // control: a FULL summarize on the same column DOES publish a real r(sd)
  // (BUG-476.do's own control leg) -- so the assertion above cannot be
  // blamed on the column or the fixture.
  {
    piet::SessionState session(workdir);
    std::string src = "clear\nset obs 10\ngen double x = _n\nsummarize x\n";
    (void)piet::doexec::executeDoSource(src, session, piet::dispatch);
    const double* sd = session.eval.rValues.find("sd");
    check(sd != nullptr && *sd < piet::eval::kStataMissingSentinel && *sd > 0,
          "BUG-476 control: a full `summarize x` (no meanonly) DOES publish a finite r(sd) -- "
          "got " + (sd != nullptr ? std::to_string(*sd) : std::string("MISSING")));
  }
}


void testC1BS025CodebookIfWiring() {
  std::printf("[selftest] --- C1/BS-025 codebook: honors `if`, carries codebook payload ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_bs025").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  auto blocksPlain = piet::dispatch("codebook income", session);
  const nlohmann::ordered_json* describePlain = nullptr;
  for (const auto& b : blocksPlain) {
    if (b.is_object() && b.value("type", std::string()) == "describe") describePlain = &b;
  }
  check(describePlain != nullptr && describePlain->contains("variables") &&
            (*describePlain)["variables"].is_array() && !(*describePlain)["variables"].empty(),
        "C1/BS-025 setup: `codebook income` returns a describe-shaped block with one variable");
  if (describePlain != nullptr && !(*describePlain)["variables"].empty()) {
    const auto& v = (*describePlain)["variables"][0];
    // Oracle (real Stata, unfiltered): mean=13880.2, p90=16915.3,
    // n_unique=333.
    check(v.contains("mean") && v["mean"].is_number() &&
              std::abs(v["mean"].get<double>() - 13880.2) < 1.0,
          "C1/BS-025: unfiltered codebook income mean ~= 13880.2 (oracle)");
    check(v.contains("p90") && v["p90"].is_number() &&
              std::abs(v["p90"].get<double>() - 16915.3) < 1.0,
          "C1/BS-025: unfiltered codebook income p90 ~= 16915.3 (oracle) -- previously codebook "
          "carried no percentile fields at all");
    check(v.value("n_unique", -1) == 333, "C1/BS-025: unfiltered n_unique == 333 (oracle)");
  }

  // `if` must actually filter -- the pre-fix arm was a bare `describe`
  // alias that dropped `if` entirely.
  auto blocksIf = piet::dispatch("codebook income if year==1982", session);
  const nlohmann::ordered_json* describeIf = nullptr;
  for (const auto& b : blocksIf) {
    if (b.is_object() && b.value("type", std::string()) == "describe") describeIf = &b;
  }
  check(describeIf != nullptr && describeIf->contains("variables") &&
            (*describeIf)["variables"].is_array() && !(*describeIf)["variables"].empty(),
        "C1/BS-025: `codebook income if year==1982` returns a describe-shaped block");
  if (describeIf != nullptr && !(*describeIf)["variables"].empty()) {
    const auto& v = (*describeIf)["variables"][0];
    // Oracle (real Stata, year==1982): mean=12998.3, n_unique=46, 48 obs.
    check(v.contains("mean") && v["mean"].is_number() &&
              std::abs(v["mean"].get<double>() - 12998.3) < 1.0,
          "C1/BS-025: `if year==1982`-filtered codebook income mean ~= 12998.3 (oracle) -- "
          "differs from the unfiltered 13880.2 above, proving `if` actually filtered");
    check(v.value("n_unique", -1) == 46,
          "C1/BS-025: `if year==1982`-filtered n_unique == 46 (oracle)");
    check(v.value("n_obs", -1) == 48,
          "C1/BS-025: `if year==1982`-filtered n_obs == 48 (oracle: 48 rows in 1982)");
  }

  fs::remove_all(workdir, ec);
}


void testC1BS026TabstatAdapter() {
  std::printf("[selftest] --- C1/BS-026 tabstat adapter forwards stats/total/vars ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_bs026").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);
  auto blocks = piet::dispatch("tabstat income beertax, stats(mean sd n)", session);

  const nlohmann::ordered_json* est = nullptr;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") est = &b;
  }
  check(est != nullptr && est->value("estimator", std::string()) == "tabstat",
        "C1/BS-026 setup: `tabstat income beertax, stats(mean sd n)` returns a tabstat block");
  if (est != nullptr) {
    check(est->contains("total") && (*est)["total"].is_object() && !(*est)["total"].empty(),
          "C1/BS-026: the block carries \"total\" (previously an empty shell -- only "
          "estimator/n_obs/by were forwarded)");
    check(est->contains("vars") && (*est)["vars"].is_array(),
          "C1/BS-026: the block carries \"vars\" (the ordered var list)");
    check(est->contains("stats") && (*est)["stats"].is_array(),
          "C1/BS-026: the block carries \"stats\" (the requested stat-name list)");
    if (est->contains("total") && (*est)["total"].contains("income")) {
      // Oracle (real Stata): Mean income=13880.18, SD=2253.046.
      const auto& incomeStats = (*est)["total"]["income"];
      check(incomeStats.contains("mean") &&
                std::abs(incomeStats["mean"].get<double>() - 13880.18) < 0.5,
            "C1/BS-026: total.income.mean ~= 13880.18 (oracle)");
      check(incomeStats.contains("sd") &&
                std::abs(incomeStats["sd"].get<double>() - 2253.046) < 0.5,
            "C1/BS-026: total.income.sd ~= 2253.046 (oracle)");
    }
  }

  fs::remove_all(workdir, ec);
}


void testC1BS028PwcorrObs() {
  std::printf("[selftest] --- C1/BS-028 correlate/pwcorr `, obs` no longer mismapped to `sig` "
              "---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_bs028").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  // `correlate, obs` (non-pairwise): real Stata rejects this outright
  // (oracle: "option obs not allowed", rc=198) -- previously it was
  // mismapped into `sig`, which then hard-errored cobra's OWN "corr --sig
  // requires --pairwise" instead.
  auto blocksCorr = piet::dispatch("correlate income beertax, obs", session);
  bool sawCorrError = false;
  std::string corrErrMsg;
  for (const auto& b : blocksCorr) {
    if (b.is_object() && b.value("type", std::string()) == "error") {
      sawCorrError = true;
      corrErrMsg = b.value("message", std::string());
    }
  }
  check(sawCorrError, "C1/BS-028: `correlate income beertax, obs` is an honest reject");
  check(corrErrMsg.find("obs") != std::string::npos,
        "C1/BS-028: the reject message names `obs` as the disallowed option, got: " + corrErrMsg);

  (void)piet::dispatch("capture correlate income beertax, obs", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "C1/BS-028: `capture correlate ..., obs` sets _rc=198 (oracle-pinned)");

  // `pwcorr, obs`: succeeds, and the block no longer force-carries `sig`
  // (previously `obs` silently became `sig`, showing p-values the user
  // never asked for).
  auto blocksPw = piet::dispatch("pwcorr income beertax, obs", session);
  const nlohmann::ordered_json* corrBlock = nullptr;
  for (const auto& b : blocksPw) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") corrBlock = &b;
  }
  check(corrBlock != nullptr, "C1/BS-028: `pwcorr income beertax, obs` succeeds (estimation "
        "block)");
  if (corrBlock != nullptr) {
    check(corrBlock->value("sig", true) == false,
          "C1/BS-028: `pwcorr ..., obs` no longer force-sets sig=true (obs != sig)");
    check(corrBlock->contains("nobs") && (*corrBlock)["nobs"].is_array(),
          "C1/BS-028: the block carries the per-pair `nobs` matrix (cobra forwards it under "
          "--pairwise regardless of --sig -- the data layer is correct even though the frontend "
          "has no per-cell N renderer yet)");
  }

  fs::remove_all(workdir, ec);
}


void testC1BS029LevelsofSeparateMissing() {
  std::printf("[selftest] --- C1/BS-029 levelsof separate()/missing + r(levels) wiring ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_bs029").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen r7 = year", session);
  // Inject the missing marker into the first 49 observations via the `in`
  // range qualifier. (Was `if rownames<50`, but P8-A17 stripped the demo
  // CSV's leading `rownames` column — an R-export row-index artifact, not
  // real data — so this now uses the Stata-idiomatic observation range,
  // which is oracle-identical: 49 rows span <7 states so every 1982-1988
  // level survives elsewhere and only the missing "." is newly introduced.)
  (void)piet::dispatch("replace r7 = . in 1/49", session);

  // separate(SEP): joins levels with SEP instead of the default space.
  // Oracle: r(levels)=[1982|1983|1984|1985|1986|1987|1988].
  auto blocksSep = piet::dispatch("levelsof r7, separate(|)", session);
  bool sawSepError = false;
  for (const auto& b : blocksSep) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawSepError = true;
  }
  check(!sawSepError, "C1/BS-029: `levelsof r7, separate(|)` is no longer an honest-reject on "
        "`separate`");
  check(session.eval.rStrings.get("levels", "") == "1982|1983|1984|1985|1986|1987|1988",
        "C1/BS-029: r(levels) == \"1982|1983|1984|1985|1986|1987|1988\" (oracle-pinned, "
        "pipe-separated, missing excluded) -- r(levels) was never wired up at all before this fix");

  // missing: appends the dataset's own missing marker "." after the
  // ordinary levels (Stata sorts missing last). Oracle:
  // r(levels)=[1982 1983 1984 1985 1986 1987 1988 .].
  auto blocksMiss = piet::dispatch("levelsof r7, missing", session);
  bool sawMissError = false;
  for (const auto& b : blocksMiss) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawMissError = true;
  }
  check(!sawMissError, "C1/BS-029: `levelsof r7, missing` is no longer an honest-reject on "
        "`missing`");
  check(session.eval.rStrings.get("levels", "") == "1982 1983 1984 1985 1986 1987 1988 .",
        "C1/BS-029: r(levels) == \"1982 1983 1984 1985 1986 1987 1988 .\" (oracle-pinned, "
        "space-separated, missing appended LAST)");

  fs::remove_all(workdir, ec);
}


void testC2CorrObsFlagComposing() {
  std::printf("[selftest] --- C2 corr `obs` flag composes with C1/BS-028 (pwcorr obs -> "
              "block[\"obs\"]=true) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c2_corrobs").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  auto blocks = piet::dispatch("pwcorr income beertax, obs", session);
  const nlohmann::ordered_json* corrBlock = nullptr;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") corrBlock = &b;
  }
  check(corrBlock != nullptr, "C2/corr-obs: `pwcorr income beertax, obs` succeeds");
  if (corrBlock != nullptr) {
    check(corrBlock->value("obs", false) == true,
          "C2/corr-obs: the emitted block carries obs=true so a frontend can decide whether to "
          "render the per-pair N matrix -- previously `obs` was read but never recorded on the "
          "block itself");
    check(corrBlock->contains("nobs"),
          "C2/corr-obs regression guard: the per-pair `nobs` matrix (C1/BS-028) still flows "
          "unchanged");
  }

  // Regression guard: `pwcorr` WITHOUT `obs` does not carry the field.
  auto blocksNoObs = piet::dispatch("pwcorr income beertax", session);
  const nlohmann::ordered_json* noObsBlock = nullptr;
  for (const auto& b : blocksNoObs) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") noObsBlock = &b;
  }
  check(noObsBlock != nullptr && !noObsBlock->contains("obs"),
        "C2/corr-obs regression guard: `pwcorr` without `obs` does not set block[\"obs\"]");

  fs::remove_all(workdir, ec);
}


// --- BUG-353: `correlate` is an r-class command and must publish -------------
// It published nothing, so `di r(rho)` answered "r(rho) is not set — run an
// r-class command first", which is honest wording on a false claim. Stata sets
// r(N) and r(rho) (StataBE 19.5: correlate mpg weight -> 74 / -.80717486), and
// with THREE variables r(rho) stays the FIRST PAIR's correlation — so the
// 3-variable form is not a case to reject.
void testBUG353CorrRClass() {
  std::printf("[selftest] --- BUG-353: correlate publishes r(N)/r(rho) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug353").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto displayOf = [](const nlohmann::ordered_json& blocks) {
    std::string out;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        out += b.value("text", std::string());
    return out;
  };
  auto errOf = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error")
        return b.value("message", std::string());
    return std::string();
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  auto corr = piet::dispatch("correlate beertax unemp", session);
  if (!errOf(corr).empty()) {
    skip("BUG-353: correlate r-class", "correlate did not run in this environment: " + errOf(corr));
  } else {
    const std::string rho = displayOf(piet::dispatch("display r(rho)", session));
    check(!rho.empty() && rho.find("not set") == std::string::npos,
          "BUG-353: r(rho) IS set after correlate (was: \"r(rho) is not set\") — got " + rho);
    // .0567718 is the engine's own [0][1] element for this pair, rounded by display.
    check(rho.find("056") != std::string::npos,
          "BUG-353: r(rho) carries the engine's matrix element, not a placeholder — got " + rho);
    const std::string n = displayOf(piet::dispatch("display r(N)", session));
    check(n.find("336") != std::string::npos, "BUG-353: r(N) is the correlate's own N — got " + n);

    // Three variables: rho stays the FIRST PAIR (Stata's measured behaviour).
    (void)piet::dispatch("correlate beertax unemp fatal", session);
    const std::string rho3 = displayOf(piet::dispatch("display r(rho)", session));
    check(rho3.find("056") != std::string::npos,
          "BUG-353: with 3 variables r(rho) is still the FIRST PAIR — got " + rho3);

    // And the values must not be sticky: the next r-class command re-seeds them.
    (void)piet::dispatch("summarize fatal", session);
    const std::string mean = displayOf(piet::dispatch("display r(mean)", session));
    check(!mean.empty() && mean.find("not set") == std::string::npos,
          "BUG-353: a following `summarize` re-seeds r() — the carriage does not pin the plane");
  }
  fs::remove_all(workdir, ec);
}


// --- BUG-354: ttest and tabulate are r-class too ----------------------------
// Same shape as BUG-353. Two oracle subtleties are pinned as their own legs,
// because a "fill in the obvious fields" fix gets both wrong: Stata leaves
// r(sd) UNSET for the one-sample ttest, and tabulate's r(N) is the tabulated
// sample (the sum of the cells), so filtered-out rows are excluded.
void testBUG354TtestTabulateRClass() {
  std::printf("[selftest] --- BUG-354: ttest/tabulate publish their r-class ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug354").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto disp = [](const nlohmann::ordered_json& blocks) {
    std::string out;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        out += b.value("text", std::string());
    return out;
  };
  auto anyError = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    return false;
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  auto tt = piet::dispatch("ttest beertax == .5", session);
  if (anyError(tt)) {
    skip("BUG-354: ttest r-class", "ttest did not run in this environment");
  } else {
    for (const char* r : {"r(t)", "r(p)", "r(N_1)", "r(mu_1)", "r(se)", "r(df_t)"}) {
      const std::string v = disp(piet::dispatch(std::string("display ") + r, session));
      check(!v.empty() && v.find("not set") == std::string::npos,
            std::string("BUG-354: ") + r + " IS set after ttest — got " + v);
    }
    const std::string n1 = disp(piet::dispatch("display r(N_1)", session));
    check(n1.find("336") != std::string::npos, "BUG-354: r(N_1) is the ttest's own N — got " + n1);
  }

  auto tb = piet::dispatch("tabulate year if year < 1985", session);
  if (anyError(tb)) {
    skip("BUG-354: tabulate r-class", "tabulate did not run in this environment");
  } else {
    const std::string rr = disp(piet::dispatch("display r(r)", session));
    check(rr.find("3") != std::string::npos,
          "BUG-354: r(r) is the number of rows SHOWN (3 retained years) — got " + rr);
    // The subtlety: r(N) is the TABULATED sample, so the `if` filter must be
    // reflected. 144 = 3 years x 48 rows; the dataset has 336.
    const std::string rn = disp(piet::dispatch("display r(N)", session));
    check(rn.find("144") != std::string::npos,
          "BUG-354: r(N) is the TABULATED sample, not the dataset's N (144, not 336) — got " + rn);
  }

  fs::remove_all(workdir, ec);
}
