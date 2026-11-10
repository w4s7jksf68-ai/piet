// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: command contracts.
#include "selftest_support.h"
#include "selftest_cases.h"


// ── COUNCIL-20260710 pre-wave sprint P2/P3/P4: native dispatch honesty fixes
// (BUG-055/BUG-056/BUG-060) ───────────────────────────────────────────────
// P2 (BUG-055): Stata's legacy `cluster(v)` — with neither `vce()` nor
// `robust` given — is a synonym for `vce(cluster v)` (implies robust-cluster
// VCE). Before this fix, `regress ..., cluster(v)` (and the shared xtreg/
// logit/probit builder) silently ran iid — v wholly ignored (oracle
// P2_legacycluster: `regress price mpg, cluster(rep78)` came back
// se_mpg~=53.08/n_obs=74/iid instead of se_mpg~=82.63/n_obs=69/
// n_clusters=5/cluster-robust).
// P3 (BUG-056): cobra's logit/probit estimator does not implement
// cluster-robust VCE — PIET must reject this HONESTLY (name the gap)
// instead of silently dropping the cluster spec (iid z/p, no warning).
// P4 (BUG-060): after every option a command implements is consumed, any
// leftover option token must be an honest r(198)-parity reject naming the
// token — never a silent no-op. Split per the oracle: a genuinely-invalid
// token (typo, e.g. `floob`) reads as Stata's own "option X not allowed";
// a real-Stata-but-PIET-unimplemented option (e.g. `beta` on regress)
// reads as an honest "not yet implement[ed]" instead. (`meanonly` on
// summarize was such a gap until BUG-090 made it an accepted no-op — see
// leg (e).) The over-rejection guard (learned from the engine's E6 net,
// which broke 5 existing goldens) is exercised deliberately below: every
// "still succeeds" leg uses ONLY options this version of PIET has always
// accepted.
void testClusterVceAndUnknownOptionFixes() {
  std::printf("[selftest] --- BUG-055/056/060: cluster()->vcov mapping, "
              "logit/probit vce(cluster) reject, unknown-option net ---\n");

  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_dispatch_honesty").u8string();
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

  // --- P2 / BUG-055: legacy cluster(v) alone maps to --vcov cluster
  // --cluster v (mirrors both copies of the fix: handlers.cpp::runRegress
  // AND estimation_m4.cpp's shared vcovFromOpts). Uses the bundled
  // `fatalities` panel sample (state/year, 1982-1988); `year` is used as
  // the cluster var (7 distinct numeric values) rather than the string/
  // factor-coded `state`, so this test isolates the vcov-mapping fix from
  // any unrelated factor-as-cluster question.
  {
    std::string wd = workdir + "/p2_legacy_cluster";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as mission 3 / BUG A / BUG-039
      // above (FIX-SELFTEST-ISOBUILD-CRASH): without fatalities.csv, `est`
      // below would be re-fetched as a null "error" block and the unguarded
      // .value() call would crash. Skip the whole legacy-cluster leg honestly.
      skip("BUG-055 legacy-cluster leg", "`sysuse fatalities` setup returned no data");
    } else {
      auto j = runCmd(session, "regress fatal beertax, cluster(year)");
      check(firstBlockOfType(j, "error").is_null(),
            "BUG-055: `regress ..., cluster(year)` (legacy form) is not an error");
      check(!firstBlockOfType(j, "estimation").is_null(),
            "BUG-055: `regress ..., cluster(year)` produces an estimation block");
      const auto& lm = session.lastModel();
      check(lm.has_value(), "BUG-055: LastModel is set after the regress");
      if (lm.has_value()) {
        check(lm->vcov == "cluster",
              "BUG-055: emitted argv maps legacy cluster(year) to --vcov cluster (was iid)");
        check(lm->cluster.size() == 1 && lm->cluster[0] == "year",
              "BUG-055: emitted argv carries --cluster year (was silently dropped before this fix)");
      }
      auto est = firstBlockOfType(j, "estimation");
      check(est.value("n_clusters", 0) >= 2,
            "BUG-055: cobra's own echoed n_clusters (>=2, one per distinct `year`) confirms "
            "cluster-robust VCE was actually applied, not iid (iid reports n_clusters=0)");
    }
  }

  // --- P3 / BUG-056: logit/probit + cluster (either syntax) is an honest
  // reject, not a silently-iid result.
  {
    std::string wd = workdir + "/p3_logit_cluster";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as mission 3 / BUG A / BUG-039
      // / BUG-055 above (FIX-SELFTEST-ISOBUILD-CRASH): the P3 reject checks
      // below assume `unemp`/`ybin` are real columns; skip honestly rather
      // than exercise them against a fixture-less session.
      skip("BUG-056 logit-cluster leg", "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "gen ybin = spirits > 2");

      // W:P5 — these three checks used to pin the HONEST REJECT (the engine
      // had no clustered MLE VCE, so refusing was the correct answer). The
      // engine now implements it, so the pin flips to the implemented truth:
      // both syntaxes must SUCCEED and must actually apply cluster-robust
      // VCE. What is being defended is unchanged — that a cluster spec is
      // never silently dropped to iid — only the correct outcome moved from
      // "reject" to "compute". The n_clusters and se>se_iid assertions are
      // what keep a future silent-iid regression caught; the reject text
      // alone could not have.
      auto j1 = runCmd(session, "logit ybin unemp, cluster(year)");
      check(firstBlockOfType(j1, "error").is_null(),
            "BUG-056/W:P5: `logit ..., cluster(year)` (legacy form) now SUCCEEDS");
      {
        auto est1 = firstBlockOfType(j1, "estimation");
        check(est1.value("n_clusters", 0) >= 2,
              "BUG-056/W:P5: the legacy form actually applied cluster-robust VCE "
              "(n_clusters >= 2; a silently-iid fit reports 0)");
      }

      auto j2 = runCmd(session, "logit ybin unemp, vce(cluster year)");
      check(firstBlockOfType(j2, "error").is_null(),
            "BUG-056/W:P5: `logit ..., vce(cluster year)` (explicit form) also succeeds");
      {
        auto est2 = firstBlockOfType(j2, "estimation");
        check(est2.value("n_clusters", 0) >= 2,
              "BUG-056/W:P5: the explicit form applied cluster-robust VCE too");
      }

      // The remaining honest gap: two-way clustering has no MLE
      // implementation, so it must still reject rather than quietly
      // collapsing to one-way.
      auto j2b = runCmd(session, "logit ybin unemp, vce(cluster year state)");
      auto err2b = firstBlockOfType(j2b, "error");
      check(!err2b.is_null(),
            "BUG-056/W:P5: two-way vce(cluster A B) on logit is STILL an honest reject");

      // Regression: plain logit (no cluster) still works — the P3 reject is
      // scoped to cluster VCE only.
      auto j3 = runCmd(session, "logit ybin unemp");
      check(firstBlockOfType(j3, "error").is_null(),
            "BUG-056 regression: plain `logit` (no vce) still succeeds");
    }
  }

  // --- P4 / BUG-060: unknown-option net — bogus token rejected by name,
  // a real-but-unimplemented option gets the honest-gap message, AND
  // realistic all-real-options commands are NOT falsely rejected.
  {
    std::string wd = workdir + "/p4_unknown_option";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    // (a) genuinely-invalid token -> r(198)-parity, names the token.
    auto jBad = runCmd(session, "regress fatal beertax, floob");
    auto errBad = firstBlockOfType(jBad, "error");
    check(!errBad.is_null(), "BUG-060: `regress ..., floob` (bogus option) is an error block");
    check(errBad.value("code", std::string()) == "unsupported_option",
          "BUG-060: the bogus-option reject uses the unsupported_option code");
    check(errBad.value("message", std::string()).find("floob") != std::string::npos,
          "BUG-060: the reject message NAMES the bogus token (`floob`)");
    check(errBad.value("message", std::string()).find("not allowed") != std::string::npos,
          "BUG-060: a genuinely-invalid token reads as Stata's own r(198) 'not allowed' wording");

    // (b) a real-Stata-but-PIET-unimplemented option reads as an honest
    // gap, not an r(198) typo-reject (oracle: `regress ,beta` is valid
    // Stata — standardized-coefficient display — just not implemented).
    auto jBeta = runCmd(session, "regress fatal beertax, beta");
    auto errBeta = firstBlockOfType(jBeta, "error");
    check(!errBeta.is_null(), "BUG-060: `regress ..., beta` is rejected (not silently ignored)");
    check(errBeta.value("message", std::string()).find("not yet implement") != std::string::npos,
          "BUG-060: `beta` (a real Stata option) reads as an honest gap, not 'not allowed'");

    // (c) OVER-REJECTION GUARD: a representative regress using ONLY options
    // this version of PIET has always accepted must still succeed
    // end-to-end (regression test for the E6-style over-rejection failure
    // mode — see the BUG-060 report).
    auto jGood = runCmd(session, "regress fatal beertax unemp, robust level(90)");
    check(firstBlockOfType(jGood, "error").is_null(),
          "BUG-060 over-rejection guard: `regress ..., robust level(90)` (all-real options) still "
          "succeeds");
    check(!firstBlockOfType(jGood, "estimation").is_null(),
          "BUG-060 over-rejection guard: the all-real-options regress produces an estimation "
          "block");

    // (d) same guard for the shared xtreg/logit/probit option net
    // (buildEstimationArgvM4) — uses logit (no xtset/panel-var setup
    // needed) so this leg stays isolated to the option-net question.
    runCmd(session, "gen ybin4 = spirits > 2");
    auto jLogitGood = runCmd(session, "logit ybin4 unemp, level(90)");
    check(firstBlockOfType(jLogitGood, "error").is_null(),
          "BUG-060 over-rejection guard: `logit ..., level(90)` (buildEstimationArgvM4's shared "
          "net) still succeeds");

    // (e) summarize's own small net: an unknown token is still r(198), but
    // `meanonly` (BUG-090) is now accepted as a no-op — it only skips the
    // variance/percentile pass + display for speed, and PIET's summarize
    // already computes the full superset, so the results a script reads
    // (r(mean)/r(N)/r(min)/r(max)) are unaffected. `detail` still works too.
    auto jSumBad = runCmd(session, "summarize beertax, floob");
    auto errSumBad = firstBlockOfType(jSumBad, "error");
    check(!errSumBad.is_null(), "BUG-060: `summarize ..., floob` is an error block");
    check(errSumBad.value("message", std::string()).find("floob") != std::string::npos,
          "BUG-060: the summarize bogus-option reject names the token");

    auto jSumMeanonly = runCmd(session, "summarize beertax, meanonly");
    check(firstBlockOfType(jSumMeanonly, "error").is_null(),
          "BUG-090: `summarize ..., meanonly` is accepted as a no-op (not rejected)");
    check(!firstBlockOfType(jSumMeanonly, "summarize").is_null(),
          "BUG-090: `summarize ..., meanonly` still produces a full summarize block");

    auto jSumGood = runCmd(session, "summarize beertax, detail");
    check(firstBlockOfType(jSumGood, "error").is_null(),
          "BUG-060 over-rejection guard: `summarize ..., detail` (the one implemented option) "
          "still succeeds");

    // BUG-088: an unsupported extended macro function in a `display` context
    // (a call-syntax token whose name the evaluator can't resolve — here
    // `_request(...)`, the interactive-input primitive that is out of scope,
    // BUG-087) must now FAIL LOUDLY at the point of failure instead of being
    // silently swallowed into literal echoed text.
    auto jReq = runCmd(session, "display _request(controls_input)");
    auto errReq = firstBlockOfType(jReq, "error");
    check(!errReq.is_null(),
          "BUG-088: `display _request(...)` (unsupported call-syntax macro fn) is an honest "
          "error, not silent literal echo");
    check(errReq.value("message", std::string()).find("_request") != std::string::npos,
          "BUG-088: the error names the unresolved token (`_request`)");
    check(firstBlockOfType(jReq, "display").is_null(),
          "BUG-088: no literal `_request(controls_input)` display text is emitted alongside");

    // BUG-088 guard, moved by name-binding-display-literals: a bare word that
    // binds to nothing is no literal text. StataBE answers r(111) `foo not
    // found` (tests/eval/golden/aer_w2/w2av_name_binding.log:157 `[foo not
    // found`, :166 `PREC|bd_dy_unb|111|`); PIET echoed `foo bar` at rc 0.
    auto jFoo = runCmd(session, "display foo bar");
    const auto errFoo = firstBlockOfType(jFoo, "error");
    check(!errFoo.is_null() && errFoo.value("reason", std::string()) == "r(111)" &&
              errFoo.value("message", std::string()) == "foo not found",
          "BUG-088 guard (moved): `display foo bar` is r(111) `foo not found`, not echoed text");
    check(firstBlockOfType(jFoo, "display").is_null(),
          "BUG-088 guard (moved): `display foo bar` prints no `foo bar` text");

    // BUG-088 regression guard: genuine arithmetic display is untouched.
    auto jArith = runCmd(session, "display 2+2*3");
    check(firstBlockOfType(jArith, "display").value("text", std::string()) == "8",
          "BUG-088 guard: `display 2+2*3` still evaluates arithmetic (8)");

    // BUG-192: Stata's `%w.Pg` is a GENERAL format, not printf's `%.Pg`.
    // `display %9.0g 42` used to print `4e+01` because printf reads `.0` as
    // "zero significant digits". Every expectation below is a VERBATIM row
    // from a StataBE 19.5 capture taken on 2026-08-06 (171 cases: 9 specs x
    // 19 values); the full capture and the derived rule are documented on
    // stataGeneral() in display_hardening.cpp. These nine are the ones that
    // pin a distinct clause of it, so a regression names which clause broke.
    {
      auto disp = [&](const char* cmd) {
        return firstBlockOfType(runCmd(session, cmd), "display").value("text", std::string());
      };
      struct GCase { const char* cmd; const char* want; const char* clause; };
      static const GCase kG[] = {
          {"display %9.0g 42", "       42", "a value that fits shows as itself"},
          {"display %9.0g 3.14159265358979", " 3.141593", "P=0 -> w-2 significant digits"},
          {"display %9.3g 123456.789", "   123457", "P never truncates the integer part"},
          {"display %9.0g 99999999", " 1.00e+08", "integer part over w-2 digits -> scientific"},
          {"display %12.0g 1234567890", "  1234567890", "...and stays fixed when it fits"},
          {"display %9.0g 0.000012345", " .0000123", "decimals shed to the w-1 field budget"},
          {"display %9.2g 42.5", "       43", "rounding is half-away-from-zero, not half-even"},
          {"display %9.2g 1234567890", "  1.2e+09", "scientific decimals = min(w-7, sig-1)"},
          {"display %9.0e 42", " 4.20e+01", "%w.0e shows w-7 decimals, not none"},
      };
      for (const auto& c : kG) {
        check(disp(c.cmd) == c.want,
              std::string("BUG-192: `") + c.cmd + "` -> [" + c.want + "] (" + c.clause + ")");
      }
      check(disp("display %9.0g .") == "        .",
            "BUG-192 rider: a missing value is right-justified in the field like any other");
    }

    // BUG-091: `c(current_date)` / `c(current_time)` (c-class system values)
    // must resolve to a real date/time instead of hard-halting analysis.do:69
    // with `NameError: name 'c' is not defined` (exposed by BUG-088's honesty
    // fix). Only these two c-names are implemented; any other c(...) still
    // errors honestly. Format is pinned by REGEX, never value — wall-clock is
    // non-deterministic.
    auto dispText091 = [&](const nlohmann::json& j) -> std::string {
      return firstBlockOfType(j, "display").value("text", std::string());
    };
    auto jDate = runCmd(session, "display c(current_date)");
    check(firstBlockOfType(jDate, "error").is_null(),
          "BUG-091: `display c(current_date)` no longer errors (c-class resolved)");
    check(std::regex_match(dispText091(jDate),
                           std::regex(R"(^ ?\d{1,2} [A-Z][a-z]{2} \d{4}$)")),
          "BUG-091: `c(current_date)` prints a real Stata-format date (`DD Mon YYYY`)");

    auto jTime = runCmd(session, "display c(current_time)");
    check(firstBlockOfType(jTime, "error").is_null(),
          "BUG-091: `display c(current_time)` no longer errors");
    check(std::regex_match(dispText091(jTime), std::regex(R"(^\d{2}:\d{2}:\d{2}$)")),
          "BUG-091: `c(current_time)` prints a real Stata-format time (`HH:MM:SS`)");

    // BUG-091 negative leg: an unimplemented c(...) name must STILL surface an
    // honest NameError — the fix must not swallow the whole c-class.
    auto jBogus = runCmd(session, "display c(bogus_setting)");
    check(!firstBlockOfType(jBogus, "error").is_null(),
          "BUG-091 negative: an unimplemented `c(bogus_setting)` is still an honest error");
    check(firstBlockOfType(jBogus, "display").is_null(),
          "BUG-091 negative: no literal `c(bogus_setting)` text emitted alongside the error");
  }

  // --- BUG-089: `do FILE arg1 arg2` must tokenize the console line into a
  // filename + positional args (seeding `1'..`N' via executeDo's args vector),
  // not treat the whole remainder as one path. Drives the real console route
  // (routes::command) so it exercises dispatcher.cpp's `do`/`run` handler.
  // NOTE: each .do file references ONLY the positional macros it is given —
  // PIET (unlike Stata) treats an *undefined* local as a hard engine_error, so
  // a macro-count mismatch would fail for a reason unrelated to this fix.
  {
    std::string doDir = workdir + "/p5_do_args";
    fs::create_directories(doDir, ec);
    auto displayText = [&](const nlohmann::json& j) -> std::string {
      return firstBlockOfType(j, "display").value("text", std::string());
    };
    auto writeDo = [&](const std::string& path, const std::string& body) {
      std::ofstream f(cecon::to_native_path(path));
      f << body << "\n";
    };

    // (a) two positional args seed `1' and `2'
    std::string twoArgPath = doDir + "/argecho2.do";
    writeDo(twoArgPath, "display \"BUG089=`1'/`2'\"");
    piet::SessionState session(doDir);
    auto jArgs = runCmd(session, "do " + twoArgPath + " alpha beta");
    check(firstBlockOfType(jArgs, "error").is_null(),
          "BUG-089: `do FILE alpha beta` opens the file (no error)");
    check(displayText(jArgs) == "BUG089=alpha/beta",
          "BUG-089: positional args seed `1'=alpha `2'=beta");

    // (b) no-arg `do FILE` stays byte-unchanged — a macro-free file still opens
    // and runs (empty args vector == the pre-fix 3-arg executeDo call).
    std::string noArgPath = doDir + "/noarg.do";
    writeDo(noArgPath, "display \"BUG089_NOARG_RAN\"");
    auto jNoArg = runCmd(session, "do " + noArgPath);
    check(firstBlockOfType(jNoArg, "error").is_null(),
          "BUG-089 guard: no-arg `do FILE` still opens the file");
    check(displayText(jNoArg) == "BUG089_NOARG_RAN",
          "BUG-089 guard: no-arg run executes the file unchanged");

    // (c) a quoted filename containing a space still opens; the arg after the
    // closing quote seeds `1' (quotes stripped AFTER isolating token 1).
    std::string spacePath = doDir + "/my run.do";
    writeDo(spacePath, "display \"BUG089=`1'\"");
    auto jQuoted = runCmd(session, "do \"" + spacePath + "\" gamma");
    check(firstBlockOfType(jQuoted, "error").is_null(),
          "BUG-089: quoted filename with a space still opens");
    check(displayText(jQuoted) == "BUG089=gamma",
          "BUG-089: arg after a quoted filename seeds `1'=gamma");
  }

  fs::remove_all(workdir, ec);
}


// --- M. BETA Phase A1 — unified prefix engine ---------------------------
// One focused leg per named defect (BETA-20260712-campaign/SCOPE.md rows
// BS-002/014/015/016/017/020/040/041). These are the primary regression net
// for this refactor beyond the freeze/kitchensink baseline, which does not
// exercise capture/quietly/noisily/version/eststo-colon/bysort-parenthetical
// at all (debug_all.do has zero capture/quietly/noisily/version lines and
// only parenthetical-free bysort lines — confirmed at implementation time).

void testBS016CaptureAbbreviations() {
  std::printf("[selftest] --- BS-016 capture abbreviation family (cap/capt/captu/captur/capture) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs016").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Every legal spelling must be recognised as `capture` and swallow an
  // inner failure (previously only "cap"/"capture" worked; captu/captur/
  // capt fell through as unknown commands and never touched _rc at all).
  for (const std::string& sp : {std::string("cap"), std::string("capt"), std::string("captu"),
                                 std::string("captur"), std::string("capture")}) {
    piet::SessionState session(workdir);
    // Load data first: PIET's dispatcher runs the data-loaded guard BEFORE
    // command recognition, so an unknown command with NO data loaded maps to
    // r(111) no_data_loaded, not r(199). Load a dataset so the coarse-rc path
    // under test is the unsupported_command -> 199 one. (The no-data-first
    // ordering divergence from Stata is a separate found-gap, not this leg's subject.)
    piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch(sp + " badcommand_xyz_does_not_exist", session);
    // BS-100 (BETA A4): the coarse rc map replaced the old boolean-collapsed
    // "_rc=1" for EVERY captured error — an unrecognized command's own
    // native code ("unsupported_command") now maps to Stata's own r(199)
    // "unrecognized command" instead of the generic "1" (see errors.h's
    // coarseRc doc comment).
    check(session.eval.locals.get("_rc", "") == "199",
          "BS-016: `" + sp + " badcommand...` is recognised as capture and sets _rc=199 "
          "(BS-100 coarse rc: unsupported_command -> 199)");
    check(blocks.size() == 1,
          "BS-016: `" + sp + " badcommand...` shows nothing but its own echo (error swallowed)");
  }

  // Success path for a couple of spellings, including a newly-recognised one.
  // N1(b) (BETA Phase C batch C1): bare `capture cmd` suppresses ALL
  // output on success too — real Stata's `capture cmd` is silent
  // regardless of outcome; only `capture noisily cmd` shows it. This used
  // to assert the OLD, buggy pass-through-on-success behavior; updated to
  // the corrected expectation and paired with a `noisily` check so both
  // sides of N1(b)'s fix are exercised here.
  for (const std::string& sp : {std::string("captur"), std::string("capture")}) {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch(sp + " count", session);
    check(session.eval.locals.get("_rc", "") == "0",
          "BS-016: `" + sp + " count` (success) sets _rc=0");
    bool sawDisplay = false;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") sawDisplay = true;
    }
    check(!sawDisplay, "BS-016/N1(b): bare `" + sp + " count` (success, no noisily) suppresses "
          "its result — only `capture noisily ...` shows output");

    auto blocksNoi = piet::dispatch(sp + " noisily count", session);
    bool sawDisplayNoi = false;
    for (const auto& b : blocksNoi) {
      if (b.is_object() && b.value("type", std::string()) == "display") sawDisplayNoi = true;
    }
    check(sawDisplayNoi, "BS-016/N1(b): `" + sp + " noisily count` (success) still shows its result");
  }

  fs::remove_all(workdir, ec);
}


