// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: command stabilization.
#include "selftest_support.h"
#include "selftest_cases.h"


// --- 9. WS-E1 native command-layer stabilization (missions 1/2/3/5) --------
//
// Mission 1 (BUG-010 residue): a shared trailing-token guard for fixed-arity
// commands (count/pwd/clear/preserve/restore/exit/sysuse) — `count gen zzz9
// = 1` used to silently drop the "gen zzz9 = 1" tail and degrade to a bare
// `count`. Mission 2 (BUG-034): `graph bar/hbar/pie/box/dot` used to fall
// through to runScatter and produce a misleading "scatter requires two
// variables" error. Mission 3 (hardening queue): `sysuse fatalities[,
// clear]` / `sysuse dir` load the one bundled sample dataset by bare name.
// Mission 5 (cosmetic no_data_loaded divergence): `describe`/`desc` with no
// dataset in memory answer an empty description, matching real Stata,
// instead of erroring.
void testCommandLayerStabilizationW3() {
  std::printf("[selftest] --- WS-E1 native command-layer (missions 1/2/3/5) ---\n");

  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_cmdlayer_w3").u8string();
  std::error_code ec;
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

  // --- missions 1 & 2: need a real dataset loaded (count needs rows to
  // count; graph bar's honest-error branch is behind graph's own inner
  // hasData() re-check).
  {
    std::string fixture = std::string(selftestSourceFile());
    auto pos = fixture.rfind("/piet/native/src/selftest.cpp");
    fixture = (pos == std::string::npos) ? "tests/validate/auto.hbin"
                                          : fixture.substr(0, pos) + "/tests/validate/auto.hbin";
    if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
      fixture = selftestValidateDir() + "/auto.hbin";
    }
    if (!std::ifstream(cecon::to_native_path(fixture)).good()) {
      skip("mission 1/2 legs (need tests/validate/auto.hbin)", "fixture not found at " + fixture);
    } else {
      fs::create_directories(workdir + "/m1m2", ec);
      piet::SessionState session(workdir + "/m1m2");
      {
        nlohmann::json body{{"filepath", fixture}};
        (void)piet::routes::open(session, body.dump());
      }
      check(session.hasData(), "mission 1/2 setup: auto.hbin fixture loaded");

      // Mission 1: BUG-010 repro — `count gen zzz9 = 1` must name the
      // dropped tail as a syntax error, NOT silently degrade to `count`.
      {
        auto j = runCmd(session, "count gen zzz9 = 1");
        auto errBlock = firstBlockOfType(j, "error");
        check(errBlock.value("code", std::string()) == "syntax_error",
              "mission 1: `count gen zzz9 = 1` is a syntax_error, not a silent bare count");
        std::string msg = errBlock.value("message", std::string());
        check(msg.find("gen zzz9 = 1") != std::string::npos,
              "mission 1: the error names the exact dropped tokens (\"gen zzz9 = 1\")");
        check(!session.datastore()->has("zzz9"),
              "mission 1: `zzz9` was never created (the `gen` never silently ran)");
      }
      // Regression: bare `count` (no trailing tokens) is untouched.
      {
        auto j = runCmd(session, "count");
        auto dispBlock = firstBlockOfType(j, "display");
        check(dispBlock.value("text", std::string()) == "74",
              "mission 1 regression: bare `count` on auto.hbin still answers 74");
      }
      // Mission 1: pwd/clear also guarded (bespoke arm-content aside, any
      // trailing token past their own fixed arity is a syntax_error).
      {
        auto j = runCmd(session, "pwd extra");
        check(firstBlockOfType(j, "error").value("code", std::string()) == "syntax_error",
              "mission 1: `pwd extra` is a syntax_error (pwd takes no arguments)");
      }

      // Mission 2: BUG-034 repro — `graph bar ...` must never misroute through
      // runScatter. graph-family AI-1: it is implemented now (graph_bar.cpp;
      // w2ai gf_bar_mean `graph bar (mean) y, over(g)` rc 0, log:213-223), so
      // it draws a bar graph; `graph pie` keeps the honest gap.
      {
        auto j = runCmd(session, "graph bar mpg, over(foreign)");
        auto g = firstBlockOfType(j, "graph");
        check(firstBlockOfType(j, "error").is_null() && g.value("kind", std::string()) == "bar",
              "mission 2: `graph bar ...` draws a bar graph (graph-family AI-1), not a scatter error");
        auto jp = runCmd(session, "graph pie mpg, over(foreign)");
        auto errBlock = firstBlockOfType(jp, "error");
        check(errBlock.value("code", std::string()) == "unsupported_command" &&
                  errBlock.value("message", std::string()).find("graph pie") != std::string::npos,
              "mission 2: `graph pie ...` is an honest unsupported_command naming `graph pie`");
      }
      // Regression: plain `scatter`/`graph twoway scatter` still render.
      {
        auto j = runCmd(session, "scatter mpg weight");
        auto graphBlock = firstBlockOfType(j, "graph");
        check(graphBlock.value("kind", std::string()) == "scatter",
              "mission 2 regression: `scatter mpg weight` still renders a scatter graph block");
      }
    }
  }

  // --- mission 3: sysuse — fresh session, no `use` needed beforehand.
  {
    fs::create_directories(workdir + "/m3", ec);
    piet::SessionState session(workdir + "/m3");
    {
      auto j = runCmd(session, "sysuse dir");
      auto dispBlock = firstBlockOfType(j, "display");
      check(dispBlock.value("text", std::string()).find("fatalities") != std::string::npos,
            "mission 3: `sysuse dir` lists the bundled `fatalities` dataset");
    }
    {
      auto j = runCmd(session, "sysuse fatalities");
      auto mutBlock = firstBlockOfType(j, "data_mutation");
      if (!mutBlock.is_object()) {
        // Isolated/subset builds (e.g. a VERIFY red-arm scratch tree of just
        // piet/native + src/io + src/core + vendor) don't ship
        // piet/packaging/sample/fatalities.csv, so both the bundle-first and
        // dev-tree-fallback lookups in dispatcher.cpp's sysuse arm miss and
        // this returns an "error" block instead of "data_mutation" — an
        // honest environment gap, not a regression. Skip rather than crash
        // on the unguarded .value() below (FIX-SELFTEST-ISOBUILD-CRASH).
        auto errBlock = firstBlockOfType(j, "error");
        skip("mission 3 sysuse-fatalities-dependent legs",
             "`sysuse fatalities` returned no data_mutation block" +
                 (errBlock.is_object()
                      ? (": " + errBlock.value("message", std::string()))
                      : std::string()));
      } else {
        check(mutBlock.value("n_rows_after", int64_t{0}) > 0,
              "mission 3: `sysuse fatalities` loads real rows (n_rows_after > 0)");
        check(session.hasData(), "mission 3: session has data after `sysuse fatalities`");
      }
    }
    {
      auto j = runCmd(session, "sysuse nlswork");
      auto errBlock = firstBlockOfType(j, "error");
      check(!errBlock.is_null(), "mission 3: `sysuse nlswork` (unbundled dataset) is an error");
      check(errBlock.value("message", std::string()).find("fatalities") != std::string::npos,
            "mission 3: the unknown-dataset error names what IS available (`fatalities`)");
    }
  }

  // --- mission 5: `describe`/`desc` with no data answers empty, not error;
  // `codebook` is deliberately left erroring (needs real statistics).
  {
    fs::create_directories(workdir + "/m5", ec);
    piet::SessionState session(workdir + "/m5");
    check(!session.hasData(), "mission 5 setup: fresh session has no data loaded");
    {
      auto j = runCmd(session, "describe");
      auto descBlock = firstBlockOfType(j, "describe");
      check(!descBlock.is_null(), "mission 5: `describe` with no data returns a describe block");
      check(descBlock.value("variables", nlohmann::json::array()).empty(),
            "mission 5: the no-data describe block has an empty variable list");
    }
    {
      auto j = runCmd(session, "desc");
      auto descBlock = firstBlockOfType(j, "describe");
      check(!descBlock.is_null() && descBlock.value("variables", nlohmann::json::array()).empty(),
            "mission 5: `desc` alias gets the same empty-description treatment");
    }
    {
      auto j = runCmd(session, "codebook");
      check(firstBlockOfType(j, "error").value("code", std::string()) == "no_data_loaded",
            "mission 5: `codebook` with no data is UNCHANGED (still no_data_loaded)");
    }
  }

  fs::remove_all(workdir, ec);
}


