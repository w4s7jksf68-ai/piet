// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Frontend location and navigation, the product window, and the --demo page.
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

std::string readFileOrDie(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "[piet_native] FATAL: cannot open page at %s\n", path.c_str());
    std::exit(1);
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

// Resolves a path relative to the development checkout this binary belongs
// to (compile-time anchor when valid, else the executable's location, else
// the cwd — see piet/bundle_paths.h).
std::string repoRelative(const std::string& suffix) {
  // BUG-1939: __FILE__ is only the first candidate; a -ffile-prefix-map build
  // makes it relative, and the executable's own location then has to anchor
  // the tree (piet::devTreePath, shared with sysuse / c(sysdir_sample) /
  // the notices route).
  return piet::devTreePath(suffix);
}

bool fileExists(const std::string& path) {
  return cecon::native_exists(path);
}

// M5 GA: resolves piet/frontend/index.html — bundled Resources/frontend/
// FIRST when running from inside a packaged .app, dev-tree __FILE__-relative
// path otherwise. Mirrors resolveCobraBin()'s bundle-first order
// (cobra_runner.cpp) so both halves of "what does a packaged .app load"
// (the UI bundle, the engine binary) resolve the same way for the same
// reason: a shipped .app must be self-contained off the machine that built
// it, and a dev checkout must keep working unmodified for every other
// invocation (dev `cmake --build` binary run in place, --run-block/
// --selftest headless harnesses, Eval's acceptance scripts run against
// piet/native/build/piet_native directly).
// Item 3 (loud-failure hardening): a packaged .app whose bundled
// Resources/frontend/index.html is missing (a packaging-script regression —
// see the Windows "resources\ runtime layer" canary bug this same
// stabilization wave already fixed once for the cobra binary) must NOT
// silently fall through to the dev-tree repoRelative() path: that path is
// derived from this SOURCE FILE's build-time __FILE__ location, which on a
// shipped .app is some OTHER machine's checkout and essentially never
// exists — navigating a file:// URL that resolves to nothing leaves a blank
// window with no diagnostic (exactly the ERR_NAME_NOT_RESOLVED-shaped
// failure mode). So: try the packaged path first; if that's not there, try
// the dev-tree path as a best-effort fallback (covers running a freshly
// packaged .app in place on the SAME machine that built it, e.g. local
// packaging QA); only when NEITHER exists do we die loud instead of
// silently navigating a dead path.
std::string resolveFrontendIndexPath() {
  std::optional<std::string> resources = piet::bundleResourcesDir();
  std::string bundled;
  if (resources.has_value()) {
    bundled = *resources + "/frontend/index.html";
    if (fileExists(bundled)) return bundled;
  }
  std::string dev = repoRelative("/piet/frontend/index.html");
  if (fileExists(dev)) return dev;

  std::fprintf(stderr,
               "[piet_native] FATAL: frontend index.html not found.\n"
               "  bundled candidate:  %s\n"
               "  dev-tree candidate: %s\n",
               resources.has_value() ? bundled.c_str() : "(not running from inside a packaged .app bundle)",
               dev.c_str());
  std::exit(1);
}

// Loads the on-disk frontend tree into the shell (WP-W4 platform branch).
// macOS/Linux navigate a file:// URL — the vendored webview.h grants file://
// documents sibling-resource access (the @a9141d1 patch) so ES-module imports
// resolve. Windows/WebView2 (Chromium) rejects ES-module imports over file://
// the same way WebKit did, so we map a virtual host to the frontend directory
// and navigate to an https:// origin instead (SetVirtualHostNameToFolderMapping).
// piet.invalid = an RFC-2606 reserved TLD that can never resolve on a real
// network — the mapping intercepts it before DNS regardless, keeping the
// offline-first guarantee even if the mapping ever fails.
void navigateFrontend(piet::Shell& shell, const std::string& indexPath) {
#ifdef _WIN32
  std::string dir = indexPath;
  std::string file = "index.html";
  auto slash = indexPath.find_last_of("/\\");
  if (slash != std::string::npos) {
    dir = indexPath.substr(0, slash);
    file = indexPath.substr(slash + 1);
  }
  shell.setVirtualHostMapping("piet.invalid", dir);
  shell.navigate("https://piet.invalid/" + file);
#else
  shell.navigate("file://" + indexPath);
#endif
}