void testBS017ByBysortAbbreviations() {
  std::printf("[selftest] --- BS-017 by/bysort abbreviation family (bys/byso/bysor) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs017").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  for (const std::string& sp : {std::string("bys"), std::string("byso"), std::string("bysor"),
                                 std::string("bysort")}) {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch(sp + " state: gen bs017idx = _n", session);
    bool sawUnsupported = false;
    bool sawMutation = false;
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) == "error" &&
          b.value("code", std::string()) == "unsupported_command") {
        sawUnsupported = true;
      }
      if (b.value("type", std::string()) == "data_mutation") sawMutation = true;
    }
    check(!sawUnsupported, "BS-017: `" + sp + " state: gen ...` is recognised (not an unknown command)");
    check(sawMutation, "BS-017: `" + sp + " state: gen ...` produced a data_mutation block");
    check(session.hasData() && session.datastore()->has("bs017idx"),
          "BS-017: `" + sp + " state: gen ...` actually created the new column");
  }

  fs::remove_all(workdir, ec);
}


// AER-SCOPE1 bundle A: the abbreviations the corpus types (forval / forv /
// forvalue / tw / sc / gr / sca de / eret / levels / loc / gl / prog def), plus
// the two commands that exist only under an old name -- `ivreg` and `winsor`.
//
// Sibling of testAerEnvBundle above, and here for the same reason: the
// floors.json rows for `ivreg` and `winsor` are `no_fixture` (the timing corpus
// is absent from this tree), so this function and pangram S25 carry the bundle
// until a --counters batch captures it.
//
// The property under test differs between the two halves, and writing both
// halves the same way is how a bundle like this gets a green gate over a broken
// route. For an ABBREVIATION the question is never "does it run" -- it is "does
// it reach the same handler", so each leg below runs the short spelling AND the
// long one and compares the two results. A truncation wired to a second,
// nearly-right code path passes a run-it-once probe and fails here.
//
// For `ivreg` and `winsor` there is no long spelling to compare against, so
// what those legs pin is the arithmetic. `winsor`'s leg is the sharpest in the
// function on purpose: the cheapest wrong way to land Cox's winsor is to alias
// it onto winsor2, which is a SILENT wrong answer (both commands succeed, both
// write a plausible column, and only the tails differ). At N=100 with p(.01)
// the two disagree by construction -- winsor takes the (h+1)-th ORDER STATISTIC
// and winsor2 interpolates the percentile -- so both are run over the same
// column and both tails are asserted. The winsor2 numbers below are measured,
// not derived: `winsor2 x, cuts(1 99)` over x = 1..100 answers min 1.5 / max
// 99.5 on this binary. If the alias were ever made, the first pair fails; if
// winsor2 itself drifts, the second pair fails.
void testAerAbbrevBundle() {
  std::printf("[selftest] --- AER-SCOPE1 bundle A: forval/forv/forvalue / loc / gl / sca de / "
              "prog def / eret / levels / tw+sc / gr / ivreg / winsor ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerabbrev").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  // Guarded rather than returning a bare json: `value()` on a null throws, so
  // an absent block would abort the run instead of failing one check (the trap
  // the BUG-129 graph legs earlier in this file record).
  auto firstOfType = [](const auto& blocks, const std::string& type) -> nlohmann::json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == type) return b;
    return nlohmann::json::object();
  };
  auto anyTextContaining = [](const auto& blocks, const std::string& needle) -> std::string {
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      const std::string t = b.value("text", std::string());
      if (!t.empty() && t.find(needle) != std::string::npos) return t;
    }
    return std::string();
  };

  // --- forvalues: the three measured truncations, against the long spelling. ---
  {
    const std::vector<std::pair<std::string, std::string>> kLoopSpellings = {
        {"forvalues", "the unabbreviated spelling, carried as this leg's control"},
        {"forval", "the corpus's most common truncation"},
        {"forv", "the shortest spelling measured in the corpus"},
        {"forvalue", "the one that reads like a typo and is not"},
    };
    for (const auto& kw : kLoopSpellings) {
      piet::SessionState session(workdir);
      const std::string src = "local k = 0\n" + kw.first + " i = 1/4 {\n"
                              "    local k = `k' + 1\n"
                              "}\n";
      auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
      check(session.eval.locals.get("k", "<unset>") == "4",
            "ABBREV/" + kw.first + ": opens a brace block and runs it FOUR times (" + kw.second +
                "), got k=" + session.eval.locals.get("k", "<unset>"));
      // A truncation that is recognized as a command but not as a loop KEYWORD
      // swallows the `{` line and stops there, which leaves k unset for the
      // reason above and would be indistinguishable from a miscount without
      // this second reading.
      check(!r.stoppedAtReason.has_value(),
            "ABBREV/" + kw.first + ": ...without stopping the do-file, stop=" +
                r.stoppedAtReason.value_or("<none>"));
    }
  }
  {  // Under a prefix: the prefix engine and the brace-block keyword table are
     // two different lookups, and only a prefixed SHORT header crosses them.
    piet::SessionState session(workdir);
    auto r = piet::doexec::executeDoSource("local k = 0\nquietly forval i = 1/4 {\n"
                                           "    local k = `k' + 1\n}\n",
                                           session, piet::dispatch);
    check(session.eval.locals.get("k", "<unset>") == "4",
          "ABBREV/quietly forval: a truncated loop header still opens its block under a prefix, "
          "got k=" + session.eval.locals.get("k", "<unset>"));
    check(!r.stoppedAtReason.has_value(),
          "ABBREV/quietly forval: ...and the do-file does not stop, stop=" +
              r.stoppedAtReason.value_or("<none>"));
  }

  // --- loc / gl: a macro that is defined but does not EXPAND is half-landed. ---
  {
    piet::SessionState session(workdir);
    auto r = piet::doexec::executeDoSource("loc todrop = 3\ngl AER \"codeSTATA\"\n"
                                           "display \"`todrop' $AER\"\n",
                                           session, piet::dispatch);
    auto d = displays(r.blocks);
    check(d.size() == 1 && d[0] == "3 codeSTATA",
          "ABBREV/loc+gl: both short spellings define a macro that EXPANDS -- expected `3 "
          "codeSTATA`, got " + (d.empty() ? std::string("no display at all")
                                          : std::string("`") + d[0] + "`"));
  }

  // --- sca de: `scalar define` with BOTH words truncated. ---
  {
    // Two executeDoSource calls, not one two-line source. anyTextContaining
    // scans EVERY block's `text`, echo blocks included, and the echo of `sca de
    // p = .05` reads ".  sca de p = .05" -- which contains "p =". That echo is
    // not emitted today (dispatcher.cpp's tryCaptureDefinition arm returns a
    // FRESH array on success, so a macro/scalar definition drops the entry echo
    // pushed ahead of it), but that is a property of that arm and not of this
    // assertion. Running the definition in its own call means the check reads
    // `scalar list`'s output and nothing else, whichever way the arm goes.
    piet::SessionState session(workdir);
    (void)piet::doexec::executeDoSource("sca de p = .05\n", session, piet::dispatch);
    auto r = piet::doexec::executeDoSource("scalar list\n", session, piet::dispatch);
    const std::string listed = anyTextContaining(r.blocks, "p =");
    double got = -999.0;
    const auto eq = listed.find("p =");
    if (eq != std::string::npos) {
      try {
        got = std::stod(listed.substr(eq + 3));
      } catch (...) {
      }
    }
    check(std::fabs(got - 0.05) < 1e-12,
          "ABBREV/sca de: `sca de p = .05` defines the scalar `scalar list` then reports -- "
          "expected 0.05, got " +
              (listed.empty() ? std::string("no line naming p") : std::string("`") + listed + "`"));
  }

  // --- capt prog drop / prog def / end: a definition that never runs is worth
  // nothing, so the leg CALLS it. The drop comes first because a replication
  // do-file is expected to be re-runnable inside one session, which is exactly
  // why the corpus writes the pair.
  {
    piet::SessionState session(workdir);
    auto r = piet::doexec::executeDoSource("capt prog drop mht_rw\n"
                                           "prog def mht_rw\n"
                                           "    display \"ABBREV_PROG_RAN\"\n"
                                           "end\n"
                                           "mht_rw\n",
                                           session, piet::dispatch);
    auto d = displays(r.blocks);
    bool ran = false;
    for (const auto& line : d)
      if (line == "ABBREV_PROG_RAN") ran = true;
    check(ran, "ABBREV/prog def: `prog def NAME` ... `end` defines a program that RUNS when called");
    // `capt prog drop` of a program that was never defined must be swallowed by
    // the capture rather than stopping the file -- that first line is the one
    // the corpus always writes and the one that would kill every replication.
    check(!r.stoppedAtReason.has_value(),
          "ABBREV/capt prog drop: dropping a program that does not exist is captured, not fatal, "
          "stop=" + r.stoppedAtReason.value_or("<none>"));
  }

  // --- eret list == ereturn list, output for output. ---
  {
    auto listing = [&](const std::string& verb) -> std::string {
      piet::SessionState session(workdir);
      auto r = piet::doexec::executeDoSource("sysuse fatalities, clear\n"
                                             "regress income beertax\n" + verb + "\n",
                                             session, piet::dispatch);
      for (const auto& b : r.blocks) {
        if (!b.is_object()) continue;
        const std::string t = b.value("text", std::string());
        if (t.find("e(N)") != std::string::npos) return t;
      }
      return std::string();
    };
    const std::string longForm = listing("ereturn list");
    const std::string shortForm = listing("eret list");
    check(!longForm.empty(), "ABBREV/eret: the control (`ereturn list`) prints an e(N) line at all");
    check(shortForm == longForm,
          "ABBREV/eret: `eret list` prints the SAME listing as `ereturn list` -- a short spelling "
          "routed to a second printer is the silent half of this class");
  }

  // --- levels == levelsof, into the same local. ---
  {
    auto levelsInto = [&](const std::string& verb) {
      piet::SessionState session(workdir);
      (void)piet::doexec::executeDoSource("sysuse fatalities, clear\n" + verb +
                                              " year, local(yr)\n",
                                          session, piet::dispatch);
      return session.eval.locals.get("yr", "<unset>");
    };
    const std::string longForm = levelsInto("levelsof");
    const std::string shortForm = levelsInto("levels");
    check(longForm.find("1982") != std::string::npos,
          "ABBREV/levels: the control (`levelsof year, local(yr)`) fills the local, got `" +
              longForm + "`");
    check(shortForm == longForm,
          "ABBREV/levels: `levels` fills the same local with the same levels -- got `" + shortForm +
              "` against `" + longForm + "`");
  }

  // --- tw + sc: inside a twoway layer, `sc` is scatter. TWO layers, because a
  // one-layer probe passes even when the layer list collapses to its head.
  {
    piet::SessionState session(workdir);
    auto r = piet::doexec::executeDoSource("sysuse fatalities, clear\n"
                                           "tw (sc income beertax) (sc income spirits)\n",
                                           session, piet::dispatch);
    auto g = firstOfType(r.blocks, "graph");
    check(g.value("kind", std::string()) == "twoway",
          "ABBREV/tw: `tw (...) (...)` builds a twoway graph block, got kind `" +
              g.value("kind", std::string()) + "`");
    auto layers = g.value("layers", nlohmann::json::array());
    check(layers.size() == 2, "ABBREV/tw: both layers survive, got " +
                                  std::to_string(layers.size()));
    if (layers.size() == 2) {
      check(layers[0].value("kind", std::string()) == "scatter" &&
                layers[1].value("kind", std::string()) == "scatter",
            "ABBREV/sc: `sc` inside a twoway layer IS scatter, got `" +
                layers[0].value("kind", std::string()) + "` and `" +
                layers[1].value("kind", std::string()) + "`");
    }
  }

  // --- gr export: the short spelling of the verb that writes the file. The
  // FILE is the assertion, not the note -- `gr export` that printed a save line
  // over an empty directory is the exact shape a report-only check misses.
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    const std::string figure = (fs::path(workdir) / "abbrev_fig.svg").u8string();
    (void)piet::doexec::executeDoSource("sysuse fatalities, clear\n"
                                        "tw (sc income beertax)\n"
                                        "gr export \"" + figure + "\", replace\n",
                                        session, piet::dispatch);
    check(fs::exists(figure) && fs::file_size(figure, ec) > 0,
          "ABBREV/gr: `gr export` actually wrote the file, not just a note about it");
  }

  // --- ivreg == ivregress 2sls, coefficient for coefficient. ---
  {
    auto coefs = [&](const std::string& cmd) {
      piet::SessionState session(workdir);
      auto r = piet::doexec::executeDoSource("sysuse fatalities, clear\n" + cmd + "\n", session,
                                             piet::dispatch);
      std::map<std::string, double> out;
      for (const auto& b : r.blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
        for (const auto& row : b.value("coeftable", nlohmann::json::array()))
          if (row.is_object()) out[row.value("name", std::string())] = row.value("b", 0.0);
      }
      return out;
    };
    const auto modern = coefs("ivregress 2sls income (beertax = spirits)");
    const auto legacy = coefs("ivreg income (beertax = spirits)");
    check(!modern.empty(), "IVREG: the control (`ivregress 2sls`) estimated at all");
    check(legacy.size() == modern.size(),
          "IVREG: `ivreg y x (d = z)` returns the same coefficient table SHAPE as `ivregress 2sls "
          "y x (d = z)` -- got " + std::to_string(legacy.size()) + " rows against " +
              std::to_string(modern.size()));
    for (const auto& row : modern) {
      const auto it = legacy.find(row.first);
      if (it == legacy.end()) {
        check(false, "IVREG: `ivreg` is missing the row `" + row.first + "` the modern spelling "
                     "returns");
        continue;
      }
      // RELATIVE, not absolute: |b| is ~2.4e4 here, so an absolute 1e-12 sits
      // below the double's own ULP at that magnitude and would be pinning
      // bit-identity while claiming a tolerance. Same handler and same data
      // should in fact give bit-identity; this states the weaker claim it can
      // honestly certify, and it is still ~4 orders tighter than any real
      // numeric drift between two estimators.
      const double tol = 1e-12 * std::max(1.0, std::fabs(row.second));
      check(std::fabs(row.second - it->second) <= tol,
            "IVREG: `ivreg` and `ivregress 2sls` agree on b[" + row.first + "] to 1e-12 relative -- "
            "got " + std::to_string(it->second) + " against " + std::to_string(row.second));
    }
  }

  // --- winsor is Cox's, and it is NOT winsor2. ---
  {
    auto tails = [&](const std::string& cmd, const std::string& col) {
      piet::SessionState session(workdir);
      auto r = piet::doexec::executeDoSource("clear\nset obs 100\ngen double x = _n\n" + cmd +
                                                 "\nsummarize " + col + "\n",
                                             session, piet::dispatch);
      auto s = firstOfType(r.blocks, "summarize").value("stats", nlohmann::json::object());
      auto one = s.is_object() ? s.value(col, nlohmann::json::object())
                               : nlohmann::json::object();
      return std::make_pair(one.value("min", -999.0), one.value("max", -999.0));
    };
    // h = floor(100 * .01) = 1, so the single lowest value is replaced by the
    // 2nd order statistic and the single highest by the 99th. Those ARE 2 and
    // 99 in 1..100 -- exact, because winsor copies an existing observation
    // rather than computing a new number.
    const auto cox = tails("winsor x, gen(xw) p(.01)", "xw");
    check(cox.first == 2.0,
          "WINSOR: Cox's winsor clamps the low tail to the ORDER STATISTIC 2, not to an "
          "interpolated 1.5 -- got " + std::to_string(cox.first));
    check(cox.second == 99.0,
          "WINSOR: ...and the high tail to the order statistic 99, not to 99.5 -- got " +
              std::to_string(cox.second));
    // The other half of the discriminator. Without it, an alias of BOTH
    // commands onto one implementation could still pass the pair above.
    const auto lian = tails("winsor2 x, cuts(1 99)", "x_w");
    check(lian.first == 1.5 && lian.second == 99.5,
          "WINSOR: winsor2 still INTERPOLATES over the same column (1.5 / 99.5, measured) -- if "
          "this moved to 2 / 99 the two commands have been collapsed onto one, got " +
              std::to_string(lian.first) + " / " + std::to_string(lian.second));
  }

  // --- the honest-rejection half. The campaign's wiring checklist owes one per
  // bundle (item 13) and this bundle had none: every leg above asserts that a
  // command RUNS, which is the half a handler that accepts anything also passes.
  // Each line names a DIFFERENT gate in runWinsorCox, and each asserts the same
  // two things. The second is the load-bearing one: a refusal that still wrote
  // the column is the silent shape this whole bundle exists to keep out, and an
  // error block alone cannot tell the two apart (the block is emitted either
  // way). The setup runs in its OWN executeDoSource call because `gen double x`
  // emits a data_mutation of its own, which would satisfy the scan below no
  // matter what winsor did.
  {
    const std::vector<std::pair<std::string, std::string>> kRefusals = {
        {"winsor x, gen(xw)", "neither p() nor h(): winsor.ado has no default tail"},
        {"winsor x, gen(xw) p(.1) h(3)", "p() and h() name two different tails at once"},
        {"winsor x, gen(xw) p(.6)", "p() outside 0 < p < 0.5 crosses the two tails"},
        {"winsor x, gen(xw) p(.1) highonly lowonly", "both -only flags together clamp nothing"},
        {"winsor x, p(.1)", "no gen(): there is no replace mode, so the name is required"},
        {"winsor x, gen(xw) p(.1) cuts(1 99)", "cuts() is winsor2's option, not winsor's"},
    };
    for (const auto& rf : kRefusals) {
      piet::SessionState session(workdir);
      (void)piet::doexec::executeDoSource("clear\nset obs 100\ngen double x = _n\n", session,
                                          piet::dispatch);
      auto r = piet::doexec::executeDoSource(rf.first + "\n", session, piet::dispatch);
      check(!firstOfType(r.blocks, "error").empty(),
            "WINSOR REFUSE: `" + rf.first + "` is refused -- " + rf.second);
      bool wrote = false;
      for (const auto& b : r.blocks)
        if (b.is_object() && b.value("type", std::string()) == "data_mutation") wrote = true;
      check(!wrote, "WINSOR REFUSE: ...and `" + rf.first +
                        "` wrote NOTHING -- no data_mutation block came back with the refusal");
    }
  }

  fs::remove_all(workdir, ec);
}


// --- N. Registry <-> dispatcher route drift guard (BETA campaign Phase A0) -
//
// registry.cpp's commandRegistry() is a hand-transcribed DESCRIPTION of
// dispatcher.cpp's ~75 `if (cmd == "...")` arms (809-2203) — nothing
// enforces that the description stays in sync with the real if-chain as
// dispatcher.cpp evolves. This leg IS that enforcement (model:
// testVersionSingleSource() above): for every canonical/abbrev spelling in
// the table, dispatch a minimal probe and assert the result is NOT
// dispatcher.cpp:2201-2203's generic `unsupported_command`("piet_gap")
// fallthrough — i.e. the name genuinely routes to a real arm. Whatever
// happens AFTER that (missing args, an engine error, a no-data reply, ...)
// is fine and not checked here; only "this name isn't recognized as a
// command at all" is a failure.
//
// A minimal SessionState with a real dataset loaded (`sysuse fatalities`)
// is required, not optional: with NO data loaded, dispatcher.cpp:758-762's
// own "no dataset loaded" guard short-circuits almost every command NOT in
// kNoDataCommands BEFORE the routing table is ever consulted — that guard
// fires purely off a fixed allow-list + hasData(), with no knowledge of
// whether cmd matches a real arm. Without loading data first, this leg
// would PASS VACUOUSLY for ~2/3 of the registry regardless of whether the
// arm actually exists (noDataLoaded()'s code is "no_data_loaded", never
// "unsupported_command", so a no-data short-circuit and genuine drift would
// look identical to a check that only tests the empty-session case).
//
// Two rows (`scalar`, `label`) need an extra sub-token to even ENTER their
// dispatcher.cpp arm (both require a specific first varlist token — `list`/
// `dir` for scalar's display sub-arm, one of variable/values/define/list/
// dir/drop for label — bare `scalar`/`label` alone matches NO 809-2203
// condition and would otherwise fall all the way through to the exact same
// 2201-2203 fallthrough this leg exists to distinguish from real drift).
// kProbeSuffix supplies the minimal extra token for exactly these two rows;
// every other row is probed by its bare canonical/abbrev spelling alone —
// deliberately not hand-tuned per command beyond that (see the A0 task's
// return summary for the design assumption this rests on: no handler is
// expected to independently manufacture an `unsupported_command` error
// whose token happens to exactly equal the bare command word it was
// legitimately given).
void testRegistryRoutes() {
  std::printf("[selftest] --- registry <-> dispatcher route drift guard (BETA A0) ---\n");

#ifdef _WIN32
  std::error_code rwec;
  std::string workdir =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_registry").u8string();
  std::filesystem::create_directories(workdir, rwec);
#else
  std::string workdir = "/tmp/piet_native_selftest_registry";
  (void)std::system(("mkdir -p '" + workdir + "'").c_str());
#endif

  static const std::map<std::string, std::string> kProbeSuffix = {
      {"scalar", "list"},
      {"label", "list"},
      // BETA campaign C4: `ereturn`'s dispatch arm only matches
      // `ereturn list` (parsed.varlist[0]=="list") — a bare `ereturn`
      // probe would otherwise legitimately fall through to the generic
      // unsupported_command fallback, same reason scalar/label are here.
      {"ereturn", "list"},
      // AER-SCOPE1 bundle B: same reason as ereturn -- the `return` arm answers
      // list/clear and reports anything else as an unknown subcommand, so a bare
      // probe would read as drift rather than as the syntax error it is.
      {"return", "list"},
      // AER-SCOPE1 bundle G: `putexcel` is STATEFUL -- every form other than
      // `set` needs an open workbook -- so a bare probe reaches the arm and
      // then errors on the missing state. `describe` is the one sub-verb that
      // is a pure query, which keeps this guard's probe free of side effects
      // (it is a route check, not a behaviour check; the behaviour lives in
      // testAerIoText leg (vii)). Drift detection is unchanged either way:
      // the guard keys on reason == "piet_gap", which no arm-reaching form
      // can produce.
      {"putexcel", "describe"},
  };

  int probed = 0;
  int drifted = 0;
  auto probeOne = [&](const std::string& name) {
    piet::SessionState session(workdir);
    // Fresh dataset re-derived from the immutable bundled CSV on EVERY
    // probe (not shared/mutated across probes): even though the on-disk
    // workdir is reused, a brand-new SessionState + a fresh `sysuse` means
    // an earlier probe's destructive command (drop/collapse/reshape/...)
    // can never leak into a later probe.
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string probe = name;
    // BUG-105 (WS-K): key this by the CANONICAL spelling. kProbeSuffix is a
    // second copy of a spelling set, and keying it by the literal made the
    // `label` row's new abbreviations (la/lab/labe) probe as a BARE command and
    // drift-fail — exactly as bare `label` would without its `list` suffix. One
    // lookup through the resolution point instead of three more table rows.
    auto sfx = kProbeSuffix.find(piet::registry::resolveCanonical(name));
    if (sfx != kProbeSuffix.end()) probe += " " + sfx->second;
    auto blocks = piet::dispatch(probe, session);
    bool driftedToUnknown = false;
    for (const auto& b : blocks) {
      // AER scope-1 (2026-09-03): `reason` is what separates DRIFT from a
      // deliberate refusal. Both emit `unsupported_command` with the command
      // word as `token`; only drift — the generic dispatcher fallthrough at
      // dispatcher.cpp's tail — carries the `piet_gap` reason. A row that is
      // registered and refuses with a named cause (`refused:<cause>`, via
      // eval::refusedCommand) is reaching its arm exactly as intended, which
      // is the opposite of what this guard exists to catch. Without this
      // clause the campaign's ~30 register-and-refuse rows would each read as
      // a drifted route and the guard would have to be switched off.
      if (b.is_object() && b.value("type", std::string()) == "error" &&
          b.value("code", std::string()) == "unsupported_command" &&
          b.value("token", std::string()) == name &&
          b.value("reason", std::string()) == "piet_gap") {
        driftedToUnknown = true;
        break;
      }
    }
    ++probed;
    if (driftedToUnknown) ++drifted;
    check(!driftedToUnknown, "registry route `" + name + "` reaches a real dispatcher arm");
  };

  for (const auto& spec : piet::registry::commandRegistry()) {
    probeOne(spec.canonical);
    for (const auto& ab : spec.abbrev) probeOne(ab);
  }

  std::printf("[selftest]      probed %d registry canonical/abbrev spellings (%d drifted)\n", probed,
              drifted);

#ifdef _WIN32
  std::error_code rrec;
  std::filesystem::remove_all(workdir, rrec);
#else
  (void)std::system(("rm -rf '" + workdir + "'").c_str());
#endif
}


