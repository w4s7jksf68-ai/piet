// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: settings and execution.
#include "selftest_support.h"
#include "selftest_cases.h"


// ── 2.2.0 item D: settings persistence round-trip (SPEC II-1/II-2) ───────────
// settings.set persists a partial; a FRESH settings::load() reads it back off
// disk (not the in-memory echo); settings.get reports detected system info;
// compute_cores routes to CECON_NUM_THREADS (the engine subprocess cap). The
// store is isolated to a throwaway $PIET_SETTINGS_FILE so a gate run never
// touches real user settings.
// SPEC II-4 (2026-09-02): the memory pre-flight. The cost model is pinned as
// arithmetic; the gate is driven end-to-end through the real dispatcher on a
// headless session (no Shell -> it cannot ask): strict refuses and leaves the
// data untouched, default warns (an echo block) and proceeds, off is silent,
// and an operation under budget never says a word.
void testMemoryPreflight() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path tmp = fs::temp_directory_path() / "piet_native_selftest_preflight";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  std::string settingsFile = (tmp / "settings.json").u8string();
#ifndef _WIN32
  setenv("PIET_SETTINGS_FILE", settingsFile.c_str(), 1);
  unsetenv("PIET_PREFLIGHT");
#else
  _putenv_s("PIET_SETTINGS_FILE", settingsFile.c_str());
  _putenv_s("PIET_PREFLIGHT", "");
#endif
  using piet::preflight::estimateBytes;
  check(estimateBytes("merge", 100, 50, 0) == 100 + 50 + 200, "II-4 model: merge = master + using + 2 x max");
  check(estimateBytes("joinby", 100, 300, 0) == 100 + 300 + 600, "II-4 model: joinby = master + using + 2 x max");
  check(estimateBytes("append", 100, 50, 0) == 300, "II-4 model: append = 2 x (master + using)");
  check(estimateBytes("reshape", 100, 0, 0) == 300, "II-4 model: reshape = 3 x master");
  check(estimateBytes("expand", 100, 0, 5) == 600, "II-4 model: expand k = master x (1 + k)");
  check(estimateBytes("cross", 100, 50, 0) == 100 + 50 + 800, "II-4 model: cross = master + using + 8 x max");
  check(piet::preflight::budgetBytes() > 0, "II-4: an unset budget resolves to the recommended one (never 0)");

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  auto blockOf = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    for (const auto& b : j.value("blocks", nlohmann::json::array()))
      if (b.value("type", std::string()) == type) return b;
    return nlohmann::json();
  };
  auto hasNote = [](const nlohmann::json& j) {
    for (const auto& b : j.value("blocks", nlohmann::json::array()))
      if (b.value("type", std::string()) == "echo" &&
          b.value("text", std::string()).find("memory pre-flight") != std::string::npos)
        return true;
    return false;
  };

  {
    nlohmann::ordered_json body;
    body["memory_budget_mb"] = 1;  // 1 MB: fatalities x 20 (~2 MB) is over, x 2 is under
    check(piet::routes::settingsSet(body.dump()).ok, "II-4: settings.set(memory_budget_mb=1) ok");
  }
  fs::path wd = tmp / "wd";
  fs::create_directories(wd, ec);
  piet::SessionState session(wd.u8string());
  runCmd(session, "sysuse fatalities, clear");
  const int64_t rows0 = session.datastore() ? session.datastore()->nRows() : 0;
  check(rows0 > 0, "II-4: fixture loaded");

  // strict -> refuse, data untouched
#ifndef _WIN32
  setenv("PIET_PREFLIGHT", "strict", 1);
#else
  _putenv_s("PIET_PREFLIGHT", "strict");
#endif
  {
    auto j = runCmd(session, "expand 20");
    auto e = blockOf(j, "error");
    check(!e.is_null() && e.value("code", std::string()) == "memory_preflight",
          "II-4 strict: an over-budget expand is REFUSED with code memory_preflight");
    check(session.datastore() && session.datastore()->nRows() == rows0,
          "II-4 strict: the refused expand changed nothing (row count unchanged)");
  }
  // default -> warn (echo block + stderr) and proceed
#ifndef _WIN32
  unsetenv("PIET_PREFLIGHT");
#else
  _putenv_s("PIET_PREFLIGHT", "");
#endif
  {
    auto j = runCmd(session, "expand 20");
    check(blockOf(j, "error").is_null(), "II-4 default (cannot ask): an over-budget expand still runs");
    check(hasNote(j), "II-4 default (cannot ask): ...and says so in an echo block (memory pre-flight note)");
    check(session.datastore() && session.datastore()->nRows() == rows0 * 20,
          "II-4 default: the expand actually happened (rows x 20)");
  }
  // under budget -> silent
  {
    nlohmann::ordered_json body;
    body["memory_budget_mb"] = 4096;
    check(piet::routes::settingsSet(body.dump()).ok, "II-4: settings.set(memory_budget_mb=4096) ok");
    fs::create_directories(tmp / "wd2", ec);
    piet::SessionState s2((tmp / "wd2").u8string());
    runCmd(s2, "sysuse fatalities, clear");
    auto j = runCmd(s2, "expand 2");
    check(blockOf(j, "error").is_null() && !hasNote(j), "II-4: an operation inside the budget says nothing");
  }
  // off -> silent even over budget
  {
    nlohmann::ordered_json body;
    body["memory_budget_mb"] = 1;
    piet::routes::settingsSet(body.dump());
#ifndef _WIN32
    setenv("PIET_PREFLIGHT", "off", 1);
#else
    _putenv_s("PIET_PREFLIGHT", "off");
#endif
    fs::create_directories(tmp / "wd3", ec);
    piet::SessionState s3((tmp / "wd3").u8string());
    runCmd(s3, "sysuse fatalities, clear");
    auto j = runCmd(s3, "expand 20");
    check(blockOf(j, "error").is_null() && !hasNote(j), "II-4 off: the gate is silent even over budget");
  }
#ifndef _WIN32
  unsetenv("PIET_PREFLIGHT");
  unsetenv("PIET_SETTINGS_FILE");
  unsetenv("CECON_MEMORY_BUDGET_MB");
  unsetenv("PIET_OWNS_CECON_MEMORY_BUDGET_MB");
#else
  _putenv_s("PIET_PREFLIGHT", "");
  _putenv_s("PIET_SETTINGS_FILE", "");
  _putenv_s("CECON_MEMORY_BUDGET_MB", "");
  _putenv_s("PIET_OWNS_CECON_MEMORY_BUDGET_MB", "");
#endif
  fs::remove_all(tmp, ec);
}


void testSettingsPersistence() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path tmp = fs::temp_directory_path() / "piet_native_selftest_settings";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  std::string settingsFile = (tmp / "settings.json").u8string();
#ifndef _WIN32
  setenv("PIET_SETTINGS_FILE", settingsFile.c_str(), 1);
  unsetenv("CECON_NUM_THREADS");
#else
  _putenv_s("PIET_SETTINGS_FILE", settingsFile.c_str());
  _putenv_s("CECON_NUM_THREADS", "");