// --- W3 fuzzer-bug fixes (PIET stabilization campaign, kitchensink-fuzz
// SILENT_SWALLOW category) --------------------------------------------------
//
// BUG A: `save FILE.dta` used to byte-copy the session's internal .hbin to
// a `.dta`-named path (a silent format lie — the bytes aren't a real .dta
// file, so a later `use SAME.dta` died with a cryptic engine error). BUG B:
// `rename` of a string/factor variable always failed, leaking a raw
// "[internal] expr: ..." engine-exception string. BUG C: `gen 9zzz = 1`
// silently created a column with an invalid Stata identifier. Each leg
// below reproduces the ORIGINAL failure shape from the fuzz campaign
// (fail_024/038/041_SILENT_SWALLOW.do) and asserts the fixed behavior; see
// fileio/dataset_save.cpp::runSaveData, datamgmt/variable_copy_rename.cpp::runRename, fileio/dataset_use.cpp::runUse/
// datamgmt/data_generate.cpp::datamgmt/data_generate.cpp::runGen/runReplace, and strutil.h::isValidStataIdentifier.
void testFuzzCampaignBugfixes() {
  std::printf("[selftest] --- W3 fuzzer-campaign bugfixes (save-.dta / rename / identifier) ---\n");

  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_fuzzbugs").u8string();
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
  auto lastBlockOfType = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    nlohmann::json found;
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == type) found = b;
    }
    return found;
  };
  auto anyBlockTextContains = [](const nlohmann::json& j, const std::string& needle) {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      std::string t = b.value("text", b.value("message", std::string()));
      if (t.find(needle) != std::string::npos) return true;
    }
    return false;
  };
  // "" when the response carried no error block at all. firstBlockOfType()
  // hands back a NULL json in that case, and nlohmann's value() THROWS on null
  // (json.exception.type_error.306 — it aborts the whole selftest binary, not
  // just the check), so every success-path read of an error message goes
  // through this instead of .value() directly.
  auto errMessage = [&firstBlockOfType](const nlohmann::json& j) -> std::string {
    nlohmann::json b = firstBlockOfType(j, "error");
    return b.is_null() ? std::string() : b.value("message", std::string());
  };

  // --- BUG A, P1-A3 RETIRE-AND-REPLACE: `save FILE.dta` used to be an honest
  // ERROR because PIET had no .dta writer; cobra's `export-dta` (format 118)
  // supplied one, so the same honesty invariant — never write one format's
  // bytes under a name implying another — is now discharged by writing GENUINE
  // Stata bytes. The leg therefore asserts the invariant DIRECTLY (a real 118
  // header, not merely "a file appeared"), which is a strictly sharper test
  // than the refusal it replaces: the pre-fix bug this leg was written for
  // (hbin bytes under a `.dta` name) fails the magic check just as loudly as it
  // failed the "is an error" check. Extensionless `save` now selects Stata `.dta`
  // and still says so.
  {
    std::string wd = workdir + "/save";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    session.cwd = wd;
    runCmd(session, "sysuse fatalities, clear");
    check(session.hasData(), "BUG A setup: `sysuse fatalities` loads for the save-honesty legs");

    // STATA PARITY on the raw fixture: `fatalities` carries string columns with
    // missing values (jail/service, CA-1988), and Stata's .dta has no string
    // missing distinct from "" — in Stata a string variable's missing IS "".
    // The writer therefore WRITES it as "", the same bytes Stata would store,
    // and names the collapse on stderr rather than failing the save. This arm
    // asserts that parity; the "a refusal leaves the file untouched" contract is
    // asserted below on the re-save-without-`replace` leg, which still refuses.
    auto jDtaRaw = runCmd(session, "save out_raw.dta");
    // CAPABILITY GATE, content-based (not a version string): the `.dta` writer
    // lives in cobra, and the engine a build RESOLVES to (Art 7 pin) can be
    // older than the writer — measured 2026-07-31, the pinned 0.34.3 answers
    // `export-dta` with `unknown command`. On such an engine `save x.dta` is an
    // honest, NAMED refusal, so this leg SKIPS its writer assertions instead of
    // failing them, and asserts the refusal's own contract (no half-file) —
    // it keys on the refusal PIET emits, so it cannot go stale-green.
    const std::string dtaErrMsg = errMessage(jDtaRaw);
    const bool engineHasDtaWriter = dtaErrMsg.find("`export-dta` writer") == std::string::npos;
    if (!engineHasDtaWriter) {
      skip("P1-A3 `.dta` writer assertions",
           "the resolved cobra engine has no `export-dta` (pin older than the writer)");
    } else {
    check(firstBlockOfType(jDtaRaw, "error").is_null(),
          "P1-A3 (Stata parity): a string-missing cell does NOT fail the save — Stata stores "
          "that as \"\" and so do we");
    check(fs::exists(wd + "/out_raw.dta"),
          "P1-A3 (Stata parity): the string-missing dataset actually reached disk");

    // Drop the two string-missing columns; the save stays legal either way. Named
    // columns, not a filter: this keeps the rest of the arm on a dataset whose
    // round trip is signature-exact, which the string-missing one is not.
    runCmd(session, "keep year spirits unemp income");
    auto jDta = runCmd(session, "save out.dta");
    check(firstBlockOfType(jDta, "error").is_null(),
          "P1-A3: `save FILE.dta` is no longer an error — the writer landed");
    check(lastBlockOfType(jDta, "echo").value("text", std::string()) == "file out.dta saved",
          "P1-A3: `save FILE.dta` echoes the REAL filename it wrote (no .hbin substitution)");
    check(fs::exists(wd + "/out.dta"), "P1-A3: `out.dta` was actually written to disk");
    {
      // The honesty assertion itself: format-118 files open with the literal
      // `<stata_dta><header><release>118</release>` opening tags (spec §2 /
      // src/io/dta_writer.h). hbin bytes under a .dta name — the ORIGINAL BUG A
      // failure — do not contain them, so this check reproduces that bug's
      // detection without depending on the refusal that used to stand in for a
      // writer.
      std::ifstream probe(cecon::to_native_path(wd + "/out.dta"), std::ios::binary);
      std::string head(64, '\0');
      probe.read(head.data(), static_cast<std::streamsize>(head.size()));
      head.resize(static_cast<size_t>(probe.gcount()));
      check(head.find("<stata_dta>") != std::string::npos,
            "P1-A3 (BUG A invariant): the bytes at `out.dta` are a REAL Stata .dta, not hbin "
            "under a .dta name");
      check(head.find("<release>118</release>") != std::string::npos,
            "P1-A3: the written .dta declares format 118 (the writer's only target, Q7)");
    }
    // Stata-parity honest-reject, unchanged by the writer: an existing file
    // without `replace` is refused, and the refusal must leave the file alone.
    auto jNoRep = runCmd(session, "save out.dta");
    check(!firstBlockOfType(jNoRep, "error").is_null(),
          "P1-A3: re-saving over an existing `.dta` without `replace` is an honest error");
    check(fs::exists(wd + "/out.dta"), "P1-A3: the refused re-save left the existing file in place");
    check(firstBlockOfType(runCmd(session, "save out.dta, replace"), "error").is_null(),
          "P1-A3: `save out.dta, replace` succeeds over an existing file");
    }

    auto jTxt = runCmd(session, "save out.txt");
    check(firstBlockOfType(jTxt, "error").value("code", std::string()) == "engine_error",
          "BUG A: an unrelated extension (`.txt`) is ALSO an honest error, not a silent write");
    check(!fs::exists(wd + "/out.txt"), "BUG A: no `out.txt` file was written either");

    auto jPlain = runCmd(session, "save plainname");
    auto echoResult = lastBlockOfType(jPlain, "echo");
    check(echoResult.value("text", std::string()) == "file plainname.dta saved",
          "save default (w2_cmdtriage erase_glued): an extensionless name selects .dta");
    check(fs::exists(wd + "/plainname.dta"),
          "save default: `plainname.dta` was actually written to disk");
    {
      std::ifstream probe(cecon::to_native_path(wd + "/plainname.dta"), std::ios::binary);
      std::string head(64, '\0');
      probe.read(head.data(), static_cast<std::streamsize>(head.size()));
      check(head.find("<release>118</release>") != std::string::npos,
            "save default: extensionless output is genuine Stata format 118");
    }
    (void)runCmd(session, "tempfile public_save");
    const std::string tempPath = session.eval.locals.get("public_save");
    const auto tempSaved = runCmd(session, "save \"`public_save'\", replace");
    std::ifstream tempProbe(cecon::to_native_path(tempPath), std::ios::binary);
    std::string tempHead(64, '\0');
    tempProbe.read(tempHead.data(), static_cast<std::streamsize>(tempHead.size()));
    check(firstBlockOfType(tempSaved, "error").is_null() &&
              tempHead.find("<release>118</release>") != std::string::npos &&
              !session.isVirtualSave(tempPath),
          "save tempfile (w2ar_data_io_family dx_sa_004): .tmp carries real Stata bytes");
  }

  // --- P1-A3: a `.dta` save carries the session's LABEL/FORMAT layer, not just
  // its values. This is the sidecar half of the surface. The hbin the writer
  // reads has a name/format/levels header and no room for a variable label, a
  // declared Stata type or a value-label set, so `save x.dta` writes an
  // `export-dta --in-meta` sidecar out of the live session maps. Without it the
  // round trip would come back value-correct and label-EMPTY — the silent
  // downgrade this leg exists to catch (a values-only check would pass).
  {
    std::string wd = workdir + "/savedta_meta";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    session.cwd = wd;
    runCmd(session, "sysuse fatalities, clear");
    auto* ds = session.hasData() ? session.datastore() : nullptr;
    std::string v0;
    if (ds != nullptr) {
      // A numeric column: `format %9.3f` is meaningless on a string, and the
      // writer refuses a value label on one (dta_writer.cpp:352-360).
      for (const piet::VarInfo& vi : ds->varlist()) {
        if (vi.dtype != "factor") {
          v0 = vi.name;
          break;
        }
      }
    }
    if (v0.empty()) {
      // Same isolated/subset-build fixture gap the hbin-sniff leg below guards
      // (FIX-SELFTEST-ISOBUILD-CRASH) — skip honestly rather than chain onto
      // null blocks.
      skip("P1-A3 .dta label/format round-trip", "`sysuse fatalities` setup returned no numeric column");
    } else {
      // Numeric-only slice: `fatalities`' string columns carry missing values,
      // which the writer refuses by design (see the honest-reject arm above).
      runCmd(session, "keep year spirits unemp income");
      runCmd(session, "label variable " + v0 + " \"P1A3 carried label\"");
      runCmd(session, "format " + v0 + " %9.3f");
      check(session.varMeta.count(v0) != 0 && session.varMeta[v0].label.has_value() &&
                *session.varMeta[v0].label == "P1A3 carried label",
            "P1-A3 setup: the session holds the variable label the sidecar must carry");

      auto jSaveMeta = runCmd(session, "save meta.dta, replace");
      const std::string saveErr = errMessage(jSaveMeta);
      if (saveErr.find("`export-dta` writer") != std::string::npos) {
        // Same content-based capability gate as the leg above.
        skip("P1-A3 .dta label/format round-trip",
             "the resolved cobra engine has no `export-dta` (pin older than the writer)");
      } else {
        check(saveErr.empty(), "P1-A3: `save meta.dta, replace` wrote the .dta");

        // Fresh session, PIET's own reader: the metadata must survive
        // sidecar -> writer -> file -> `import-dta --out-meta` -> applyDtaMeta.
        piet::SessionState reloaded(wd);
        reloaded.cwd = wd;
        runCmd(reloaded, "use meta.dta");
        check(reloaded.hasData(), "P1-A3: the written .dta loads back through PIET's own reader");
        check(reloaded.varMeta.count(v0) != 0 && reloaded.varMeta[v0].label.has_value() &&
                  *reloaded.varMeta[v0].label == "P1A3 carried label",
              "P1-A3: the variable LABEL survived the .dta round trip (sidecar carriage)");
        check(reloaded.varMeta.count(v0) != 0 && reloaded.varMeta[v0].format.has_value() &&
                  *reloaded.varMeta[v0].format == "%9.3f",
              "P1-A3: the display FORMAT survived the .dta round trip");
      }
    }
  }

  // --- FIX:PIET-SAVE-DTA-CHARS-CARRIAGE: `data_label` and `<characteristics>`
  // survive a `use A.dta` -> `save B.dta` round trip too — the two blocks
  // applyDtaMeta() used to leave unread entirely (see that function's and
  // writeDtaMetaSidecar's doc comments). No in-session `label data`/`char`
  // verb exists to set these through PIET's own command surface (out of this
  // task's scope, carriage only), so this leg seeds session.dataLabel /
  // session.characteristics directly — exactly the shape applyDtaMeta() would
  // have populated them with from a real .dta's own metadata — and checks
  // both a dataset-level ("_dta") and a per-variable characteristic survive
  // alongside the dataset label.
  {
    std::string wd = workdir + "/savedta_charscarry";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    session.cwd = wd;
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      skip("FIX:PIET-SAVE-DTA-CHARS-CARRIAGE round trip",
           "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "keep year spirits");
      session.dataLabel = "carriage test dataset";
      session.characteristics.push_back({"_dta", "note1", "dataset-level char"});
      session.characteristics.push_back({"year", "note1", "variable-level char"});

      auto jSave = runCmd(session, "save charscarry.dta, replace");
      const std::string saveErr = errMessage(jSave);
      if (saveErr.find("`export-dta` writer") != std::string::npos) {
        // Same content-based capability gate as the legs above.
        skip("FIX:PIET-SAVE-DTA-CHARS-CARRIAGE round trip",
             "the resolved cobra engine has no `export-dta` (pin older than the writer)");
      } else {
        check(saveErr.empty(),
              "FIX:PIET-SAVE-DTA-CHARS-CARRIAGE: `save charscarry.dta, replace` wrote the .dta");

        // Fresh session, PIET's own reader: dataLabel/characteristics must
        // survive sidecar -> writer -> file -> `import-dta --out-meta` ->
        // applyDtaMeta, same as the label/format leg above.
        piet::SessionState reloaded(wd);
        reloaded.cwd = wd;
        runCmd(reloaded, "use charscarry.dta");
        check(reloaded.hasData(),
              "FIX:PIET-SAVE-DTA-CHARS-CARRIAGE: the written .dta loads back through PIET's own reader");
        check(reloaded.dataLabel == "carriage test dataset",
              "FIX:PIET-SAVE-DTA-CHARS-CARRIAGE: the dataset label survived the .dta round trip");
        bool hasDatasetChar = false, hasVarChar = false;
        for (const auto& c : reloaded.characteristics) {
          if (c.varname == "_dta" && c.charname == "note1" && c.value == "dataset-level char")
            hasDatasetChar = true;
          if (c.varname == "year" && c.charname == "note1" && c.value == "variable-level char")
            hasVarChar = true;
        }
        check(hasDatasetChar,
              "FIX:PIET-SAVE-DTA-CHARS-CARRIAGE: the dataset-level characteristic (_dta) survived "
              "the round trip");
        check(hasVarChar,
              "FIX:PIET-SAVE-DTA-CHARS-CARRIAGE: the variable-level characteristic survived the "
              "round trip");
      }
    }
  }

  // --- FIX:PIET-SAVE-SUBOPTION-STRICT: unsupported `save` suboptions
  // (nolabel/orphans/emptyok/version()) used to vanish silently the moment
  // the dispatcher found `replace` anywhere in the option tail — the caller
  // believed they applied. Now each is a named honest-reject, and `replace`
  // alone is still recognized.
  {
    std::string wd = workdir + "/save_suboption";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    session.cwd = wd;
    runCmd(session, "sysuse fatalities, clear");

    auto jBad = runCmd(session, "save f.hbin, nolabel replace");
    auto errBad = firstBlockOfType(jBad, "error");
    check(errBad.value("code", std::string()) == "unsupported_option",
          "FIX:PIET-SAVE-SUBOPTION-STRICT: `nolabel` is a named unsupported_option reject, not "
          "a silent no-op");
    check(errBad.value("message", std::string()).find("nolabel") != std::string::npos,
          "FIX:PIET-SAVE-SUBOPTION-STRICT: the reject names the actual offending token "
          "(`nolabel`)");
    check(!fs::exists(wd + "/f.hbin"),
          "FIX:PIET-SAVE-SUBOPTION-STRICT: no file was written on the honest-reject path");

    // B5-D9: `emptyok` is now accepted -- a validated no-op on `save`.
    // Measured B4REC|save|replace_emptyok|0| (tests/eval/golden/aer_b4/
    // pins_oracle_r.log:90-93) on `clear` + `set obs 0` + `save F,
    // replace emptyok`.
    runCmd(session, "clear");
    runCmd(session, "set obs 0");
    auto jOk = runCmd(session, "save empty.hbin, replace emptyok");
    auto errOk = firstBlockOfType(jOk, "error");
    check(errOk.is_null(),
          "B5-D9: `save F, replace emptyok` on a 0-var/0-obs frame is accepted (rc 0)");
    check(fs::exists(wd + "/empty.hbin"),
          "B5-D9: the empty file was actually written, not silently skipped");
  }

  // --- BUG A (hbin-sniff): a file whose CONTENT is native .hbin loads
  // correctly via `use` regardless of its extension — rescuing exactly the
  // mislabeled-`.dta` survivors the pre-fix `save` bug could produce (and
  // any other simple misnaming).
  {
    std::string wd = workdir + "/sniff";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    session.cwd = wd;
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as mission 3 above
      // (FIX-SELFTEST-ISOBUILD-CRASH): without real.hbin's source data,
      // `save`/`copy_file`/`use` below would chain onto null blocks and
      // crash the unguarded .value() calls that assume this setup always
      // succeeds. Skip the whole hbin-sniff leg honestly instead.
      skip("BUG A hbin-sniff leg", "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "save real.hbin, replace");
      std::error_code cec;
      fs::copy_file(wd + "/real.hbin", wd + "/mislabeled.dta", fs::copy_options::overwrite_existing,
                    cec);
      check(!cec, "BUG A hbin-sniff setup: mislabeled.dta fixture (real hbin bytes) copied into place");

      piet::SessionState session2(wd);
      session2.cwd = wd;
      auto j = runCmd(session2, "use mislabeled.dta, clear");
      check(anyBlockTextContains(j, "native .hbin format"),
            "BUG A hbin-sniff: `use mislabeled.dta` surfaces an honest note explaining the mismatch");
      auto mutBlock = lastBlockOfType(j, "data_mutation");
      check(mutBlock.value("n_rows_after", int64_t{0}) > 0,
            "BUG A hbin-sniff: the mislabeled file still loads its real rows (content wins over name)");
      check(session2.hasData(), "BUG A hbin-sniff: session has usable data after the sniffed load");
    }
  }

  // --- RED-fix (piet_2.1.0_gate VERDICT.md Leg 3,
  // test_postfile_using_replace_roundtrips#5): postfile's OWN round-trip
  // (postfile ... using PATH / post / postclose / use PATH) must NOT get
  // the BUG-A hbin-sniff honesty note — this session wrote those bytes
  // itself, there is nothing to explain — while a genuinely external
  // mislabeled file in the SAME session still gets the note (both
  // directions checked here, per the Eval finding's "narrow, session-local
  // check" framing).
  {
    std::string wd = workdir + "/postfile_sniff";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    session.cwd = wd;

    // Direction 1: postfile's own tempfile-shaped round-trip -> no note.
    runCmd(session, "postfile ph str32 depvar double b using pf_out.tmp, replace");
    runCmd(session, "post ph (\"ppda\") (1.5)");
    auto jClose = runCmd(session, "postclose ph");
    check(firstBlockOfType(jClose, "error").is_null(),
          "RED-fix setup: postclose ph succeeds (no error block)");
    check(fs::exists(wd + "/pf_out.tmp"),
          "RED-fix setup: postclose wrote pf_out.tmp (hbin bytes under a non-.hbin name)");

    auto jUse = runCmd(session, "use pf_out.tmp, clear");
    check(!anyBlockTextContains(jUse, "native .hbin format"),
          "RED-fix: `use`-ing postfile's OWN round-trip target emits NO hbin-sniff note");
    auto mutBlock = lastBlockOfType(jUse, "data_mutation");
    check(mutBlock.value("n_rows_after", int64_t{0}) == 1,
          "RED-fix: the postfile round-trip data still loads correctly (1 posted row)");

    // Direction 2: a genuinely external mislabeled hbin file, in the SAME
    // session, still gets the note — the suppression is narrowly scoped to
    // paths THIS session's postfile machinery itself wrote, not a blanket
    // disabling of the sniff note.
    std::error_code cec;
    fs::copy_file(wd + "/pf_out.tmp", wd + "/external_mislabeled.dta",
                  fs::copy_options::overwrite_existing, cec);
    check(!cec, "RED-fix setup: external_mislabeled.dta fixture (real hbin bytes, untracked path) "
                "copied into place");
    auto jExternal = runCmd(session, "use external_mislabeled.dta, clear");
    check(anyBlockTextContains(jExternal, "native .hbin format"),
          "RED-fix: `use`-ing a hbin-content file OUTSIDE the postfile-written set STILL gets the "
          "honesty note");
  }

  // --- BUG B: rename of a string/factor variable works (no leaked
  // "[internal]" engine-exception text), and the round-trip preserves the
  // VALUE exactly (rename is metadata-only).
  {
    std::string wd = workdir + "/rename";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    runCmd(session, "gen str10 gs2 = \"hi\"");
    check(session.datastore() != nullptr && session.datastore()->has("gs2") &&
              session.datastore()->dtype("gs2") == "factor",
          "BUG B setup: `gen str10 gs2 = \"hi\"` created a factor/string column");

    auto j = runCmd(session, "rename gs2 rn3");
    check(!anyBlockTextContains(j, "[internal]"),
          "BUG B: no leaked '[internal]' engine-exception text anywhere in the rename result");
    check(firstBlockOfType(j, "error").is_null(),
          "BUG B: `rename gs2 rn3` (string/factor variable) succeeds, no error block");
    piet::Datastore* ds = session.datastore();
    check(ds != nullptr && !ds->has("gs2"), "BUG B: old name `gs2` is gone after rename");
    check(ds != nullptr && ds->has("rn3") && ds->dtype("rn3") == "factor",
          "BUG B: new name `rn3` exists and is STILL a factor/string column (dtype preserved)");
    if (ds != nullptr && ds->has("rn3")) {
      const cecon::Column& col = ds->col("rn3");
      int32_t code0 = col.codes()[0];
      check(code0 >= 0 && static_cast<size_t>(code0) < col.levels.size() &&
                col.levels[static_cast<size_t>(code0)] == "hi",
            "BUG B: renamed string column's VALUE round-trips exactly (\"hi\", unchanged)");
    }

    // Regression: the session keeps working afterward (mirrors fail_038's
    // tail command, `gen gv4 = firmsize + 1`-shaped numeric gen).
    auto j2 = runCmd(session, "gen gv8 = baptist + 1");
    check(firstBlockOfType(j2, "error").is_null(),
          "BUG B regression: a later numeric `gen` still works after the string rename");
  }

  // --- BUG C: Stata-identifier validation is enforced at gen's numeric
  // path AND at rename's target name (the ONE shared validator,
  // strutil::isValidStataIdentifier) — valid names still work, invalid ones
  // get an honest syntax_error instead of silently succeeding.
  {
    std::string wd = workdir + "/ident";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    auto okUnderscore = runCmd(session, "gen _ok = 1");
    check(firstBlockOfType(okUnderscore, "error").is_null(),
          "BUG C: `gen _ok = 1` (leading underscore) still works");
    auto okAlnum = runCmd(session, "gen a1 = 1");
    check(firstBlockOfType(okAlnum, "error").is_null(),
          "BUG C: `gen a1 = 1` (letter followed by digit) still works");

    auto badDigit = runCmd(session, "gen 9zzz = 1");
    check(firstBlockOfType(badDigit, "error").value("code", std::string()) == "syntax_error",
          "BUG C: `gen 9zzz = 1` (leading digit) is an honest syntax_error");
    check(session.datastore() != nullptr && !session.datastore()->has("9zzz"),
          "BUG C: `9zzz` was never silently created");

    auto badChar = runCmd(session, "gen a-b = 1");
    check(firstBlockOfType(badChar, "error").value("code", std::string()) == "syntax_error",
          "BUG C: `gen a-b = 1` (disallowed character) is an honest syntax_error");

    auto badRename = runCmd(session, "rename baptist 9bad");
    check(firstBlockOfType(badRename, "error").value("code", std::string()) == "syntax_error",
          "BUG C: `rename baptist 9bad` (invalid new name) is an honest syntax_error");
    check(session.datastore() != nullptr && session.datastore()->has("baptist"),
          "BUG C: `baptist` was NOT renamed away when the new name was rejected");
  }

  fs::remove_all(workdir, ec);
}


