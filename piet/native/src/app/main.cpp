// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// PIET 2.0 native shell — entry point.
//
// Usage:
//   piet_native                open the real frontend (piet/frontend/) in
//                               the native webview, bridge-backed (M4b)
//   piet_native --demo          open the OLD M0 walking-skeleton demo page
//                               (piet/native/demo/index.html) — kept for the
//                               bridge self-test / historical reference, no
//                               longer the default
//   piet_native --selftest      run the headless self-test (no window) and
//                               exit 0/1 — see selftest.cpp
//   piet_native --evalcase      M1 differential-parity driver: JSONL eval
//                               cases on stdin -> JSONL results on stdout
//                               (see piet/native/tests/eval_diff/)
//   piet_native --run-block     M2 candidate JSON adapter: one piet2-corpus
//                               cell JSON object on stdin -> one block-JSON
//                               result object on stdout (see src/run_block.cpp)
//   piet_native --version       print the COMPILED piet-axis version tuple as
//                               one JSON line and exit 0 (2.0.2 WP-2 single
//                               version source: the packaging script derives
//                               Info.plist from THIS output — see
//                               piet/native/CMakeLists.txt's definition-site
//                               comment)
//   piet_native --ui-smoke      packaged-app UI journey gate (2.0.2 WP-1):
//                               opens the REAL window/WKWebView, drives a
//                               scripted boot -> bridge -> open-dataset ->
//                               varlist -> estimation -> rendered-block
//                               journey, writes a JSON verdict file, exits
//                               0 (GREEN) / 1 (RED) — see runUiSmoke() below
//
// M4b (ROADMAP §2.0.0 "M4b — frontend swap + graph rendering + run-cancel"):
// this is where the real frontend (piet/frontend/, ~4,900 LOC, UNTOUCHED
// beyond api.js's transport swap) gets loaded against the FULL bridge route
// surface (routes.cpp) for the first time — M0's toy demo page is retired
// as the default. The real do-executor's RunProgress publishing goes live
// off the main thread here for the first time (thread-lifetime discipline,
// ROADMAP "M0 shell-review carry-forward" #1, binds here): AppState owns a
// joinable background-run thread registry (NOT a detached fire-and-forget
// thread like M0's synthetic progress.start) so a window-close during an
// in-flight run joins cleanly instead of leaving a dangling Shell&.
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

// Defined in selftest.cpp, external linkage (deliberately outside the
// anonymous namespace below — it's a cross-TU symbol, not file-local).
int runSelftest();

// Defined in src/eval/evalcase.cpp — the M1 differential-parity driver
// (reads JSONL eval cases on stdin, writes JSONL results on stdout; see
// piet/native/tests/eval_diff/).
namespace piet::eval {
int runEvalcaseDriver();
}

// Defined in src/run_block.cpp — the M2 candidate JSON adapter (reads one
// piet2-corpus cell JSON object on stdin, writes one block-JSON result
// object to stdout; see that file's header comment for the protocol).
namespace piet {
int runBlockAdapter();
}

using namespace app_entry_detail;

int main(int argc, char** argv) {
#ifdef _WIN32
  // GUI double-click must NOT pop a console window (this is a -mwindows /
  // WINDOWS-subsystem build, like the macOS .app has no terminal). But the CLI
  // modes (--do/--selftest/--version/--ui-smoke) still need stdout: when we were
  // launched from an interactive terminal, reattach to that parent console so
  // their output is visible. On a double-click there is no parent console,
  // AttachConsole fails harmlessly, and nothing appears. Redirected/piped stdout
  // (e.g. `--do x > out`) already works via the inherited handle regardless of
  // subsystem.
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    // Reattach a std stream to the parent console ONLY where it is not already
    // redirected to a pipe or file — otherwise we'd clobber `--do x > out` or a
    // shell pipe (`--do x | ...`), swallowing the output.
    auto redirected = [](DWORD which) {
      DWORD t = GetFileType(GetStdHandle(which));
      return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
    };
    if (!redirected(STD_OUTPUT_HANDLE)) (void)std::freopen("CONOUT$", "w", stdout);
    if (!redirected(STD_ERROR_HANDLE)) (void)std::freopen("CONOUT$", "w", stderr);
    if (!redirected(STD_INPUT_HANDLE)) (void)std::freopen("CONIN$", "r", stdin);
  }
  // AFTER the freopen()s above (freopen resets the fd's translation mode): put
  // the std streams in binary so the CRT does not rewrite every '\n' the
  // --do/--run-block/--dump-registry writers emit into CRLF. The port is gated
  // on BYTE-IDENTICAL fixture comparison, so a text-mode stdout would fail
  // every freeze check for a reason that has nothing to do with the engine.
  cecon::native_set_standard_streams_binary();
  // BUG-050: recover real Unicode argv from the wide command line. A narrow
  // main() receives argv re-decoded through the ANSI codepage, which on a
  // CP949 (Korean) system turns a Hangul path arg (e.g. --do
  // "...\분석.do") into CP949 bytes that are NOT valid UTF-8 — so the first
  // to_native_path()/u8path() on it throws filesystem "Illegal byte sequence".
  // Re-expose args as UTF-8 (same treatment cobra's main already gets) so every
  // downstream path arg is UTF-8. u8store/u8argv live for all of main().
  std::vector<std::string> u8store;
  std::vector<char*> u8argv;
  if (int wargc = 0; LPWSTR* wv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
    u8store.reserve(static_cast<size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
      int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string s;
      if (n > 0) {
        s.resize(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, s.data(), n, nullptr, nullptr);
        s.resize(static_cast<size_t>(n - 1));  // drop the trailing NUL
      }
      u8store.push_back(std::move(s));
    }
    LocalFree(wv);
    for (auto& s : u8store) u8argv.push_back(s.data());
    u8argv.push_back(nullptr);
    argc = wargc;
    argv = u8argv.data();
  }