#endif

  {
    auto r = piet::routes::settingsGet("{}");
    check(r.ok, "settings.get returns ok on a fresh store");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("memory_budget_mb", -1) == 0,
          "settings.get default memory_budget_mb == 0 (means: use recommended)");
    check(j.contains("sys") && j["sys"].value("cpu_count", 0) > 0,
          "settings.get reports detected cpu_count > 0 (hardware detection, SPEC II-1)");
    check(j.contains("sys") && j["sys"].value("recommended_budget_mb", 0) > 0,
          "settings.get reports a recommended budget > 0 (SPEC II-2)");
    check(j.value("settings_file", std::string()) == settingsFile,
          "settings.get echoes the isolated settings_file path");
  }

  {
    nlohmann::ordered_json body;
    body["scratch_path"] = "/tmp/piet_scratch_choice";
    body["default_working_dir"] = "/tmp/piet_work_choice";
    body["memory_budget_mb"] = 1536;
    body["compute_cores"] = 3;
    auto rs = piet::routes::settingsSet(body.dump());
    check(rs.ok, "settings.set returns ok");

    piet::settings::Settings s2 = piet::settings::load();  // fresh read off disk
    check(s2.scratchPath == "/tmp/piet_scratch_choice", "settings round-trip: scratch_path persisted");
    check(s2.defaultWorkingDir == "/tmp/piet_work_choice",
          "settings round-trip: default_working_dir persisted");
    check(s2.memoryBudgetMb == 1536, "settings round-trip: memory_budget_mb persisted");
    check(s2.computeCores == 3, "settings round-trip: compute_cores persisted");

    const char* cec = std::getenv("CECON_NUM_THREADS");
    check(cec != nullptr && std::string(cec) == "3",
          "settings.set routes compute_cores -> CECON_NUM_THREADS=3 (engine subprocess cap)");
    const char* vl = std::getenv("VECLIB_MAXIMUM_THREADS");
    check(vl != nullptr && std::string(vl) == "3",
          "settings.set routes compute_cores -> VECLIB_MAXIMUM_THREADS=3 (Accelerate BLAS follows the core budget)");
    const char* mem = std::getenv("CECON_MEMORY_BUDGET_MB");
    check(mem != nullptr && std::string(mem) == "1536",
          "settings.set routes memory_budget_mb -> CECON_MEMORY_BUDGET_MB=1536 (engine memory budget)");
  }

  // BUG-386: returning to Auto must RELEASE the cap PIET applied earlier in the
  // same process (it used to stay at the old value until a restart), and an
  // auto memory budget must still export the recommended number — the engine
  // reads an undeclared budget as unbounded.
  {
    nlohmann::ordered_json body;
    body["compute_cores"] = 0;
    body["memory_budget_mb"] = 0;
    auto rs = piet::routes::settingsSet(body.dump());
    check(rs.ok, "settings.set(auto, auto) returns ok");
    check(std::getenv("CECON_NUM_THREADS") == nullptr,
          "BUG-386: compute_cores=auto releases the CECON_NUM_THREADS cap PIET set earlier");
    check(std::getenv("VECLIB_MAXIMUM_THREADS") == nullptr,
          "BUG-386: compute_cores=auto releases VECLIB_MAXIMUM_THREADS too");
    const char* mem = std::getenv("CECON_MEMORY_BUDGET_MB");
    const auto sys = piet::settings::detectSysInfo();
    check(mem != nullptr && std::string(mem) == std::to_string(sys.recommendedBudgetMb),
          "memory_budget_mb=auto exports the recommended budget (half of RAM) to the engine");
  }

  // Ownership rule: a pin PIET did not set (a gate's, a harness's, a parent
  // `parallel` block's per-worker share) survives Auto; an explicit user value
  // overrides it; and Auto then releases only what PIET set.
  {
#ifndef _WIN32
    setenv("CECON_NUM_THREADS", "7", 1);
    setenv("CECON_MEMORY_BUDGET_MB", "123", 1);
#else
    _putenv_s("CECON_NUM_THREADS", "7");
    _putenv_s("CECON_MEMORY_BUDGET_MB", "123");
#endif
    piet::settings::applyComputeCores(0);
    const char* v = std::getenv("CECON_NUM_THREADS");
    check(v != nullptr && std::string(v) == "7",
          "Auto leaves a CECON_NUM_THREADS pin PIET did not set exactly as found");
    piet::settings::applyMemoryBudget(0);
    const char* m = std::getenv("CECON_MEMORY_BUDGET_MB");
    check(m != nullptr && std::string(m) == "123",
          "Auto leaves a CECON_MEMORY_BUDGET_MB pin PIET did not set exactly as found");
    piet::settings::applyComputeCores(2);
    v = std::getenv("CECON_NUM_THREADS");
    check(v != nullptr && std::string(v) == "2", "an explicit compute_cores overrides a foreign pin (the user acted)");
    piet::settings::applyMemoryBudget(999);
    m = std::getenv("CECON_MEMORY_BUDGET_MB");
    check(m != nullptr && std::string(m) == "999", "an explicit memory_budget_mb overrides a foreign pin");
    piet::settings::applyComputeCores(0);
    check(std::getenv("CECON_NUM_THREADS") == nullptr, "...and Auto releases what PIET set");
  }

  // P6: provenance_hash round-trip — OFF (the interactive default) makes
  // estimation/estimation_support.cpp's shared runCobraJsonM4 append --no-provenance to
  // every M4a cobra call; ON leaves the engine's own CLI default
  // (provenance ON) untouched. Exercised end-to-end through the real
  // dispatcher + cobra subprocess (logit, M4a-scoped) rather than
  // re-deriving the decision, so a real wiring regression in
  // estimation/estimation_support.cpp would actually fail this leg. cobra's raw JSON
  // carries a top-level `provenance` object (argv / cobra_version / a data
  // hash) when not suppressed — confirmed directly against the pinned
  // cobra 0.20.3 binary (`--no-provenance` drops the key; its absence
  // leaves the key present) — and estimation/panel_estimation.cpp's cacheLastModelM4
  // caches that raw JSON verbatim onto LastModel::result (no allow-list
  // filtering like the ResultBlock returned to the frontend has), so it is
  // the one place in the public SessionState surface this native-only
  // setting is observable.
  {
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

    fs::path wd2 = tmp / "provenance_probe";
    fs::create_directories(wd2, ec);
    piet::SessionState session(wd2.u8string());
    runCmd(session, "sysuse fatalities, clear");
    runCmd(session, "gen ybin = spirits > 2");
    runCmd(session, "gen cst = 2");

    // provenanceHash defaults to false (OFF) on a fresh Settings{}; set it
    // explicitly so this leg doesn't depend on struct-default drift.
    nlohmann::ordered_json offBody;
    offBody["provenance_hash"] = false;
    auto rsOff = piet::routes::settingsSet(offBody.dump());
    check(rsOff.ok, "provenance round-trip: settings.set(provenance_hash=false) ok");

    auto jOff = runCmd(session, "logit ybin cst");
    check(firstBlockOfType(jOff, "error").is_null(), "provenance OFF: logit did not error");
    auto lmOff = session.lastModel();
    check(lmOff.has_value() && lmOff->result.has_value(),
          "provenance OFF: logit cached a raw cobra result");
    check(lmOff.has_value() && lmOff->result.has_value() && !lmOff->result->contains("provenance"),
          "provenance round-trip: OFF -> argv carried --no-provenance (provenance{} block absent)");

    nlohmann::ordered_json onBody;
    onBody["provenance_hash"] = true;
    auto rsOn = piet::routes::settingsSet(onBody.dump());
    check(rsOn.ok, "provenance round-trip: settings.set(provenance_hash=true) ok");

    auto jOn = runCmd(session, "logit ybin cst");
    check(firstBlockOfType(jOn, "error").is_null(), "provenance ON: logit did not error");
    auto lmOn = session.lastModel();
    check(lmOn.has_value() && lmOn->result.has_value(),
          "provenance ON: logit cached a raw cobra result");
    check(lmOn.has_value() && lmOn->result.has_value() && lmOn->result->contains("provenance"),
          "provenance round-trip: ON -> engine default provenance{} block is present");
  }

#ifndef _WIN32
  unsetenv("PIET_SETTINGS_FILE");
  unsetenv("CECON_NUM_THREADS");
  unsetenv("VECLIB_MAXIMUM_THREADS");
  unsetenv("CECON_MEMORY_BUDGET_MB");
  unsetenv("PIET_OWNS_CECON_NUM_THREADS");
  unsetenv("PIET_OWNS_VECLIB_MAXIMUM_THREADS");
  unsetenv("PIET_OWNS_CECON_MEMORY_BUDGET_MB");
#else
  _putenv_s("PIET_SETTINGS_FILE", "");
  _putenv_s("CECON_NUM_THREADS", "");
  _putenv_s("VECLIB_MAXIMUM_THREADS", "");
  _putenv_s("CECON_MEMORY_BUDGET_MB", "");
  _putenv_s("PIET_OWNS_CECON_NUM_THREADS", "");
  _putenv_s("PIET_OWNS_VECLIB_MAXIMUM_THREADS", "");
  _putenv_s("PIET_OWNS_CECON_MEMORY_BUDGET_MB", "");
#endif
  fs::remove_all(tmp, ec);
}