// ── BUG-450: the varlist expansion point ────────────────────────────────────
//
// `expandVarlist` calls itself "the ONE varlist wildcard/range expander" and,
// until 2026-09-08, its callers were the loop header, `ds`/`unab` and a handful
// of AER handlers written against it directly. The plain data verbs were not
// among them, so `drop t*` answered r(111) while `foreach v of varlist t*`
// expanded correctly in the same session on the same data.
//
// Every expected value below is a MEASUREMENT against StataNow 19.5 BE over
// auto.dta, taken before the wiring was written.
//
// THE TWO LEGS THAT ARE GUARDS RATHER THAN FEATURES, and they are the reason
// this is a flag and not a blanket change:
//   * `rename t* T*` must NOT be expanded. It is Stata's rename-GROUPS syntax
//     -- measured, it renames trunk->Trunk and turn->Turn -- so the `*` on each
//     side stands for the same captured text. Expanding the left side and
//     leaving the right literal would rename both columns to `T*`. `rename` is
//     therefore absent from the opt-in list and reaches its own arm unexpanded,
//     where it answers honestly (BUG-453).
//   * A varlist with NO pattern character must pass through byte-identically,
//     so the common case cannot regress behind the new code path.
void testVarlistExpansionWiring() {
  std::printf("[selftest] --- BUG-450: varlist wildcard/range wiring on the data verbs ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_varlist").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // A fixture whose names mirror auto.dta's, so the measured shapes read the
  // same way. Built in-body: this leg is about NAME resolution, not values.
  const std::string kFix =
      "clear\nset obs 3\ngen make = 1\ngen price = 2\ngen mpg = 3\ngen rep78 = 4\n"
      "gen headroom = 5\ngen trunk = 6\ngen weight = 7\ngen length = 8\ngen turn = 9\n";

  auto run = [&](const std::string& src) {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;
    return piet::doexec::executeDoSource(src, s, piet::dispatch);
  };
  auto lastDisplay = [](const nlohmann::ordered_json& blocks) {
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        out = b.value("text", std::string());
    }
    return out;
  };
  auto namesAfter = [&](const std::string& body) {
    return lastDisplay(run(kFix + body + "\nds\n").blocks);
  };
  auto rcOfSrc = [&](const std::string& body) {
    return lastDisplay(run(kFix + body + "\nlocal rc = _rc\ndisplay \"`rc'\"\n").blocks);
  };

  check(namesAfter("drop t*") == "make price mpg rep78 headroom weight length",
        "BUG-450 drop: a trailing wildcard drops trunk and turn");
  check(namesAfter("drop price-trunk") == "make weight length turn",
        "BUG-450 drop: a hyphen RANGE drops the five names between the endpoints, inclusive");
  check(namesAfter("keep t*") == "trunk turn",
        "BUG-450 keep: the same expansion on the other verb");
  check(namesAfter("keep price-trunk") == "price mpg rep78 headroom trunk",
        "BUG-450 keep: ...and the same range");
  check(namesAfter("order t*") == "trunk turn make price mpg rep78 headroom weight length",
        "BUG-450 order: the matched names move to the front IN CORPUS ORDER, not pattern order");
  check(lastDisplay(run(kFix + "quietly summarize t*\ndisplay r(N)\n").blocks) == "3",
        "BUG-450 summarize: a read-only verb takes the pattern too");
  check(lastDisplay(run(kFix + "quietly summarize t?rn\ndisplay r(N)\n").blocks) == "3",
        "BUG-450: `?` matches exactly one character here as well as in `unab`");
  check(lastDisplay(run(kFix + "quietly summarize _all\ndisplay r(N)\n").blocks) == "3",
        "BUG-450: `_all` is a varlist keyword on these rows too");
  check(rcOfSrc("capture summarize zz*") == "111",
        "BUG-450: a pattern matching nothing is r(111), which is what Stata answers");
  check(rcOfSrc("capture drop zz*") == "111", "BUG-450: ...on the writing verbs too");

  // ---- the two guards ---------------------------------------------------
  // The rename arm now serves the group form itself (2026-10-21), so the
  // measured Stata result is the expectation: trunk->Trunk, turn->Turn (the
  // measurement this leg's header records), with the `*`s paired, never the
  // left side expanded on its own.
  check(namesAfter("capture rename t* T*") ==
            "make price mpg rep78 headroom Trunk weight length Turn",
        "BUG-450 GUARD: `rename t* T*` is rename-GROUPS, where the two `*`s are a PAIRING -- "
        "trunk->Trunk and turn->Turn as measured, never both columns onto one literal name");
  check(namesAfter("drop trunk turn") == "make price mpg rep78 headroom weight length",
        "BUG-450 GUARD: a varlist with no pattern character takes the unchanged path -- the common "
        "case cannot regress behind the new one");
}


void testAerCampaignRatchets() {
  std::printf("[selftest] --- AER scope-1 campaign ratchets (effect table + no-data routes) ---\n");

  // (1) ---------------------------------------------------------------------
  static const std::set<std::string> kEffectTableExempt = {
      "binscatter", "chow", "clonevar", "coefplot", "compress", "corr", "cross",
      "datasignature", "dfuller", "did_imputation", "drdid", "erase", "ereturn",
      "eventstudyinteract", "feols", "file", "fracreg", "frget", "frlink", "gmm", "hausman",
      "heckman", "irf", "isid", "ivprobit", "ivtobit", "labelbook", "logistic",
      "marginsplot", "mean", "mediate", "mi", "mixed", "mkdir", "mkspline", "mlogit", "newey",
      "nl", "ologit", "oprobit", "pperron", "prais", "proportion", "pwd", "qreg", "rddensity",
      "rdplot", "rdrobust", "reg3", "rm", "rmdir", "sdid", "sdtest", "ssc", "stcox", "streg",
      "stset", "svyset", "tabstat", "tobit", "tsline", "var", "vargranger", "version",
      "wntestq", "xtabond", "xtline", "zinb", "zip"
  };

  std::vector<std::string> unclassified;
  for (const auto& spec : piet::registry::commandRegistry()) {
    if (piet::parallel::lookupCommand(spec.canonical) != nullptr) continue;
    if (kEffectTableExempt.count(spec.canonical) != 0) continue;
    unclassified.push_back(spec.canonical);
  }
  check(unclassified.empty(),
        "every registry row is effect-table classified or frozen-exempt" +
            (unclassified.empty() ? std::string()
                                  : std::string(" (unclassified: ") +
                                        joinForMsg(unclassified) + ")"));

  // The exemption list may only shrink. If a name on it has since been
  // classified that is progress, not a failure — but the list must be pruned
  // so it never shelters a NEW row behind an old name.
  std::vector<std::string> staleExempt;
  for (const auto& name : kEffectTableExempt) {
    if (piet::parallel::lookupCommand(name) != nullptr) staleExempt.push_back(name);
  }
  check(staleExempt.empty(),
        "no frozen-exempt name has been classified without being pruned" +
            (staleExempt.empty() ? std::string()
                                 : std::string(" (prune: ") + joinForMsg(staleExempt) + ")"));

  // (2) ---------------------------------------------------------------------
  // Seeded with the five no-data routes that exist today; each AER bundle adds
  // its own rows here as it lands (ENV contributes all nineteen of its own).
  //
  // The bundle-D block is why this axis exists at all: an environment verb is
  // what a do-file runs BEFORE it loads anything, so a `net install` that
  // answered "No dataset is loaded" would fail on line 1 of every replication
  // package that needs it -- BUG-343's class, one layer earlier. Only the
  // no_data_loaded code is under test here, so the probes that legitimately
  // refuse (winexec, bcal, twoway__histogram_gen) or legitimately error
  // (findfile on a miss) belong on this list exactly as much as the rest.
  static const std::vector<std::string> kNoDataProbes = {
      "display 1", "pwd", "version", "timer on 1", "scalar list",
      // AER-SCOPE1 bundle D (ENV, 2026-09-03).
      "shell echo nodata_probe",
      "winexec notepad.exe",
      "type nodata_probe_absent.txt",
      "dir",
      "ls",
      "net install reghdfe",
      "ado uninstall moremata",
      "adopath + nodata_probe_dir",
      "sysdir set PLUS nodata_probe_dir",
      "findfile nodata_probe_absent.ado",
      "sleep 1",
      "pause on",
      "cls",
      "gr_edit .plotregion1.style.editstyle margin(medium) editcopy",
      "error 0",
      "bcal create nodata_probe_cal",
      "twoway__histogram_gen y, gen(ndh ndx)",
      "frames reset",
      // AER-SCOPE1 bundle I (UTIL, 2026-09-04). `filelist` is the one verb in
      // that bundle whose entire job runs BEFORE anything is loaded -- it
      // REPLACES the frame with a directory listing, so a do-file's first line
      // is exactly where it belongs and exactly where a missing
      // kNoDataCommands entry would answer "No dataset is loaded". Only the
      // no_data_loaded code is under test; an empty probe directory legitimately
      // producing some other outcome is fine here, the same way `findfile` on a
      // miss is.
      "filelist, directory(\".\") norecursive",
  };

#ifdef _WIN32
  std::error_code ndec;
  std::string ndWorkdir =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_nodata").u8string();
  std::filesystem::create_directories(ndWorkdir, ndec);
#else
  std::string ndWorkdir = "/tmp/piet_native_selftest_nodata";
  (void)std::system(("mkdir -p '" + ndWorkdir + "'").c_str());
#endif

  for (const auto& probe : kNoDataProbes) {
    piet::SessionState session(ndWorkdir);  // deliberately NO sysuse
    auto blocks = piet::dispatch(probe, session);
    bool sawNoData = false;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("code", std::string()) == "no_data_loaded") {
        sawNoData = true;
        break;
      }
    }
    check(!sawNoData, "`" + probe + "` runs with no dataset loaded (kNoDataCommands)");
  }

#ifdef _WIN32
  std::error_code ndrec;
  std::filesystem::remove_all(ndWorkdir, ndrec);
#else
  (void)std::system(("rm -rf '" + ndWorkdir + "'").c_str());
#endif
}


// --- BUG-105 abbreviation stopgap (2026-07-31): the 5 command
// abbreviations real Stata accepts that PIET silently rejected as unknown
// commands — `l`(list), `su`(summarize), `d`(describe), `ta`(tabulate),
// `cor`(correlate). This is the BUGLOG's own sanctioned STOPGAP: five
// hand-written aliases at the dispatcher arms, NOT a general
// minimum-unambiguous-prefix resolver (that is a separate arc, BACKLOG
// BUG105-112-HARDENING-REMAINDER).
//
// The assertion shape is deliberately stronger than testRegistryRoutes'
// drift guard above (which only proves a spelling does not fall through to
// `unsupported_command`): each abbreviation is run against its OWN fresh
// session and its non-echo block stream is compared for EQUALITY with the
// canonical spelling's. "Routes somewhere" is not the claim being made —
// "is the same command" is, and only equality can carry that. The echo
// block is the one legitimate difference (it quotes the raw input text).
void testBUG105AbbreviationStopgap() {
  std::printf("[selftest] --- BUG-105 abbreviation stopgap (l/su/d/ta/cor) ---\n");

#ifdef _WIN32
  std::error_code abec;
  std::string workdir =
      (std::filesystem::temp_directory_path() / "piet_native_selftest_bug105abbrev").u8string();
  std::filesystem::create_directories(workdir, abec);
#else
  std::string workdir = "/tmp/piet_native_selftest_bug105abbrev";
  (void)std::system(("mkdir -p '" + workdir + "'").c_str());
#endif

  auto meat = [](const nlohmann::json& blocks) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "echo") continue;
      out.push_back(b);
    }
    return out;
  };
  // Fresh SessionState + fresh `sysuse` per invocation, so the canonical
  // and the abbreviation are each measured from an identical starting
  // state and neither can inherit the other's session mutations.
  auto runFresh = [&](const std::string& cmd) {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    return meat(piet::dispatch(cmd, session));
  };
  auto hasErrorBlock = [](const nlohmann::json& blocks) {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") return true;
    }
    return false;
  };

  static const std::vector<std::pair<std::string, std::string>> kAbbrevPairs = {
      {"list beertax", "l beertax"},
      {"summarize beertax", "su beertax"},
      {"describe", "d"},
      {"tabulate year", "ta year"},
      {"correlate beertax unemp", "cor beertax unemp"},
  };
  for (const auto& pr : kAbbrevPairs) {
    const std::string& canonical = pr.first;
    const std::string& abbrev = pr.second;
    nlohmann::json canonBlocks = runFresh(canonical);
    nlohmann::json abbrevBlocks = runFresh(abbrev);
    // VACUITY GUARD. An equality assertion between two EMPTY block streams
    // passes trivially, and so does an absence-of-error check over an empty
    // stream — if `sysuse` ever stopped loading here, every assertion below
    // would go green while measuring nothing. Require real output first.
    check(!canonBlocks.empty(),
          "BUG-105 abbrev vacuity guard: `" + canonical + "` produced a non-empty block stream");
    check(!hasErrorBlock(canonBlocks),
          "BUG-105 abbrev control: `" + canonical + "` itself answers without an error block");
    check(abbrevBlocks == canonBlocks,
          "BUG-105 abbrev: `" + abbrev + "` produces the same blocks as `" + canonical + "`");
  }

  // DISCRIMINATOR. The equality comparator above is only worth anything if
  // it can also tell two DIFFERENT things apart. A deliberately unknown
  // token stands in for the seeded-RED: it must NOT compare equal to the
  // canonical. `zzlist` is used rather than a plausible near-spelling like
  // `li` on purpose — a real Stata abbreviation would become legal the day
  // the general prefix-resolver arc lands, turning this control into a
  // false RED; a nonsense token never will.
  {
    nlohmann::json canonList = runFresh("list beertax");
    nlohmann::json bogus = runFresh("zzlist beertax");
    check(bogus != canonList,
          "BUG-105 abbrev discriminator: an unknown token (`zzlist`) does NOT compare equal to "
          "`list` — the block-equality comparator distinguishes, it does not pass everything");
    check(hasErrorBlock(bogus),
          "BUG-105 abbrev discriminator: `zzlist` is still an honest error, so the 5 aliases "
          "widened the accepted set by exactly the 5 spellings and not by a catch-all");
  }

  // BUG-105 secondary (2) interaction, called out by this task's DoD: the
  // tokenizer's `in`-range gate resolves the command token through
  // registry::findByName(), and it runs at Step 5 — BEFORE dispatch ever
  // sees the command. So a dispatcher-only alias would ROUTE correctly and
  // still be rejected the moment an `in` qualifier is attached. This
  // assertion is what pins the registry `abbrev` half of the fix; it fails
  // if `l` is ever removed from the `list` row while the dispatcher arm
  // keeps the alias.
  {
    nlohmann::json canonIn = runFresh("list beertax in 1/5");
    nlohmann::json abbrevIn = runFresh("l beertax in 1/5");
    check(!hasErrorBlock(canonIn),
          "BUG-105 abbrev control: `list beertax in 1/5` answers without an error block");
    check(!hasErrorBlock(abbrevIn),
          "BUG-105 abbrev: `l beertax in 1/5` clears the registry-driven `in` gate too");
    check(abbrevIn == canonIn,
          "BUG-105 abbrev: `l ... in 1/5` matches `list ... in 1/5` block-for-block");
  }

#ifdef _WIN32
  std::error_code abrec;
  std::filesystem::remove_all(workdir, abrec);
#else
  (void)std::system(("rm -rf '" + workdir + "'").c_str());
#endif
}