// --- W3-final fixer bugfixes (BUGLOG BUG-037/038/039) -----------------------
//
// BUG-037: `clonevar new = stringvar` had the SAME numeric-only-`--gen`
// disease `rename` had (fixed above) — cloning a factor/string column leaked
// a raw "[internal] expr: ..." engine-exception string. BUG-038(a): any
// cobra `internal`-coded error (its generic C++-exception catch-all) leaked
// its raw "[internal] ..." shape verbatim to the user instead of an honest
// rewrite. BUG-039: cobra can legitimately exit nonzero while still printing
// a fully-formed, valid result JSON to stdout (an estimator that ran to
// completion but did not CONVERGE) — the pre-fix code discarded that JSON
// and manufactured a generic "[engine_error] cobra exited 1" instead of
// surfacing the honest non-convergence diagnostics. See
// datamgmt/variable_copy_rename.cpp::runClonevar (the CSV round-trip helper it once used is deleted),
// piet/cobra_runner.h and engine/cobra_runner.cpp::cobraErrorMessage, and engine/cobra_runner.cpp::runCobraJson.
void testW3FinalFixerBugfixes() {
  std::printf("[selftest] --- W3-final fixer bugfixes (clonevar-string / [internal] leak / "
              "exit-1 JSON) ---\n");

  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w3final").u8string();
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
  auto anyBlockTextContains = [](const nlohmann::json& j, const std::string& needle) {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      std::string t = b.value("text", b.value("message", std::string()));
      if (t.find(needle) != std::string::npos) return true;
    }
    return false;
  };

  // --- BUG-037: `clonevar new = stringvar` — no leaked "[internal]" text,
  // OLDVAR survives untouched (a clone is a COPY, not a move), NEWVAR is a
  // factor/string column with the SAME levels and the SAME per-row value.
  {
    std::string wd = workdir + "/clonevar_str";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    piet::Datastore* ds0 = session.datastore();
    check(ds0 != nullptr && ds0->has("state") && ds0->dtype("state") == "factor",
          "BUG-037 setup: bundled `fatalities` sample has a factor/string `state` column");

    auto j = runCmd(session, "clonevar st2 = state");
    check(!anyBlockTextContains(j, "[internal]"),
          "BUG-037: no leaked '[internal]' engine-exception text in `clonevar st2 = state`");
    check(firstBlockOfType(j, "error").is_null(),
          "BUG-037: `clonevar st2 = state` (string/factor variable) succeeds, no error block");

    piet::Datastore* ds = session.datastore();
    check(ds != nullptr && ds->has("state") && ds->dtype("state") == "factor",
          "BUG-037: `state` (OLDVAR) survives untouched — clonevar is a COPY, not a move");
    check(ds != nullptr && ds->has("st2") && ds->dtype("st2") == "factor",
          "BUG-037: `st2` (NEWVAR) exists and is a factor/string column (dtype cloned)");
    if (ds != nullptr && ds->has("state") && ds->has("st2")) {
      const cecon::Column& stateCol = ds->col("state");
      const cecon::Column& st2Col = ds->col("st2");
      check(stateCol.levels.size() == st2Col.levels.size(),
            "BUG-037: cloned column has the same number of factor levels as the original");
      int32_t code0 = stateCol.codes()[0];
      int32_t code0New = st2Col.codes()[0];
      std::string v0 = (code0 >= 0 && static_cast<size_t>(code0) < stateCol.levels.size())
                           ? stateCol.levels[static_cast<size_t>(code0)]
                           : std::string();
      std::string v0New = (code0New >= 0 && static_cast<size_t>(code0New) < st2Col.levels.size())
                               ? st2Col.levels[static_cast<size_t>(code0New)]
                               : std::string();
      check(!v0.empty() && v0 == v0New,
            "BUG-037: cloned string column's row-0 VALUE matches the original exactly");
    }

    // Regression: numeric clonevar stays on the ORIGINAL fast `--gen` path
    // (byte-identical A/B, the stabilization brief's explicit requirement)
    // — still works, and the cloned value matches the source exactly.
    auto j2 = runCmd(session, "clonevar yr2 = year");
    check(firstBlockOfType(j2, "error").is_null(),
          "BUG-037 regression: numeric `clonevar yr2 = year` still works (fast path unchanged)");
    // Re-fetch the datastore: the clonevar above is a mutation, so mutationResult
    // called session.invalidateDatastore() and the earlier `ds` now DANGLES — its
    // cecon::Dataset was destroyed and its hbin mmap released. Reading through the
    // stale `ds` is a use-after-free: benign on macOS (munmap keeps the pages
    // readable a while) but a crash / garbage read on Windows, where
    // UnmapViewOfFile invalidates the view immediately.
    piet::Datastore* ds2 = session.datastore();
    if (ds2 != nullptr && ds2->has("year") && ds2->has("yr2")) {
      check(ds2->col("year").f64()[0] == ds2->col("yr2").f64()[0],
            "BUG-037 regression: numeric clone's row-0 value matches the source exactly");
    }

    // BUG C parity: clonevar never had the shared identifier validator wired
    // at all before this fix — mirror rename/gen's check.
    auto badClone = runCmd(session, "clonevar 9bad = year");
    check(firstBlockOfType(badClone, "error").value("code", std::string()) == "syntax_error",
          "BUG-037/BUG-C: `clonevar 9bad = year` (invalid new name) is an honest syntax_error");
    check(session.datastore() != nullptr && !session.datastore()->has("9bad"),
          "BUG-037/BUG-C: `9bad` was never silently created");
  }

  // --- BUG-038(a): a cobra `internal`-coded error never surfaces its raw
  // "[internal] ..." shape — an unknown-function reference gets the named
  // "function X() is not supported" rewrite; any OTHER internal shape gets
  // a bracket-free "engine error: ..." wrapper. Every OTHER (stable) code
  // keeps its existing bracketed "[code] ..." format (regression, exercised
  // implicitly by every other still-passing selftest leg above).
  {
    std::string wd = workdir + "/internal_leak";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    // P1 wave: `mod()` USED to be the missing builtin this leg probed with —
    // it is now implemented (floored modulus, Stata-oracle-pinned), so the
    // rewrite is probed with a function the engine still does not provide.
    // The leg is about the ERROR SHAPE, not about which name is absent.
    auto jMod = runCmd(session, "gen region = 1 + digamma(_n)");
    auto errMod = firstBlockOfType(jMod, "error");
    check(!errMod.is_null(),
          "BUG-038(a): `gen ... digamma(_n)` (missing builtin) is an error block");
    check(errMod.value("message", std::string()) == "function digamma() is not supported",
          "BUG-038(a): unknown-function internal error rewrites to 'function digamma() is not "
          "supported'");
    check(!anyBlockTextContains(jMod, "[internal]"),
          "BUG-038(a): no leaked '[internal]' text in the unknown-function error");

    auto jGeneric = runCmd(session, "gen z = unemp +");
    auto errGeneric = firstBlockOfType(jGeneric, "error");
    check(!errGeneric.is_null(), "BUG-038(a): a malformed expression is an error block");
    check(errGeneric.value("message", std::string()).find("[internal]") == std::string::npos,
          "BUG-038(a): a generic internal error (not the unknown-function shape) also drops the "
          "'[internal]' tag");
    check(errGeneric.value("message", std::string()).rfind("engine error: ", 0) == 0,
          "BUG-038(a): a generic internal error gets the honest 'engine error: ...' wrapper");
  }

  // --- BUG-039: cobra exit(1) with a valid result JSON on stdout (a logit
  // stopped by iterate(1) before it converges) is returned as the honest
  // result, not discarded for a generic "[engine_error] cobra exited 1".
  // Built entirely from the bundled `fatalities` sample (no external fixture
  // dependency) — verified directly against `cobra logit --iterate 1`: exit
  // code 1, EMPTY stderr, a full `{"coefs":[...], "converged": false, ...}`
  // payload on stdout. (This leg used `logit ybin cst`, a constant regressor,
  // until BUG-1232: the engine now omits cst and converges, as StataBE does.)
  {
    std::string wd = workdir + "/exit1_json";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as mission 3 / BUG A above
      // (FIX-SELFTEST-ISOBUILD-CRASH): without fatalities.csv, the `logit`
      // leg below never produces a real "estimation" block, so `est` would
      // be null and the unguarded .value() calls would crash. Skip the
      // whole degenerate-logit leg honestly instead.
      skip("BUG-039 non-converged-logit leg", "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "gen ybin = spirits > 2");
      runCmd(session, "gen cst = 2");

      auto j = runCmd(session, "logit ybin beertax, iterate(1)");
      check(firstBlockOfType(j, "error").is_null(),
            "BUG-039: a degenerate (non-converged) `logit` is NOT rendered as a generic error block");
      auto est = firstBlockOfType(j, "estimation");
      check(!est.is_null(), "BUG-039: the non-converged logit still produces an `estimation` block");
      check(est.value("converged", true) == false,
            "BUG-039: the estimation block honestly reports `converged: false`");
      check(!est.value("coeftable", nlohmann::json::array()).empty(),
            "BUG-039: the honest non-convergence coeftable is present, not swallowed");
      check(!anyBlockTextContains(j, "cobra exited 1"),
            "BUG-039: no generic 'cobra exited 1' message anywhere in the result");

      // Regression: the session is still usable afterward (the honest result
      // did not leave the session/datastore in some half-broken state).
      auto j2 = runCmd(session, "gen z9 = cst + 1");
      check(firstBlockOfType(j2, "error").is_null(),
            "BUG-039 regression: a later `gen` still works after the non-converged logit");
    }
  }

  fs::remove_all(workdir, ec);
}


