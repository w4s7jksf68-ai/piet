// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PIET_SELFTEST_SUPPORT_H
#define PIET_SELFTEST_SUPPORT_H
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Headless self-test for M0. Design constraint (per the M0 directive and the
// project's standing "no computer-use for PIET testing" rule): our gates
// must stay headless — no visible window is ever driven by automation. The
// vendored webview library has NO headless mode (webview_create always
// instantiates a real WKWebView/GTK/WebView2 window; see webview.h's
// webview_create()), so --selftest deliberately never constructs a
// piet::Shell or calls run()/navigate() on a real window. Instead it
// exercises the exact same C++-side logic the interactive demo wires up:
//
//   1. Bridge echo — calls piet::routes::echo() directly (the identical
//      function main.cpp registers via shell.bindRoute("echo", ...)), AND
//      runs the ACTUAL JS shim source text (piet::internal::pietEventsShimJs
//      / pietNativeRequestShimJs — the exact strings Shell::installBridge()
//      injects, not a paraphrase) through a real headless JS engine (Node,
//      if available on PATH) with a mocked pietNativeInvoke(), to prove the
//      Promise-based window.pietNative.request() plumbing and the
//      window.pietEvents CustomEvent dispatch both work end-to-end. This is
//      the literal "injected JS eval" the M0 directive asks for — it is a
//      real JS interpreter executing the real shim, just not inside
//      WKWebView. If Node is not on PATH, this leg is skipped with a clear
//      SKIP marker (not silently reported as PASS).
//   2. Progress stream — runs piet::runSyntheticProgress() synchronously to
//      completion AND exercises the cancel path, collecting every tick and
//      validating the B1 JSON schema field-for-field.
//   3. cobra roundtrip — calls piet::routes::cobraVersion() directly (same
//      function the demo's "cobra.version" route uses).
//   4. (M4b) bridge routes — every routes::* adapter added for the M4b
//      frontend swap (command/do/do_string/run_progress/run_cancel/open/
//      varlist/browse/history/version/rename/label.*/sort/recode/cell/
//      tabulate/describe/summarize), driven against a REAL SessionState the
//      same way main.cpp's bindAllRoutes() closures do, PLUS a Node-driven
//      JS-shim test proving window.pietNative.request()'s ASYNC dispatch
//      contract (bindAsyncRoute/resolveInvoke — do/do_string's "resolve
//      later, from a worker thread" path) round-trips correctly through the
//      REAL shim source text, not a paraphrase.
//
// This is real coverage of the bridge/progress/cobra C++ logic, not a
// reimplementation: main.cpp and selftest.cpp both call into
// piet/routes.h + piet/progress.h + piet/cobra_runner.h. What this does NOT
// cover: the actual WKWebView JS execution environment (Promise resolution,
// CustomEvent dispatch, window.pietNative binding installation at runtime).
// That gap is exactly why the M0 gate also ships a User checklist for the
// visual window demo — see the final report.
#ifndef _WIN32
#include <sys/wait.h>  // WIFEXITED/WEXITSTATUS for pclose (WP-W5: absent on MinGW)
#include <unistd.h>    // fork/_exit/getpid — BUG-133 orphan-reap leg (dead-pid)
#endif

#include <algorithm>  // std::find — the S1b fold gates' card-membership checks
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>  // WP-W5: portable temp dir + mkdir/rm (replaces /tmp + shell)
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/base64.h"  // P8.1-W2: base64Encode roundtrip for the file.save leg
#include "app/bridge_internal.h"
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink (BUG-050 wide open)
#include "piet/bundle_paths.h"
#include "piet/coord_root.h"  // BUG-209: upward search for the coordination tree (releases/CURRENT leg below)
#include "fileio/xlsx.h"  // P1 wave: the .xlsx reader/writer under selftest
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/parallel.h"
#include "piet/preflight.h"
#include "piet/parallel_auto.h"
#include "piet/parallel_effects.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/gettoken.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/bcal.h"  // AER scope-2 B2-3: the calendar reader/writer under test
#include "piet/progress.h"
#include "piet/routes.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/strmutate.h"  // S4a gate: needsStringCount -> factorColumns, called directly
#include "dispatch/registry.h"
#include "vendor/nlohmann/json.hpp"
#include "selftest_feature_cases.h"
#include "selftest_suites.h"
#include "piet/strutil.h"

// GA sweep 2026-08-22 (leg ⑤). The fixture fallbacks below used to end in a
// hardcoded absolute tests/validate path — one developer's disk
// layout, compiled into shipped source. It was never a correctness risk (every
// site skips honestly when the file is absent, which is what a clone already
// does), but it broadcast the layout and was dead weight anywhere else. The
// override that already existed for ONE of the eight sites, COBRA_VALIDATE_DIR,
// is now the mechanism for all of them; with it unset the relative path from
// __FILE__ is the only source, and a miss is a declared skip as before.


#ifdef _WIN32
// <windows.h> defines the 16-bit-era keywords `near` and `far` as empty macros,
// which turns the `auto near = ...` helpers below into a syntax error.
#undef near
#undef far

// MinGW/MSVC do not provide the POSIX setenv()/unsetenv() helpers used by
// several escape-hatch self-tests below. Keep those tests enabled on Windows
// and preserve setenv(overwrite=0) semantics instead of compiling them out —
// weakening the gate to make it build is exactly what a port must not do.
static inline int setenv(const char* name, const char* value, int overwrite) {
  if (!overwrite && std::getenv(name) != nullptr) return 0;
  return _putenv_s(name, value);
}

// _putenv_s with an empty value DELETES the variable, which is unsetenv's
// contract (the escape-hatch tests assert the var is gone, not empty).
static inline int unsetenv(const char* name) {
  return _putenv_s(name, "");
}
#endif

// Defined in main.cpp (external linkage, `namespace piet` — NOT the file's
// anonymous namespace): the SAME cwd-seeding logic runRealFrontend() calls
// right after AppState construction. Forward-declared here (no shared
// header — main.cpp and selftest.cpp already cross-call this way, e.g.
// `int runSelftest()` is declared in main.cpp and defined here) so the
// cwd-seeded-at-startup leg below exercises the REAL function, not a
// paraphrase of its logic.
namespace piet {
std::string defaultUserCwd();

// Defined in main.cpp, right above bindAllRoutes(): the resolve+validate+
// read logic backing the `file.read` bridge route (WS-F1 scope addendum).
// Forward-declared here for the same reason as defaultUserCwd() above —
// piet::Shell's constructor opens a REAL window, so selftest can't reach
// an inline-bindRoute-only implementation; this free function is what
// makes the route's rules testable headlessly.
struct FileReadResult {
  bool ok = false;
  std::string errorMessage;
  std::string source;
  std::string resolvedPath;
};
FileReadResult readDoFileForBridge(const SessionState& session, const std::string& rawPath);

// Defined in main.cpp (BUG-133 orphan-scratch reaping). Forward-declared here
// for the same reason as above — the leg below drives the REAL startup sweep and
// marker writer makeWorkdir() uses, not a paraphrase of the reap rules.
const char* scratchOwnerMarkerName();
void writeScratchOwnerMarker(const std::string& dir);
int reapDeadScratch(const std::string& base);
}


namespace piet::selftest {
extern int g_failures;
extern int g_skipped;
const char* selftestSourceFile();
std::string selftestValidateDir();
void check(bool cond, const std::string& label);
void skip(const std::string& label, const std::string& why);
std::pair<int, std::string> runCommandCapture(const std::string& cmd);
std::string commandPathQuote(const std::string& s);
std::pair<int, std::string> runNode(const std::string& scriptPath);
bool nodeAvailable();
bool parseSemver(const std::string& s, int out[3]);
int cmpSemver(const int a[3], const int b[3]);
bool parsePietVersion(const std::string& s, int out[4]);
int cmpPietVersion(const int a[4], const int b[4]);
std::string plistStringForKey(const std::string& plist, const std::string& key);
std::string currentFileValue(const std::string& text, const std::string& name);
std::string joinForMsg(const std::vector<std::string>& xs);
const nlohmann::ordered_json* firstBlockOfTypeD1(const nlohmann::ordered_json& blocks,
                                                  const std::string& type);
std::optional<double> coefFieldD1(const nlohmann::ordered_json& est, const std::string& name,
                                   const std::string& field);
}
using namespace piet::selftest;
#endif