// --- P. BETA campaign Phase A3: registry-driven qualifier policy table
// (BS-096) + D7 `in` mapping. ------------------------------------------
//
// A3 replaced eval/tokenizer.cpp's two hardcoded qualifier allow-lists
// (weight, `in`) with lookups against the SAME registry A0 built
// (registry::findByName()), and taught dispatcher.cpp's by:/bysort:
// fallback reject to distinguish Stata-structurally-illegal stems
// (codebook/describe/levelsof/table — audit G3) from Stata-legal-but-
// not-yet-implemented ones (everything else reaching that fallback) via
// the new CommandSpec::byIllegalInStata field. This leg is the
// policy-matrix gate BS-096's own SCOPE.md row calls for: a representative
// {command x qualifier} grid asserting LEGAL cells still pass through
// UNCHANGED (the freeze-preservation promise) and ILLEGAL cells reject
// with a command-aware — not command-blind — message.
void testBS096QualifierPolicyTable() {
  std::printf("[selftest] --- BS-096 qualifier policy table (BETA A3) ---\n");

  using piet::eval::tokenize;
  using piet::eval::TokenizerError;

  // --- registry sanity: byIllegalInStata lands on EXACTLY the 4
  // confirmed-illegal rows and nothing else — a drift guard for this new
  // field, same spirit as testRegistryRoutes() above. ------------------
  {
    int illegalCount = 0;
    // T7 (package-007 unblock) adds `lookfor`: StataBE answers a by-prefixed
    // lookfor with r(190) (lane L1 probe), so it is the fifth stem.
    bool codebookOk = false, describeOk = false, levelsofOk = false, tableOk = false;
    bool lookforOk = false;
    for (const auto& spec : piet::registry::commandRegistry()) {
      if (!spec.byIllegalInStata) continue;
      ++illegalCount;
      if (spec.canonical == "codebook") codebookOk = true;
      else if (spec.canonical == "describe") describeOk = true;
      else if (spec.canonical == "levelsof") levelsofOk = true;
      else if (spec.canonical == "table") tableOk = true;
      else if (spec.canonical == "lookfor") lookforOk = true;
    }
    check(illegalCount == 5 && codebookOk && describeOk && levelsofOk && tableOk && lookforOk,
          "A3: registry byIllegalInStata is set on exactly {codebook, describe, levelsof, table, "
          "lookfor} (audit G3's 4 Stata-structurally-illegal-with-by stems, plus lookfor from T7), "
          "nothing more/less");
  }

  // --- weight policy: LEGAL cells pass through unchanged ----------------
  try {
    auto pc = tokenize("summarize wage [fweight=x1]");
    check(pc.weightType && *pc.weightType == "fweight",
          "A3 weight: summarize+fweight still legal (freeze-preservation)");
  } catch (const TokenizerError&) {
    check(false, "A3 weight: summarize+fweight must NOT throw (freeze-preservation)");
  }
  try {
    auto pc = tokenize("tabulate industry [fw=x1]");
    check(pc.weightType && *pc.weightType == "fw",
          "A3 weight: tabulate+fw (abbrev spelling) still legal (freeze-preservation)");
  } catch (const TokenizerError&) {
    check(false, "A3 weight: tabulate+fw must NOT throw (freeze-preservation)");
  }

  // --- weight policy: ILLEGAL cells reject, command-aware ---------------
  // (a) zero weight support anywhere (corr — WeightPolicy::None, untouched
  // by the P3 weights wave) -> message NAMES it (was fully command-blind
  // before A3 — audit g#15/SCOPE.md BS-105). `regress` used to be this
  // probe's command but moved to WeightPolicy::AllWeights in P3 — see
  // testP3WeightsWave below for its newly-legal cells.
  // `corr` was this probe's command until BUG-1014 served correlate's [aw=]/[fw=]
  // (help correlate; w2aj_qualifiers.log:1869 qf_co_aw rc 0), then `tobit` until
  // BUG-1544 served its weights; `reg3` stays WeightPolicy::None while help reg3
  // allows aweights and fweights -- an engine gap.
  try {
    tokenize("reg3 y x1 [aw=x2]");
    check(false, "A3 weight: reg3+aweight must still throw (WeightPolicy::None)");
  } catch (const TokenizerError& e) {
    check(e.block.at("code") == "unsupported_option", "A3 weight: reg3+aweight -> unsupported_option");
    std::string msg = e.block.at("message").get<std::string>();
    check(msg.find("reg3") != std::string::npos,
          "A3 weight: reject message NAMES the command (was command-blind pre-A3) — got: " + msg);
  }
  try {
    auto pc = tokenize("corr x1 x2 [aw=x2]");
    check(pc.weightType && *pc.weightType == "aw",
          "A3 weight: corr+aweight is LEGAL now, as Stata's (help correlate; "
          "w2aj_qualifiers.log:1869 qf_co_aw rc 0)");
  } catch (const TokenizerError&) {
    check(false, "A3 weight: corr+aweight must NOT throw (help correlate)");
  }
  // (b) fweight-only command given a DIFFERENT weight type -> message
  // distinguishes "fweight-only" from "no weight support at all" (the
  // g#15/BS-105 nuance). `tabulate` was this probe's command until the
  // qualifier wave served [aw=]/[iw=] (f8413c27; w2aj_qualifiers.log:2041
  // qf_tb_aw_sp rc 0), then `collapse` until it served its documented
  // `[aw fw iw pw]` (w2au_collapse_family.log cz_c_w_aweight rc 0); `tab2` keeps
  // FweightOnly and refuses an aweight in Stata's own words.
  try {
    tokenize("tab2 x1 x2 [aweight=x1]");
    check(false, "A3 weight: tab2+aweight must still throw (FweightOnly in this engine)");
  } catch (const TokenizerError& e) {
    std::string msg = e.block.at("message").get<std::string>();
    check(msg.find("aweights not allowed") != std::string::npos,
          "A3 weight: tab2+aweight is refused as `aweights not allowed` — got: " + msg);
  }
  try {
    auto pc = tokenize("collapse (mean) x1 [aweight=x1]");
    check(pc.weightType && *pc.weightType == "aweight",
          "A3 weight: collapse+aweight is LEGAL now (collapse takes [aw fw iw pw]; "
          "w2au_collapse_family.log cz_c_w_aweight rc 0)");
  } catch (const TokenizerError&) {
    check(false, "A3 weight: collapse+aweight must NOT throw (collapse takes aweight)");
  }
  try {
    auto pc = tokenize("tabulate industry [aweight=x1]");
    check(pc.weightType && *pc.weightType == "aweight",
          "A3 weight: tabulate+aweight is LEGAL now (help tabulate_oneway; "
          "w2aj_qualifiers.log:2041 qf_tb_aw_sp rc 0)");
  } catch (const TokenizerError&) {
    check(false, "A3 weight: tabulate+aweight must NOT throw (help tabulate_oneway)");
  }
  // (c) a `bysort`-prefixed line: the weight is the SUB-command's; the outer
  // pass carries it and the by-driver's re-tokenize of the sub-command gates it
  // (SUM-BYWEIGHT, w2as_summary_stats_family sx_cumul_by_equal rc 0).
  try {
    auto pc = tokenize("bysort grp: summarize wage [fw=x1]");
    check(true, "A3 weight: bysort-prefixed weight passes the outer token");
    (void)pc;
  } catch (const TokenizerError&) {
    check(false, "A3 weight: bysort-prefixed weight must not be refused by the outer token");
  }

  // --- `in` policy: LEGAL cells pass through unchanged (D7) -------------
  try {
    auto pc = tokenize("list id year in 1/5");
    check(pc.inRange && *pc.inRange == "1/5", "A3/D7 in: list+in still legal (freeze-preservation)");
  } catch (const TokenizerError&) {
    check(false, "A3/D7 in: list+in must NOT throw (freeze-preservation)");
  }
  try {
    auto pc = tokenize("gen byte flag = 1 in 1/5");
    check(pc.inRange && *pc.inRange == "1/5", "A3/D7 in: gen+in still legal (freeze-preservation)");
  } catch (const TokenizerError&) {
    check(false, "A3/D7 in: gen+in must NOT throw (freeze-preservation)");
  }

  // --- `in` policy: ILLEGAL cells reject, command-aware ------------------
  // BS-090 (BETA campaign, Phase G): this leg used to name `summarize` —
  // summarize's allowInRange is now TRUE (see testBS090InRangeExpansion
  // below for its own dedicated positive coverage), so this leg was
  // repointed at `tabstat`, deliberately left OUT of BS-090's flip list
  // (tabstat/tobit share applyIfFilterM4 with corr/ttest but their call
  // sites still pass no inRange argument — registry.cpp's own comment).
  try {
    tokenize("tabstat wage in 1/5");
    check(false, "A3/D7 in: tabstat+in must still throw (freeze-preservation; BS-090 left it "
                 "out of scope)");
  } catch (const TokenizerError& e) {
    std::string msg = e.block.at("message").get<std::string>();
    check(msg.find("tabstat") != std::string::npos,
          "A3/D7 in: reject message NAMES the command (was command-blind pre-A3) — got: " + msg);
  }
  // BS-090: egen's allowInRange is now TRUE (flipped this phase — real
  // `in`-range support, reusing the SAME synthetic row-index column the
  // if-merge-back path already materializes; see egen.cpp's runEgen) —
  // this leg used to assert the opposite ("must still throw", Phase-F
  // scope). Flipped to a positive legality check; the actual composition
  // behavior (in ANDed with if, correct group-relative merge-back) is
  // exercised end-to-end by testBS090InRangeExpansion below.
  try {
    auto pc = tokenize("egen newv = mean(x1) in 1/5");
    check(pc.inRange && *pc.inRange == "1/5", "BS-090: egen+in now legal (allowInRange flipped)");
  } catch (const TokenizerError&) {
    check(false, "BS-090: egen+in must NOT throw (allowInRange flipped true this phase)");
  }

  // --- D7 end-to-end: `in` composes with `if` (AND) on a legal command --
  {
    namespace fs = std::filesystem;
    std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs096_d7").u8string();
    std::error_code ec;
    fs::remove_all(workdir, ec);
    fs::create_directories(workdir, ec);
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch("list state year if year>=1983 in 1/3", session);
    bool sawBrowse = false;
    bool sawError = false;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "browse") sawBrowse = true;
      if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
    }
    check(sawBrowse && !sawError,
          "A3/D7: `list ... if COND in a/b` composes both qualifiers on a legal command "
          "(no error, produces a browse block)");
    fs::remove_all(workdir, ec);
  }

  // --- by-gate: structurally-illegal stems get the NEW honest message;
  // Stata-legal-but-unimplemented stems KEEP the old "not supported yet"
  // wording; the real gen/replace/egen implementation is untouched. All
  // via full dispatch() since the by-gate lives in dispatcher.cpp, not
  // the tokenizer. -------------------------------------------------------
  {
    namespace fs = std::filesystem;
    std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs096_by").u8string();
    std::error_code ec;
    fs::remove_all(workdir, ec);
    fs::create_directories(workdir, ec);

    auto firstError = [](const nlohmann::ordered_json& blks) -> nlohmann::ordered_json {
      for (const auto& b : blks) {
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
      }
      return nlohmann::ordered_json();
    };

    // (a) codebook: structurally illegal in Stata -> NEW wording, no more
    // misleading "not supported yet" (audit G3 finding #10).
    {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("bysort state: codebook year", session);
      auto err = firstError(blocks);
      check(!err.is_null(), "A3 by-gate: `bysort: codebook` still rejects (freeze-preservation)");
      std::string msg = err.value("message", std::string());
      check(msg.find("not supported yet") == std::string::npos &&
                msg.find("may not be combined") != std::string::npos,
            "A3 by-gate: `bysort: codebook` gets the NEW honest 'Stata disallows this' wording, "
            "not the misleading 'not supported yet' text (audit G3#10/BS-096) — got: " + msg);
    }
    // (b) describe: same illegal bucket as codebook.
    {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("bysort state: describe", session);
      auto err = firstError(blocks);
      check(!err.is_null(), "A3 by-gate: `bysort: describe` still rejects (freeze-preservation)");
      std::string msg = err.value("message", std::string());
      check(msg.find("not supported yet") == std::string::npos,
            "A3 by-gate: `bysort: describe` drops the misleading 'not supported yet' text — got: " + msg);
    }
    // (c) count: graduated out of the BS-107 "not supported yet" bucket (FU-163):
    // `bysort state: count` now runs per group, so it raises no error block.
    // (BS-092 had repointed this leg from summarize to count for the same reason.)
    {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("bysort state: count", session);
      check(firstError(blocks).is_null(),
            "A3 by-gate: `bysort: count` runs per group (FU-163, graduated out of BS-107)");
    }
    // (d) gen: the REAL by-group implementation is untouched by this
    // message-only refactor — regression guard.
    {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      auto blocks = piet::dispatch("bysort state: gen byte flag = 1", session);
      auto err = firstError(blocks);
      check(err.is_null(), "A3 by-gate regression guard: `bysort: gen` still WORKS (real "
                            "implementation untouched by the message-only refactor)");
    }

    fs::remove_all(workdir, ec);
  }
}


void testBS091CClassRegistryInExpressions() {
  std::printf("[selftest] --- BS-091/BS-101 c()-class registry in NON-display expression "
              "contexts (gen/scalar/if), single source with display ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs091").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  int64_t n = session.hasData() ? session.datastore()->nRows() : 0;
  check(n > 0, "BS-091 setup: fatalities loaded with >0 rows");

  // (a) `gen k = c(N)` — a NUMERIC expression context, NOT display. Before
  // this fix, c() resolution lived ONLY inside display's own bespoke
  // tokenizer (BS-091's exact finding) — a bare `c(N)` reaching cobra's
  // row-wise `eval --gen` would have been an undefined-name failure.
  auto genBlocks = piet::dispatch("gen nobs_c = c(N)", session);
  bool genOk = true;
  for (const auto& b : genBlocks) {
    if (b.value("type", "") == "error") genOk = false;
  }
  check(genOk, "BS-091: `gen nobs_c = c(N)` does not error");
  check(session.hasData() && session.datastore()->has("nobs_c"),
        "BS-091: `gen nobs_c = c(N)` created the new variable");
  if (session.hasData() && session.datastore()->has("nobs_c") &&
      session.datastore()->nRows() > 0) {
    double v = session.datastore()->col("nobs_c").f64()[0];
    check(v == static_cast<double>(n), "BS-091: `nobs_c`'s value equals c(N), the row count");
  }

  // (b) `di c(N)*2` — c() combined with a real arithmetic operator, through
  // the SAME evalArith path `gen`'s cobra-forwarded expression used above —
  // single source, not two divergent resolvers.
  auto dblocks = piet::dispatch("display c(N)*2", session);
  std::string dtext;
  for (const auto& b : dblocks) {
    if (b.value("type", "") == "display") dtext = b.value("text", std::string());
  }
  double dval = -1.0;
  try {
    dval = std::stod(dtext);
  } catch (...) {
  }
  check(std::abs(dval - static_cast<double>(n) * 2.0) < 1e-6,
        "BS-091: `display c(N)*2` shows N*2 (got '" + dtext + "')");

  // (c) `scalar bs091_k = c(N)` — the scalar/local capture-definition path
  // (dispatcher.cpp's tryCaptureDefinition call site), a THIRD distinct
  // context sharing the same substituteCRefs pre-pass.
  (void)piet::dispatch("scalar bs091_k = c(N)", session);
  const std::string* scalarVal = session.eval.scalars.find("bs091_k");
  check(scalarVal != nullptr && *scalarVal == std::to_string(n),
        "BS-091: `scalar bs091_k = c(N)` resolves via the same substituteCRefs pre-pass");

  // (d) a string-valued key (c(os)) resolves to one of Stata's own c(os)
  // strings through display.
  auto osBlocks = piet::dispatch("display c(os)", session);
  std::string osText;
  for (const auto& b : osBlocks) {
    if (b.value("type", "") == "display") osText = b.value("text", std::string());
  }
  check(osText == "MacOSX" || osText == "Windows" || osText == "Unix",
        "BS-091: `display c(os)` resolves to one of Stata's own c(os) strings (got: '" + osText +
            "')");

  // (e) an UNKNOWN c(...) name (outside this build's ~15-key spine) still
  // surfaces an honest error — never silently swallowed (BUG-090/BUG-091
  // philosophy preserved by the new, larger registry).
  auto unkBlocks = piet::dispatch("display c(totally_bogus_key_xyz)", session);
  bool unkIsError = false;
  for (const auto& b : unkBlocks) {
    if (b.value("type", "") == "error") unkIsError = true;
  }
  check(unkIsError,
        "BS-091: an unknown c(...) name surfaces an honest error, never silently swallowed");

  fs::remove_all(workdir, ec);
}


void testBS046To048SetRegistry() {
  std::printf("[selftest] --- BS-046/047/048/101 set registry: level/seed/graphics honored, "
              "unknown set subcommand gets a visible note (never silent, never an error) ---\n");
  namespace fs = std::filesystem;
  std::string base = (fs::temp_directory_path() / "piet_native_selftest_bs046").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);

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

  // --- set level 90 (a) updates session.level, (b) reaches c(level), and
  // (c) genuinely narrows a `ttest`'s CI — the ONE PIET-wired command whose
  // cobra CLI actually reads --level (verified: cmd_ttest, cmds_summary.cpp
  // reads opt(a,"--level") and applies it to alpha/t_critical throughout;
  // cmd_feols/cmd_xtreg/cmd_logit/cmd_probit do NOT — see estimation_m4.cpp/
  // handlers.cpp's own comments on why the session.level fallback was
  // deliberately NOT extended to those).
  {
    std::string wd = base + "/level";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    check(session.level == 95, "BS-048 setup: session.level defaults to 95");
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as the earlier BUG-039/
      // BUG-055/BUG-056/BS-013 legs above (FIX-SELFTEST-ISOBUILD-CRASH):
      // without fatalities.csv, `ttest fatal == 5` never produces a real
      // "estimation" block and the unguarded est90/est95 `.value()` calls
      // would crash. Skip the whole level-fallback leg honestly (both
      // sessions share the same missing fixture, so session95 would fail
      // identically).
      skip("BS-048 level-fallback leg", "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "set level 90");
      check(session.level == 90, "BS-048: `set level 90` updates session.level");

      auto jc = runCmd(session, "display c(level)");
      auto dispC = firstBlockOfType(jc, "display");
      check(dispC.value("text", std::string()) == "90",
            "BS-091: c(level) reflects the `set level` just run");

      auto j90 = runCmd(session, "ttest fatal == 5");
      auto est90 = firstBlockOfType(j90, "estimation");
      auto coefs90 = est90.value("coefs", nlohmann::json::array());
      check(!coefs90.empty(), "BS-048 setup: `ttest fatal == 5` (session level 90) returns coefs");
      double width90 = coefs90.empty()
                            ? -1.0
                            : (coefs90[0].value("ci_hi", 0.0) - coefs90[0].value("ci_lo", 0.0));

      std::string wd95 = base + "/level95";
      fs::create_directories(wd95, ec);
      piet::SessionState session95(wd95);
      runCmd(session95, "sysuse fatalities, clear");
      // no `set level` here -> stays at the untouched 95 default.
      auto j95 = runCmd(session95, "ttest fatal == 5");
      auto est95 = firstBlockOfType(j95, "estimation");
      auto coefs95 = est95.value("coefs", nlohmann::json::array());
      double width95 = coefs95.empty()
                            ? -1.0
                            : (coefs95[0].value("ci_hi", 0.0) - coefs95[0].value("ci_lo", 0.0));

      check(width90 > 0 && width95 > 0, "BS-048 setup: both ttest CI widths were captured");
      check(width95 > width90,
            "BS-048: a session default of `set level 90` (width " + std::to_string(width90) +
                ") produces a NARROWER ttest CI than the untouched 95 default (width " +
                std::to_string(width95) +
                ") for the identical one-sample test, with NEITHER command passing an explicit "
                "level() — the session default genuinely reaches ttest's own --level flow");

      // Regression-safety: `set level 90` must NOT break an ordinary plain
      // regress afterward (cobra's feols CLI never reads --level at all —
      // see the comment in handlers.cpp::runRegress on why that fallback
      // was deliberately NOT wired there).
      auto jReg = runCmd(session, "regress fatal beertax");
      bool regOk = true;
      for (const auto& b : jReg.value("blocks", nlohmann::json::array())) {
        if (b.value("type", std::string()) == "error") regOk = false;
      }
      check(regOk,
            "BS-048 regression guard: `regress` after `set level 90` still succeeds (the fallback "
            "is scoped to ttest only, not extended to feols/xtreg/logit/probit)");
    }
  }

  // --- set seed persistence + c(seed) exposure (BS-046).
  {
    std::string wd = base + "/seed";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    check(!session.seed.has_value(), "BS-046 setup: seed unset by default");
    runCmd(session, "set seed 20260712");
    check(session.seed.has_value() && *session.seed == 20260712,
          "BS-046: `set seed 20260712` persists on the session");
    // c(seed) is the generator state `set seed` produced, as c(rngstate) is
    // (BUG-1051: StataBE 19.5 answers the 5011-character state, not the number).
    auto j = runCmd(session, "display c(seed)");
    auto disp = firstBlockOfType(j, "display");
    check(disp.value("text", std::string()) == cecon::Mt64(20260712).state_string(),
          "BS-046: c(seed) reflects the `set seed` just run");
  }

  // --- set graphics off honored by the renderer's `suppressed` field (BS-047).
  {
    std::string wd = base + "/graphics";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as the level leg above
      // (FIX-SELFTEST-ISOBUILD-CRASH): without fatalities.csv, `twoway`
      // never produces a real "graph" block and the unguarded `g.value()`
      // below would crash. Skip the whole graphics-suppression leg honestly.
      skip("BS-047 graphics-suppression leg", "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "set graphics off");
      check(session.graphicsOff == true, "BS-047: `set graphics off` sets session.graphicsOff");
      auto j = runCmd(session, "twoway (scatter fatal beertax)");
      auto g = firstBlockOfType(j, "graph");
      check(g.value("suppressed", false) == true,
            "BS-047: a twoway graph built AFTER `set graphics off` reports suppressed=true "
            "(previously hardcoded false regardless of `set graphics`)");
      runCmd(session, "set graphics on");
      check(session.graphicsOff == false, "BS-047: `set graphics on` clears session.graphicsOff");
    }
  }

  // --- unknown `set` subcommand: explicit no-op NOTE, never silent, never
  // an error (real Stata do-files run `set more off`/`set matsize N`
  // constantly; turning these into errors would halt them for no reason).
  {
    std::string wd = base + "/unknown";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    auto j = runCmd(session, "set matsize 800");
    bool sawErr = false, sawNote = false;
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == "error") sawErr = true;
      if (b.value("type", std::string()) == "echo" &&
          b.value("text", std::string()).find("matsize") != std::string::npos) {
        sawNote = true;
      }
    }
    check(!sawErr, "BS-101: an unrecognized `set` subcommand is NEVER an error");
    check(sawNote,
          "BS-101: `set matsize 800` (an unimplemented subcommand) now emits a visible note "
          "naming it, instead of a totally silent no-op");
  }

  fs::remove_all(base, ec);
}


// --- BETA campaign Phase C batch C1 (silent-wrong sweep: estimation/summary
// adapters + spine defects). Every numeric expectation below is pinned from
// a real StataBE 19.5 oracle capture
// (internal capture c-oracle-stata.log),
// re-run against sysuse fatalities + `gen high = (beertax > .5)`, NOT from
// memory. BS-023 (reghdfe without absorb()) is deliberately NOT covered by
// a leg here: the oracle showed real Stata's own `reghdfe income beertax`
// (no absorb()) SUCCEEDING with numbers identical to plain OLS — the
// opposite of this batch's original "should r(198) error" brief — so
// nothing was changed there (see the batch's final report for the evidence).

void testC1N1CaptureNoisilyAbbrev() {
  std::printf("[selftest] --- C1/N1 capture noisily abbreviation + success/error visibility ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_n1").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // (a) `noi` (previously missing from kQuietlyPrefixes) must be recognised
  // as the `noisily` abbreviation, not dispatched as an unknown command
  // (which was swallowing the WHOLE inner command's output, rc=199).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch("cap noi count", session);
    check(session.eval.locals.get("_rc", "") == "0",
          "C1/N1(a): `cap noi count` recognises `noi` as noisily (not an unknown command) -- "
          "_rc=0, not 199");
    bool sawDisplay = false;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") sawDisplay = true;
    }
    check(sawDisplay, "C1/N1(a)+(b): `cap noi count` (success) shows its result");
  }

  // (b) `cap noi` on an ERRORING inner command shows the error too (not
  // just swallowed) -- noisily-rooted error path.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch("cap noi summarize nonexistvar_c1n1", session);
    bool sawError = false;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawError = true;
    }
    check(sawError, "C1/N1(b): `cap noi <erroring cmd>` shows the error");
  }

  // (b) regression guard: bare `cap` (no noisily) on a SUCCESSFUL command
  // suppresses the result entirely (previously it was always forwarded,
  // regardless of noisily).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch("cap count", session);
    check(blocks.size() == 1, "C1/N1(b) regression guard: bare `cap count` (success) shows "
          "nothing but its own echo");
  }

  fs::remove_all(workdir, ec);
}