// ── P8.1 Wave 2 (contract_B 2.4.0) native lane ──────────────────────────────
// Exercises the headless-reachable core of every W2 native task directly
// against the routes/doexec surfaces (no window):
//   N1  file.save writer — text roundtrip, content_b64 binary roundtrip, and
//       the HONEST-failure contract (a failed write is {type:error}, never a
//       false {ok:true}); malformed base64 / missing-payload rejects.
//   N7  var_detail — numeric moments present + string-col omission; unknown
//       name → engine error.
//   N2  settings ui_state set→get roundtrip + the ≤64KB oversize reject
//       (prior blob left intact); default_export_dir (N1) roundtrip.
//   N6  live-block streaming onBlock hook — seq monotone-from-0, count ==
//       final blocks.size(), and a with/without-hook FREEZE equality.
//   N3a a failing csv `use` surfaces cobra's OWN message, not the generic
//       "conversion failed for <path>" (the dead-catch, BUG-082).
void testP81Wave2NativeLane() {
  std::printf("[selftest] --- P8.1 W2 native lane (file.save / var_detail / ui_state / streaming) ---\n");
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path tmp = fs::temp_directory_path() / "piet_native_selftest_p81w2";
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  // --- N1: file.save explicit-path text write + content roundtrip ---
  {
    std::string dest = (tmp / "export_roundtrip.log").u8string();
    std::string payload = "line1\nline2\tTAB\n\xE2\x9C\x93 unicode ok\n";
    auto r = piet::routes::fileSaveToPath(dest, "log", payload, std::nullopt);
    check(r.ok, "N1 file.save(text): route returns ok");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("ok", false) && j.value("path", std::string()) == dest,
          "N1 file.save(text): response is {ok:true, path:<dest>}");
    std::ifstream in(cecon::to_native_path(dest), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    check(ss.str() == payload, "N1 file.save(text): bytes on disk are byte-identical to the payload");
  }

  // --- N1: file.save content_b64 BINARY write (embedded NUL + high byte) ---
  {
    std::string dest = (tmp / "export_binary.png").u8string();
    std::string raw("PIET\x00\xFF", 6);  // 6 bytes incl. NUL and 0xFF
    std::string b64 = piet::base64Encode(raw);
    auto r = piet::routes::fileSaveToPath(dest, "png", std::nullopt, b64);
    check(r.ok, "N1 file.save(content_b64): route returns ok");
    std::ifstream in(cecon::to_native_path(dest), std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(got == raw,
          "N1 file.save(content_b64): decoded bytes on disk match the pre-encode payload (NUL+0xFF survive)");
  }

  // --- N1: honest failures — never a false {ok:true} ---
  {
    std::string bad = (tmp / "no_such_dir" / "x.log").u8string();  // parent absent
    auto r = piet::routes::fileSaveToPath(bad, "log", std::string("data"), std::nullopt);
    auto j = nlohmann::json::parse(r.json);
    check(j.value("type", std::string()) == "error",
          "N1 file.save(unwritable path): honest {type:error}, never a false success (COMMS section 4.8)");
    check(!fs::exists(fs::path(bad), ec), "N1 file.save(unwritable path): no file created");
  }
  {
    std::string dest = (tmp / "neither.log").u8string();
    auto r = piet::routes::fileSaveToPath(dest, "log", std::nullopt, std::nullopt);
    check(nlohmann::json::parse(r.json).value("type", std::string()) == "error",
          "N1 file.save(no text and no content_b64): honest error");
  }
  {
    std::string dest = (tmp / "badb64.png").u8string();
    auto r = piet::routes::fileSaveToPath(dest, "png", std::nullopt, std::string("@@@ not base64 @@@"));
    check(nlohmann::json::parse(r.json).value("type", std::string()) == "error",
          "N1 file.save(malformed content_b64): honest error, no garbage file");
  }

  // --- N7: var_detail known (numeric moments) / string omit / unknown ---
  {
    fs::path wd = tmp / "vardetail";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd.u8string());
    {
      nlohmann::json b{{"cmd", "sysuse fatalities, clear"}};
      (void)piet::routes::command(session, b.dump());
    }
    if (!session.hasData()) {
      skip("N7 var_detail leg", "sysuse fatalities unavailable in this environment");
    } else {
      {
        nlohmann::json body{{"name", "spirits"}};
        auto r = piet::routes::varDetail(session, body.dump());
        check(r.ok, "N7 var_detail(spirits): route returns ok");
        auto j = nlohmann::json::parse(r.json);
        check(j.value("name", std::string()) == "spirits" && j.value("dtype", std::string()) == "f64",
              "N7 var_detail(spirits): name+dtype echoed");
        check(j.contains("N") && j.contains("n_missing") && j.contains("mean") && j.contains("sd") &&
                  j.contains("min") && j.contains("max"),
              "N7 var_detail(spirits): numeric col carries N, n_missing, mean, sd, min, max");
        check(j.contains("min") && j.contains("max") && j["max"].get<double>() >= j["min"].get<double>(),
              "N7 var_detail(spirits): max >= min");
      }
      {
        nlohmann::json body{{"name", "no_such_var"}};
        auto r = piet::routes::varDetail(session, body.dump());
        check(nlohmann::json::parse(r.json).value("type", std::string()) == "error",
              "N7 var_detail(unknown): honest {type:error}, not a fabricated row");
      }
    }
  }

  // --- N2: ui_state set→get roundtrip + ≤64KB oversize reject ---
  {
    std::string settingsFile = (tmp / "settings_uistate.json").u8string();
#ifndef _WIN32
    setenv("PIET_SETTINGS_FILE", settingsFile.c_str(), 1);
#else
    _putenv_s("PIET_SETTINGS_FILE", settingsFile.c_str());
#endif
    std::string blob = R"({"panels":{"left":320},"theme":"dark"})";
    nlohmann::ordered_json setBody;
    setBody["ui_state"] = blob;
    auto rs = piet::routes::settingsSet(setBody.dump());
    check(rs.ok, "N2 settings.set(ui_state): ok");
    auto rg = piet::routes::settingsGet("{}");
    check(nlohmann::json::parse(rg.json).value("ui_state", std::string()) == blob,
          "N2 ui_state roundtrip: settings.get returns the exact opaque blob back");

    nlohmann::ordered_json bigBody;
    bigBody["ui_state"] = std::string(64 * 1024 + 1, 'x');
    auto rbig = piet::routes::settingsSet(bigBody.dump());
    check(!rbig.ok, "N2 settings.set(ui_state > 64KB): rejected with a bridge error");
    auto rg2 = piet::routes::settingsGet("{}");
    check(nlohmann::json::parse(rg2.json).value("ui_state", std::string()) == blob,
          "N2 ui_state oversize reject leaves the prior blob intact");

    // N1: default_export_dir roundtrip (same store).
    nlohmann::ordered_json edBody;
    edBody["default_export_dir"] = "/tmp/piet_exports";
    auto red = piet::routes::settingsSet(edBody.dump());
    check(red.ok, "N1 settings.set(default_export_dir): ok");
    auto rg3 = piet::routes::settingsGet("{}");
    check(nlohmann::json::parse(rg3.json).value("default_export_dir", std::string()) == "/tmp/piet_exports",
          "N1 default_export_dir roundtrip via settings.get");
#ifndef _WIN32
    unsetenv("PIET_SETTINGS_FILE");
#else
    _putenv_s("PIET_SETTINGS_FILE", "");
#endif
  }

  // --- N6: live-block streaming onBlock hook ---
  {
    fs::path wd = tmp / "stream";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd.u8string());
    std::string src = "display 1\ndisplay 2\ndisplay 3\n";
    int seqCount = 0;
    std::vector<int> seqs;
    auto onBlock = [&](const nlohmann::ordered_json& b) {
      (void)b;
      seqs.push_back(seqCount++);
    };
    auto streamed = piet::doexec::executeDoSource(src, session, piet::dispatch, {}, /*progressOwner=*/true,
                                                  std::nullopt, onBlock);
    check(seqCount == static_cast<int>(streamed.blocks.size()),
          "N6 streaming: onBlock fired exactly once per final block (streamed count == blocks.size())");
    bool monotone = seqs.size() == streamed.blocks.size();
    for (size_t i = 0; i < seqs.size(); ++i)
      if (seqs[i] != static_cast<int>(i)) monotone = false;
    check(monotone, "N6 streaming: seq is monotone from 0 with no gaps");

    piet::SessionState session2(wd.u8string());
    auto plain = piet::doexec::executeDoSource(src, session2, piet::dispatch, {}, /*progressOwner=*/true,
                                               std::nullopt, nullptr);
    check(plain.blocks == streamed.blocks,
          "N6 streaming: blocks array is byte-identical with and without the onBlock hook (freeze)");
  }

  // --- N3a (BUG-082): failing csv `use` surfaces cobra's own message ---
  {
    fs::path wd = tmp / "csvfail";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd.u8string());
    std::string bad = (wd / "garbage.csv").u8string();
    {
      std::ofstream g(cecon::to_native_path(bad), std::ios::binary);
      const unsigned char junk[] = {0x00, 0x01, 0x02, 0xff, 0xfe, 0x00};
      g.write(reinterpret_cast<const char*>(junk), sizeof(junk));
    }
    nlohmann::json body{{"filepath", bad}};
    auto r = piet::routes::open(session, body.dump());
    auto j = nlohmann::json::parse(r.json);
    if (j.value("type", std::string()) == "error") {
      std::string msg = j.value("message", std::string());
      check(msg.find("conversion failed for") == std::string::npos,
            "N3a: failing csv `use` surfaces cobra's OWN diagnostic, not the generic "
            "'conversion failed for <path>' (BUG-082 dead-catch fixed)");
    } else {
      skip("N3a csv-fail leg", "this cobra build converted the binary-garbage input without error");
    }
  }

  // --- PP-L2A / K1a: same-file `use` conversion cache -----------------------
  // The csv branch is the whole K1a mechanism minus the label sidecar (the dta
  // branch differs only in also memoizing metaPath), and it is hermetic — the
  // fixture is authored right here, so these legs need no corpus.
  //
  // Two gate classes, the S1b/C4-L2A law: the ANSWER is identical by design on
  // every arm, so only a MECHANISM probe can see the fast path die. The probe
  // is currentHbin(): a conversion mints a fresh scratchSeqSuffix name, a cache
  // hit re-opens the previous one, so path identity across two `use`s IS the
  // hit, and path difference IS the conversion. Backed by the scratch file
  // count, which cannot be faked by a stale pointer.
  {
    fs::path wd = tmp / "k1a_usecache";
    fs::create_directories(wd, ec);
    const std::string csv = (wd / "k1a.csv").u8string();
    auto writeCsv = [&](const std::string& body) {
      std::ofstream f(cecon::to_native_path(csv), std::ios::binary | std::ios::trunc);
      f << body;
    };
    auto countLoadedHbins = [&]() {
      int n = 0;
      std::error_code cec;
      for (const auto& e : fs::directory_iterator(wd, cec)) {
        const std::string name = e.path().filename().u8string();
        if (name.rfind("loaded", 0) == 0 && e.path().extension() == ".hbin") ++n;
      }
      return n;
    };
    auto useOnce = [&](piet::SessionState& s) {
      nlohmann::json body{{"filepath", csv}};
      return nlohmann::json::parse(piet::routes::open(s, body.dump()).json);
    };

    writeCsv("id,x\n1,10\n2,20\n3,30\n");
    piet::SessionState session(wd.u8string());
    nlohmann::json u1 = useOnce(session);
    if (u1.value("action", std::string()) != "open") {
      skip("PP-L2A K1a use-cache gates", "this cobra build could not convert the csv fixture");
    } else {
      const std::string h1 = session.currentHbin().value_or("");
      check(countLoadedHbins() == 1,
            "PP-L2A K1a setup: the first `use` of a .csv converts exactly once (one loaded*.hbin "
            "in the session workdir)");

      // 1) HIT — MECHANISM. Same unchanged file, so the stat key matches and
      //    the memo is re-opened: no second conversion, no second scratch file.
      nlohmann::json u2 = useOnce(session);
      check(session.currentHbin().value_or("") == h1,
            "PP-L2A K1a MECHANISM gate: a re-`use` of the UNCHANGED source re-opens the memoized "
            "conversion output (currentHbin identical) instead of converting again — the fast "
            "path no value pin can see");
      check(countLoadedHbins() == 1,
            "PP-L2A K1a MECHANISM gate: the cache hit writes NO new scratch hbin (still exactly "
            "one loaded*.hbin) — the conversion re-write this leg exists to remove");

      // 2) HIT — PARITY. The cached open must render the same dataset card the
      //    converting open did, field for field.
      check(u2["varlist_after"] == u1["varlist_after"] &&
                u2["n_rows_after"] == u1["n_rows_after"],
            "PP-L2A K1a PARITY gate: the cache-hit `use` returns byte-identical varlist_after "
            "and n_rows_after to the converting `use`");

      // 3) STAT GUARD — the anti-stale mutant-kill. Rewrite the source (size,
      //    mtime and content all move). A guard that ignored any field would
      //    serve the OLD memo here, and the check below would read the old
      //    2-column card for a 3-column file.
      writeCsv("id,x,y\n1,10,100\n2,20,200\n3,30,300\n4,40,400\n");
      nlohmann::json u3 = useOnce(session);
      check(session.currentHbin().value_or("") != h1,
            "PP-L2A K1a stat-guard gate: a source that CHANGED forces a fresh conversion (new "
            "currentHbin) — the memo is retired, not served");
      check(u3["varlist_after"].size() == 3 && u3["n_rows_after"] == 4,
            "PP-L2A K1a stat-guard gate: the post-change `use` reports the NEW shape (3 vars, 4 "
            "rows) — a stat guard that missed the edit would have served the stale 2-var card");

      // 4) ESCAPE — PIET_NO_USE_CACHE=1 restores the pre-K1a path verbatim.
      //    (a) it really converts again (mutant-kill for a dropped env check —
      //    this is the floor_gate RED arm's in-process twin), and (b) it
      //    answers byte-identically, which is the A/B correctness arm.
      setenv("PIET_NO_USE_CACHE", "1", 1);
      nlohmann::json e1 = useOnce(session);
      const std::string he1 = session.currentHbin().value_or("");
      nlohmann::json e2 = useOnce(session);
      const std::string he2 = session.currentHbin().value_or("");
      unsetenv("PIET_NO_USE_CACHE");
      check(!he1.empty() && he1 != he2,
            "PP-L2A K1a ESCAPE MECHANISM gate: under PIET_NO_USE_CACHE=1 two `use`s of the SAME "
            "unchanged file convert twice (different currentHbin) — proof the escape reaches the "
            "old path, and the mutant-kill for a dropped env check");
      check(e1["varlist_after"] == u3["varlist_after"] &&
                e2["varlist_after"] == u3["varlist_after"] &&
                e1["n_rows_after"] == u3["n_rows_after"] &&
                e2["n_rows_after"] == u3["n_rows_after"],
            "PP-L2A K1a ESCAPE PARITY gate: the escape arm's blocks are byte-identical to the "
            "cached arm's (A/B correctness — the cache changes cost, never the answer)");

      // 5) The memo survives an intervening mutation. A cached hbin becomes the
      //    SUPERSEDED predecessor the moment any verb reloads, so without
      //    reclaimSupersededScratch's exclusion (3) the budget eviction would
      //    eventually delete exactly the file the next `use` wants.
      nlohmann::json body{{"filepath", csv}};
      (void)piet::routes::open(session, body.dump());
      const std::string hCached = session.currentHbin().value_or("");
      nlohmann::ordered_json mut = nlohmann::ordered_json::object();
      for (const auto& b : piet::dispatch("gen z = x + 1", session)) {
        if (b.is_object() && b.value("type", std::string()) == "data_mutation") mut = b;
      }
      if (mut.value("type", std::string()) != "data_mutation") {
        skip("PP-L2A K1a GC-exclusion gate", "gen did not produce a data_mutation on this build");
      } else {
        check(fs::exists(fs::path(hCached), ec),
              "PP-L2A K1a GC-exclusion gate: the memoized conversion output survives a following "
              "mutation that supersedes it (scratch-GC exclusion 3) — without the pin the cache "
              "would silently degrade to a no-op under a long session's budget eviction");
        nlohmann::json u4 = useOnce(session);
        check(session.currentHbin().value_or("") == hCached,
              "PP-L2A K1a GC-exclusion gate: and the `use` after that mutation still HITS the "
              "surviving memo");
      }
    }
  }

  // --- PP-L2B / K1b: same-file `use` resident-stash cache -------------------
  // The layer ABOVE K1a: a re-`use` of an unchanged source restores the worker's
  // stashed frame from RAM — NO disk open at all, where K1a still re-opens the
  // hbin. The K1a MECHANISM probe (currentHbin identity) cannot distinguish the
  // two — a K1b hit re-adopts the SAME conversion output — so the discriminator
  // here is the use_disk_loads counter: a K1b hit does NOT increment it, and
  // use_stash_hits does. Counters are read as DELTAS around each `use` (no global
  // reset, so surrounding legs are undisturbed). The §27 resident plane is
  // required; when it is unavailable the PUT's frameStash silently fails and
  // every re-`use` falls to the disk path, so the leg SKIPs honestly — the same
  // tri-state fallback the engine gives a §26-only build.
  {
    fs::path wd = tmp / "k1b_usestash";
    fs::create_directories(wd, ec);
    const std::string csv = (wd / "k1b.csv").u8string();
    auto writeCsv = [&](const std::string& body) {
      std::ofstream f(cecon::to_native_path(csv), std::ios::binary | std::ios::trunc);
      f << body;
    };
    auto useDelta = [&](piet::SessionState& s, nlohmann::json& out) {
      const int64_t d0 = piet::counters::useDiskLoads();
      const int64_t s0 = piet::counters::useStashHits();
      nlohmann::json body{{"filepath", csv}};
      out = nlohmann::json::parse(piet::routes::open(s, body.dump()).json);
      return std::pair<int64_t, int64_t>{piet::counters::useDiskLoads() - d0,
                                         piet::counters::useStashHits() - s0};
    };

    writeCsv("id,x\n1,10\n2,20\n3,30\n");
    piet::SessionState session(wd.u8string());
    nlohmann::json u1;
    auto [disk1, stash1] = useDelta(session, u1);
    if (u1.value("action", std::string()) != "open") {
      skip("PP-L2B K1b use-stash gates", "this cobra build could not convert the csv fixture");
    } else {
      check(disk1 == 1 && stash1 == 0,
            "PP-L2B K1b setup: the first `use` pays the disk path exactly once (use_disk_loads +1, "
            "use_stash_hits +0) — its stash PUT is a silent no-op while the worker's frames "
            "capability is still Unknown (residentSyncWorkerToHead requires Yes and deliberately "
            "never probes on the `use` path)");

      // Engage the resident plane the way every real corpus does: a mutation.
      // A use-only session can never stash (frames stays Unknown, see above),
      // so the FIRST `use` after the plane goes live is the memo-creating one —
      // the a2 shape verbatim (use → gen/replace/… → re-`use` the same corpus).
      const int64_t liveBefore = piet::SessionState::residentLiveCount();
      (void)piet::dispatch("replace x = x + 1", session);
      if (piet::SessionState::residentLiveCount() == liveBefore) {
        skip("PP-L2B K1b MECHANISM gate",
             "the section 27 resident plane is unavailable in this build — the engaging mutation "
             "ran one-shot on disk, so no stash PUT can ever take and every re-`use` falls to the "
             "disk path (the honest tri-state fallback this leg documents, not a failure)");
        nlohmann::json uf;
        auto [dFall, sFall] = useDelta(session, uf);
        check(dFall == 1 && sFall == 0 && uf["varlist_after"] == u1["varlist_after"] &&
                  uf["n_rows_after"] == u1["n_rows_after"],
              "PP-L2B K1b PARITY (fallback): even without the resident plane the re-`use` pays "
              "the disk path and renders the identical card");
      } else {
        // 1) The memo-creating re-`use`. The mutation left a dirty head this
        //    discards (Stata `use ,clear`); it pays disk ONCE more and — with
        //    the plane live — the PUT takes. load() cleared residentToken_, so
        //    the sync frame-uses the PRISTINE file before stashing: the mutated
        //    frame can never be what gets cached (the silent-wrong this pins).
        nlohmann::json u2;
        auto [disk2, stash2] = useDelta(session, u2);
        check(disk2 == 1 && stash2 == 0,
              "PP-L2B K1b setup: the first re-`use` after the plane went live pays the disk path "
              "once more and memoizes — one extra disk load per source is the lazy-probe price, "
              "never paid again below");
        check(u2["varlist_after"] == u1["varlist_after"] && u2["n_rows_after"] == u1["n_rows_after"],
              "PP-L2B K1b setup PARITY: the memo-creating re-`use` renders the pristine card — the "
              "discarded mutation never leaks into the reloaded dataset");

        // 2) HIT — MECHANISM. The re-`use` restored the stash: no new disk load.
        nlohmann::json u3;
        auto [disk3, stash3] = useDelta(session, u3);
        check(disk3 == 0 && stash3 == 1,
              "PP-L2B K1b MECHANISM gate: a re-`use` of the UNCHANGED source restores the worker "
              "stash — use_disk_loads UNCHANGED, use_stash_hits +1 (the disk open this leg "
              "removes, a fast path no value pin can see)");

        // 3) HIT — PARITY. The stash-restored card equals the disk card, field
        //    for field (the whole renders-vs-works discriminator).
        check(u3["varlist_after"] == u2["varlist_after"] && u3["n_rows_after"] == u2["n_rows_after"],
              "PP-L2B K1b PARITY gate: the stash-restored `use` returns byte-identical "
              "varlist_after and n_rows_after to the disk `use`");

        // 4) STAT GUARD — the anti-stale mutant-kill. Rewrite the source (size,
        //    mtime, content all move). A guard that ignored any field would
        //    restore the OLD 2-var stash for a now-3-var file.
        writeCsv("id,x,y\n1,10,100\n2,20,200\n3,30,300\n4,40,400\n");
        nlohmann::json u4;
        auto [disk4, stash4] = useDelta(session, u4);
        check(disk4 == 1 && stash4 == 0,
              "PP-L2B K1b stat-guard gate: a source that CHANGED forces the disk path "
              "(use_disk_loads +1, no stash restore) — the stale stash is retired, not served");
        check(u4["varlist_after"].size() == 3 && u4["n_rows_after"] == 4,
              "PP-L2B K1b stat-guard gate: the post-change `use` reports the NEW shape (3 vars, 4 "
              "rows) — a guard that missed the edit would have restored the stale 2-var stash");

        // 5) ESCAPE — PIET_NO_USE_CACHE=1 restores the pre-K1 path verbatim: two
        //    `use`s of the same unchanged file BOTH pay the disk path (mutant-kill
        //    for a dropped env check — the floor_gate RED arm's in-process twin),
        //    and answer byte-identically (the A/B correctness arm).
        setenv("PIET_NO_USE_CACHE", "1", 1);
        nlohmann::json esc1;
        auto [dEsc1, sEsc1] = useDelta(session, esc1);
        nlohmann::json esc2;
        auto [dEsc2, sEsc2] = useDelta(session, esc2);
        unsetenv("PIET_NO_USE_CACHE");
        check(dEsc1 == 1 && dEsc2 == 1 && sEsc1 == 0 && sEsc2 == 0,
              "PP-L2B K1b ESCAPE MECHANISM gate: under PIET_NO_USE_CACHE=1 two `use`s of the SAME "
              "unchanged file BOTH pay the disk path (use_disk_loads +1 each, no stash restore) — "
              "proof the escape reaches the pre-K1 path and the mutant-kill for a dropped env check");
        check(esc1["varlist_after"] == u4["varlist_after"] &&
                  esc2["varlist_after"] == u4["varlist_after"] &&
                  esc1["n_rows_after"] == u4["n_rows_after"] &&
                  esc2["n_rows_after"] == u4["n_rows_after"],
              "PP-L2B K1b ESCAPE PARITY gate: the escape arm's blocks are byte-identical to the "
              "cached arm's (A/B correctness — the cache changes cost, never the answer)");
      }
    }
  }

  // --- STALE RESIDENT FRAME after a re-`use` of the resident token path -----
  // load() reads `path == residentToken_` as "this load CONTINUES the resident
  // chain" and therefore leaves the worker's frame in place. That is right for
  // a mutation reloading its own virtual --out and WRONG for a `use` reading a
  // file off disk — and the two are indistinguishable whenever the resident
  // token is a real, user-named file.
  //
  // K1b makes that routine with no help from anything else: for an `.hbin`
  // SOURCE the use-stash memo records hbinPath = the source itself (there is no
  // conversion output to point at), so a memo hit adopts the user's own file as
  // the token. Edit that file behind PIET's back and the next `use` reloads the
  // new bytes into the Datastore while the WORKER keeps answering from the
  // pre-edit frame: `browse` shows the new values and `summarize` reports the
  // old ones. Silent wrong answer, and reachable without any of this stint's
  // other work — the runUse token drop is the fix.
  //
  // THE ASSERTION IS A MEAN, NOT A SHAPE, and that is the point: the two frames
  // here have identical columns and identical row counts, so every structural
  // check passes on the stale answer. Only a value tells them apart.
  {
    fs::path wd = tmp / "stale_resident_token";
    fs::create_directories(wd, ec);
    // Two .hbin files of the SAME shape and different values, built by a
    // throwaway session so nothing about their construction touches the session
    // under test.
    auto mintHbin = [&](const std::string& tag, const std::string& body) -> std::string {
      fs::path bwd = wd / ("mint_" + tag);
      fs::create_directories(bwd, ec);
      const std::string bcsv = (bwd / "src.csv").u8string();
      {
        std::ofstream f(cecon::to_native_path(bcsv), std::ios::binary | std::ios::trunc);
        f << body;
      }
      const std::string out = (bwd / (tag + ".hbin")).u8string();
      piet::SessionState bs(bwd.u8string());
      nlohmann::json b{{"filepath", bcsv}};
      auto o = nlohmann::json::parse(piet::routes::open(bs, b.dump()).json);
      if (o.value("action", std::string()) != "open") return std::string();
      (void)piet::dispatch("save \"" + out + "\", replace", bs);
      std::error_code mec;
      return fs::exists(cecon::to_native_path(out), mec) ? out : std::string();
    };
    const std::string origin = mintHbin("origin", "id,x\n1,10\n2,20\n3,30\n");
    const std::string replacement = mintHbin("replacement", "id,x\n1,11\n2,21\n3,31\n");
    const std::string live = (wd / "live.hbin").u8string();
    std::error_code cec;
    if (origin.empty() || replacement.empty()) {
      skip("stale-resident-token gates", "this cobra build could not mint the .hbin fixtures");
    } else {
      fs::copy_file(cecon::to_native_path(origin), cecon::to_native_path(live),
                    fs::copy_options::overwrite_existing, cec);
      auto useDelta = [&](piet::SessionState& s, nlohmann::json& out) {
        const int64_t d0 = piet::counters::useDiskLoads();
        const int64_t s0 = piet::counters::useStashHits();
        nlohmann::json body{{"filepath", live}};
        out = nlohmann::json::parse(piet::routes::open(s, body.dump()).json);
        return std::pair<int64_t, int64_t>{piet::counters::useDiskLoads() - d0,
                                           piet::counters::useStashHits() - s0};
      };
      auto meanOfX = [&](piet::SessionState& s) {
        double m = std::nan("");
        auto blocks = piet::dispatch("summarize x", s);
        std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& n) {
          if (n.is_object()) {
            auto it = n.find("mean");
            if (it != n.end() && it->is_number()) m = it->get<double>();
            for (auto& [k, v] : n.items()) {
              (void)k;
              walk(v);
            }
          } else if (n.is_array()) {
            for (const auto& v : n) walk(v);
          }
        };
        walk(blocks);
        return m;
      };

      piet::SessionState session(wd.u8string());
      nlohmann::json u0;
      auto [d0, s0] = useDelta(session, u0);
      (void)d0;
      (void)s0;
      if (u0.value("action", std::string()) != "open") {
        skip("stale-resident-token gates", "the .hbin fixture would not load");
      } else {
        // Engage the resident plane (K1b's memo PUT is a no-op while the
        // worker's frames capability is still Unknown), then take the memo-
        // creating `use` and the hit.
        const int64_t liveBefore = piet::SessionState::residentLiveCount();
        (void)piet::dispatch("replace x = x + 0", session);
        nlohmann::json u1;
        (void)useDelta(session, u1);  // memo-creating re-`use`
        nlohmann::json u2;
        auto [d2, s2] = useDelta(session, u2);
        if (piet::SessionState::residentLiveCount() == liveBefore || s2 != 1) {
          skip("stale-resident-token gates",
               "the section 27 resident plane is unavailable in this build — no memo hit, so the "
               "resident token never becomes the user's own file and the hazard is unreachable "
               "here (the honest tri-state fallback, not a failure)");
        } else {
          check(d2 == 0 && s2 == 1,
                "stale-resident-token setup: a re-`use` of the unchanged .hbin restores the "
                "worker stash (use_disk_loads +0, use_stash_hits +1) — which is what adopts the "
                "USER'S OWN FILE as the resident token and arms the hazard below");
          check(std::abs(meanOfX(session) - 20.0) < 1e-9,
                "stale-resident-token setup: mean(x) is 20 before the edit");

          // Edit the file behind PIET's back. Same shape, same length, only the
          // values and mtime move — a shape check cannot see this.
          fs::copy_file(cecon::to_native_path(replacement), cecon::to_native_path(live),
                        fs::copy_options::overwrite_existing, cec);
          nlohmann::json u3;
          auto [d3, s3] = useDelta(session, u3);
          check(d3 == 1 && s3 == 0,
                "stale-resident-token gate: the edited file fails the stat key, so the memo "
                "retires and the `use` pays the disk path (K1b's own guard, working)");
          check(u3["varlist_after"].size() == 2 && u3["n_rows_after"] == 3,
                "stale-resident-token gate: the reloaded card has the same 2 vars and 3 rows as "
                "before — recorded to show that EVERY structural check passes on the stale "
                "answer, which is why the assertion below is a value");
          check(std::abs(meanOfX(session) - 21.0) < 1e-9,
                "stale-resident-token gate (SEEDED-RED, runUse's residentDropToken): mean(x) "
                "after the edit is the file's own 21, not the worker's retained 20. Without the "
                "token drop load() reads `loadPath == residentToken_` as a resident-chain "
                "continuation, the Datastore reloads and the engine does not, and `browse` and "
                "`summarize` disagree on the same dataset — measured RED at 20 on 2026-08-06");
        }
      }
    }
  }

  // --- S07-ROUNDTRIP: the frlink/frget left-join fold, A/B in one binary -----
  // frlink and frget used to spend a SECOND whole-frame round trip
  // (`eval --keepif "_merge!=2" --drop _merge`) deleting rows and a column the
  // merge could simply not have emitted. `--keep master,matched --nogenerate`
  // is the same left join in one trip. The claim under test is EQUIVALENCE, so
  // both arms run here — PIET_NO_MERGE_FOLD=1 is the pre-fold path verbatim —
  // and the browse payloads of the two results must be identical cell for cell.
  //
  // The fixture deliberately covers all three _merge codes: master rows with a
  // match (3), a master row the frame does not cover (1, must SURVIVE with
  // missings), and a frame row no master row points at (2, must be DROPPED —
  // the row a bare `--keep master` would also have thrown away and a default
  // merge would have kept). It also carries a string column, because factor
  // level dictionaries are the one thing a post-filter could have re-derived.
  {
    fs::path wd = tmp / "s07_mergefold";
    fs::create_directories(wd, ec);
    const std::string masterCsv = (wd / "m.csv").u8string();
    const std::string frameCsv = (wd / "f.csv").u8string();
    {
      std::ofstream f(cecon::to_native_path(masterCsv), std::ios::binary | std::ios::trunc);
      f << "id,v,s\n1,10,aa\n1,11,aa\n2,20,bb\n9,90,zz\n";  // id 9 has no frame row (_merge 1)
    }
    {
      std::ofstream f(cecon::to_native_path(frameCsv), std::ios::binary | std::ios::trunc);
      f << "id,w,t\n1,100,pp\n2,200,qq\n7,700,rr\n";  // id 7 has no master row (_merge 2)
    }
    auto runArm = [&](bool foldOff) -> nlohmann::json {
      fs::path awd = wd / (foldOff ? "armA" : "armB");
      fs::create_directories(awd, ec);
      piet::SessionState s(awd.u8string());
      if (foldOff) setenv("PIET_NO_MERGE_FOLD", "1", 1);
      nlohmann::json out = nlohmann::json::object();
      nlohmann::json mb{{"filepath", masterCsv}};
      auto opened = nlohmann::json::parse(piet::routes::open(s, mb.dump()).json);
      if (opened.value("action", std::string()) == "open") {
        // `frame put` needs the frame's rows to come from the CURRENT dataset,
        // so the frame is built by loading the frame csv, putting all of it
        // into a named frame, then returning to the master.
        nlohmann::json fb{{"filepath", frameCsv}};
        (void)piet::routes::open(s, fb.dump());
        (void)piet::dispatch("frame put id w t, into(ff)", s);
        (void)piet::routes::open(s, mb.dump());
        out["link"] = piet::dispatch("frlink m:1 id, frame(ff) generate(lnk)", s);
        out["get"] = piet::dispatch("frget wcopy=w, from(lnk)", s);
        nlohmann::json bb{{"offset", 0}, {"limit", 50}};
        out["browse"] = nlohmann::json::parse(piet::routes::browse(s, bb.dump()).json);
      }
      if (foldOff) unsetenv("PIET_NO_MERGE_FOLD");
      return out;
    };
    nlohmann::json armA = runArm(/*foldOff=*/true);
    nlohmann::json armB = runArm(/*foldOff=*/false);
    if (!armA.contains("browse") || !armB.contains("browse")) {
      skip("S07 merge-fold gates", "this cobra build could not run the frames fixture");
    } else {
      check(armA["browse"] == armB["browse"],
            "S07 merge-fold EQUIVALENCE gate: `--keep master,matched --nogenerate` in ONE merge "
            "produces a browse payload identical, cell for cell, to merge + `eval --keepif "
            "_merge!=2 --drop _merge` — same rows, same order, same columns, same factor levels");
      check(armA["link"] == armB["link"] && armA["get"] == armB["get"],
            "S07 merge-fold EQUIVALENCE gate: the frlink/frget report blocks (the matched/"
            "unmatched counts users read) are byte-identical across the two arms");
      // Semantics the fold must not have quietly changed, asserted directly on
      // the folded arm rather than only relative to the old path.
      const auto& cols = armB["browse"]["columns"];
      bool sawMerge = false, sawLnk = false, sawCopy = false;
      for (const auto& c : cols) {
        const std::string n = c.value("name", std::string());
        if (n == "_merge") sawMerge = true;
        if (n == "lnk") sawLnk = true;
        if (n == "wcopy") sawCopy = true;
      }
      check(!sawMerge && sawLnk && sawCopy,
            "S07 merge-fold gate: the folded result carries the link and the copied variable and "
            "NOT `_merge` — `--nogenerate` replaces the drop, it does not leak the helper column");
      check(armB["browse"]["n_rows_total"] == 4,
            "S07 merge-fold gate: all 4 master rows survive and the frame-only row (id 7) is "
            "never added — the left join's own definition, now enforced by --keep master,matched "
            "instead of by a second pass");
    }
  }

  // --- C4-L4 / C2(a): resident setcol chain + the computed-column cache ------
  // The C2 §9.2 blocker made resident setcol un-shippable: a chained consumer
  // (tostring → destring reading the column tostring just wrote) re-
  // materialized the virtual head via its entry datastore(). The computed-
  // column cache serves that read from the values PIET itself committed.
  // Gates: chain MECHANISM (zero constructs, zero closes), value correctness
  // THROUGH the cache, resident encode fold, A/B parity vs the forced-
  // stateless arm, escape mutant-kill.
  {
    fs::path wd = tmp / "c4l4_setcol";
    fs::create_directories(wd / "A", ec);
    fs::create_directories(wd / "B", ec);
    const std::string csv = (wd / "c4l4.csv").u8string();
    {
      std::ofstream f(cecon::to_native_path(csv), std::ios::binary | std::ios::trunc);
      f << "id,x,s\n1,10,b\n2,20,a\n3,30,b\n";
    }
    auto mutVarlist = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
      for (const auto& b : blocks) {
        if (b.is_object() && b.value("type", std::string()) == "data_mutation") {
          return b.value("varlist_after", nlohmann::ordered_json::array());
        }
      }
      return nlohmann::ordered_json::array();
    };
    auto rN = [](piet::SessionState& s) -> double {
      const double* p = s.eval.rValues.find("N");
      return p ? *p : -1.0;
    };

    piet::SessionState sesA((wd / "A").u8string());
    nlohmann::json openBody{{"filepath", csv}};
    auto openA = nlohmann::json::parse(piet::routes::open(sesA, openBody.dump()).json);
    if (openA.value("action", std::string()) != "open") {
      skip("C4-L4 resident-setcol gates", "this cobra build could not convert the csv fixture");
    } else {
      (void)piet::dispatch("tostring x, gen(xs)", sesA);
      if (!sesA.residentHeadDirty()) {
        skip("C4-L4 resident-setcol gates",
             "resident plane unavailable — the tostring ran stateless, no chain to probe");
      } else {
        // 1) Chain MECHANISM: destring reads the column the resident tostring
        //    just computed — the cache answers, nothing materializes.
        piet::Datastore::resetConstructCount();
        const int64_t snapChain = piet::counters::snapshotCount();
        auto dsBlk = piet::dispatch("destring xs, gen(xn)", sesA);
        check(piet::Datastore::constructCount() == 0 &&
                  piet::counters::snapshotCount() == snapChain,
              "C4-L4 chain MECHANISM gate: destring of the column the previous resident tostring "
              "computed reads the computed-column cache — ZERO Datastore constructs, ZERO window "
              "closes (pre-L4 this exact chain re-materialized the virtual head: the C2 section "
              "9.2 blocker, destring.json's floor RED)");
        // 2) Values THROUGH the cache round-trip: x=10/20/30 → xs strings →
        //    xn parses back to the same numbers.
        (void)piet::dispatch("count if xn == 20", sesA);
        check(rN(sesA) == 1.0,
              "C4-L4 chain VALUE gate: the cache-served destring parsed the true committed "
              "strings (count if xn == 20 finds exactly the one row)");
        // 3) Resident encode: the same §26 bridge, zero reconstructs, labels
        //    from the engine's own alphabetical levels.
        piet::Datastore::resetConstructCount();
        auto encBlk = piet::dispatch("encode s, gen(se)", sesA);
        check(piet::Datastore::constructCount() == 0,
              "C4-L4 encode MECHANISM gate: resident encode folds its vars_delta — ZERO "
              "Datastore reconstructs (the ~263ms materialization tax the C2 brief priced)");
        (void)piet::dispatch("count if se == 2", sesA);
        check(rN(sesA) == 2.0,
              "C4-L4 encode VALUE gate: alphabetical codes arrived through the resident fold "
              "(s=b,a,b → se=2,1,2 → count se==2 is 2)");
        check(sesA.valueLabels.count("se") > 0 && sesA.valueLabels["se"].count(1) > 0 &&
                  sesA.valueLabels["se"][1] == "a" && sesA.valueLabels["se"][2] == "b",
              "C4-L4 encode LABEL gate: the value-label set built from the resident run's own "
              "stdout levels matches the codes it assigned");

        // 4) A/B PARITY + escape MECHANISM: the forced-stateless arm answers
        //    identically (dtype/n_missing/n_levels field-for-field) and REALLY
        //    reconstructs (mutant-kill for a dropped env check).
        setenv("PIET_NO_RESIDENT_SETCOL", "1", 1);
        piet::SessionState sesB((wd / "B").u8string());
        (void)piet::routes::open(sesB, openBody.dump());
        (void)piet::dispatch("tostring x, gen(xs)", sesB);
        piet::Datastore::resetConstructCount();
        auto dsBlkB = piet::dispatch("destring xs, gen(xn)", sesB);
        const int64_t escConstructs = piet::Datastore::constructCount();
        auto encBlkB = piet::dispatch("encode s, gen(se)", sesB);
        (void)piet::dispatch("count if xn == 20", sesB);
        const double xnCountB = rN(sesB);
        (void)piet::dispatch("count if se == 2", sesB);
        const double seCountB = rN(sesB);
        unsetenv("PIET_NO_RESIDENT_SETCOL");
        check(escConstructs >= 1,
              "C4-L4 escape MECHANISM gate: under PIET_NO_RESIDENT_SETCOL=1 the destring REALLY "
              "runs the stateless path (Datastore reconstructs ≥1) — the mutant-kill proving the "
              "env check is live and the resident win is measurable");
        check(mutVarlist(dsBlkB) == mutVarlist(dsBlk) && mutVarlist(encBlkB) == mutVarlist(encBlk),
              "C4-L4 A/B PARITY gate: destring and encode report field-identical varlist_after "
              "(name/dtype/n_missing/n_levels — the section 8.3 symmetry: factor→f64 CLEARS "
              "n_levels, f64→factor SETS it) across the resident and stateless arms");
        check(xnCountB == 1.0 && seCountB == 2.0,
              "C4-L4 A/B VALUE gate: the stateless arm computes the identical values (the plane "
              "changes cost, never the answer)");

        // ── MECHOPT W3: string gen/replace joins the resident plane ──────
        // The census priced the stateless habit at 2,560 full-frame --out
        // writes / 283 s per acceptance-corpus run. These legs pin the four
        // contracts the destring precedent carries: the mechanism (stateless
        // requests do NOT grow, the frame-run channel serves the setcol),
        // value parity across arms, varlist_after field parity, and the
        // escape mutant-kill.
        fs::create_directories(wd / "S", ec);
        fs::create_directories(wd / "T", ec);
        piet::SessionState sesS((wd / "S").u8string());
        (void)piet::routes::open(sesS, openBody.dump());
        const int64_t statelessBefore = piet::counters::engineReqStateless();
        auto sgBlk = piet::dispatch("gen tag = upper(s)", sesS);
        auto srBlk = piet::dispatch("replace tag = tag + \"!\"", sesS);
        const int64_t statelessAfter = piet::counters::engineReqStateless();
        check(statelessAfter == statelessBefore,
              "W3 strmut MECHANISM gate: resident string gen + replace add ZERO stateless "
              "requests — the setcol rides the frame-run channel and the full-frame --out "
              "write (2,560 per corpus run) never happens");
        (void)piet::dispatch("count if tag == \"B!\"", sesS);
        check(rN(sesS) == 2.0,
              "W3 strmut VALUE gate (resident): upper(s)+\"!\" over s=b,a,b gives tag==\"B!\" "
              "on exactly the two b-rows");

        setenv("PIET_NO_RESIDENT_SETCOL", "1", 1);
        piet::SessionState sesT((wd / "T").u8string());
        (void)piet::routes::open(sesT, openBody.dump());
        piet::Datastore::resetConstructCount();
        auto sgBlkB = piet::dispatch("gen tag = upper(s)", sesT);
        auto srBlkB = piet::dispatch("replace tag = tag + \"!\"", sesT);
        const int64_t strmutEscConstructs = piet::Datastore::constructCount();
        (void)piet::dispatch("count if tag == \"B!\"", sesT);
        const double tagCountB = rN(sesT);
        unsetenv("PIET_NO_RESIDENT_SETCOL");
        check(strmutEscConstructs >= 1,
              "W3 strmut escape MECHANISM gate: under PIET_NO_RESIDENT_SETCOL=1 the string "
              "gen/replace REALLY runs the pre-W3 stateless path (Datastore reconstructs ≥1) — "
              "the mutant-kill proving the env check is live");
        check(mutVarlist(sgBlkB) == mutVarlist(sgBlk) && mutVarlist(srBlkB) == mutVarlist(srBlk),
              "W3 strmut A/B PARITY gate: gen and replace report field-identical varlist_after "
              "across the resident and stateless arms (the fold and the hand rebuild agree "
              "byte-for-byte, freeze's own parapet)");
        check(tagCountB == 2.0,
              "W3 strmut A/B VALUE gate: the stateless arm computes the identical strings");
      }
    }
  }

  fs::remove_all(tmp, ec);
}