void runRealFrontend() {
  const piet::settings::Settings cfg = applyPersistedSettings();

  std::string workdir = makeWorkdir();
  ScratchDirGuard scratchGuard{workdir};  // reaped at process exit (after app dtor joins threads)
  AppState app(workdir);
  // Seed cwd BEFORE any command runs (item 1, native-paths stabilization):
  // an unseeded session.cwd falls back to getcwd(), which under a Finder/
  // dock launch is "/" — see piet::defaultUserCwd()'s comment. 2.2.0 item D
  // lets the user pin a default working directory (used only if it exists —
  // else fall back to defaultUserCwd()). Mirror into eval.cwd the same way
  // runCd does (environment/workspace_commands.cpp) so the macro layer's own cwd view (used by
  // e.g. `local d : pwd` idioms) agrees with session.cwd from the first
  // command onward.
  std::string seedCwd = piet::defaultUserCwd();
  if (!cfg.defaultWorkingDir.empty()) {
    std::error_code seedEc;
    if (std::filesystem::is_directory(cecon::to_native_path(cfg.defaultWorkingDir), seedEc)) {
      seedCwd = cfg.defaultWorkingDir;
      app.session.cwdChosenByUser = true;
    }
  }
  app.session.cwd = seedCwd;
  app.session.eval.cwd = *app.session.cwd;

  piet::Shell shell(/*debug=*/true);
  shell.setTitle("PIET");
  shell.setSize(1280, 860, /*resizable=*/true);
  shell.installBridge();
  bindAllRoutes(shell, app);
  // BUG-087/D9 (BETA Phase F): wire the _request() dialog-push hook now
  // that a real Shell exists — SessionState::hasRequestPostEvent() is the
  // headless-vs-windowed signal language/display_command.cpp's _request() handling
  // consults. `shell` is captured by reference: it outlives every use of
  // this callback the same way runDoInBackground's captured Shell* does
  // (see that function's doc comment) — every do-run worker thread that
  // could invoke this is joined (app.joinAll(), below) before `shell` goes
  // out of scope at the end of this function.
  app.session.setRequestPostEvent([&shell](const std::string& json) { shell.postEvent(json); });

  std::string indexPath = resolveFrontendIndexPath();
  // file:// (NOT a data: URI): the real frontend uses ES module relative
  // imports (`import {...} from './js/api.js'` etc.) which need a real
  // filesystem base URL to resolve — a data: URI has an opaque origin and
  // cannot resolve relative module specifiers. This is the actual M4b
  // transport: the webview loads the SAME on-disk frontend tree the old
  // Python-backend era served over HTTP, just via file:// instead of
  // http://127.0.0.1:7331. M5 GA: resolveFrontendIndexPath() prefers a
  // bundled Resources/frontend/index.html when running from inside a
  // packaged .app, so the SAME file:// mechanism now also serves the
  // shipped bundle's copy — no code path divergence between dev and GA.
  // (Windows/WebView2 serves the same tree over a mapped virtual host — see
  // navigateFrontend.)
  navigateFrontend(shell, indexPath);

  std::printf("[piet_native] window open (real frontend, %s) — close it to exit.\n", indexPath.c_str());
  shell.run();

  // Owned join point (thread-lifetime discipline): every background run
  // thread is joined here, AFTER shell.run() returns (window closed) but
  // BEFORE shell's destructor tears down the webview/Shell — so no worker
  // thread's postEvent()/resolveInvoke() call can ever race a torn-down
  // Shell. If a run was genuinely still in flight when the window closed,
  // this blocks briefly until the do-executor reaches its next
  // cancel-check point (session.progressCancelRequested(), checked between
  // every top-level do-file item) — a bounded wait, not a hang, since we
  // also request cancellation first.
  app.session.progressRequestCancel();
  app.joinAll();
}

void runDemoPage() {
  piet::Shell shell(/*debug=*/true);
  shell.setTitle("PIET 2.0 — M0 walking skeleton (--demo)");
  shell.setSize(720, 640, /*resizable=*/true);
  shell.installBridge();

  shell.bindRoute("echo", piet::routes::echo);
  shell.bindRoute("cobra.version", piet::routes::cobraVersion);
  shell.bindRoute("progress.start", [&shell](const std::string&) -> piet::BridgeResult {
    std::thread([&shell]() {
      piet::runSyntheticProgress([&shell](const std::string& tickJson) { shell.postEvent(tickJson); });
    }).detach();
    return piet::BridgeResult::success(R"({"started":true})");
  });

  std::string html = readFileOrDie(repoRelative("/piet/native/demo/index.html"));
  std::string dataUri = "data:text/html;base64," + piet::base64Encode(html);
  shell.navigate(dataUri);

  std::printf("[piet_native] window open (M0 demo) — close it to exit.\n");
  shell.run();
}

}  // namespace app_entry_detail