// ── B4/W1: a trailing `, if …` / `, in …` qualifier (AFTER the options) ─────
//
// Stata accepts ONE if/in qualifier after a command's option list, and what
// makes this a specification rather than a convenience is that the TWO
// rejects carry DIFFERENT messages. Measured on StataBE 19.5 and captured in
// tests/eval/golden/aer_b4/pins_rc.{do,log} + the oracle records
// summarize|trailing_if / trailing_in / leading_if_AND_trailing_if /
// two_trailing_if in tests/eval/golden/aer_b4/aer_b4_oracle.json:
//
//   summarize price if mpg>15, detail              rc 0    Obs 64  (single_if_opt)
//   summarize price, detail, if mpg<30             rc 0    Obs 67  (trailing_if)
//   summarize price, detail, in 1/10               rc 0    Obs 10  (trailing_in)
//   summarize price if mpg>15, detail, if mpg<30   rc 198  "invalid syntax"
//   summarize price, detail, if mpg<30, if mpg>10  rc 198  "option if not allowed"
//
// So Stata consumes EXACTLY ONE trailing qualifier: a second one is no longer
// a qualifier at all — it falls through to the ordinary option net and is
// reported as an option NAMED `if` — while a LEADING qualifier plus a trailing
// one is rejected earlier, as syntax. An implementation that answers "invalid
// syntax" for both is wrong in the second case, one that answers "option if
// not allowed" for both is wrong in the first, and BOTH of those wrong
// implementations pass a test that only checks rc == 198. That is why every
// leg below asserts the MESSAGE, not just the code.
//
// PIET before this fix (measured): all four rejected with unsupported_option /
// "option `if` not allowed" — the two rc-0 statements were refused outright,
// and the leading+trailing case carried the wrong one of the two messages.
void testB4W1TrailingQualifier() {
  std::printf("[selftest] --- B4/W1: trailing `, if` / `, in` qualifier ---\n");
  namespace fs = std::filesystem;
  std::error_code ec;

  using piet::eval::ParsedCommand;
  using piet::eval::tokenize;

  auto optOf = [](const ParsedCommand& pc, const std::string& key) -> const piet::eval::OptValue* {
    for (const auto& kv : pc.opts)
      if (kv.first == key) return &kv.second;
    return nullptr;
  };
  auto ifOf = [](const ParsedCommand& pc) {
    return pc.ifExpr ? ("'" + *pc.ifExpr + "'") : std::string("<none>");
  };
  auto inOf = [](const ParsedCommand& pc) {
    return pc.inRange ? ("'" + *pc.inRange + "'") : std::string("<none>");
  };

  // ── (1) tokenizer level: the trailing qualifier is parsed as a QUALIFIER,
  // not as an option key. That is the entire delta, asserted where it happens.
  {
    ParsedCommand pc = tokenize("summarize price, detail, if mpg<30");
    check(pc.ifExpr.has_value() && *pc.ifExpr == "mpg<30",
          "W1: `summarize price, detail, if mpg<30` parses a trailing if-qualifier — got " +
              ifOf(pc));
    check(optOf(pc, "detail") != nullptr, "W1: the real option before it survives (`detail`)");
    check(optOf(pc, "if") == nullptr,
          "W1: `if` is NOT left behind as an option key (that was the pre-fix reject path)");
    check(pc.varlist.size() == 1 && pc.varlist[0] == "price",
          "W1: the varlist is untouched by the trailing split");
  }
  {
    ParsedCommand pc = tokenize("summarize price, detail, in 1/10");
    check(pc.inRange.has_value() && *pc.inRange == "1/10",
          "W1: `summarize price, detail, in 1/10` parses a trailing in-qualifier — got " +
              inOf(pc));
    check(optOf(pc, "in") == nullptr, "W1: `in` is not left behind as an option key");
  }

  // ── (2) SEEDED-RED ARM: a comma inside a STRING LITERAL is not a split
  // point.
  //
  // The naive implementation of this feature is find(", if "). The first
  // assertion of each arm is the seed: the statement really does contain that
  // byte sequence, so a naive splitter WOULD fire on it, hand the command a
  // truncated literal and invent an if-condition out of the rest of the
  // string. The split here is driven by findUnquotedComma — the quote /
  // compound-quote / paren scanner this file already used for the command's
  // OWN first comma — so it does not fire. (BUG-115 class,
  // [[piet-quote-aware-qualifier]].)
  {
    const std::string stmt = "gen z = \"a, if b\"";
    check(stmt.find(", if ") != std::string::npos,
          "W1 seeded-RED seed: `gen z = \"a, if b\"` DOES contain the literal \", if \" that a "
          "find()-based splitter fires on");
    ParsedCommand pc = tokenize(stmt);
    check(!pc.ifExpr.has_value(),
          "W1 seeded-RED: a comma+`if ` inside a quoted literal is NOT a trailing qualifier — "
          "got if=" + ifOf(pc));
    check(pc.opts.empty() && pc.cmd == "gen",
          "W1 seeded-RED: ...and the line is not split into options at all");
  }
  {
    // The same trap at the exact surface the new helper walks: a quoted comma
    // inside the OPTION tail, i.e. after a real top-level comma has already
    // been consumed. Quote-blind, this splits and yields ifExpr = `b"`.
    const std::string stmt = "somecmd x, label \"a, if b\" replace";
    check(stmt.find(", if ") != std::string::npos,
          "W1 seeded-RED seed (option tail): the quoted \", if \" is in the post-comma text the "
          "trailing-qualifier scan walks");
    ParsedCommand pc = tokenize(stmt);
    check(!pc.ifExpr.has_value(),
          "W1 seeded-RED: a quoted comma in the option tail is not a trailing-qualifier split "
          "point — got if=" + ifOf(pc));
    check(optOf(pc, "label") != nullptr && optOf(pc, "replace") != nullptr,
          "W1 seeded-RED: ...and the option list is intact on both sides of the literal");
  }

  // ── (3) `if(` / `in(` are NOT qualifiers: the keyword must be followed by
  // WHITESPACE. `nl (...) , in(b 2)` is nl's initial() option at its minimum
  // abbreviation, and a keyword-only (\b-style) test eats it — this exact bug
  // was found in the census tool during this campaign.
  {
    ParsedCommand pc = tokenize("nl func, nolog, in(b 2)");
    check(!pc.inRange.has_value(),
          "W1: `, in(b 2)` is an OPTION (nl's initial() abbreviated), not an `in` row-range "
          "qualifier — got in=" + inOf(pc));
    const piet::eval::OptValue* v = optOf(pc, "in");
    check(v != nullptr && !v->isBool && v->strValue == "b 2",
          "W1: ...and it keeps its parenthesised value `b 2`");
  }
  {
    ParsedCommand pc = tokenize("somecmd x, robust, if(a)");
    check(!pc.ifExpr.has_value() && optOf(pc, "if") != nullptr,
          "W1: `, if(a)` is likewise an option key, not a trailing if-qualifier — got if=" +
              ifOf(pc));
  }

  // ── (4) EXACTLY ONE trailing qualifier is consumed. The second `if` stays in
  // the option list — that is what makes its reject the option-net sentence
  // instead of a syntax error.
  {
    ParsedCommand pc = tokenize("summarize price, detail, if mpg<30, if mpg>10");
    check(pc.ifExpr.has_value() && *pc.ifExpr == "mpg<30",
          "W1: the FIRST trailing qualifier is the one consumed — got " + ifOf(pc));
    check(optOf(pc, "if") != nullptr,
          "W1: the SECOND one falls through to the option net as an option named `if` (this is "
          "what produces Stata's `option if not allowed`, not a syntax error)");
  }

  // ── (5) leading qualifier + trailing qualifier is a SYNTAX rejection, and a
  // different one.
  {
    bool threw = false;
    std::string msg, code;
    try {
      (void)tokenize("summarize price if mpg>15, detail, if mpg<30");
    } catch (const piet::eval::TokenizerError& e) {
      threw = true;
      msg = e.block.value("message", std::string());
      code = e.block.value("code", std::string());
    }
    check(threw && code == "syntax_error" && msg == "invalid syntax",
          "W1: leading `if` + trailing `if` is rejected as `invalid syntax` (Stata's own "
          "wording), not as a bad option — got code='" + code + "' msg='" + msg + "'");
    nlohmann::ordered_json blk;
    blk["code"] = code;
    blk["message"] = msg;
    check(threw && piet::eval::coarseRc(blk) == "198",
          "W1: ...and that block answers r(198), the rc Stata returned");
  }

  // ── (6) end-to-end through dispatch, on the real fixture, with the Stata
  // numbers. tests/validate/auto.hbin IS Stata's auto.dta — the dataset the
  // oracle was captured on.
  const std::string autoFix = selftestValidateDir() + "/auto.hbin";
  if (!std::ifstream(cecon::to_native_path(autoFix)).good()) {
    skip("B4/W1 end-to-end legs (need tests/validate/auto.hbin)", "fixture not found at " + autoFix);
    return;
  }
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_b4w1").u8string();
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json::object();
  };
  // Stata prints Mean to 7 significant digits; the oracle carries the full
  // double as %21x, so these are pinned BIT-exactly rather than to a
  // tolerance — a mean over a fixture's own doubles is not an estimated
  // quantity, and a tolerance here would hide a wrong-sample answer.
  const double kMeanTrailingIf = 6362.582089552238;  // %21x +1.8da9503d22635X+00c
  const double kMeanTrailingIn = 5517.4;             // %21x +1.58d6666666666X+00c
  const double kMeanLeadingIf = 5517.546875;         // Stata's 5517.547 block, exact in binary

  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use " + autoFix + ", clear", session);

    // (6a) regression guard: the LEADING qualifier still works, unchanged.
    {
      auto blocks = piet::dispatch("summarize price if mpg>15, detail", session);
      const auto* sm = firstBlockOfTypeD1(blocks, "summarize");
      check(sm != nullptr && sm->at("stats").at("price").value("N", -1.0) == 64.0 &&
                sm->at("stats").at("price").value("mean", 0.0) == kMeanLeadingIf,
            "W1 regression guard: `summarize price if mpg>15, detail` still answers Stata's "
            "Obs 64 / Mean 5517.547 (rc.log RC:single_if_opt:0)");
    }

    // (6b) the trailing `if` really RESTRICTS the computation — Obs 67, and a
    // different mean from the leading-if run above.
    {
      auto blocks = piet::dispatch("summarize price, detail, if mpg<30", session);
      const auto* sm = firstBlockOfTypeD1(blocks, "summarize");
      check(firstError(blocks).empty(),
            "W1: `summarize price, detail, if mpg<30` is accepted (Stata rc 0)");
      check(sm != nullptr && sm->at("stats").at("price").value("N", -1.0) == 67.0,
            "W1: ...over Stata's Obs 67 (oracle summarize|trailing_if)");
      check(sm != nullptr && sm->at("stats").at("price").value("mean", 0.0) == kMeanTrailingIf,
            "W1: ...with Stata's Mean, bit-exact against the oracle's %21x "
            "+1.8da9503d22635X+00c");
    }

    // (6c) trailing `in`.
    {
      auto blocks = piet::dispatch("summarize price, detail, in 1/10", session);
      const auto* sm = firstBlockOfTypeD1(blocks, "summarize");
      check(firstError(blocks).empty(),
            "W1: `summarize price, detail, in 1/10` is accepted (Stata rc 0)");
      check(sm != nullptr && sm->at("stats").at("price").value("N", -1.0) == 10.0 &&
                sm->at("stats").at("price").value("mean", 0.0) == kMeanTrailingIn,
            "W1: ...over Stata's Obs 10 / Mean +1.58d6666666666X+00c (oracle "
            "summarize|trailing_in)");
    }

    // (6d)/(6e) the two rejects, asserted on their DISTINCT messages. If these
    // ever print the same sentence, one of them is wrong.
    std::string msgLeadingPlusTrailing, msgTwoTrailing;
    {
      auto blocks = piet::dispatch("summarize price if mpg>15, detail, if mpg<30", session);
      nlohmann::ordered_json e = firstError(blocks);
      msgLeadingPlusTrailing = e.value("message", std::string());
      check(e.value("code", std::string()) == "syntax_error" &&
                msgLeadingPlusTrailing == "invalid syntax" && piet::eval::coarseRc(e) == "198",
            "W1: leading + trailing `if` -> r(198) `invalid syntax` (oracle "
            "summarize|leading_if_AND_trailing_if) — got rc=" +
                (e.empty() ? std::string("<no error>") : piet::eval::coarseRc(e)) + " msg='" +
                msgLeadingPlusTrailing + "'");
    }
    {
      auto blocks = piet::dispatch("summarize price, detail, if mpg<30, if mpg>10", session);
      nlohmann::ordered_json e = firstError(blocks);
      msgTwoTrailing = e.value("message", std::string());
      // PIET's option-net sentence is Stata's, modulo this product's house
      // convention of backticking the option name (errors.cpp's
      // rejectUnknownOptions, shared by every option reject in the engine):
      // Stata "option if not allowed", PIET "option `if` not allowed".
      check(e.value("code", std::string()) == "unsupported_option" &&
                msgTwoTrailing.find("option") != std::string::npos &&
                msgTwoTrailing.find("if") != std::string::npos &&
                msgTwoTrailing.find("not allowed") != std::string::npos &&
                piet::eval::coarseRc(e) == "198",
            "W1: two trailing `if`s -> r(198) `option if not allowed` (oracle "
            "summarize|two_trailing_if) — got rc=" +
                (e.empty() ? std::string("<no error>") : piet::eval::coarseRc(e)) + " msg='" +
                msgTwoTrailing + "'");
    }
    check(!msgLeadingPlusTrailing.empty() && msgLeadingPlusTrailing != msgTwoTrailing,
          "W1: the two r(198) cases carry DIFFERENT messages — that difference IS the "
          "specification, and a one-message implementation passes an rc-only test");

    // (6f) a by-prefixed statement goes through the same path. 50 + 17 = 67 —
    // the same restricted sample the ungrouped leg (6b) measured, and NOT the
    // unrestricted 52 + 22.
    {
      auto blocks = piet::dispatch("by foreign: summarize price, detail, if mpg<30", session);
      check(firstError(blocks).empty(),
            "W1: `by foreign: summarize price, detail, if mpg<30` is accepted too");
      std::vector<double> ns;
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "summarize")
          ns.push_back(b.at("stats").at("price").value("N", -1.0));
      check(ns.size() == 2 && ns[0] == 50.0 && ns[1] == 17.0,
            "W1: ...and the by-groups are the RESTRICTED sample (foreign 0/1 & mpg<30 = 50 + 17 "
            "= 67 in auto.dta), not the unrestricted 52 + 22");
    }

    // (6g) the seeded-RED arm end to end: the literal survives into the data.
    {
      auto blocks = piet::dispatch("gen str20 z = \"a, if b\"", session);
      check(firstError(blocks).empty(), "W1 seeded-RED (e2e): `gen str20 z = \"a, if b\"` runs");
      auto listBlocks = piet::dispatch("list z in 1/1", session);
      std::string cell;
      for (const auto& b : listBlocks) {
        if (b.is_object() && b.value("type", std::string()) == "browse") {
          const auto& rows = b.value("rows", nlohmann::ordered_json::array());
          if (!rows.empty() && !rows[0].empty()) cell = rows[0][0].get<std::string>();
        }
      }
      check(cell == "a, if b",
            "W1 seeded-RED (e2e): the stored string is the WHOLE literal, comma and `if` "
            "included — got '" + cell + "'");
    }

    // (6h) the trailing qualifier reaches the HANDLER as a qualifier. Until
    // BUG-1015 (99968638) xtile refused `if` in either spelling and this leg pinned
    // the two refusals as identical. xtile now cuts over the restricted sample, so
    // the leg pins the oracle: xtile|nq()_trailing_if is rc 0 with 22 of 74 rows
    // non-missing (pins_oracle_a.log:22-30), and the trailing spelling lands
    // on exactly the leading one's bins -- never the option net's "option `if` not
    // allowed".
    {
      auto lead = piet::dispatch("xtile qlead = price if foreign==1, nq(4)", session);
      auto trail = piet::dispatch("xtile qtrail = price, nq(4), if foreign==1", session);
      const std::string mLead = firstError(lead).value("message", std::string());
      const std::string mTrail = firstError(trail).value("message", std::string());
      check(mTrail.empty() && mLead.empty(),
            "W1: `xtile ..., nq(4), if foreign==1` and the leading spelling both run at rc 0 "
            "(pins_oracle_a.log:25 `B4REC|xtile|nq()_trailing_if|0|`) — got trailing='" +
                mTrail + "' leading='" + mLead + "'");
      auto countN = [&](const std::string& cond) -> double {
        (void)piet::dispatch("count if " + cond, session);
        const double* n = session.eval.rValues.find("N");
        return n != nullptr ? *n : -1.0;
      };
      const double nTrail = countN("qtrail < .");
      const double nDiff = countN("qtrail != qlead");
      check(nTrail == 22.0 && nDiff == 0.0,
            "W1: ...non-missing on exactly the 22 foreign rows of 74 (pins_oracle_a.log:30 "
            "`B4VAL|nonmissing|22|rows|74|`) and in the same bins as the leading spelling — got " +
                std::to_string(nTrail) + " non-missing, " + std::to_string(nDiff) + " differing");
    }
  }

  fs::remove_all(workdir, ec);
}


// W:P8 (BUG-174). `cd DIR` then `do "sibling.do"` must find the sibling. The
// pre-fix executeDo() used std::filesystem::absolute() and opened the raw path,
// both process-cwd relative, so this failed in exactly the case that matters —
// a GUI session, whose process cwd is wherever the launcher left it while
// session.cwd is the seeded ~/Documents.
// FULL FUNCTIONAL CHECK (2026-08-05). The prefix registry's drift guard:
// dispatch each declared prefix's probe and assert it does NOT fall through to
// the generic `unsupported_command` fallthrough. Modelled on
// testRegistryRoutes() — a declared prefix that no longer routes is a stale
// registry, which is exactly the failure mode a hand-kept list has.
//
// Deliberately weak on OUTCOME and strong on REACHABILITY: the probes run
// against a tiny synthetic dataset, so a probe may legitimately end in a
// domain error (too few observations for a replicate, say). What must never
// happen is PIET answering "Command `X` is not supported", because that means
// the registry claims a prefix the dispatcher does not have.
void testPrefixRegistryRoutes() {
  std::printf("[selftest] --- prefix registry <-> dispatcher route drift guard ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_prefixreg").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  const auto& prefixes = piet::registry::prefixRegistry();
  check(!prefixes.empty(), "prefix registry is non-empty");

  for (const auto& spec : prefixes) {
    piet::SessionState session(workdir);
    auto run = [&](const std::string& cmd) { return piet::dispatch(cmd, session); };
    run("clear");
    run("set obs 40");
    run("generate y = _n");
    run("generate grp = mod(_n, 4)");
    nlohmann::ordered_json res = run(spec.probe);

    bool unsupported = false;
    std::string firstMsg;
    for (const auto& b : res) {
      if (!b.is_object() || b.value("type", std::string()) != "error") continue;
      if (firstMsg.empty()) firstMsg = b.value("message", std::string());
      if (b.value("code", std::string()) == "unsupported_command") unsupported = true;
    }
    check(!unsupported,
          "prefix `" + spec.name + "` routes (probe: " + spec.probe + ")" +
              (unsupported ? " — got unsupported_command: " + firstMsg : ""));
  }
}


