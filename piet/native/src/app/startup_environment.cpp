// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Process startup environment: per-process scratch workdir, persisted settings to env.
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

#ifdef _WIN32
// Windows has no mkdtemp. Create a uniquely-named directory under the base:
// GetTempFileNameA yields a unique name (and creates a placeholder FILE); we
// delete that file and CreateDirectory the same path.
std::string makeWorkdir() {
  // BUG-050 (scratch side): a Korean username makes %TEMP% a Hangul path (and a
  // Hangul scratch dir can be set in Settings). Reading it with ANSI getenv and
  // creating the workdir with GetTempFileNameA/CreateDirectoryA mangles it
  // through the CP949/CP1252 codepage, so every command's scratch write fails.
  // Read the base WIDE and create the dir WIDE, then return UTF-8 — the encoding
  // the rest of piet hands to cobra (cobra_runner's CreateProcessW uses CP_UTF8),
  // keeping the whole workdir Unicode-clean end to end.
  auto wenv = [](const wchar_t* k) -> std::wstring {
    DWORD n = GetEnvironmentVariableW(k, nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring v(n, L'\0');
    DWORD m = GetEnvironmentVariableW(k, &v[0], n);
    v.resize(m);
    return v;
  };
  auto w2u = [](const std::wstring& w) -> std::string {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
  };
  std::wstring wbase = wenv(L"PIET_WORKDIR");
  if (wbase.empty()) wbase = wenv(L"TEMP");
  if (wbase.empty()) wbase = wenv(L"TMP");
  if (wbase.empty()) wbase = L".";
  // BUG-134 (Windows half): a Settings scratch_path seeded into PIET_WORKDIR may
  // not exist yet, and CreateDirectoryW below creates only the LAST component.
  // Mirror the POSIX branch's create_directories step so the first launch after
  // the user picks a new scratch dir does not fall through to the PID fallback.
  {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(wbase), ec);
  }
  // Atomic, collision-free per-process scratch dir (mirrors the POSIX mkdtemp
  // branch below). The prior GetTempFileNameW + DeleteFileW + CreateDirectoryW
  // sequence was a TOCTOU race: two piet_native processes started in the same
  // tick (e.g. the student opens the app twice) draw the same time-seeded name
  // from GetTempFileNameW, the DeleteFileW discards its on-disk uniqueness
  // guarantee, and the CreateDirectoryW loser fell through to a FIXED
  // "piet_native_fallback" dir — so concurrent losers then collided on
  // identically-named scratch files, fatal under the Windows mmap file-lock
  // (macOS's mkdtemp is atomic and random-named, so POSIX never saw this).
  // CreateDirectoryW is itself atomic (fails ERROR_ALREADY_EXISTS if the name
  // is taken), so retry it directly with a process-unique candidate and let the
  // OS arbitrate — there is no delete-then-create window for another process to
  // slip into. The last-resort fallback is PID-qualified so it cannot collide.
  // BUG-133: reap our own SIGTERM/SIGKILL-orphaned scratch dirs under this base
  // before minting a new one (mirrors the POSIX branch).
  piet::reapDeadScratch(w2u(wbase));
  const DWORD pid = GetCurrentProcessId();
  for (unsigned i = 0; i < 4096; ++i) {
    std::wstring cand =
        wbase + L"\\piet_native_" + std::to_wstring(pid) + L"_" + std::to_wstring(i);
    if (CreateDirectoryW(cand.c_str(), nullptr)) {
      std::string dir = w2u(cand);
      piet::writeScratchOwnerMarker(dir);  // BUG-133: stamp for later reaping
      return dir;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) break;  // real error — stop retrying
  }
  return w2u(wbase) + "\\piet_native_fallback_" + std::to_string(pid);
}
#else
std::string makeWorkdir() {
  // Scratch base selection (PIET-TMP / BUG-092): PIET_WORKDIR (explicit env
  // override, or the Settings scratch_path seeded into it) wins; else honor the
  // POSIX TMPDIR convention — this is what lets a user or the GA-peel harness
  // redirect the per-op hbin/csv scratch off a nearly-full boot volume with
  // `TMPDIR=/big/vol` (previously TMPDIR was ignored and scratch always landed
  // in /tmp); else /tmp. Trailing slashes are trimmed so a `TMPDIR=/dir/` value
  // doesn't yield a `//piet_native_XXXXXX` double-slash template.
  const char* envDir = std::getenv("PIET_WORKDIR");
  if (!envDir || !*envDir) envDir = std::getenv("TMPDIR");
  std::string base = envDir && *envDir ? envDir : "/tmp";
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  // BUG-134: mkdtemp(3) requires its parent directory to ALREADY exist. The
  // Settings scratch_path is seeded into PIET_WORKDIR (runRealFrontend, below)
  // with no validation, so the very first launch after a user picks a not-yet-
  // existing custom scratch dir made mkdtemp ENOENT out and silently degrade to
  // the fixed "piet_native_fallback" name — a NON-unique dir two concurrent
  // piet_native processes would share, letting the first to exit remove_all() it
  // out from under the other. Create the base first (best-effort, a no-op when it
  // already exists, as on the /tmp|TMPDIR gate paths) so the normal atomic random
  // mkdtemp path succeeds and the fallback is never reached for this reason.
  {
    std::error_code ec;
    std::filesystem::create_directories(cecon::to_native_path(base), ec);
  }
  // BUG-133: before minting our own dir, reap any of OUR earlier scratch dirs
  // under this base that a SIGTERM/SIGKILL orphaned (dead owner pid + settled
  // mtime). Runs once per process (makeWorkdir is called once per entry point).
  piet::reapDeadScratch(base);
  std::string tmpl = base + "/piet_native_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  char* result = mkdtemp(buf.data());
  if (result) {
    piet::writeScratchOwnerMarker(result);  // BUG-133: stamp for later reaping
    return std::string(result);
  }
  // Fallback (mkdtemp failed — e.g. the base is uncreatable/unwritable): PID-
  // qualify the name exactly like the Windows branch above so even this degraded
  // path stays collision-safe between concurrent processes, and warn LOUDLY — a
  // silent runtime degradation is otherwise only discoverable by inspecting the
  // filesystem by hand (which is how BUG-134 was found).
  const int mkdErrno = errno;
  std::string fb = base + "/piet_native_fallback_" + std::to_string(::getpid());
  std::fprintf(stderr,
               "[piet_native] WARNING: could not create a unique scratch dir under "
               "'%s' (mkdtemp: %s); falling back to '%s'. Check the scratch base "
               "exists and is writable (Settings > Engine & scratch).\n",
               base.c_str(), std::strerror(mkdErrno), fb.c_str());
  return fb;
}
#endif

