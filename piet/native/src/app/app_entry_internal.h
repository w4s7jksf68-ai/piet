// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private declarations shared by the native app entry translation units (main.cpp,
// bridge_route_bindings.cpp, startup_environment.cpp, frontend_window.cpp,
// headless_do_runner.cpp, ui_smoke.cpp, ui_smoke_driver.cpp). Not part of piet_core.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/shell.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

namespace piet {
// R1 (selftest split): the app-support block that lived here (defaultUserCwd,
// readDoFileForBridge, the BUG-133 scratch-owner trio) moved VERBATIM to
// src/app_support.cpp so piet_selftest can link it without main.cpp. These are
// the same forward declarations selftest.cpp has always used.
std::string defaultUserCwd();
struct FileReadResult {
  bool ok = false;
  std::string errorMessage;
  std::string source;
  std::string resolvedPath;
};
FileReadResult readDoFileForBridge(const SessionState& session, const std::string& rawPath);
std::string followDoFileDirectory(SessionState& session, const std::string& resolvedDoFile);
const char* scratchOwnerMarkerName();
void writeScratchOwnerMarker(const std::string& dir);
int reapDeadScratch(const std::string& base);
}  // namespace piet

namespace app_entry_detail {

// AppState — owns the ONE persistent SessionState the live UI's bridge
// routes all close over (unlike run_block.cpp's fresh-session-per-cell
// harness), plus the background-run thread registry (thread-lifetime
// discipline, see file header). A do/do_string call spawns a worker thread
// that:
//   1. Runs doexec::executeDo/executeDoSource (which internally publishes
//      to session.progress_ — mutex-guarded, safe to read concurrently from
//      routes::runProgress on the UI thread's bind() callback).
//   2. Periodically (and at the end) calls shell.postEvent() with the SAME
//      RunProgress JSON schema (safe from any thread per shell.h's
//      threading note) so the frontend's progress strip updates live
//      instead of only on poll.
//   3. Resolves the ORIGINAL bind() promise (via BridgeResult delivered
//      through a std::promise/future pair) once done, so `await
//      pietNative.request('do_string', ...)` still returns the final
//      {blocks,...} payload the frontend awaits — do/do_string remain
//      request/response from the JS caller's point of view; the progress
//      PUSH is an independent side channel, not a replacement for the
//      response.
// The thread is OWNED (joined, never detached) — AppState::joinAll() is
// called before the window closes (Shell::terminate() is UI-thread-only per
// shell.h; a run's worker thread never calls it — it only postEvent()s,
// which is thread-safe from anywhere).
struct AppState {
  explicit AppState(std::string workdir) : session(std::move(workdir)) {}

  piet::SessionState session;

  std::mutex runThreadsMutex;
  // BUG-391 (2026-09-02): each run thread carries its own completion flag. The
  // old comment here said reaping "happens in joinAll(), called after each run
  // completes" — joinAll() was called ONLY at shutdown, so every do/do_string run
  // left a joinable thread (handle + stack) in this vector for the life of the
  // app. std::thread has no is-done query; the flag is the query. The worker sets
  // it as its very last statement, so a thread whose flag is set is guaranteed
  // to be past its last access to the Shell/AppState and joins without waiting.
  struct RunThread {
    std::thread t;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<RunThread> runThreads;

  // Reap every FINISHED thread (join it, drop its slot) — called at the start
  // of each new do/do_string call and before shutdown, so runThreads is bounded
  // by the number of runs still executing (normally 0 or 1).
  void reapFinished() {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    std::vector<RunThread> stillRunning;
    for (auto& rt : runThreads) {
      if (rt.t.joinable() && rt.done && rt.done->load(std::memory_order_acquire)) {
        rt.t.join();
        continue;
      }
      if (rt.t.joinable()) stillRunning.push_back(std::move(rt));
    }
    runThreads = std::move(stillRunning);
  }

  size_t liveRunThreads() {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    return runThreads.size();
  }

  void registerThread(std::thread t, std::shared_ptr<std::atomic<bool>> done) {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    runThreads.push_back(RunThread{std::move(t), std::move(done)});
  }

  // Joins every registered thread. Safe to call multiple times. MUST be
  // called before the Shell is destroyed (window close) — this is the
  // "owned join point" the thread-lifetime discipline requires: no run
  // thread outlives the Shell it postEvent()s into.
  void joinAll() {
    std::lock_guard<std::mutex> lock(runThreadsMutex);
    for (auto& rt : runThreads) {
      if (rt.t.joinable()) rt.t.join();
    }
    runThreads.clear();
  }
};

// PIET-TMP (BUG-092): the per-process scratch dir makeWorkdir() mkdtemp's is
// disposable — every result/data path resolves against session.cwd, never this
// scratch dir (see runDoFileHeadless' comment) — so it must NOT outlive the
// process, or each run leaks its whole scratch tree (a ~350MB hbin/csv pair per
// string-op accumulates under the in-session budget GC, but the DIRECTORY itself
// was never reaped) onto the user's disk. This RAII guard removes the ENTIRE
// session dir at scope exit. Declare it BEFORE the AppState it pairs with so it
// destructs AFTER (AppState::joinAll() has joined every run thread by then, so
// nothing writes scratch during removal). It only ever removes our own
// mkdtemp'd `piet_native_XXXXXX` subdir, never the user's PIET_WORKDIR/TMPDIR
// base. Set PIET_KEEP_SCRATCH to retain it (debugging a run's intermediate hbins).
struct ScratchDirGuard {
  std::string path;
  ScratchDirGuard() = default;
  explicit ScratchDirGuard(std::string p) : path(std::move(p)) {}
  ScratchDirGuard(const ScratchDirGuard&) = delete;
  ScratchDirGuard& operator=(const ScratchDirGuard&) = delete;
  ~ScratchDirGuard() {
    if (path.empty()) return;
    if (const char* keep = std::getenv("PIET_KEEP_SCRATCH"); keep && *keep) {
      std::fprintf(stderr, "[piet_native] PIET_KEEP_SCRATCH set — leaving scratch: %s\n",
                   path.c_str());
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(cecon::to_native_path(path), ec);  // best-effort
  }
};

// frontend_window.cpp
std::string repoRelative(const std::string& suffix);
bool fileExists(const std::string& path);
std::string resolveFrontendIndexPath();
void navigateFrontend(piet::Shell& shell, const std::string& indexPath);
void runRealFrontend();
void runDemoPage();

// bridge_route_bindings.cpp
void bindAllRoutes(piet::Shell& shell, AppState& app);

// startup_environment.cpp
std::string makeWorkdir();
piet::settings::Settings applyPersistedSettings();

// headless_do_runner.cpp
int runDoFileHeadless(const std::string& filepath, const std::vector<std::string>& args);

// ui_smoke_driver.cpp
std::string smokeDriverJs(const std::string& dataPath, const std::string& doFixturePath,
                          const std::string& saveTargetPath, bool occludeStep8);
std::string seedInjectionProbeJs();

// ui_smoke.cpp
int runUiSmoke();

}  // namespace app_entry_detail