// ── BUG-105 abbreviation arc (ONESHOT-PREP9 WS-K, 2026-08-07) ────────────────
//
// THREE arms, because three different mistakes are possible and each has its
// own observable:
//
//   (a) TABLE DEFECT — two rows claiming the same spelling. Under a declared
//       set that is a table bug, not a runtime condition, so it is asserted at
//       construction rather than shipped as an "ambiguous prefix" reject that
//       could never fire (this replaces the parent row's DoD ④, whose premise
//       the live oracle removed).
//
//   (b) THE RESOLUTION POINT IS LOAD-BEARING, both ways. dispatcher.cpp
//       normalises the routed token through registry::resolveCanonical() once;
//       every arm then compares canonical spellings. Two-sided seeded-RED:
//         · POSITIVE half — spellings that exist ONLY in the registry (no
//           hardcoded literal anywhere in the if-chain) must route. Delete the
//           `cmd = registry::resolveCanonical(cmdTyped);` line and every one of
//           these flips to r(199). Verified by actually reverting that line:
//           27 FAIL, "ALL CHECKS PASSED" withheld.
//         · NEGATIVE half — sibling prefixes the live StataBE oracle measured
//           as REJECTED (`histo`, `scat`, `tabl`, `dr`, `re`, …) must stay
//           r(199). Replace the declared set with any computed prefix rule and
//           these flip to rc 0, which is fabricated parity — the exact defect
//           BUG-105 exists to close (internal notes BUG105-ABBREV-SOURCING-20260731 §2).
//       The negative half is the one that cannot be satisfied by accident:
//       Stata's accepted set is genuinely non-contiguous (`hist`✓ `histo`✗
//       `histogram`✓), so passing both halves at once is only possible for a
//       declared set.
//
//   (c) THE TWO REMOVALS. `r` was a fabricated abbreviation of `regress`
//       (measured r(199) in Stata) and must now be rejected. `logistic` was
//       mis-filed as an abbreviation of `logit`; it is a different command with
//       its own odds-ratio metric, so it must still route AND must still return
//       its own estimator label — the regression that would have shipped had it
//       been normalised to `logit`.
void testBUG105AbbrevArc() {
  std::printf("[selftest] --- BUG-105 abbreviation arc: one resolution point + declared sets ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug105_arc").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ---- (a) no spelling may appear on two rows -------------------------------
  {
    std::map<std::string, std::string> owner;  // spelling -> canonical that claims it
    std::vector<std::string> clashes;
    int spellings = 0;
    for (const auto& spec : piet::registry::commandRegistry()) {
      std::vector<std::string> all;
      all.push_back(spec.canonical);
      for (const auto& ab : spec.abbrev) all.push_back(ab);
      for (const auto& s : all) {
        ++spellings;
        auto it = owner.find(s);
        if (it != owner.end()) {
          clashes.push_back(s + " (claimed by both `" + it->second + "` and `" + spec.canonical + "`)");
        } else {
          owner[s] = spec.canonical;
        }
      }
    }
    check(clashes.empty(), "BUG-105(a): no spelling appears twice across all registry rows (" +
                               std::to_string(spellings) + " spellings checked" +
                               (clashes.empty() ? "" : "; clash: " + clashes.front()) + ")");
    // Lowercase-only is the contract every caller relies on (findByName and
    // resolveCanonical take an already-lowercased token).
    bool allLower = true;
    for (const auto& kv : owner) {
      for (char c : kv.first) {
        if (std::isupper(static_cast<unsigned char>(c)) != 0) allLower = false;
      }
    }
    check(allLower, "BUG-105(a): every registry spelling is lowercase (findByName's contract)");
  }

  // ---- (b) resolveCanonical maps every spelling; community names exempt ----
  {
    int mapped = 0, exempt = 0;
    bool ok = true;
    std::string firstBad;
    for (const auto& spec : piet::registry::commandRegistry()) {
      std::vector<std::string> all;
      all.push_back(spec.canonical);
      for (const auto& ab : spec.abbrev) all.push_back(ab);
      for (const auto& s : all) {
        bool isCommunity = false;
        for (const auto& cn : spec.communityNames) {
          if (cn == s) isCommunity = true;
        }
        const std::string want = isCommunity ? s : spec.canonical;
        const std::string got = piet::registry::resolveCanonical(s);
        if (got != want) {
          ok = false;
          if (firstBad.empty()) firstBad = s + " -> " + got + " (wanted " + want + ")";
        }
        if (isCommunity) ++exempt; else ++mapped;
      }
    }
    check(ok, "BUG-105(b): resolveCanonical maps every registry spelling to its row's canonical (" +
                  std::to_string(mapped) + " mapped, " + std::to_string(exempt) +
                  " community names returned unchanged)" +
                  (ok ? "" : " — first mismatch: " + firstBad));
    // An unregistered token must come back UNCHANGED — callers still compare it
    // against their own non-registry literals (by/bysort, the pre-tokenize
    // keywords), so a rewrite here would break routing rather than widen it.
    check(piet::registry::resolveCanonical("bysort") == "bysort" &&
              piet::registry::resolveCanonical("zzz_not_a_command") == "zzz_not_a_command",
          "BUG-105(b): an unregistered token passes through resolveCanonical unchanged");
    // reghdfe is the one spelling the community exemption actually reaches, and
    // it is load-bearing: handlers.cpp's runRegress reads `stataCmd ==
    // "reghdfe"` for the keepsingletons net, and the extcompat gate matches on
    // the typed name. Folding it to `feols` would kill both silently.
    check(piet::registry::resolveCanonical("reghdfe") == "reghdfe",
          "BUG-105(b): `reghdfe` is NOT folded to `feols` (community name, not an abbreviation)");
  }

  auto routeVerdict = [&](const std::string& line, const std::string& token) {
    // true == the line reached a real dispatcher arm; false == it fell through
    // to the generic unsupported_command r(199) fallback.
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch(line, session);
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error" &&
          b.value("code", std::string()) == "unsupported_command" &&
          b.value("token", std::string()) == token) {
        return false;
      }
    }
    return true;
  };

  // ---- (b) POSITIVE half: registry-only spellings must route ---------------
  // Every one of these exists ONLY as a registry entry — none appears as a
  // literal in any dispatcher if-arm, so each one routes solely because of the
  // single resolveCanonical() call. This is the list that goes RED (27 FAIL) if
  // that line is reverted.
  {
    static const std::vector<std::pair<std::string, std::string>> kRegistryOnly = {
        {"de", "de"},
        {"des", "des"},
        {"descri", "descri"},
        {"summ", "summ beertax"},
        {"summariz", "summariz beertax"},
        {"li", "li beertax in 1/2"},
        {"lis", "lis beertax in 1/2"},
        {"tabu", "tabu state"},
        {"tabulat", "tabulat state"},
        {"corre", "corre beertax income"},
        {"correlat", "correlat beertax income"},
        {"ge", "ge zz1 = 1"},
        {"generat", "generat zz2 = 1"},
        {"regr", "regr income beertax"},
        {"regres", "regres income beertax"},
        {"dis", "dis 2+2"},
        {"displa", "displa 2+2"},
        {"matr", "matr list ZZ"},
        {"matri", "matri list ZZ"},
        {"esti", "esti store m1"},
        {"la", "la variable beertax \"t\""},
        {"lab", "lab variable beertax \"t\""},
        {"labe", "labe variable beertax \"t\""},
        {"sa", "sa \"pg_scratch_dir/wsk_selftest_a.dta\", replace"},
        {"sav", "sav \"pg_scratch_dir/wsk_selftest_b.dta\", replace"},
        {"so", "so beertax"},
        {"sor", "sor beertax"},
        {"cou", "cou"},
        {"coun", "coun"},
        {"te", "te beertax"},
        {"tes", "tes beertax"},
        {"appen", "appen using \"pg_scratch_dir/wsk_selftest_a.dta\""},
        {"brow", "brow beertax in 1/2"},
        {"rena", "rena beertax bt_zz"},
        {"prob", "prob jail beertax"},
        {"probi", "probi jail beertax"},
        {"logi", "logi jail beertax"},
    };
    int routed = 0;
    std::vector<std::string> unrouted;
    for (const auto& [tok, line] : kRegistryOnly) {
      // Self-validating: if one of these ever becomes a hardcoded arm literal
      // or leaves the registry, the premise of this leg is gone and it must be
      // re-derived rather than silently passing.
      check(piet::registry::findByName(tok) != nullptr,
            "BUG-105(b) premise: `" + tok + "` is a registry spelling");
      if (routeVerdict(line, tok)) ++routed; else unrouted.push_back(tok);
    }
    check(unrouted.empty(),
          "BUG-105(b) POSITIVE: all " + std::to_string(kRegistryOnly.size()) +
              " registry-only spellings route through the ONE resolution point (" +
              std::to_string(routed) + " routed" +
              (unrouted.empty() ? "" : "; first unrouted: " + unrouted.front()) + ")");
  }

  // ---- (b) NEGATIVE half: measured-rejected siblings must stay r(199) ------
  // Straight off the live-oracle table. These are prefixes of commands PIET
  // routes, and real Stata answers r(111)/r(199) for every one — which is why
  // the policy is a declared set and NOT any computed prefix rule. If a future
  // change makes any of these run, PIET is fabricating parity.
  {
    static const std::vector<std::string> kMeasuredRejected = {
        "histo", "histog", "histogra",  // hist✓ histogram✓, the middle is not
        "scat",                          // sc✓ scatter✓, scat✗
        "tabl",                          // ta✓ tab✓ table✓, tabl✗
        "dr",   "dro",                   // drop is full-spelling-only
        "re",                            // regress starts at `reg`
        "r",                             // removal (c) — no longer an abbreviation
        "es",                            // est✓ estimates✓, es✗
        "hi",                            // h is an ado shim, hi is nothing
        "su1",                           // control: a spelling no rule would accept
    };
    int rejected = 0;
    std::vector<std::string> leaked;
    for (const auto& tok : kMeasuredRejected) {
      check(piet::registry::findByName(tok) == nullptr,
            "BUG-105(b) premise: `" + tok + "` is NOT a registry spelling");
      if (!routeVerdict(tok + " income beertax", tok)) ++rejected; else leaked.push_back(tok);
    }
    check(leaked.empty(),
          "BUG-105(b) NEGATIVE: all " + std::to_string(kMeasuredRejected.size()) +
              " oracle-REJECTED sibling spellings stay r(199) (" + std::to_string(rejected) +
              " rejected" + (leaked.empty() ? "" : "; leaked: " + leaked.front()) + ")");
  }

  // ---- (c) the two removals ------------------------------------------------
  {
    // `r` for regress: gone from the registry row AND from the arm literal.
    // Both halves matter — the registry edit alone would have left `cmd == "r"`
    // still matching in the if-chain.
    check(piet::registry::findByName("r") == nullptr,
          "BUG-105(c): `r` is no longer a registry spelling of regress");
    check(!routeVerdict("r income beertax", "r"),
          "BUG-105(c): `r income beertax` is r(199), not a regress fit (Stata measured r199)");
    check(routeVerdict("reg income beertax", "reg") && routeVerdict("regr income beertax", "regr"),
          "BUG-105(c): the MEASURED regress abbreviations (reg/regr/...) still route");

    // `logistic`: its own row, its own command, its own metric.
    const piet::registry::CommandSpec* logisticSpec = piet::registry::findByName("logistic");
    check(logisticSpec != nullptr && logisticSpec->canonical == "logistic",
          "BUG-105(c): `logistic` is its OWN registry row, not an abbrev of logit");
    const piet::registry::CommandSpec* logitSpec = piet::registry::findByName("logit");
    bool logisticStillAbbrev = false;
    if (logitSpec != nullptr) {
      for (const auto& ab : logitSpec->abbrev) {
        if (ab == "logistic") logisticStillAbbrev = true;
      }
    }
    check(!logisticStillAbbrev,
          "BUG-105(c): logit's abbrev list no longer claims `logistic`");
    check(piet::registry::resolveCanonical("logistic") == "logistic",
          "BUG-105(c): `logistic` does NOT normalise to `logit` (it would have killed BS-022's "
          "odds-ratio branch)");

    // The behavioural half: the odds-ratio transform is still reachable. Same
    // fixture BS-022's own arm uses, so this cannot pass by accident.
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("gen high = (beertax > .5)", session);
    auto blocks = piet::dispatch("logistic high income", session);
    const nlohmann::ordered_json* est = nullptr;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "estimation") est = &b;
    }
    check(est != nullptr && est->value("estimator", std::string()) == "logistic",
          "BUG-105(c): `logistic` still returns estimator \"logistic\" (odds-ratio metric intact)");
    auto blocksLogit = piet::dispatch("logi high income", session);
    const nlohmann::ordered_json* estLogi = nullptr;
    for (const auto& b : blocksLogit) {
      if (b.is_object() && b.value("type", std::string()) == "estimation") estLogi = &b;
    }
    check(estLogi != nullptr && estLogi->value("estimator", std::string()) == "logit",
          "BUG-105(c): the NEW `logi` abbreviation reaches logit, with logit's own estimator label");
  }


  // ---- (d) BUG-231 / OPTNET-ABBREV-SWEEP: the option-name resolution point --
  // Bidirectional per net, the BUG-148 fixture pattern: the abbreviation passes,
  // a typo is still rejected, and an unimplemented REAL Stata option still gets
  // the honest "valid Stata option, not yet implemented" wording.
  //
  // The first block is BUG-231's own regression and the reason this is a FIX and
  // not a widening: an abbreviated option already PASSED rejectUnknownOptions'
  // >=2-char prefix resolution while the handler's `k == "offset"` comparison
  // still saw the raw key, so the option was accepted and silently DROPPED.
  // `offs(pop)` returned the no-option fit (6.78117) instead of offset's
  // (-8861267.42121) with no diagnostic at all.
  {
    auto fitOf = [&](const std::string& line) {
      piet::SessionState session(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", session);
      (void)piet::dispatch("gen high = (beertax > .5)", session);
      auto blocks = piet::dispatch(line, session);
      std::string sig;
      for (const auto& b : blocks) {
        if (!b.is_object()) continue;
        if (b.value("type", std::string()) == "error") {
          sig += "ERR:" + b.value("code", std::string()) + "|" +
                 b.value("message", std::string()).substr(0, 40);
        } else if (b.value("type", std::string()) == "estimation" && b.contains("coeftable") &&
                   b["coeftable"].is_array()) {
          for (const auto& row : b["coeftable"]) {
            if (row.contains("b") && row["b"].is_number()) {
              char buf[64];
              std::snprintf(buf, sizeof(buf), "%.6f|", row["b"].get<double>());
              sig += buf;
            }
            if (row.contains("se") && row["se"].is_number()) {
              char buf[64];
              std::snprintf(buf, sizeof(buf), "se%.6f|", row["se"].get<double>());
              sig += buf;
            }
          }
        }
      }
      return sig;
    };

    // BUG-231: every accepted prefix must produce the FULL spelling's behaviour.
    const std::string offFull = fitOf("poisson fatal beertax, offset(pop)");
    const std::string noOpt = fitOf("poisson fatal beertax");
    check(!offFull.empty() && offFull != noOpt,
          "BUG-231 premise: poisson's offset() actually changes the fit");
    check(fitOf("poisson fatal beertax, offs(pop)") == offFull &&
              fitOf("poisson fatal beertax, off(pop)") == offFull,
          "BUG-231: `offs(pop)`/`off(pop)` give offset()'s fit, not the silently-dropped one");
    const std::string expFull = fitOf("poisson fatal beertax, exposure(pop)");
    check(expFull.find("ERR:") != std::string::npos,
          "BUG-231 premise: poisson's exposure() is an honest unimplemented reject");
    check(fitOf("poisson fatal beertax, expo(pop)") == expFull &&
              fitOf("poisson fatal beertax, exp(pop)") == expFull,
          "BUG-231: `expo`/`exp` reach the SAME honest reject, instead of running without it");

    // The nine measured minimums (audit §2), each with its intermediate prefixes.
    static const std::vector<std::pair<std::string, std::vector<std::string>>> kMinimums = {
        {"regress income beertax, level(90)",
         {"regress income beertax, l(90)", "regress income beertax, lev(90)",
          "regress income beertax, leve(90)"}},
        {"regress income beertax, robust",
         {"regress income beertax, r", "regress income beertax, robu"}},
        {"regress income beertax, cluster(state)",
         {"regress income beertax, cl(state)", "regress income beertax, clust(state)"}},
        {"summarize beertax, meanonly",
         {"summarize beertax, mean", "summarize beertax, meanonl"}},
        {"xtset state year, delta(1)", {"xtset state year, del(1)"}},
        // BUG-332: `noconstant` on regress. Measured against StataBE 19.5 by
        // the BUG-264 sweep: `noc`(3) rc=0, `nocon` rc=0, `nocons` rc=0.
        // Before the kOptMinimums row, PIET took only `nocons`/`noconstant` —
        // the net lists BOTH spellings, so every shorter prefix was ambiguous
        // under the unique-prefix rule and got rejected. Seeded-RED proof: drop
        // the {"noconstant", 3} row from dispatcher.cpp's regress entry and the
        // two short legs below FAIL while everything else stays green.
        {"regress income beertax, noconstant",
         {"regress income beertax, noc", "regress income beertax, nocon",
          "regress income beertax, nocons"}},
        // BUG-334: same class, on commands the BUG-264 manifest never swept.
        // `logit ..., r` is the everyday spelling for robust SEs and PIET
        // demanded all six characters. Stata minimums measured live on the
        // shared fatalities fixture: robust 1 (logit/logistic/xtreg),
        // noconstant 5 (logit/probit).
        {"logit jail beertax, robust", {"logit jail beertax, r"}},
        {"logistic jail beertax, robust", {"logistic jail beertax, r"}},
        {"logit jail beertax, noconstant", {"logit jail beertax, nocon"}},
        {"probit jail beertax, noconstant", {"probit jail beertax, nocon"}},
    };
    for (const auto& [full, abbrevs] : kMinimums) {
      const std::string want = fitOf(full);
      for (const auto& ab : abbrevs) {
        check(fitOf(ab) == want,
              "BUG-105 optnet: `" + ab.substr(ab.find(',') + 2) + "` == `" +
                  full.substr(full.find(',') + 2) + "`");
      }
    }

    // BUG-105's LAST open item, measured 2026-08-17. Its 2026-07-31 audit left
    // six commands at exact-match with the note "same code shape, likely, not
    // individually re-verified"; five turned out to be covered by the BUG-264
    // sweep, and `xtreg, cluster()` was the one real hole — `cl(state)` answered
    // ``option `cl' not allowed``. StataBE 19.5 (webuse nlswork, `xtreg ln_wage
    // age, fe`) adjudicated by VALUE, not by rc, because rc 0 does not prove the
    // prefix bound to THIS option: _se[age] is .00034771 unclustered and
    // .00060992 under cluster(idcode), and cluster/clu/cl/c all reproduce
    // .00060992 ⇒ minimum 1.
    //
    // NOT written through fitOf: that helper rebuilds the session with a bare
    // `sysuse`, so `xtreg ..., fe` would answer "run xtset first" on EVERY arm
    // and the comparison would pass vacuously with all arms equally broken.
    // This block xtsets, and asserts on the SE — the value the option actually
    // moves — with the unclustered SE as the discriminator, so an arm that
    // silently ignored the option could not pass either.
    {
      auto seOf = [&](const std::string& line) -> std::optional<double> {
        piet::SessionState s(workdir);
        (void)piet::dispatch("sysuse fatalities, clear", s);
        (void)piet::dispatch("egen sid = group(state)", s);
        (void)piet::dispatch("xtset sid year", s);
        for (const auto& b : piet::dispatch(line, s)) {
          if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
          if (!b.contains("coeftable")) continue;
          for (const auto& row : b["coeftable"])
            if (row.value("name", std::string()) == "beertax" && row.contains("se") &&
                row["se"].is_number())
              return row["se"].get<double>();
        }
        return std::nullopt;
      };
      auto plain = seOf("xtreg fatal beertax, fe");
      auto full = seOf("xtreg fatal beertax, fe cluster(sid)");
      if (!plain.has_value() || !full.has_value()) {
        skip("BUG-105: xtreg cluster() minimum",
             "no xtreg estimation block — no frames-capable `cobra serve` here");
      } else {
        check(*plain != *full,
              "BUG-105 precondition: cluster(sid) MOVES the SE (" + std::to_string(*plain) +
                  " -> " + std::to_string(*full) + ") — without this the arms below could pass "
                  "while doing nothing");
        for (const char* ab : {"cl", "clu", "c"}) {
          auto got = seOf(std::string("xtreg fatal beertax, fe ") + ab + "(sid)");
          check(got.has_value() && *got == *full,
                std::string("BUG-105 optnet: `xtreg, ") + ab + "(sid)` clusters exactly like "
                "`cluster(sid)` (Stata minimum 1)");
        }
      }
    }

    // BUG-336: four estimation commands consumed their real options but never
  // rejected UNKNOWN ones, so a typo was accepted and the model ran WITHOUT it.
  // These legs pin BOTH halves — the guard must reject garbage AND must not eat
  // the options the handler genuinely honours. The second half is the one that
  // matters: the first attempt at this fix used an empty known-set, which ate
  // vce()/instruments() and broke 14 freeze fixtures.
  {
    const std::string bogus = fitOf("mixed beertax unemp || state:, zzqqx");
    check(bogus.find("ERR:unsupported_option") != std::string::npos,
          "BUG-336: `mixed` names a bogus option instead of silently dropping it");
    const std::string real = fitOf("mixed beertax unemp || state:, vce(robust)");
    check(real.find("ERR:unsupported_option") == std::string::npos,
          "BUG-336: `mixed, vce(robust)` survives the guard — real options must still reach it");
    // BUG-333 SUPERSEDES the original form of this leg. When BUG-336 landed,
    // `mixed, level()` was promoted from a SILENT DROP to an honest
    // unimplemented-reject, and this leg pinned that. BUG-333 then measured that
    // cobra's mixed has had `--level` all along (options_table.cpp:116) — PIET
    // simply never forwarded it — so the honest reject was itself the interim
    // state, not the destination. The leg now pins the destination: it runs, and
    // it MOVES the interval.
    // NOTE the discriminator: level() cannot move `b`, only the interval, and
    // fitOf's signature is built from `b` alone — so comparing fitOf strings here
    // would have compared two identical strings and "passed" for the wrong reason
    // on a broken build. This reads ci_lo directly.
    auto ciLoOf = [&](const std::string& line) {
      piet::SessionState ms(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", ms);
      double lo = std::nan("");
      for (const auto& b : piet::dispatch(line, ms)) {
        if (!b.is_object() || !b.contains("coeftable") || !b["coeftable"].is_array()) continue;
        if (!b["coeftable"].empty() && b["coeftable"][0].contains("ci_lo") &&
            b["coeftable"][0]["ci_lo"].is_number())
          lo = b["coeftable"][0]["ci_lo"].get<double>();
      }
      return lo;
    };
    const std::string lv90sig = fitOf("mixed beertax unemp || state:, vce(robust) level(90)");
    check(lv90sig.find("ERR:unsupported_option") == std::string::npos,
          "BUG-333: `mixed, level()` RUNS (superseding BUG-336's interim honest-reject leg)");
    const double lo95 = ciLoOf("mixed beertax unemp || state:, vce(robust)");
    const double lo90 = ciLoOf("mixed beertax unemp || state:, vce(robust) level(90)");
    check(std::isfinite(lo95) && std::isfinite(lo90) && std::fabs(lo95 - lo90) > 1e-12,
          "BUG-333: and mixed's level(90) MOVES ci_lo — accepted-but-ignored would be the "
          "silent drop wearing a fix's clothes");
    const std::string fr = fitOf("fracreg logit emppop beertax, vce(robust)");
    check(fr.find("ERR:unsupported_option") == std::string::npos,
          "BUG-336: `fracreg, vce(robust)` survives the guard");
  }

  // BUG-204's remaining half: `estat` after a non-OLS fit. These diagnostics are
  // least-squares constructs, so the varlist FALLBACK would refit OLS behind an ML
  // model and answer a different question at rc 0 — the same shape as BUG-203/297's
  // `predict`, whose half is already closed. Measured before the fix: `estat hettest`
  // after `logit` returned F .0064338 / chi2 .0025107; Stata rejects it outright.
  {
    piet::SessionState es(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", es);
    (void)piet::dispatch("gen high = (beertax > .5)", es);
    (void)piet::dispatch("logit high income", es);
    bool mlRejected = false;
    for (const auto& b : piet::dispatch("estat hettest", es))
      if (b.is_object() && b.value("type", std::string()) == "error") mlRejected = true;
    check(mlRejected, "BUG-204: `estat hettest` after an ML fit is an honest reject, not an OLS "
                      "refit smuggled in behind the model");
    piet::SessionState os(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", os);
    (void)piet::dispatch("regress fatal beertax", os);
    bool olsRan = true;
    for (const auto& b : piet::dispatch("estat hettest", os))
      if (b.is_object() && b.value("type", std::string()) == "error") olsRan = false;
    check(olsRan, "BUG-204: and after OLS it still runs — the guard is on the fitted model, not "
                  "on the diagnostic");
  }

  // BUG-264/BUG-335 THE NARROWING FLIP. For a command whose whole vocabulary is
  // oracle-adjudicated, PIET's accepted spelling set is now exactly Stata's:
  // {prefix p : |p| >= measured minimum}. `save filename, r` silently meaning
  // `replace` was the headline hazard — a typo overwriting a file.
  //
  // These legs pin BOTH directions, because a flip that only rejects is just a
  // regression: the measured minimum must still RUN.
  {
    check(fitOf("save /tmp/piet_flip_probe.dta, r").find("ERR:") != std::string::npos,
          "BUG-335: `save, r` is REJECTED — Stata requires all seven characters of `replace`");
    check(fitOf("save /tmp/piet_flip_probe.dta, replace").find("ERR:") == std::string::npos,
          "BUG-335: and the full spelling still runs");
    check(fitOf("ttest fatal, by(high) re").find("ERR:") != std::string::npos,
          "BUG-264: `ttest, re` is rejected (measured minimum for `reverse` is 3)");
    check(fitOf("ttest fatal, by(high) rev").find("ERR:") == std::string::npos,
          "BUG-264: `rev` — the measured minimum itself — still runs");
    check(fitOf("summarize beertax, mea").find("ERR:") != std::string::npos,
          "BUG-264: `summarize, mea` is rejected (meanonly's measured minimum is 4)");
    check(fitOf("summarize beertax, mean").find("ERR:") == std::string::npos,
          "BUG-264: `mean` runs");
    // The other half of the campaign's law: a command NOT fully adjudicated keeps
    // the unique-prefix fallback, because narrowing on an incomplete measurement
    // is the BUG-148 failure this work has been careful not to re-commit.
    check(fitOf("tostring fatal, gener(zzs)").find("ERR:") == std::string::npos,
          "BUG-264: an INCOMPLETE command keeps the prefix fallback — `tostring, gener()` still "
          "resolves, because its generate row is withheld and the table is not a complete "
          "statement for it");
  }

  // BUG-336 (graph arm): nine command tokens shared one arm with no unknown-option
  // net. These legs pin both directions AND the ordering that four older legs
  // caught me getting wrong: a command-level gap must still be named as a command
  // gap, not re-reported as an option problem.
  {
    check(fitOf("scatter fatal beertax, zzqqx").find("ERR:unsupported_option") != std::string::npos,
          "BUG-336: `scatter` names a bogus option instead of dropping it");
    check(fitOf("histogram beertax, zzqqx(3)").find("ERR:unsupported_option") != std::string::npos,
          "BUG-336: `histogram` too");
    // The corpus vocabulary must survive — these four are live in pangram/p5/p8.
    check(fitOf("histogram beertax, normal name(h1, replace)").find("ERR:unsupported_option") ==
              std::string::npos,
          "BUG-336: histogram's normal/name survive the net (pangram + p5 + p8 use them)");
    check(fitOf("scatter fatal beertax, title(\"t\") note(\"n\")").find("ERR:unsupported_option") ==
              std::string::npos,
          "BUG-336: the title/note family survives");
    check(fitOf("kdensity beertax, kernel(gaussian) bwidth(1)").find("ERR:unsupported_option") ==
              std::string::npos,
          "BUG-336: kdensity's own reads survive");
    // AER scope-2 W4 RE-ADJUDICATED this leg, and the half that mattered is kept.
    // BUG-336's claim was "not silently dropped"; it enforced that with a REJECT,
    // because a note had nowhere to go at the time. W4 gives it somewhere to go:
    // a purely cosmetic option is now accepted and NAMED in the block's own note
    // (dispatcher.cpp's graphAcceptedNotApplied), which honours the same claim
    // without failing a statement over a colour scheme. So the leg now pins BOTH
    // halves that BUG-336's single check was standing in for -- accepted, and
    // still not silent -- and the reject direction is pinned on `aspectratio`,
    // which stays in the unimplemented half.
    check(fitOf("scatter fatal beertax, scheme(s2color)").find("ERR:") == std::string::npos,
          "W4: a cosmetic graph option is ACCEPTED (was r(198) under BUG-336's reject-only rule)");
    // serset-twoway VOC: aspectratio is drawn at rc 0 by Stata (w2al tf_wt_077a,
    // log:937), so the unimplemented direction is pinned on by(), which the
    // single-plot arm still does not facet.
    check(fitOf("scatter fatal beertax, by(state)").find("ERR:unsupported_option") !=
              std::string::npos,
          "W4: a real Stata graph option PIET does not implement still says so "
          "(unimplemented), rather than being silently dropped");
    // ORDERING: `graph pie` is a COMMAND gap (graph bar, this leg's first
    // subject, is implemented: graph-family AI-1). If the option net ran first
    // this would answer 'option over not allowed' — the bug this leg catches.
    const std::string barSig = fitOf("graph pie beertax, over(state)");
    check(barSig.find("ERR:unsupported_option") == std::string::npos,
          "BUG-336: `graph pie, over(...)` is still reported as a COMMAND gap, not an option "
          "problem — the option net must run AFTER the unsupported-plot-type reject");
  }

  // BUG-333: the seven options Stata implements and PIET used to reject outright.
  // Six are now implemented; `test`'s accumulate family is an honest
  // unimplemented-reject. Each leg pins the BEHAVIOUR, not the wording.
  {
    const std::string orSig = fitOf("logit high income, or");
    check(orSig.find("ERR:") == std::string::npos,
          "BUG-333: `logit, or` runs (odds-ratio display metric)");
    const std::string plainSig = fitOf("logit high income");
    check(plainSig != orSig,
          "BUG-333: and `or` actually CHANGES the table — a no-op accept would be the "
          "silent-drop defect wearing a fix's clothes");
    check(fitOf("probit high income, or").find("ERR:unsupported_option") != std::string::npos,
          "BUG-333: `or` stays rejected on probit — the accept is logit-gated, not global "
          "(Stata's probit has no or)");
    check(fitOf("logit high income, iterate(2)").find("ERR:") == std::string::npos,
          "BUG-333: `logit, iterate(#)` runs");
    check(fitOf("logit high income, iterate(0)").find("ERR:") == std::string::npos,
          "BUG-1244: iterate(0) runs and reports the starting values (StataBE e(ic) 0; it was "
          "refused by the engine's old [1, 16000] range)");
    // lincom/nlcom need a prior fit IN THE SAME session — fitOf builds a fresh
    // one per call, so these use their own session and a leading regress.
    auto postestSig = [&](const std::string& line) {
      piet::SessionState ps(workdir);
      (void)piet::dispatch("sysuse fatalities, clear", ps);
      (void)piet::dispatch("regress income beertax unemp", ps);
      std::string sig;
      for (const auto& b : piet::dispatch(line, ps)) {
        if (!b.is_object()) continue;
        if (b.value("type", std::string()) == "error")
          sig += "ERR:" + b.value("code", std::string());
        else if (b.contains("coeftable") && b["coeftable"].is_array() &&
                 !b["coeftable"].empty() && b["coeftable"][0].contains("ci_lo") &&
                 b["coeftable"][0]["ci_lo"].is_number()) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "lo=%.9f", b["coeftable"][0]["ci_lo"].get<double>());
          sig += buf;
        }
      }
      return sig;
    };
    const std::string ci95 = postestSig("lincom beertax");
    const std::string ci90 = postestSig("lincom beertax, level(90)");
    check(ci90.find("ERR:") == std::string::npos,
          "BUG-333: `lincom, level(#)` runs (engine --level landed this wave)");
    check(!ci95.empty() && ci95 != ci90,
          "BUG-333: and level(90) MOVES the interval — an accepted-but-ignored level is the "
          "silent-drop defect, not a fix");
    check(postestSig("lincom beertax, level(200)").find("ERR:") != std::string::npos,
          "BUG-333: an out-of-range level is refused, by the engine's [10,99.99] rule");
    check(postestSig("nlcom _b[beertax]/_b[unemp], level(90)").find("ERR:") == std::string::npos,
          "BUG-333: `nlcom, level(#)` runs");
    check(postestSig("lincom beertax, eform").find("ERR:") == std::string::npos,
          "BUG-333: `lincom, eform` runs (Stata allows it even after regress — measured)");
    check(postestSig("lincom beertax, or hr").find("ERR:unsupported_option") != std::string::npos,
          "BUG-333: two eform-family options together is a reject, matching Stata's r(198)");
    check(fitOf("summarize beertax, separator(2)").find("ERR:") == std::string::npos,
          "BUG-333: `summarize, separator(#)` runs");
    check(fitOf("summarize beertax, separator(x)").find("ERR:") != std::string::npos,
          "BUG-333: separator(x) is refused — Stata answers r(198) to a non-numeric argument");
    check(fitOf("test income, notest").find("ERR:unsupported_option") != std::string::npos,
          "BUG-333: `test, notest` is an HONEST unimplemented-reject, not a silent drop");
  }

  // A typo must still be a named reject — the sweep is one-sided by
    // construction and must not have widened into a spell-checker.
    static const std::vector<std::string> kTypos = {
        "regress income beertax, robsut", "regress income beertax, levl(90)",
        "poisson fatal beertax, ofset(pop)", "regress income beertax, clyster(state)",
        // BUG-332's boundary, and the reason the row is 3 and not lower:
        // StataBE 19.5 answers r(198) to `regress price mpg, no` — `no` is
        // ambiguous there across noconstant/noheader/notable. Widening
        // `noconstant` must not swallow it. (Stata does accept a bare `n`,
        // non-monotonically; that oddity is recorded in the campaign doc and
        // deliberately NOT reproduced — an unexplained accept is not a spec.)
        "regress income beertax, no"};
    for (const auto& t : kTypos) {
      const std::string sig = fitOf(t);
      check(sig.find("ERR:unsupported_option") != std::string::npos,
            "BUG-105 optnet: typo `" + t.substr(t.find(',') + 2) + "` is still rejected");
    }

    // An unimplemented REAL Stata option keeps the honest wording (BUG-148).
    check(fitOf("regress income beertax, beta").find("valid Stata option") != std::string::npos,
          "BUG-105 optnet: an unimplemented real option still gets the honest message");
  }

  fs::remove_all(workdir, ec);
}


// BUG-233 (residual of BUG-231): the option-net abbreviation sweep, and its
// guard. BUG-231 was ONE net measured; the mechanism was live in every net whose
// `known` set is non-empty (57 of 77 rejectUnknownOptions call sites), because
// eval::rejectUnknownOptions accepts a >=2-char unique prefix while the
// handler's own `k == "fullname"` comparisons still see the raw key. Accepted,
// unread, gone — a different answer under a command line that looks correct.
//
// Measured accept-then-drop before the fix, three files, three shapes:
//   winsor2 fatal, cuts(5 95) -> mean 864.4791666666666   (dispatcher.cpp)
//   winsor2 fatal, cut(5 95)  -> mean 925.5386904761905   == no option, WRONG
//   ttest fatal, by(grp) unequal -> se 103.88516730576413 (estimation_m4.cpp)
//   ttest fatal, by(grp) uneq    -> se 103.0699932930575  == no option, WRONG
//   esttab m1, nostar -> stars ""                         (eststo_esttab.cpp)
//   esttab m1, nosta  -> stars "***"                      == no option, WRONG
//
// Bidirectional per net, the BUG-148 fixture pattern: the abbreviation now works
// · a typo is still rejected · an unimplemented-but-real option still gets the
// honest message. Plus the case the sweep must NOT "fix": two options sharing a
// prefix are not unique, so they stay rejected.
void testBUG233OptNetVocabularySweep() {
  std::printf("[selftest] --- BUG-233 option-net vocabulary sweep + regrowth guard ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug233_optnet").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Observable signature of a command (plus an optional readout line, for the
  // nets whose effect is a mutated column or a set local rather than a block).
  // data_mutation/echo are dropped: they carry the whole varlist and would make
  // every comparison a whole-schema diff rather than a behaviour diff.
  auto sigOf = [&](const std::vector<std::string>& setup, const std::string& cmd,
                   const std::string& readout) -> std::string {
    piet::SessionState session(workdir);
    for (const auto& s : setup) (void)piet::dispatch(s, session);
    std::string sig;
    auto acc = [&sig](const nlohmann::ordered_json& blocks) {
      for (const auto& b : blocks) {
        if (!b.is_object()) continue;
        const std::string t = b.value("type", std::string());
        if (t == "data_mutation" || t == "echo") continue;
        if (t == "error") {
          sig += "ERR|" + b.value("message", std::string()).substr(0, 60) + "|";
        } else {
          sig += b.dump() + "|";
        }
      }
    };
    acc(piet::dispatch(cmd, session));
    if (!readout.empty()) acc(piet::dispatch(readout, session));
    return sig;
  };

  static const std::vector<std::string> kFatal = {"sysuse fatalities, clear"};
  static const std::vector<std::string> kGrp = {"sysuse fatalities, clear",
                                                 "gen byte grp = (year > 1985)"};
  static const std::vector<std::string> kEst = {"sysuse fatalities, clear",
                                                 "quietly regress fatal beertax unemp",
                                                 "eststo m1"};
  struct Leg {
    const std::vector<std::string>* setup;
    std::string full;      // the canonical spelling
    std::string abbrev;    // an abbreviation the reject net already accepted
    std::string readout;   // "" when the command's own blocks carry the answer
    std::string label;
  };
  // One leg per FILE the sweep touched, so a per-file regression cannot hide,
  // plus the two nets whose defect was measured live.
  static const std::vector<Leg> kLegs = {
      {&kFatal, "winsor2 fatal, cuts(5 95)", "winsor2 fatal, cut(5 95)",
       "summarize fatal_w", "dispatcher.cpp winsor2 cuts()"},
      {&kFatal, "levelsof year, local(yy)", "levelsof year, loc(yy)",
       "display \"`yy'\"", "dispatcher.cpp levelsof local()"},
      {&kFatal, "tabstat fatal, statistics(mean sd)", "tabstat fatal, stati(mean sd)", "",
       "dispatcher.cpp tabstat statistics()"},
      {&kGrp, "ttest fatal, by(grp) unequal", "ttest fatal, by(grp) uneq", "",
       "estimation_m4.cpp ttest unequal"},
      {&kFatal, "qreg fatal beertax, quantile(.25)", "qreg fatal beertax, quant(.25)", "",
       "estimation_m4.cpp qreg quantile()"},
      {&kEst, "esttab m1, nostar", "esttab m1, nosta", "", "eststo_esttab.cpp esttab nostar"},
      {&kFatal, "tostring year, generate(ys)", "tostring year, gener(ys)",
       "summarize beertax", "dispatcher.cpp tostring generate()"},
  };

  // The guard is armed across every abbreviation leg at once: any late
  // resolution anywhere in this block means some net is not covered.
  piet::eval::resetOptNetLateResolutionCount();

  for (const auto& leg : kLegs) {
    const std::string want = sigOf(*leg.setup, leg.full, leg.readout);
    const std::string got = sigOf(*leg.setup, leg.abbrev, leg.readout);
    check(!want.empty(), "BUG-233 premise: `" + leg.full + "` produces an observable result");
    check(got == want, "BUG-233 " + leg.label + ": the abbreviation equals the full spelling");
  }

  // The premise the whole entry rests on: for at least one leg the dropped
  // option genuinely changes the answer, so "equal to full" is not vacuously
  // "equal to no-option too".
  {
    const std::string full = sigOf(kFatal, "winsor2 fatal, cuts(5 95)", "summarize fatal_w");
    const std::string none = sigOf(kFatal, "winsor2 fatal", "summarize fatal_w");
    check(full != none, "BUG-233 premise: winsor2 cuts() actually changes the winsorized column");
    const std::string uneq = sigOf(kGrp, "ttest fatal, by(grp) unequal", "");
    const std::string pooled = sigOf(kGrp, "ttest fatal, by(grp)", "");
    check(uneq != pooled, "BUG-233 premise: ttest unequal actually changes the SE and df");
  }

  // --- the other two thirds of the BUG-148 triple ---------------------------
  static const std::vector<std::pair<std::string, const std::vector<std::string>*>> kTypos = {
      {"winsor2 fatal, cutz(5 95)", &kFatal},
      {"ttest fatal, by(grp) uneqal", &kGrp},
      {"tabstat fatal, statz(mean)", &kFatal},
      {"levelsof year, lokal(yy)", &kFatal},
  };
  for (const auto& [line, setup] : kTypos) {
    check(sigOf(*setup, line, "").find("ERR|") != std::string::npos,
          "BUG-233: the typo `" + line + "` is still a named reject");
  }
  check(sigOf(kFatal, "poisson fatal beertax, expo(pop)", "").find("valid Stata option") !=
            std::string::npos,
        "BUG-233: an abbreviated unimplemented-but-real option keeps the honest message");

  // --- the case the sweep must NOT resolve ----------------------------------
  // Two options in one net sharing a prefix: `sta` prefixes tabstat's stat,
  // statistics AND stats; `un` prefixes ttest's unequal AND unpaired. Not
  // unique, therefore not resolved, therefore still rejected — in Stata too.
  // BUG-330 INVERTS this leg. It used to assert that `sta` stays rejected, with
  // the reason "not unique, therefore not resolved, therefore still rejected —
  // in Stata too". The last four words were measured FALSE: StataBE 19.5 takes
  // `tabstat price, s(mean)` and every longer prefix at rc 0. The ambiguity was
  // PIET's own manufacture — its net listed `stat` (an abbreviation) beside the
  // two real spellings `stats`/`statistics`. It now resolves via the measured
  // minimums table, and the typo pin below still holds the other side.
  check(sigOf(kFatal, "tabstat fatal, sta(mean sd)", "").find("ERR|") == std::string::npos,
        "BUG-330: `sta` RESOLVES (Stata takes it, and `s`, down to one char)");
  check(sigOf(kFatal, "tabstat fatal, s(mean sd)", "").find("ERR|") == std::string::npos,
        "BUG-330: so does one-char `s` — the measured Stata minimum");
  check(sigOf(kFatal, "tabstat fatal, stats(mean sd)", "").find("ERR|") == std::string::npos,
        "BUG-330: and `stats` still works — it is NOT a prefix of `statistics`, so the two "
        "are two real spellings and the net is right to carry both");
  check(sigOf(kGrp, "ttest fatal, by(grp) un", "").find("ERR|") != std::string::npos,
        "BUG-233: `un` is ambiguous across unequal/unpaired and stays rejected");

  // --- BUG-262: the one net the shared point structurally cannot reach ------
  // runLog re-parses its options out of the raw line instead of reading
  // parsed.opts, so the dispatcher's resolution point never touches its vector.
  // `repl` was accepted and dropped, and the log was not replaced.
  {
    const std::string logPath = (fs::path(workdir) / "bug262_log.log").u8string();
    for (const std::string& spelling : {std::string("replace"), std::string("repl")}) {
      fs::remove(logPath, ec);
      { std::ofstream pre(logPath); pre << "PRE-EXISTING\n"; }
      piet::SessionState session(workdir);
      (void)piet::dispatch("log using \"" + logPath + "\", " + spelling, session);
      (void)piet::dispatch("log close", session);
      std::ifstream in(logPath);
      std::string first;
      std::getline(in, first);
      check(first != "PRE-EXISTING",
            "BUG-262: `log using ..., " + spelling + "` really truncates the existing file "
            "(the abbreviation used to be accepted and then dropped)");
    }
    fs::remove(logPath, ec);
  }

  // --- an exact spelling is never an abbreviation of anything ---------------
  // Found by the freeze matrix, not by this file: did_imputation carries
  // `autosample` in its implemented vocabulary and `autosample_off` in its
  // unimplemented one, so a resolution step that consults the second whenever
  // the first was a no-op rewrites the WORKING full spelling into the
  // unimplemented sibling and rejects a command that used to run. The pinned
  // freeze row went RED on exactly this. Kept here so the next change to the
  // step order fails in one second instead of at the freeze gate.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("use \"" + (fs::path("tests") / "kitchensink" / "kitchensink_panel.dta").u8string() + "\", clear", session);
    auto blocks = piet::dispatch(
        "did_imputation y id year cohort, horizons(0/3) pretrends(2) autosample", session);
    bool sawAutosampleOffReject = false;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error" &&
          b.value("message", std::string()).find("autosample_off") != std::string::npos) {
        sawAutosampleOffReject = true;
      }
    }
    check(!sawAutosampleOffReject,
          "BUG-233: the exact spelling `autosample` is NOT re-resolved into its unimplemented "
          "sibling `autosample_off`");
  }

  // --- THE GUARD ------------------------------------------------------------
  // eval::rejectUnknownOptions counts every key it has to resolve ITSELF, which
  // can only happen when the dispatcher's resolution point did not cover that
  // net. Across every leg above the count must be zero. Falsification: delete
  // any one registerOptVocab() call and its leg's key arrives raw, the count
  // goes non-zero, and this check names the offender.
  check(piet::eval::optNetLateResolutionCount() == 0,
        "BUG-233 GUARD: no net resolved an abbreviation itself (late resolution means that "
        "net's vocabulary is unregistered); last seen: " +
            (piet::eval::optNetLastLateResolution().empty()
                 ? std::string("none")
                 : piet::eval::optNetLastLateResolution()));

  // The inventory, so a registration silently disappearing is also caught — the
  // guard above only fires for nets a leg actually exercises.
  {
    const std::vector<std::string> cmds = piet::eval::registeredOptVocabCommands();
    // Floor raised 60 -> 209 by the AER scope-2 W2 sweep (measured on this
    // build: 160 tokens before it, 209 after). A floor, so growth is fine and
    // shrinkage is the failure — which is exactly what a registration silently
    // disappearing looks like.
    check(cmds.size() >= 209,
          "BUG-233: the vocabulary registry still carries the swept nets (>=209 command tokens), "
          "got " + std::to_string(cmds.size()));
    static const std::vector<std::string> kMustBeRegistered = {
        "winsor2", "levelsof", "tabstat", "ttest",  "qreg",  "esttab",
        "tostring", "poisson", "nbreg",   "merge",  "egen",  "reg3",
        "csdid",   "rdrobust", "log",     "order",  "mean",  "estat",
        // AER scope-2 W2. Every command the 12-paper census reported as
        // `no_vocabulary` and this build actually implements, pinned by NAME:
        // the count floor above cannot tell which token went missing, and a
        // dropped registration takes its command straight back to unmeasured.
        "codebook", "describe", "rename",  "generate", "sort",     "gsort",
        "compress", "recode",   "list",    "label",    "labelbook", "encode",
        "expand",   "joinby",   "import",  "export",   "nlcom",    "nl",
        "gmm",      "mixed",    "ivprobit", "ivtobit", "fracreg",
        "regress",  "feols",    "xtreg",   "summarize", "save",    "predict",
        "estimates", "eststo",  "ivreg2",  "xtivreg2", "tsfilter",
        "collapse", "file",     "net",     "ssc",      "set",      "restore",
        // the BUG-336 graph arm's nine tokens (W2 registers, W4 implements)
        "scatter",  "line",     "tsline",  "histogram", "hist",    "kdensity",
        "xtline",   "twoway",   "graph",
        // W4's own. `twoway_layer` is NOT a command: runTwowayLayers reads its
        // net's vocabulary out of the registry under that token, so if the
        // registration ever disappears the net silently stops running (a
        // fail-open, which is the shape a gate must never be left in). This row
        // is what makes that disappearance a RED instead of a quiet hole. The
        // plot-type heads beside it are what the option census resolves a `||`
        // chain segment against.
        "twoway_layer", "connected", "lfit",  "rarea",  "rcap",   "area",
        "function",     "coefplot",  "marginsplot", "binscatter"};
    for (const auto& c : kMustBeRegistered) {
      check(piet::eval::findOptVocab(c) != nullptr,
            "BUG-233: `" + c + "`'s option vocabulary is registered at the resolution point");
    }
  }

  fs::remove_all(workdir, ec);
}


