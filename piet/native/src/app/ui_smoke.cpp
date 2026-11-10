// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// --ui-smoke: packaged-app UI journey gate (fixtures, test-only routes, watchdog, verdict).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>   // kill(pid, 0) — BUG-133 orphan-scratch liveness probe
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "base64.h"
#include "piet/bundle_paths.h"
#include "piet/counters.h"
#include "piet/strmutate.h"
#include "piet/doexec.h"
#include "piet/dispatcher.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/progress.h"
#include "piet/routes.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/shell.h"
#include "dispatch/registry.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "vendor/nlohmann/json.hpp"
#include "app_entry_internal.h"

namespace app_entry_detail {

namespace {

// --- --ui-smoke (2.0.2, MI-138 WP-1): packaged-app UI journey gate ----------
//
// Drives a scripted user journey through the REAL window + REAL WKWebView +
// REAL frontend SPA + REAL bridge + REAL cobra subprocess — the exact stack a
// user launch exercises (Constitution Part 1 Verification law: "renders" !=
// "works"; headless PASS never declares a UI release GREEN). There are NO
// mocks anywhere in this mode: the window that opens IS the product window
// wired by the SAME bindAllRoutes()/AppState code path runRealFrontend()
// uses, so the mock-fidelity law is satisfied vacuously.
//
// Journey (driver JS injected via initJs AFTER the bridge shims, so it runs
// in the page at document-start with window.pietNative already installed):
//   1. bridge_echo         — window.pietNative.request('echo') round-trip
//                            through the real shim + real WKWebView bind
//   2. kernel_dot_green    — the SPA booted and its own ping() varlist
//                            round-trip turned #kernel-dot 'dot-ok'; the
//                            mock banner must be hidden (a mock-mode
//                            fallback is a FAIL, never a pass)
//   3. varlist_populated   — submit `use "<fixture>"` through the real
//                            #cmd-input + #btn-run click; #var-list-wrap
//                            .var-row rows must appear
//   4. estimation_rendered — submit `regress fatal beertax unemp`; a
//                            rendered .block-estimation .coeftable naming
//                            the regressor must appear in #console-body,
//                            with no .block-error present
//
// Steps 5-9 (stabilization campaign delta — verification law: gates follow
// the delta, so paste policy / Run.do / overlay mechanics that this campaign
// actually changed must be exercised, not just the M4b-era boot journey
// above):
//   5. paste_routing       — a synthetic multi-line `paste` DOM event on
//                            #cmd-input (piet-paste-autorun OFF, the
//                            default) must route to the do-editor
//                            (#doedit-overlay opens, #doedit-text gets the
//                            pasted lines verbatim, #cmd-input clears) and
//                            must NOT auto-run (no new console block within
//                            a settle window) — design decision 4.
//   6. editor_first_run    — window.pietNative.request('file.read', {path})
//                            against a tiny fixture .do this driver writes
//                            into its own scratch workdir; the returned
//                            source is loaded into #doedit-text (the
//                            externally-observable effect of
//                            onRunDo()/readDoFile(), whose own trigger is a
//                            native Finder picker unreachable headlessly),
//                            #doedit-run is clicked, and an honest
//                            .block-table (describe) result must land in
//                            #console-body with no .block-error.
//   7. overlay_exclusivity — opening the graphics overlay then the do-editor
//                            via their toolbar anchors must never leave more
//                            than one of the four console overlays visible
//                            at once (design decision 5 / A2-05's OVERLAYS
//                            registry); Escape then closes the one that's
//                            open.
//   8. transition_sanity   — opening an overlay must reach a REAL rendered
//                            end-state (getComputedStyle opacity '1' +
//                            visibility 'visible'), not just a class flip
//                            with no visible effect — polled, not a single
//                            sleep-then-check. Runs under the frontend's own
//                            reduced-motion configuration (--dur-* zeroed
//                            inline, mirroring piet.css §1's
//                            prefers-reduced-motion block) because WKWebView
//                            suspends the CSS transition clock while the
//                            window is occluded — asserting an ANIMATED
//                            computed value was the only occlusion-sensitive
//                            assertion in this journey and produced a false
//                            RED on a covered window (2026-07-09). Set
//                            PIET_UI_SMOKE_OCCLUDE_STEP8=1 to make this step
//                            wait for real occlusion (document.hidden) before
//                            running — the deterministic covered-window
//                            verification protocol.
//   9. busy_lock            — submitting a command must set the busy-lock
//                            classes (File▸Open toolbar anchor,
//                            #var-list-wrap) SYNCHRONOUSLY (checked with no
//                            await/setTimeout between the click and the
//                            assertion, since setBusy(true) runs before
//                            runCommand()'s first await) and release them
//                            once the run settles (A1-01/A1-02 regression).
//
// Verdict channel: each step reports back over the REAL bridge via a
// 'smoke.report' route registered ONLY in this mode (runRealFrontend never
// binds it, the frontend never calls it — it is a test-harness binding, NOT
// part of the Boundary-B contract surface; see CONTRACT.md which documents
// the product route set). Detection property for the a9141d1 double-parse
// class: the JS->C++ invoke leg of the bridge still delivers even when the
// C++->JS resolve leg is broken, so a broken-bridge run still lands a
// precise FAIL report; a page that never executes anything at all is caught
// by the 90s C++ watchdog (Shell::terminateFromAnyThread). Exit 0 iff the
// final report is a pass; a JSON verdict file (file-backed verdict, charter
// #2) is written to $PIET_UI_SMOKE_OUT (default
// /tmp/piet_ui_smoke_verdict.json).
//
// Run it against the PACKAGED artifact (the actual gate):
//   dist/PIET.app/Contents/MacOS/piet_native --ui-smoke
// (bundle-first resolution picks up Resources/frontend + Resources/cobra —
// same order the product launch uses). Running the dev build binary smokes
// the dev tree instead. The fixture dataset defaults to the SHIPPED sample
// (bundled Resources/sample/fatalities.csv, or the worktree's
// piet/packaging/sample/fatalities.csv for dev runs; override:
// $PIET_UI_SMOKE_DATA).
struct SmokeState {
  std::mutex m;
  std::condition_variable cv;
  bool finalReceived = false;
  bool finalPass = false;
  std::vector<nlohmann::ordered_json> reports;
};

}  // namespace

int runUiSmoke() {
  // Fixture default (WI-4, 2.0.3): the SHIPPED sample dataset — bundled
  // Resources/sample/fatalities.csv when running from a packaged .app (the
  // gate is then fully self-contained on any machine), else the same file in
  // the worktree (piet/packaging/sample/). $PIET_UI_SMOKE_DATA overrides.
  // env_utf8: a Korean %USERPROFILE%-rooted override arrives from narrow
  // getenv as CP949, which no wide open would ever find (BUG-051).
  std::string dataEnv = cecon::env_utf8("PIET_UI_SMOKE_DATA");
  std::string dataDefault;
  if (std::optional<std::string> resources = piet::bundleResourcesDir()) {
    dataDefault = *resources + "/sample/fatalities.csv";
  } else {
    dataDefault = repoRelative("/piet/packaging/sample/fatalities.csv");
  }
  std::string dataPath = !dataEnv.empty() ? dataEnv : dataDefault;
  std::string outEnv = cecon::env_utf8("PIET_UI_SMOKE_OUT");
  // /tmp does not exist on Windows. Only the Windows arm moves to %TEMP% —
  // temp_directory_path() is $TMPDIR on macOS, so making this unconditional
  // would silently relocate the Mac verdict file.
#ifdef _WIN32
  std::string outDefault =
      (std::filesystem::temp_directory_path() / "piet_ui_smoke_verdict.json").u8string();
#else
  std::string outDefault = "/tmp/piet_ui_smoke_verdict.json";
#endif
  std::string outPath = !outEnv.empty() ? outEnv : outDefault;

  if (!fileExists(dataPath)) {
    std::fprintf(stderr,
                 "[ui-smoke] FATAL: fixture dataset not found at %s (set PIET_UI_SMOKE_DATA)\n",
                 dataPath.c_str());
    return 1;
  }

  std::string workdir = makeWorkdir();
  ScratchDirGuard scratchGuard{workdir};  // reaped at process exit (PIET-TMP/BUG-092)
  AppState app(workdir);
  SmokeState smoke;

  // 2.2.0 item D: isolate the settings store to this run's scratch workdir so
  // the step-13 settings.set journey never clobbers the User's real settings.
  {
    std::string smokeSettings = workdir + "/ui_smoke_settings.json";
#ifndef _WIN32
    setenv("PIET_SETTINGS_FILE", smokeSettings.c_str(), 1);
#else
    // Wide put: a Hangul workdir would be stored mangled by the narrow form,
    // and settings.cpp reads this back through env_utf8/wide open.
    _wputenv_s(L"PIET_SETTINGS_FILE", cecon::to_native_path(smokeSettings).c_str());
#endif
  }

  // s6 EDITOR-FIRST RUN fixture (stabilization campaign delta): a tiny
  // one-line .do file written into this smoke run's OWN scratch workdir (an
  // absolute path, so resolveUserPath() passes it through unchanged
  // regardless of session.cwd — see piet::resolveUserPath's selftest
  // invariants) so the driver's file.read step exercises the real
  // resolve+validate+read path (piet::readDoFileForBridge) against a file
  // that genuinely exists on disk, independent of repo-tree layout.
  std::string doFixturePath = workdir + "/ui_smoke_fixture.do";
  {
    std::ofstream fixtureFile(cecon::to_native_path(doFixturePath));
    fixtureFile << "describe\n";
  }

  // P8.1 W2 (N1): file_save_roundtrip target — an ABSOLUTE .do path in this
  // run's own scratch workdir. The driver writes it via file.save (explicit
  // path) then reads it back via file.read to prove a real disk write; a .do
  // name so file.read (which only serves .do) can verify it.
  std::string saveTargetPath = workdir + "/ui_smoke_save_roundtrip.do";

  // do_file_folder_cwd target: a do-file in a folder of its own, Hangul-named
  // ("do 폴더", spelled as its UTF-8 bytes).
  const std::string siblingDir = workdir + "/do \xED\x8F\xB4\xEB\x8D\x94";
  const std::string siblingDoPath = siblingDir + "/smoke_sibling.do";
  {
    std::error_code siblingEc;
    std::filesystem::create_directories(cecon::to_native_path(siblingDir), siblingEc);
    std::ofstream siblingFile(cecon::to_native_path(siblingDoPath));
    siblingFile << "* ui smoke sibling\n";
  }

  // --- BUG-197: hermetic-verdict regime -------------------------------------
  // Two independent layers, and the report below names both so a future
  // reader of one log line knows which regime judged that run:
  //   1. Ephemeral WKWebsiteDataStore (vendor/webview/webview.h Cocoa patch,
  //      "PIET_WEBVIEW_EPHEMERAL_DATASTORE") — real isolation for this run's
  //      cookies/cache/storage from the host's shared store. Default ON for
  //      this gate; `setenv(..., /*overwrite=*/0)` lets a caller force it off
  //      (used by the seeded-RED probe below to show the census fires
  //      identically either way — see the BUG-197 close note for why: it is
  //      not the layer the confirmed injector goes through).
  //   2. Boot-time injected-content census, always on (main.cpp's
  //      domCensus() inside smokeDriverJs) — THIS is the layer that actually
  //      judges the run: it is what a seeded fake extension trips.
  // Measured non-fix: WKWebsiteDataStore governs persisted browsing state
  // (cookies/cache/IndexedDB/service workers). Grammarly Desktop — the one
  // injector this fleet has confirmed hitting this DOM (piet/README.md
  // "Known issues"; the internal bug ledger BUG-197) — attaches through
  // the macOS Accessibility API / a global input-event tap, neither of which
  // has any relationship to a WKWebView's data-store identity. Item 1 alone
  // is therefore not a fix for BUG-197's finding; it ships anyway as
  // legitimate defense-in-depth for the storage-borne class of interference
  // (a stale cookie/cache left by a PREVIOUS run, a service worker, etc.),
  // and item 2 is the layer whose regime line below is the honest answer to
  // "what judged this run".
#ifndef _WIN32
  setenv("PIET_WEBVIEW_EPHEMERAL_DATASTORE", "1", /*overwrite=*/0);
#endif
  const char* ephemeralEnv = std::getenv("PIET_WEBVIEW_EPHEMERAL_DATASTORE");
  bool ephemeralDatastore = ephemeralEnv && *ephemeralEnv && std::strcmp(ephemeralEnv, "0") != 0;

  // Falsification hook (BUG-197 DONE-WHEN #2): PIET_UI_SMOKE_SEED_INJECTION=1
  // makes THIS run seed a fake foreign <script> node + the confirmed
  // Grammarly DOM markers into the page before the driver's own boot census
  // runs — a hermetic stand-in for "an extension/injector got to this page
  // first" that needs no actual Grammarly install to exercise. A GREEN run
  // under this flag is the gate failing to do its job.
  const char* seedEnv = std::getenv("PIET_UI_SMOKE_SEED_INJECTION");
  bool seedInjection = seedEnv && *seedEnv && std::strcmp(seedEnv, "0") != 0;

  piet::Shell shell(/*debug=*/true);
  shell.setTitle("PIET 2.0 — UI smoke");
  shell.setSize(1280, 860, /*resizable=*/true);
  shell.installBridge();
  // The EXACT product route surface — same function runRealFrontend() uses.
  bindAllRoutes(shell, app);
  // BUG-087/D9 (BETA Phase F): same _request() dialog-push wiring
  // runRealFrontend() does — see that call site's doc comment. Required for
  // the request_dialog_journey step below to exercise a REAL modal, not a
  // headless fallback (this mode always constructs a real Shell/webview).
  app.session.setRequestPostEvent([&shell](const std::string& json) { shell.postEvent(json); });

  // Test-mode-only verdict channel (see the mode doc comment above — NOT a
  // Boundary-B route; never bound in the product run mode).
  shell.bindRoute("smoke.report", [&smoke](const std::string& body) -> piet::BridgeResult {
    nlohmann::ordered_json rep;
    try {
      rep = nlohmann::ordered_json::parse(body);
    } catch (const nlohmann::json::exception&) {
      rep = nlohmann::ordered_json{
          {"final", true}, {"pass", false}, {"step", "report_parse"}, {"detail", body}};
    }
    bool isFinal = rep.value("final", false);
    bool pass = rep.value("pass", false);
    std::printf("[ui-smoke] %s %s %s%s%s\n", isFinal ? "FINAL" : "step ",
                pass ? "PASS" : "FAIL", rep.value("step", std::string("?")).c_str(),
                rep.value("detail", std::string()).empty() ? "" : " — ",
                rep.value("detail", std::string()).c_str());
    {
      std::lock_guard<std::mutex> lock(smoke.m);
      smoke.reports.push_back(rep);
      if (isFinal) {
        smoke.finalReceived = true;
        smoke.finalPass = pass;
      }
    }
    if (isFinal) smoke.cv.notify_all();
    return piet::BridgeResult::success(R"({"ok":true})");
  });

  // Test-mode only, like smoke.report: pushes the same event the native drop
  // handler does (shell.cpp's installFileDropHandler), which page script cannot.
  shell.bindRoute("smoke.drop_do_file", [&shell, siblingDoPath](const std::string&) -> piet::BridgeResult {
    nlohmann::ordered_json ev;
    ev["kind"] = "file_dropped";
    ev["path"] = siblingDoPath;
    shell.postEvent(ev.dump());
    return piet::BridgeResult::success(R"({"ok":true})");
  });

  // Occlusion-verification knob (2026-07-09 transition_sanity hardening): with
  // PIET_UI_SMOKE_OCCLUDE_STEP8=1 the driver WAITS (up to 30s) for the window
  // to actually be occluded (document.hidden, which WebKit ties to
  // NSWindow occlusion on macOS) before running step 8 — so "verify with the
  // window covered" is a deterministic protocol (cover the window after
  // launch), not a race against the ~3s journey.
  const char* occludeEnv = std::getenv("PIET_UI_SMOKE_OCCLUDE_STEP8");
  bool occludeStep8 = occludeEnv && *occludeEnv && std::strcmp(occludeEnv, "0") != 0;

  std::string indexPath = resolveFrontendIndexPath();
  // AFTER installBridge(): user scripts run in registration order, so the
  // driver sees window.pietNative already installed at document start.
  // BEFORE smokeDriverJs when armed: the seeded probe's DOMContentLoaded
  // listener must be registered first so its synchronous DOM mutation lands
  // ahead of the driver's own boot census (see seedInjectionProbeJs()'s
  // comment for why registration order is sufficient here).
  if (seedInjection) {
    shell.initJs(seedInjectionProbeJs());
    std::printf(
        "[ui-smoke] PIET_UI_SMOKE_SEED_INJECTION active — this run WILL seed a fake "
        "injected script + Grammarly DOM markers and expects the boot census to refuse it\n");
  }
  shell.initJs(smokeDriverJs(dataPath, doFixturePath, saveTargetPath, occludeStep8));
  navigateFrontend(shell, indexPath);

  // BUG-197 self-check line: names the regime that judges THIS run's
  // verdict, printed before the journey runs so it is present even on a
  // 90s-timeout RED with zero step reports. The operative regime is always
  // "prereq-checked" (the boot census in smokeDriverJs) — ephemeral_datastore
  // is reported alongside as a second, independently-toggleable fact, not as
  // an alternative regime, since it is measured to not affect this gate's
  // verdict (see the close note above the setenv call).
  std::printf("[ui-smoke] mode: prereq-checked (ephemeral_datastore=%s%s)\n",
              ephemeralDatastore ? "on" : "off", seedInjection ? ", seed_injection=on" : "");

  // Watchdog/terminator: waits for the final verdict (or 90s — covers a page
  // that never executes anything), then exits the UI run loop from this
  // background thread via the dispatch-marshalled terminate. A normal
  // completion signals the cv immediately, so GREEN runs exit fast.
  std::thread terminator([&shell, &smoke]() {
    {
      std::unique_lock<std::mutex> lock(smoke.m);
      smoke.cv.wait_for(lock, std::chrono::seconds(90),
                        [&smoke] { return smoke.finalReceived; });
    }
    shell.terminateFromAnyThread();
  });

  std::printf("[ui-smoke] window open (%s) — journey driver running...\n", indexPath.c_str());
  shell.run();

  terminator.join();
  // Same owned-join discipline as runRealFrontend().
  app.session.progressRequestCancel();
  app.joinAll();

  bool green;
  nlohmann::ordered_json verdict;
  {
    std::lock_guard<std::mutex> lock(smoke.m);
    green = smoke.finalReceived && smoke.finalPass;
    verdict["gate"] = "piet_native --ui-smoke";
    verdict["verdict"] = green ? "GREEN" : "RED";
    verdict["final_received"] = smoke.finalReceived;
    if (!smoke.finalReceived) {
      verdict["timeout"] =
          "no final report within the 90s watchdog — the page/bridge never ran the journey driver";
    }
    verdict["piet_version"] = PIET_VERSION_STRING;
    verdict["contract_a"] = PIET_CONTRACT_A_VERSION;
    verdict["contract_b"] = PIET_CONTRACT_B_VERSION;
    verdict["frontend_index"] = indexPath;
    verdict["data_fixture"] = dataPath;
    // BUG-197: which regime judged this run (see the mode line above for the
    // rationale — prereq-checked is the layer that can actually flip the
    // verdict; ephemeral_datastore is reported as a second independent fact).
    verdict["ui_smoke_regime"] = "prereq-checked";
    verdict["ephemeral_datastore"] = ephemeralDatastore;
    verdict["seed_injection_probe"] = seedInjection;
    verdict["steps"] = smoke.reports;
  }
  {
    std::ofstream vf(cecon::to_native_path(outPath));
    if (vf) {
      vf << verdict.dump(2) << "\n";
      std::printf("[ui-smoke] verdict written: %s\n", outPath.c_str());
    } else {
      std::fprintf(stderr, "[ui-smoke] WARNING: could not write verdict file %s\n",
                   outPath.c_str());
    }
  }
  std::printf("[ui-smoke] %s\n",
              green ? "GREEN — full journey passed in the real platform webview"
                    : "RED — journey failed (see verdict file)");
  return green ? 0 : 1;
}

}  // namespace app_entry_detail