// ── 2.2.0 item B: run-selection route ───────────────────────────────────────
// The editor's Execute-selection (⇧⌘D) hands the highlighted range verbatim to
// the SAME do_string path Run .do uses (no new engine surface). Prove the path
// runs EXACTLY the source handed to it — the selected line(s) alone, isolated
// from the unselected lines of the buffer.
void testRunSelectionRoute() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path workdir = fs::temp_directory_path() / "piet_native_selftest_runsel";
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir.u8string());

  {
    nlohmann::ordered_json body;
    body["source"] = "display \"SELA\"\n";  // the "selected" middle line, alone
    auto r = piet::routes::doString(session, body.dump());
    check(r.ok, "run-selection: do_string(selected line) returns ok");
    std::string dump = nlohmann::json::parse(r.json).dump();
    check(dump.find("SELA") != std::string::npos, "run-selection: the selected line's result is present");
    check(dump.find("SELB") == std::string::npos && dump.find("SELC") == std::string::npos,
          "run-selection: unselected lines did NOT run (selection isolation)");
  }
  {
    nlohmann::ordered_json body;
    body["source"] = "display \"SELB\"\ndisplay \"SELC\"\n";  // a multi-line selection
    auto r = piet::routes::doString(session, body.dump());
    check(r.ok, "run-selection: do_string(2-line selection) returns ok");
    std::string dump = nlohmann::json::parse(r.json).dump();
    check(dump.find("SELB") != std::string::npos && dump.find("SELC") != std::string::npos,
          "run-selection: a multi-line selection runs every selected line");
  }

  fs::remove_all(workdir, ec);
}