// 2.2.0 item D — apply the persisted engine/scratch settings.
//
// BUG-346: this used to run on the GUI launch path ONLY, and the comment here
// said why: "--do/--ui-smoke/--selftest stay deterministic on $PIET_WORKDIR/tmp".
// Determinism is the right instinct for a GATE and the wrong answer for a user's
// own run. Measured 2026-08-14: with scratch_path pointed at an external volume
// (767 GiB free) precisely so scratch would not sit on the boot disk, a
// `--do` run of the acceptance corpus under `parallel, workers(2)` put every
// worker's scratch on the boot volume instead, failed 99 writes with "disk full",
// and silently finished with 208 estimations out of 640.
//
// So the settings now apply to `--do` as well, and the two properties the old
// comment was protecting are kept explicitly instead of by exclusion:
//   * an explicit $PIET_WORKDIR still wins (it always did — the seed is
//     conditional on the variable being unset), which is how every gate and
//     harness pins its own scratch;
//   * `applyGates = false` lets the deterministic entry points (--selftest,
//     --ui-smoke) opt out entirely, so a gate's environment is still decided by
//     the gate and never by whatever the user last clicked in Settings.
piet::settings::Settings applyPersistedSettings() {
  piet::settings::Settings cfg = piet::settings::load();
  if (!cfg.scratchPath.empty()) {
    const char* envWd = std::getenv("PIET_WORKDIR");
    if (!envWd || !*envWd) {
#ifndef _WIN32
      setenv("PIET_WORKDIR", cfg.scratchPath.c_str(), 1);
#else
      _wputenv_s(L"PIET_WORKDIR", cecon::to_native_path(cfg.scratchPath).c_str());
#endif
    }
  }
  piet::settings::applyEngineBudget(cfg);
  return cfg;
}

}  // namespace app_entry_detail