#endif
  bool demo = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--selftest") == 0) {
      // R1 (selftest split): the suite lives in the sibling `piet_selftest`
      // binary now — exec it so every script that says `piet_native --selftest`
      // keeps working, dev tree and CI alike. The product binary itself no
      // longer carries the 24k-line suite; a packaged .app that does not bundle
      // piet_selftest gets an honest pointer instead of a silent no-op.
#ifndef _WIN32
      std::error_code selfEc;
      std::filesystem::path self =
          std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), selfEc);
      std::filesystem::path sib =
          (selfEc || !self.has_parent_path()) ? std::filesystem::path("piet_selftest")
                                              : self.parent_path() / "piet_selftest";
      ::execl(sib.string().c_str(), "piet_selftest", static_cast<char*>(nullptr));
      std::fprintf(stderr,
                   "[piet_native] --selftest: could not exec %s (%s). The suite builds as the "
                   "separate `piet_selftest` binary since the R1 split — build it with "
                   "`cmake --build <builddir> --target piet_selftest` and run it directly.\n",
                   sib.string().c_str(), std::strerror(errno));
      return 2;
#else
      std::fprintf(stderr,
                   "[piet_native] --selftest: run the sibling piet_selftest.exe directly on "
                   "Windows (R1 split).\n");
      return 2;
#endif
    }
    if (std::strcmp(argv[i], "--version") == 0) {
      // WP-2 single-version-source derivation point: prints the COMPILED
      // piet-axis tuple. package_native_app.sh reads this back out of the
      // binary it bundles to stamp Info.plist — never a hand-synced copy.
      std::printf("{\"piet_version\":\"%s\",\"contract_a\":\"%s\",\"contract_b\":\"%s\"}\n",
                  PIET_VERSION_STRING, PIET_CONTRACT_A_VERSION, PIET_CONTRACT_B_VERSION);
      return 0;
    }
    if (std::strcmp(argv[i], "--dump-registry") == 0) {
      // BETA campaign Phase A0: prints registry::commandRegistry() as JSON —
      // MUST be a returning branch here (not a bool-then-fallthrough like
      // --demo below), since the post-loop fallthrough calls
      // runRealFrontend(), which hangs headless (see registry.h's doc
      // comment + the A0 anchor map's LANDMINE note).
      return piet::registry::runDumpRegistry();
    }
    if (std::strcmp(argv[i], "--dump-prefix-registry") == 0) {
      return piet::registry::runDumpPrefixRegistry();
    }
    // AER scope-2 A1: the option-vocabulary census, and the one-option probe of
    // dispatcher.cpp's resolution point. RETURNING arms for the same reason
    // --dump-registry above spells out. Both stand alone and must precede any
    // --do, which swallows every remaining argv below.
    if (std::strcmp(argv[i], "--dump-optvocab") == 0) {
      if (i + 1 < argc && std::strcmp(argv[i + 1], "--resolve") == 0) {
        // Arity error follows the --parallel-worker/--do precedent: stderr and
        // rc 2, never a fallthrough into runRealFrontend()'s headless hang.
        if (i + 3 >= argc) {
          std::fprintf(stderr,
                       "[piet_native] --dump-optvocab --resolve requires <command> <option>\n");
          return 2;
        }
        return piet::registry::runDumpOptVocabResolve(argv[i + 2], argv[i + 3]);
      }
      return piet::registry::runDumpOptVocab();
    }
    if (std::strcmp(argv[i], "--dump-functions") == 0) {
      return piet::registry::runDumpFunctions();
    }
    if (std::strcmp(argv[i], "--ui-smoke") == 0) {
      return runUiSmoke();
    }
    if (std::strcmp(argv[i], "--evalcase") == 0) {
      return piet::eval::runEvalcaseDriver();
    }
    if (std::strcmp(argv[i], "--run-block") == 0) {
      return piet::runBlockAdapter();
    }
    // PLEVEL-IMPL-WAVE arc ①: the parallel WORKER entry point (design §3.2).
    // A returning branch alongside --run-block/--evalcase, for the same reason
    // --dump-registry documents: falling through would reach runRealFrontend(),
    // which hangs headless. Arc ② is what spawns this; landing the entry point
    // (and its spec schema) first means that arc only has to add the fork/exec
    // half, against a child that already exists and can be driven by hand.
    if (std::strcmp(argv[i], "--parallel-worker") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "[piet_native] --parallel-worker requires a task-spec path\n");
        return 2;
      }
      return piet::parallel::runParallelWorker(argv[i + 1]);
    }
    if (std::strcmp(argv[i], "--do") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "[piet_native] --do requires a .do file path\n");
        return 2;
      }
      std::string filepath = argv[i + 1];
      // Everything after the file path is a do-file arg (e.g. `no`/`yes`).
      std::vector<std::string> doArgs;
      for (int j = i + 2; j < argc; ++j) doArgs.push_back(argv[j]);
      return runDoFileHeadless(filepath, doArgs);
    }
    if (std::strcmp(argv[i], "--demo") == 0) {
      demo = true;
    }
  }
  if (demo) {
    runDemoPage();
  } else {
    runRealFrontend();
  }
  return 0;
}