void testC4BS124Timer() {
  std::printf("[selftest] --- C4/BS-124 `timer` on/off/clear/list ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c4_bs124").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("timer clear", session);

  // Previously an honest hard-stop (unsupported_command, "piet_gap") even
  // though "timer" was already allow-listed in kNoDataCommands — no
  // dispatch arm existed at all.
  auto onBlocks = piet::dispatch("timer on 5", session);
  bool sawUnsupported = false;
  for (const auto& b : onBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error" &&
        b.value("code", std::string()) == "unsupported_command") {
      sawUnsupported = true;
    }
  }
  check(!sawUnsupported, "C4/BS-124: `timer on 5` no longer hits the honest hard-stop");

  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  (void)piet::dispatch("timer off 5", session);

  // BUG-151: timer is r-class. Targeted list refreshes only the selected
  // slot's pair and preserves unrelated results (including another slot's
  // pre-existing pair).
  session.eval.rValues.set("N", 74.0);
  session.eval.rValues.set("t7", -7.0);
  session.eval.rValues.set("nt7", -7.0);
  auto listBlocks = piet::dispatch("timer list 5", session);
  std::string listText;
  for (const auto& b : listBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "echo" && b.contains("text")) {
      listText = b.value("text", std::string());
    }
  }
  // Format shape, not exact elapsed (real elapsed time is inherently
  // non-deterministic under test-runner scheduling) — oracle's own column
  // widths: "%4d: %9.2f / %8.0f = %12.4f".
  static const std::regex reTimerLine(R"(^\s*5:\s+\d+\.\d{2} /\s+1 =\s+\d+\.\d{4}$)");
  check(std::regex_match(listText, reTimerLine),
        "C4/BS-124: `timer list 5` matches the oracle's exact column format, got: [" + listText +
            "]");

  // Monotonicity: total elapsed must be strictly positive after a real
  // on/off cycle with an actual delay in between.
  size_t slashPos = listText.find('/');
  double totalSeconds = -1.0;
  if (slashPos != std::string::npos) {
    try {
      totalSeconds = std::stod(listText.substr(0, slashPos));
    } catch (...) {
    }
  }
  check(totalSeconds > 0.0,
        "C4/BS-124: `timer list` reports strictly positive elapsed time after a real on/off "
        "cycle with a delay, got total=" + std::to_string(totalSeconds));

  // count==1 after exactly one on/off cycle.
  check(listText.find("/        1 =") != std::string::npos ||
            std::regex_search(listText, std::regex(R"(/\s+1 =)")),
        "C4/BS-124: count==1 after exactly one on/off cycle, got: [" + listText + "]");

  const double* t5 = session.eval.rValues.find("t5");
  const double* nt5 = session.eval.rValues.find("nt5");
  check(t5 != nullptr && *t5 == session.timers[5].totalSeconds,
        "C4/BS-124 BUG-151: targeted `timer list 5` sets r(t5) to total seconds");
  check(nt5 != nullptr && *nt5 == 1.0,
        "C4/BS-124 BUG-151: targeted `timer list 5` sets r(nt5) to cycle count");
  check(session.eval.rValues.find("N") != nullptr && *session.eval.rValues.find("N") == 74.0,
        "C4/BS-124 BUG-151: targeted list preserves unrelated r(N)");
  check(session.eval.rValues.find("t7") != nullptr &&
            session.eval.rValues.find("nt7") != nullptr,
        "C4/BS-124 BUG-151: targeted list updates only the selected timer pair");

  // Pin the real benchmark consumer, not just the backing store: expansion
  // of r(t5) into a local must succeed after `quietly timer list 5`.
  (void)piet::dispatch("local elapsed = r(t5)", session);
  const std::string* elapsed = session.eval.locals.find("elapsed");
  bool elapsedMatches = false;
  if (elapsed != nullptr) {
    try {
      elapsedMatches = std::stod(*elapsed) == session.timers[5].totalSeconds;
    } catch (...) {
    }
  }
  check(elapsedMatches,
        "C4/BS-124 BUG-151: benchmark-style `local elapsed = r(t5)` consumes the result");

  // Add a second populated slot so bare list must publish every live pair.
  (void)piet::dispatch("timer on 9", session);
  (void)piet::dispatch("timer off 9", session);

  // Bare `timer list` shows ONLY slots with data — slot 7 was never
  // touched, must be absent.
  auto bareListBlocks = piet::dispatch("timer list", session);
  std::string bareListText;
  for (const auto& b : bareListBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "echo" && b.contains("text")) {
      bareListText = b.value("text", std::string());
    }
  }
  check(bareListText.find("5:") != std::string::npos,
        "C4/BS-124: bare `timer list` includes touched slot 5");
  check(bareListText.find("7:") == std::string::npos,
        "C4/BS-124: bare `timer list` OMITS never-touched slot 7 (only slots with data)");
  check(session.eval.rValues.find("t5") != nullptr &&
            session.eval.rValues.find("nt5") != nullptr &&
            session.eval.rValues.find("t9") != nullptr &&
            session.eval.rValues.find("nt9") != nullptr,
        "C4/BS-124 BUG-151: bare `timer list` publishes every nonempty slot pair");
  check(session.eval.rValues.find("t7") == nullptr &&
            session.eval.rValues.find("nt7") == nullptr,
        "C4/BS-124 BUG-151: bare `timer list` retires stale empty-slot pairs");
  check(session.eval.rValues.find("N") != nullptr && *session.eval.rValues.find("N") == 74.0,
        "C4/BS-124 BUG-151: bare list preserves unrelated r(N)");

  // `timer list` on an untouched slot: silent — no LISTING output. Every
  // dispatch() result carries the command-echo block itself (text ".  timer
  // list 7"), so "silent" must be judged on echo blocks that are NOT the
  // command echo (listing lines don't start with ". ").
  auto isListingEcho = [](const nlohmann::ordered_json& b) {
    if (!b.is_object() || b.value("type", std::string()) != "echo") return false;
    std::string t = b.value("text", std::string());
    return t.rfind(". ", 0) != 0;  // command echoes start ". "
  };
  session.eval.rValues.set("t7", -7.0);
  session.eval.rValues.set("nt7", -7.0);
  auto emptyBlocks = piet::dispatch("timer list 7", session);
  bool emptyHasListing = false;
  for (const auto& b : emptyBlocks) {
    if (isListingEcho(b)) emptyHasListing = true;
  }
  check(!emptyHasListing, "C4/BS-124: `timer list` on an untouched slot is silent");
  check(session.eval.rValues.find("t7") == nullptr &&
            session.eval.rValues.find("nt7") == nullptr &&
            session.eval.rValues.find("N") != nullptr,
        "C4/BS-124 BUG-151: targeted empty list retires only its stale pair");

  // `timer clear` resets a slot back to empty.
  (void)piet::dispatch("timer clear 5", session);
  auto afterClearBlocks = piet::dispatch("timer list 5", session);
  bool afterClearHasListing = false;
  for (const auto& b : afterClearBlocks) {
    if (isListingEcho(b)) afterClearHasListing = true;
  }
  check(!afterClearHasListing, "C4/BS-124: `timer clear 5` resets the slot back to empty");
  check(session.eval.rValues.find("t5") == nullptr &&
            session.eval.rValues.find("nt5") == nullptr &&
            session.eval.rValues.find("t9") != nullptr &&
            session.eval.rValues.find("N") != nullptr,
        "C4/BS-124 BUG-151: clear then targeted list retires stale slot keys only");

  // `timer off` on a never-started slot is a silent no-op (live-probed:
  // rc=0, no error, no accumulation) — not a crash, not an error.
  auto offNeverOnBlocks = piet::dispatch("timer off 42", session);
  bool offNeverOnError = false;
  for (const auto& b : offNeverOnBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") offNeverOnError = true;
  }
  check(!offNeverOnError, "C4/BS-124: `timer off <never-started slot>` is a silent no-op");

  // Slot out of 1..100 -> Stata's own "invalid syntax" rc=198.
  (void)piet::dispatch("capture timer on 101", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "C4/BS-124: `timer on 101` (out of the 1..100 range) rejects with rc=198");

  fs::remove_all(workdir, ec);
}