void testG_BUG371FeatureWaveLock() {
  std::printf("[selftest] --- BUG-371 lock: date family, decode, ds/unab, inspect, "
              "codebook compact, xtsum/xttab, xtile cutpoints, tabulate missing ---\n");
  namespace fs = std::filesystem;
  auto freshSession = [](const char* tag) {
    std::string wd =
        (fs::temp_directory_path() / (std::string("piet_native_selftest_") + tag)).u8string();
    std::error_code ec2;
    fs::remove_all(wd, ec2);
    fs::create_directories(wd, ec2);
    return wd;
  };
  auto displayText = [](const nlohmann::ordered_json& blocks) {
    const auto* d = firstBlockOfTypeD1(blocks, "display");
    return d == nullptr ? std::string() : d->value("text", std::string());
  };

  // ---- (a) the date family -------------------------------------------------
  // Reference values computed OUTSIDE this code (Python datetime) and pinned here.
  // The epoch, the Unix offset, a negative date, a leap day, and a date whose
  // weekday is independently known (the moon landing was a Sunday -> dow 0).
  {
    std::string workdir = freshSession("bug371a");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 1", session);
    struct DCase { const char* expr; const char* want; };
    const DCase cases[] = {
        {"mdy(1,1,1960)", "0"},        // Stata's epoch IS day zero
        {"mdy(1,1,1970)", "3653"},     // the Unix offset
        {"mdy(12,31,1959)", "-1"},     // dates before the epoch are NEGATIVE
        {"mdy(7,20,1969)", "3488"},
        {"mdy(2,29,2020)", "21974"},   // a real leap day
        {"year(21974)", "2020"},
        {"month(21974)", "2"},
        {"day(21974)", "29"},
        {"dow(3488)", "0"},            // the moon landing was a Sunday
        {"dow(0)", "5"},               // 01jan1960 was a Friday
        {"doy(23741)", "366"},         // 31dec2024, a leap year
        {"quarter(21974)", "1"},
        {"halfyear(23741)", "2"},
    };
    for (const auto& c : cases) {
      auto blocks = piet::dispatch(std::string("display ") + c.expr, session);
      const std::string got = displayText(blocks);
      check(got == c.want, std::string("BUG-371(a): display ") + c.expr + " == " + c.want +
                               " (got " + got + ")");
    }
    // An impossible date is MISSING, not the silently-normalised date the raw
    // arithmetic would hand back (Feb 30 would otherwise become Mar 1).
    {
      auto blocks = piet::dispatch("gen bad = mdy(2,30,2020)", session);
      check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-371(a): mdy(2,30,2020) evaluates");
      check(displayText(piet::dispatch("count if missing(bad)", session)) == "1",
            "BUG-371(a): mdy(2,30,2020) is MISSING — an impossible date is not normalised into "
            "a real one");
    }
  }
  // The two expression layers must agree: cobra's parser answers `gen`, PIET's
  // pyeval registry answers `display`. Same calendar, so the same answer.
  {
    std::string workdir = freshSession("bug371a2");
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("gen dt = mdy(1, 1, year)", session);
    (void)piet::dispatch("gen yr2 = year(dt)", session);
    check(displayText(piet::dispatch("count if yr2 != year", session)) == "0",
          "BUG-371(a): mdy() then year() round-trips through the ENGINE path for every row");
    auto fmt = piet::dispatch("format dt %td", session);
    check(firstBlockOfTypeD1(fmt, "error") == nullptr,
          "BUG-371(a): `format dt %td` now works — it used to cascade because dt never existed");
  }

  // ---- (b) decode: the inverse encode never had ----------------------------
  {
    std::string workdir = freshSession("bug371b");
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    (void)piet::dispatch("encode state, gen(stcode)", session);
    auto blocks = piet::dispatch("decode stcode, gen(stback)", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-371(b): `decode` runs");
    check(displayText(piet::dispatch("count if stback != state", session)) == "0",
          "BUG-371(b): encode->decode round-trips EXACTLY (0 rows differ from the original)");
    check(firstBlockOfTypeD1(piet::dispatch("decode state, gen(z)", session), "error") != nullptr,
          "BUG-371(b): decoding a STRING variable is refused (decode maps a numeric back through "
          "its value labels)");
  }

  // ---- (c) ds / unab -------------------------------------------------------
  {
    std::string workdir = freshSession("bug371c");
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    const std::string dsAll = displayText(piet::dispatch("ds", session));
    check(dsAll.find("beertax") != std::string::npos && dsAll.find("state") != std::string::npos,
          "BUG-371(c): bare `ds` lists every variable");
    const std::string dsGlob = displayText(piet::dispatch("ds fatal*", session));
    check(dsGlob == "fatal fatal1517 fatal1820 fatal2124",
          "BUG-371(c): `ds fatal*` expands the wildcard (got: " + dsGlob + ")");
    // r(varlist) is a STRING r-class result — the thing that makes ds usable from
    // a macro-driven do-file at all.
    (void)piet::dispatch("local v `r(varlist)'", session);
    check(session.eval.locals.get("v", "") == "fatal fatal1517 fatal1820 fatal2124",
          "BUG-371(c): `ds` sets r(varlist), and it survives into a local");
    // B5-D7: `ds VAR, not` -- r(varlist) becomes the COMPLEMENT (every OTHER
    // variable), preserving dataset column order. Measured B4REC|ds|not|0|
    // (tests/eval/golden/aer_b4/pins_oracle_r.log:163-164).
    const std::string dsNot = displayText(piet::dispatch("ds fatal*, not", session));
    // Whole-token comparison, not substring: this frame carries nfatal, sfatal, afatal,
    // nfatal1517 ... which all CONTAIN "fatal" and none of which MATCH the glob `fatal*`.
    // A substring test here fails on a correct complement (measured 2026-09-09).
    const std::string dsNotPadded = " " + dsNot + " ";
    bool dsNotExcludes = true;
    for (const char* matched : {" fatal ", " fatal1517 ", " fatal1820 ", " fatal2124 "}) {
      if (dsNotPadded.find(matched) != std::string::npos) dsNotExcludes = false;
    }
    check(dsNotExcludes,
          "B5-D7: `ds fatal*, not` excludes every variable the varlist matched (got: " + dsNot +
              ")");
    check(dsNot.find("beertax") != std::string::npos && dsNot.find("state") != std::string::npos,
          "B5-D7: `ds fatal*, not` includes the OTHER variables (got: " + dsNot + ")");
    (void)piet::dispatch("local vnot `r(varlist)'", session);
    check(session.eval.locals.get("vnot", "") == dsNot,
          "B5-D7: `ds ..., not` also sets r(varlist) to the complement, and it survives into a "
          "local");
    (void)piet::dispatch("unab u : fatal*", session);
    check(session.eval.locals.get("u", "") == "fatal fatal1517 fatal1820 fatal2124",
          "BUG-371(c): `unab u : fatal*` defines the local");
    check(firstBlockOfTypeD1(piet::dispatch("unab bad : nosuchvar", session), "error") != nullptr,
          "BUG-371(c): `unab` on a name that does not exist is an ERROR, not a local naming a "
          "column the dataset lacks");
  }

  // ---- (d) inspect ---------------------------------------------------------
  {
    std::string workdir = freshSession("bug371d");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 5", session);
    (void)piet::dispatch("gen x = _n - 3", session);       // -2 -1 0 1 2
    (void)piet::dispatch("replace x = . in 5", session);   // -2 -1 0 1 .
    const std::string t = displayText(piet::dispatch("inspect x", session));
    // 2 negative, 1 zero, 1 positive, 1 missing, 4 total, all integers.
    check(t.find("Negative") != std::string::npos && t.find("Missing") != std::string::npos,
          "BUG-371(d): `inspect` emits its table");
    auto rowOf = [&t](const char* label) {
      size_t p = t.find(label);
      if (p == std::string::npos) return std::string();
      size_t e = t.find('\n', p);
      return t.substr(p, (e == std::string::npos ? t.size() : e) - p);
    };
    // BUG-377 changed the LAYOUT to Stata's own: three count columns
    // (Total / Integers / Nonintegers) with "-" for a zero cell, and no
    // percentages anywhere. The COUNTS are the same; only the rendering moved,
    // so this re-pins them against the new shape rather than dropping the check.
    //   x = {-2, -1, 0, 1, .}  ->  2 negative, 1 zero, 1 positive, 1 missing,
    //                              4 integers, 0 nonintegers
    check(rowOf("Negative").find("2") != std::string::npos &&
              rowOf("Zero").find("1") != std::string::npos &&
              rowOf("Positive").find("1") != std::string::npos &&
              rowOf("Missing").find("1") != std::string::npos,
          "BUG-371(d): inspect counts negative=2 zero=1 positive=1 missing=1");
    // This checks the LAYOUT only. The counts themselves are pinned exactly by
    // the BUG-373 r-class lock (r(N)/r(N_neg)/... against the live Stata capture)
    // and by check (d) above, so re-deriving them from the rendered text here
    // would only add a fragile second copy — the first attempt at it matched the
    // COLUMN HEADER instead of the row, twice.
    check(t.find("Integers") != std::string::npos &&
              t.find("Nonintegers") != std::string::npos &&
              t.find("unique values") != std::string::npos,
          "BUG-377: inspect renders Stata's three count columns (Total / Integers / "
          "Nonintegers) and the unique-values line");
    check(t.find('%') == std::string::npos,
          "BUG-377: inspect prints NO percentages — real Stata prints counts, and the old "
          "rendering was a single percentage column");
    check(rowOf("Negative").find("2") != std::string::npos,
          "BUG-377: the Negative row still carries its count (2) after the layout change");
  }

  // ---- (e) codebook, compact ----------------------------------------------
  {
    std::string workdir = freshSession("bug371e");
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto blocks = piet::dispatch("codebook beertax, compact", session);
    const auto* de = firstBlockOfTypeD1(blocks, "describe");
    check(de != nullptr && de->value("compact", false),
          "BUG-371(e): `codebook, compact` is implemented and flags the block");
    bool hasStats = de != nullptr && de->contains("variables") &&
                    (*de)["variables"].size() == 1 && (*de)["variables"][0].contains("mean") &&
                    (*de)["variables"][0].contains("n_unique");
    check(hasStats,
          "BUG-371(e): the compact block already carries obs/unique/mean — nothing new is "
          "computed for it");
  }

  // ---- (f) xtsum / xttab ---------------------------------------------------
  {
    std::string workdir = freshSession("bug371f");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 4", session);
    (void)piet::dispatch("gen id = cond(_n<=2, 1, 2)", session);
    (void)piet::dispatch("gen t = cond(_n<=2, _n, _n-2)", session);
    (void)piet::dispatch("gen y = _n", session);            // 1 2 3 4
    (void)piet::dispatch("xtset id t", session);
    auto blocks = piet::dispatch("xtsum y", session);
    const auto* d = firstBlockOfTypeD1(blocks, "display");
    check(d != nullptr && d->contains("xtsum"), "BUG-371(f): `xtsum` emits its stats");
    if (d != nullptr && d->contains("xtsum") && (*d)["xtsum"].contains("y")) {
      const auto& st = (*d)["xtsum"]["y"];
      // Hand-checkable: means are 1.5 and 3.5, grand mean 2.5.
      //   between sd = sd(1.5, 3.5) = sqrt(2) ~ 1.41421356
      //   within values = y - unit mean + 2.5 = {2,3,2,3}, sd = sqrt(1/3) ~ 0.57735
      const double bsd = st.value("sd_between", -1.0);
      const double wsd = st.value("sd_within", -1.0);
      check(st.value("N", -1) == 4 && st.value("n_units", -1) == 2 &&
                std::fabs(st.value("T_bar", -1.0) - 2.0) < 1e-12,
            "BUG-371(f): xtsum N=4, n=2 units, T-bar=2");
      check(std::fabs(bsd - std::sqrt(2.0)) < 1e-9,
            "BUG-371(f): BETWEEN sd is the sd of the UNIT MEANS (sqrt 2), not of the raw column");
      check(std::fabs(wsd - std::sqrt(1.0 / 3.0)) < 1e-9,
            "BUG-371(f): WITHIN sd is the sd of y - unit mean + grand mean");
    }
    auto tb = piet::dispatch("xttab t", session);
    const auto* d2 = firstBlockOfTypeD1(tb, "display");
    check(d2 != nullptr && d2->contains("xttab") && (*d2)["xttab"].size() == 2 &&
              (*d2)["xttab"][0].value("units", -1) == 2,
          "BUG-371(f): xttab reports how many UNITS ever take each value (both units take t==1)");
    // Without xtset there is no unit, so the decomposition is meaningless — refuse.
    {
      std::string wd2 = freshSession("bug371f2");
      piet::SessionState s2(wd2);
      (void)piet::dispatch("clear", s2);
      (void)piet::dispatch("set obs 3", s2);
      (void)piet::dispatch("gen y = _n", s2);
      check(firstBlockOfTypeD1(piet::dispatch("xtsum y", s2), "error") != nullptr,
            "BUG-371(f): `xtsum` without xtset is refused — between/within need a unit");
    }
  }

  // ---- (g) xtile cutpoints() ----------------------------------------------
  {
    std::string workdir = freshSession("bug371g");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 10", session);
    (void)piet::dispatch("gen x = _n", session);
    (void)piet::dispatch("gen c = .", session);
    (void)piet::dispatch("replace c = 3 in 1", session);
    (void)piet::dispatch("replace c = 7 in 2", session);
    auto blocks = piet::dispatch("xtile g = x, cutpoints(c)", session);
    check(firstBlockOfTypeD1(blocks, "error") == nullptr, "BUG-371(g): `xtile, cutpoints()` runs");
    // Edges 3 and 7 -> group 1 for x<=3 (3 rows), 2 for 3<x<=7 (4), 3 for x>7 (3).
    check(displayText(piet::dispatch("count if g==1", session)) == "3" &&
              displayText(piet::dispatch("count if g==2", session)) == "4" &&
              displayText(piet::dispatch("count if g==3", session)) == "3",
          "BUG-371(g): cutpoints(3,7) bin 10 rows as 3/4/3 — the edges come from the VARIABLE, "
          "not from quantiles");
    check(firstBlockOfTypeD1(piet::dispatch("xtile h = x, cutpoints(c) nquantiles(3)", session),
                             "error") != nullptr,
          "BUG-371(g): cutpoints() and nquantiles() together are refused — they are alternative "
          "sources of the same edges");
  }

  // ---- (h) tabulate, missing ----------------------------------------------
  {
    std::string workdir = freshSession("bug371h");
    piet::SessionState session(workdir);
    (void)piet::dispatch("clear", session);
    (void)piet::dispatch("set obs 5", session);
    (void)piet::dispatch("gen g = cond(_n<=2, 1, 2)", session);
    (void)piet::dispatch("replace g = . in 5", session);   // 1 1 2 2 .
    auto plain = piet::dispatch("tabulate g", session);
    auto withM = piet::dispatch("tabulate g, missing", session);
    const auto* p1 = firstBlockOfTypeD1(plain, "tabulate");
    const auto* p2 = firstBlockOfTypeD1(withM, "tabulate");
    int64_t t1 = (p1 != nullptr) ? (*p1)["table"]["margins"].value("total", -1LL) : -1;
    int64_t t2 = (p2 != nullptr) ? (*p2)["table"]["margins"].value("total", -1LL) : -1;
    check(t1 == 4 && t2 == 5,
          "BUG-371(h): `tabulate g` counts 4 rows and `tabulate g, missing` counts 5 — the "
          "missing row becomes a category instead of being dropped");
    bool hasDot = false;
    if (p2 != nullptr)
      for (const auto& r : (*p2)["table"]["rows"])
        if (r.get<std::string>() == ".") hasDot = true;
    check(hasDot, "BUG-371(h): the missing category is labelled `.` and sorts LAST");
  }
}