// AER scope-2 A1 — the drift guard for the two lexical dumps. piet_selftest's
// main() takes no argv and cannot exec piet_native, so these assert on the SAME
// in-process builders the flags print (registry::optVocabDumpJson /
// functionDumpJson). That is also what makes the payload check sharp: break the
// serializer and this leg goes RED, not just the CLI.
//
// READ-ONLY on purpose. Nothing here pushes a key through
// eval::rejectUnknownOptions, which would bump optNetLateResolutionCount() and
// turn testBUG233OptNetVocabularySweep's guard RED from an unrelated leg.
void testAerA1VocabDumps() {
  std::printf("[selftest] --- AER scope-2 A1: --dump-optvocab / --dump-functions ---\n");

  const nlohmann::json vocab = nlohmann::json::parse(piet::registry::optVocabDumpJson());
  const size_t live = piet::eval::registeredOptVocabCommands().size();
  check(vocab.is_array() && vocab.size() == live,
        "A1: --dump-optvocab serializes every registered command (" + std::to_string(live) +
            " live, " + std::to_string(vocab.size()) + " emitted)");

  // The one aliases-bearing net in the tree (handlers_aer_graphx.cpp's geonear:
  // `n` prefixes BOTH neighbors and nearcount, so only geonear.ado's own
  // declaration can bind it). Asserted THROUGH the serializer, so an aliases
  // map that quietly stops being emitted fails here and not only in the CLI.
  std::string geonearN;
  for (const auto& row : vocab) {
    if (row.value("command", std::string()) != "geonear") continue;
    geonearN = row.value("aliases", nlohmann::json::object()).value("n", std::string());
  }
  check(geonearN == "neighbors",
        "A1: --dump-optvocab carries geonear's declared alias n -> neighbors (got \"" + geonearN +
            "\")");

  const nlohmann::json fns = nlohmann::json::parse(piet::registry::functionDumpJson());
  std::set<std::string> layers;
  size_t nonCi = 0;
  for (const auto& row : fns) {
    const std::string layer = row.value("layer", std::string());
    layers.insert(layer);
    if (layer != "ciparser") ++nonCi;
  }
  check(layers.count("exprfns") != 0 && layers.count("pyeval") != 0 &&
            layers.count("ciparser") != 0 && nonCi >= 44,
        "A1: --dump-functions names all three layers and reports >= 44 map-backed functions (" +
            std::to_string(nonCi) + ")");
}


// ---------------------------------------------------------------------------
// AER scope-2 B4 — the abbreviation FLOOR, and the destructive regression it
// closes.
//
// Every leg here is a line real StataBE 19.5 ran, captured in
// internal capture oracle_p.log; the log line is
// cited on each. rc AND wording are both asserted, never rc alone: an rc-only
// assertion passes when the right code arrives for the wrong reason, which is
// how this campaign shipped `re` -> `replace` in the first place.
//
// THE SEEDED RED. The P0-A leg does not merely check an rc — it puts a SENTINEL
// file on disk and asserts the bytes are still there afterwards. Against the
// pre-fix binary (e1faab17..0a4907ca) that leg fails twice over: rc 0 instead of
// r(198), and the sentinel replaced by a CSV. That is the failure this test
// exists to have caught.
void testW1bVceAbbrev() {
  std::printf("[selftest] --- W1b: vce()'s ARGUMENT abbreviates (w1b_vce_abbrev.log) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w1bvce").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto runOf = [&](const std::string& cmd) -> std::pair<std::string, std::string> {
    piet::SessionState session(workdir);
    for (const char* s0 : {"clear", "set obs 74", "generate mpg = 12 + mod(_n,25)",
                           "generate foreign = mod(_n,2)",
                           "generate price = 4000 + 50*mpg + 30*foreign"}) {
      (void)piet::dispatch(s0, session);
    }
    std::string rc = "0", msg;
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) != "error") continue;
      rc = piet::eval::coarseRc(b);
      msg = b.value("message", std::string());
    }
    return {rc, msg};
  };

  // `robust` takes ONE character and `cluster` takes TWO. Both floors are
  // measured, and the pair is the measurement: `vce(c foreign)` is r(198) in
  // Stata, so a single rule "any prefix" would invent a spelling Stata refuses.
  for (const char* ab : {"r", "ro", "rob", "robu", "robus", "robust"}) {
    auto r = runOf(std::string("regress price mpg, vce(") + ab + ")");
    check(r.first == "0",
          std::string("W1b(vce): `vce(") + ab + ")` runs -- every prefix of `robust` down to ONE "
          "character is rc 0 in the oracle, and `vce(r)` alone is 83 corpus uses in one paper. "
          "This build answered `Unknown vce type: r` while the BARE `robust` option abbreviated "
          "fine, so the gap was the sub-argument. Got rc " + r.first + " `" + r.second + "`");
  }
  for (const char* ab : {"cl", "clu", "clus", "clust", "cluste", "cluster"}) {
    auto r = runOf(std::string("regress price mpg, vce(") + ab + " foreign)");
    check(r.first == "0",
          std::string("W1b(vce): `vce(") + ab + " foreign)` runs. Got rc " + r.first + " `" +
              r.second + "`");
  }
  {
    auto r = runOf("regress price mpg, vce(c foreign)");
    check(r.first == "198",
          "W1b(vce): ...but ONE character is NOT enough for cluster -- `vce(c foreign)` is r(198) "
          "in Stata (w1b_vce_abbrev.log v_c). The two floors differ, which is why this is a "
          "per-type minimum and not one rule. Got rc " + r.first);
  }
  // The routing is what matters, not the rc: `vce(r)` must produce ROBUST standard
  // errors, not merely be accepted.
  {
    piet::SessionState session(workdir);
    for (const char* s0 : {"clear", "set obs 74", "generate mpg = 12 + mod(_n,25)",
                           "generate price = 4000 + 50*mpg + mod(_n,7)"}) {
      (void)piet::dispatch(s0, session);
    }
    auto seOf = [&](const std::string& cmd) {
      (void)piet::dispatch(cmd, session);
      auto b = piet::dispatch("display _se[mpg]", session);
      for (const auto& x : b) {
        if (x.is_object() && x.value("type", std::string()) == "display") {
          return x.value("text", std::string());
        }
      }
      return std::string();
    };
    const std::string sePlain = seOf("regress price mpg");
    const std::string seRobust = seOf("regress price mpg, vce(robust)");
    const std::string seAbbrev = seOf("regress price mpg, vce(r)");
    check(!seRobust.empty() && seAbbrev == seRobust,
          "W1b(vce): `vce(r)` produces the SAME standard error as `vce(robust)` -- the oracle's own "
          "check (W1BVCESE, 57.477009 both ways). Accepting the spelling without routing it would "
          "pass every rc arm above. Got robust=" + seRobust + " r=" + seAbbrev);
    check(seAbbrev != sePlain,
          "W1b(vce): ...and differs from the unadjusted one, so `r` really reached the robust "
          "path rather than being dropped. Got plain=" + sePlain);
  }
  // oim/opg are refused by regress in the ORACLE, so they are not added here.
  for (const char* bad : {"oim", "opg", "o", "op", "oi"}) {
    auto r = runOf(std::string("regress price mpg, vce(") + bad + ")");
    check(r.first == "198",
          std::string("W1b(vce): `vce(") + bad + ")` stays refused -- regress does not take "
          "oim/opg at all in the oracle (v_oim/v_opg), so implementing them on the strength of "
          "their being real Stata vce types elsewhere would be invented parity. Got rc " + r.first);
  }
  // Valid Stata this build does not implement: a NAMED refusal, not "unknown".
  for (const char* un : {"bootstrap", "jackknife", "hc2", "hc3"}) {
    auto r = runOf(std::string("regress price mpg, vce(") + un + ")");
    check(r.first == "198" && r.second.find("valid Stata option") != std::string::npos,
          std::string("W1b(vce): `vce(") + un + ")` is rc 0 in the oracle and refused here -- so it "
          "is named as a valid Stata option this build does not implement, not reported as "
          "`Unknown vce type`. `unknown` is the wrong diagnosis for a real option. Got rc " +
              r.first + " `" + r.second + "`");
  }
  for (const char* ab : {"boo", "b", "j", "h"}) {
    auto r = runOf(std::string("regress price mpg, vce(") + ab + ")");
    check(r.first == "198",
          std::string("W1b(vce): ...and those do NOT abbreviate -- `vce(") + ab +
              ")` is r(198) in the oracle too, unlike robust. Recognising an abbreviation there "
              "would invent a refusal message Stata never gives. Got rc " + r.first);
  }
  fs::remove_all(workdir, ec);
}


void testAerB4AbbrevFloor() {
  std::printf("[selftest] --- AER scope-2 B4: the option-abbreviation floor (oracle_p.log) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerb4").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Runs `cmd` after `setup` and returns (rc, joined error message). rc comes
  // from eval::coarseRc — the same function the session's `_rc` comes from — so
  // this asserts the number a do-file would actually see, not a proxy for it.
  auto runOf = [&](const std::vector<std::string>& setup,
                   const std::string& cmd) -> std::pair<std::string, std::string> {
    piet::SessionState session(workdir);
    for (const auto& s : setup) (void)piet::dispatch(s, session);
    std::string rc = "0";
    std::string msg;
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) != "error") continue;
      rc = piet::eval::coarseRc(b);
      msg += b.value("message", std::string());
    }
    return {rc, msg};
  };

  const std::string autoCsv = selftestValidateDir() + "/auto.csv";
  const std::vector<std::string> kData = {
      "import delimited using \"" + autoCsv + "\", clear"};

  // ── P0-A. The destructive one, with the sentinel that makes it a seeded RED.
  {
    const std::string sentinelPath = (fs::path(workdir) / "b4_sentinel.csv").u8string();
    const std::string kSentinel = "B4_SENTINEL_DO_NOT_DESTROY\n";
    for (const char* abbrev : {"re", "rep"}) {
      {
        std::ofstream os(sentinelPath, std::ios::binary);
        os << kSentinel;
      }
      const auto r = runOf(kData, "export delimited using \"" + sentinelPath + "\", " + abbrev);
      // oracle_p.log:23-27 (`re`) and :29-33 (`rep`): both r(198), "option ...
      // not allowed". Stata does not resolve them — it names what was typed.
      check(r.first == "198",
            std::string("B4 P0-A: `export delimited using <f>, ") + abbrev +
                "` is r(198) as StataBE is (oracle_p.log:23-33) -- got " + r.first);
      check(r.second.find(std::string("option `") + abbrev + "` not allowed") != std::string::npos,
            std::string("B4 P0-A: ...and the message names `") + abbrev +
                "`, the spelling the user typed, not `replace` -- got " + r.second);
      // THE POINT OF THE WHOLE ITEM: the file is still the file.
      std::ifstream is(sentinelPath, std::ios::binary);
      std::string after((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
      check(after == kSentinel,
            std::string("B4 P0-A SEEDED RED: the sentinel file survives `, ") + abbrev +
                "` byte for byte. Before the floor, `re` resolved to `replace` and this "
                "assertion found a CSV where the sentinel had been -- got " +
                std::to_string(after.size()) + " bytes");
    }
    // The full spelling is NOT floored: a floor tests proper prefixes only.
    const std::string okPath = (fs::path(workdir) / "b4_ok.csv").u8string();
    const auto full = runOf(kData, "export delimited using \"" + okPath + "\", replace");
    check(full.first == "0",
          "B4 P0-A: the FULL `replace` still works (oracle_p.log:16-21, rc 0) -- a floor that "
          "caught its own option's full spelling would be a second regression; got " + full.first);
  }

  // `save` carries the same measured verdict on the same word (oracle_p.log:
  // 61-65). It already refused, for an unrelated reason — the save arm re-parses
  // the raw tail — so this leg pins the OUTCOME, and the floor row makes it
  // structural rather than incidental.
  {
    const std::string dta = (fs::path(workdir) / "b4.dta").u8string();
    const auto r = runOf(kData, "save \"" + dta + "\", re");
    check(r.first == "198",
          "B4 P0-A: `save <f>, re` is r(198) (oracle_p.log:61-65) -- got " + r.first);
    check(r.second.find("`re`") != std::string::npos,
          "B4 P0-A: ...naming `re` -- got " + r.second);
  }

  // `graph export`'s own `replace` — the row carried from the two measured
  // verdicts above rather than separately captured. What is asserted here is
  // exactly what was MEASURED before the fix: with the target already present,
  // `re` returned rc 0 and defeated the r(602) file-exists guard. With the
  // floor, the guard stands. (`graph export` swallows unrecognized options
  // instead of refusing them — measured: `, zzz` also gives r(602) — so r(602),
  // not r(198), is this arm's honest answer today. That silence is a separate,
  // pre-existing gap and is NOT claimed fixed here.)
  {
    const std::string png = (fs::path(workdir) / "b4_guard.png").u8string();
    {
      std::ofstream os(png, std::ios::binary);
      os << "B4_SENTINEL_PNG\n";
    }
    std::vector<std::string> setup = kData;
    setup.push_back("scatter price mpg");
    const auto r = runOf(setup, "graph export \"" + png + "\", as(png) re");
    check(r.first == "602",
          "B4 P0-A: `graph export <existing>, as(png) re` keeps the file-exists guard -- before "
          "the floor `re` resolved to `replace` and this was rc 0; got " + r.first);
    check(r.second.find("already exists") != std::string::npos,
          "B4 P0-A: ...with the file-exists wording -- got " + r.second);
  }

  // Non-regression: an exact member of the vocabulary is a SPELLING, not an
  // abbreviation, and is never floored even when it is a proper prefix of
  // another option. `delim` prefixes `delimiter`; both are real keys here.
  {
    const auto r = runOf(kData, "import delimited using \"" + autoCsv + "\", delim(\",\") clear");
    check(r.first == "0",
          "B4: `import delimited ..., delim(\",\")` still runs -- `delim` is an exact member of "
          "the vocabulary, so the floor must not treat it as an abbreviation of `delimiter`; "
          "got " + r.first);
  }

  // Non-regression, the constraint pulling the other way: the floor must not
  // simply forbid more. `regress ..., noc` is rc 0 in REAL Stata
  // (oracle_p.log:140-156) and the minimums table already resolves it.
  {
    const auto r = runOf(kData, "regress price mpg, noc");
    check(r.first == "0",
          "B4: `regress price mpg, noc` stays rc 0 -- StataBE accepts it (oracle_p.log:140-156) "
          "and narrowing must not reach it; got " + r.first);
  }

  // ── P0-B / P1. Each row is rc AND wording, because an rc-only row passes for
  // the wrong reason — the failure mode this campaign has now been bitten by
  // twice. `want` empty means "runs", and the message is then not consulted.
  struct Leg {
    std::string cmd;
    std::string rc;
    std::string want;  // substring the message must contain
    std::string why;
  };
  const std::string csv = (fs::path(workdir) / "b4_leg.csv").u8string();
  const std::vector<Leg> kLegs = {
      // P0-B: ambiguous across the REAL Stata vocabulary. `no` prefixes
      // `nolabel` (known) and `novarnames` (unimplemented, i.e. a real Stata
      // option), so it resolves to nothing. It used to resolve to `nolabel` and
      // be HONOURED — rc 0, value labels stripped, nothing to see.
      {"export delimited using \"" + csv + "\", no", "198", "option `no` not allowed",
       "oracle_p.log:35-39"},
      {"list make in 1/2, no", "198", "option `no` not allowed", "oracle_p.log:68-72"},
      // ...and the accepting neighbour one character longer, which is what
      // makes the line above a floor and not a blanket refusal.
      {"list make in 1/2, noo", "0", "", "oracle_p.log:74-84"},
      // P1: separator's minimum is 3, not the generic 2.
      {"summarize price mpg, se(2)", "198", "option `se` not allowed", "oracle_p.log:159-163"},
      {"summarize price mpg, sep(2)", "0", "", "oracle_p.log:165-173"},
      // P1: `vce` takes no abbreviation; `vcov` is not a Stata spelling at all.
      {"regress price mpg, vc(robust)", "198", "option `vc` not allowed", "oracle_p.log:109-113"},
      {"regress price mpg, vco(robust)", "198", "option `vco` not allowed",
       "oracle_p.log:115-119"},
      {"regress price mpg, vce(robust)", "0", "", "oracle_p.log:121-138"},
      // BUG-473 (User ruling 2026-09-09: match Stata). Plain `regress`/`reg`
      // has no `absorb()` option in any Stata edition on this machine --
      // r(198), message "option absorb() not allowed" -- for both the full
      // spelling and the `ab()` abbreviation (Stata resolves the
      // abbreviation before refusing it, so the message names the full
      // word either way): b5_absorb_class.log:32-36 (full), and
      // pins_oracle_p.log:103-107 (the `ab()` abbreviation, same
      // message and rc). `areg`/`reghdfe` share this handler and DO have
      // the option -- b5_absorb_class.log:80-99 (areg rc 0) and :125-151
      // (reghdfe rc 0) are the over-rejection guards that must stay green.
      {"regress price mpg, absorb(rep78)", "198", "absorb",
       "b5_absorb_class.log:32-36"},
      {"regress price mpg, ab(rep78)", "198", "absorb",
       "b5_absorb_class.log:32-36 + pins_oracle_p.log:103-107 (abbrev)"},
      {"areg price mpg, absorb(rep78)", "0", "", "b5_absorb_class.log:80-99"},
      {"reghdfe price mpg, absorb(rep78)", "0", "", "b5_absorb_class.log:125-151"},
  };
  for (const auto& lg : kLegs) {
    const auto r = runOf(kData, lg.cmd);
    check(r.first == lg.rc,
          "B4: `" + lg.cmd + "` is r(" + lg.rc + ") as StataBE is (" + lg.why + ") -- got " +
              r.first);
    if (!lg.want.empty()) {
      check(r.second.find(lg.want) != std::string::npos,
            "B4: ...and the message is `" + lg.want + "` -- rc alone would pass for the wrong "
            "reason; got " + r.second);
    }
  }

  // `estimates table` needs a stored model, so it gets its own setup. Both
  // spellings are r(198) in StataBE (oracle_p.log:176-186) — and the rc here
  // was 1, not 198: the arm's refusal defaulted to `engine_error`.
  {
    std::vector<std::string> setup = kData;
    setup.push_back("quietly regress price mpg weight");
    setup.push_back("estimates store b4m1");
    for (const char* k : {"st", "sta"}) {
      const auto r = runOf(setup, std::string("estimates table b4m1, ") + k + "(N)");
      check(r.first == "198",
            std::string("B4 P1: `estimates table b4m1, ") + k +
                "(N)` is r(198) (oracle_p.log:176-186) -- it was rc 1, an `engine_error` code no "
                "Stata command answers with; got " + r.first);
      check(r.second.find(std::string("`") + k + "`") != std::string::npos,
            std::string("B4 P1: ...naming `") + k + "` -- got " + r.second);
    }
  }

  // ── P1 predict. Not an over-accept: a WRONG TARGET. `resid` and `residuals`
  // are both in `known` (one Stata option, two spellings this arm reads), so
  // `re`/`res`/`resi` prefixed two members, were called ambiguous, fell through
  // to the unimplemented vocabulary, and landed on `response` — a statistic the
  // user never typed — refused under rc 1. StataBE resolves all three to
  // `residuals` and runs (oracle_p.log:87-100).
  //
  // The assertion is the STATISTIC, not the rc: a build that resolved `res` to
  // `xb` would be rc 0 too. So each spelling's summary is compared against the
  // fully-spelled `residuals` run and required to DIFFER from `xb`.
  {
    auto statOf = [&](const std::string& spelling) -> std::pair<double, double> {
      piet::SessionState session(workdir);
      for (const auto& s : kData) (void)piet::dispatch(s, session);
      (void)piet::dispatch("quietly regress price mpg weight", session);
      (void)piet::dispatch("predict double b4p, " + spelling, session);
      for (const auto& b : piet::dispatch("summarize b4p", session)) {
        if (!b.is_object() || b.value("type", std::string()) != "summarize") continue;
        const auto st = b.value("stats", nlohmann::json::object());
        if (!st.contains("b4p")) continue;
        return {st["b4p"].value("mean", 0.0), st["b4p"].value("sd", 0.0)};
      }
      return {std::numeric_limits<double>::quiet_NaN(),
              std::numeric_limits<double>::quiet_NaN()};
    };
    const auto ref = statOf("residuals");
    const auto xb = statOf("xb");
    check(std::isfinite(ref.second) && std::isfinite(xb.second) && ref.second != xb.second,
          "B4 P1 setup: `residuals` and `xb` are different statistics, so the legs below can "
          "tell a right resolution from a wrong one");
    for (const char* sp : {"res", "re", "resi"}) {
      const auto r = runOf(std::vector<std::string>{kData[0], "quietly regress price mpg weight"},
                           std::string("predict double b4q, ") + sp);
      check(r.first == "0",
            std::string("B4 P1: `predict double b4q, ") + sp +
                "` runs, as StataBE does (oracle_p.log:87-100) -- it was rc 1, refusing under "
                "the name `response`, which the user never typed; got " + r.first + " " +
                r.second);
      const auto g = statOf(sp);
      check(std::isfinite(g.second) && g.first == ref.first && g.second == ref.second,
            std::string("B4 P1: ...and `") + sp +
                "` is RESIDUALS, byte for byte with the fully-spelled run -- rc 0 alone would "
                "also pass for a build that resolved it to `xb`");
      check(std::isfinite(g.second) && g.second != xb.second,
            std::string("B4 P1: ...and NOT `xb` -- got sd ") + std::to_string(g.second));
    }
    // cooksd after regress is served (BUG-925; w2b_estim.log predict_reg_cooksd
    // rc 0, `0|1.114410353704443|.0252696031556107`). It was r(198) here; before
    // that, the `engine_error` rc 1.
    const auto ck = runOf(std::vector<std::string>{kData[0], "quietly regress price mpg weight"},
                          "predict double b4r, cooksd");
    check(ck.first == "0",
          "B4 P1: `predict ..., cooksd` after regress runs rc 0, as StataBE does "
          "(w2b_estim.log predict_reg_cooksd) -- got " + ck.first);
  }

  // The MECHANISM, pinned directly, so a future refactor cannot quietly restore
  // the sequential search that made `no` resolve.
  {
    const std::unordered_set<std::string> known = {"nolabel"};
    const std::unordered_set<std::string> unimpl = {"novarnames", "noquote"};
    check(piet::eval::resolveOptionAbbrevAcross("no", known, unimpl) == "no",
          "B4 P0-B mechanism: `no` is AMBIGUOUS across known+unimplemented and resolves to "
          "nothing -- searching `known` first and falling back let `novarnames` sit out the "
          "uniqueness test, and `no` became `nolabel`");
    check(piet::eval::resolveOptionAbbrevAcross("nol", known, unimpl) == "nolabel",
          "B4 P0-B mechanism: ...while an unambiguous prefix still resolves, into `known`");
    check(piet::eval::resolveOptionAbbrevAcross("noq", known, unimpl) == "noquote",
          "B4 P0-B mechanism: ...and into `unimplemented`, which is what keeps the honest "
          "not-yet-implemented wording reachable");
  }

  fs::remove_all(workdir, ec);
}
