// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: startup and paths.
#include "selftest_support.h"
#include "selftest_cases.h"
#include "piet/user_paths.h"


void testVersionSingleSource() {
  std::printf("[selftest] --- version single-source (MI-138 WP-2) ---\n");

  const std::string compiledPiet = PIET_VERSION_STRING;
  const std::string compiledA = PIET_CONTRACT_A_VERSION;
  const std::string compiledB = PIET_CONTRACT_B_VERSION;

  // (a) the /version bridge route derives from the compiled macros.
  {
#ifdef _WIN32
    std::error_code vwec;
    std::string workdir = (std::filesystem::temp_directory_path() / "piet_native_selftest_version").u8string();
    std::filesystem::create_directories(workdir, vwec);
#else
    std::string workdir = "/tmp/piet_native_selftest_version";
    (void)std::system(("mkdir -p '" + workdir + "'").c_str());
#endif
    piet::SessionState session(workdir);
    auto r = piet::routes::version(session, "{}");
    check(r.ok, "version route returns ok=true");
    auto j = nlohmann::json::parse(r.json);
    check(j.value("piet_version", std::string()) == compiledPiet,
          "version route piet_version == compiled PIET_VERSION_STRING (" + compiledPiet + ")");
    check(j.value("contract_a", std::string()) == compiledA,
          "version route contract_a == compiled PIET_CONTRACT_A_VERSION (" + compiledA + ")");
    check(j.value("contract_b", std::string()) == compiledB,
          "version route contract_b == compiled PIET_CONTRACT_B_VERSION (" + compiledB + ")");
#ifdef _WIN32
    std::error_code vrec;
    std::filesystem::remove_all(workdir, vrec);
#else
    (void)std::system(("rm -rf '" + workdir + "'").c_str());
#endif
  }

  // (b) packaged Info.plist matches the compiled tuple. macOS .app only: the
  // Windows flat bundle (resources\ beside PIET.exe) has no Info.plist — its
  // version single-source is the compiled macro == /version route, already
  // asserted in (a) above. So skip this sub-check on Windows.
#ifdef _WIN32
  skip("Info.plist tuple assert", "Windows flat bundle has no Info.plist");
#else
  if (std::optional<std::string> resources = piet::bundleResourcesDir()) {
    // ".../Foo.app/Contents/Resources" -> ".../Foo.app/Contents/Info.plist"
    std::string contents =
        resources->substr(0, resources->size() - std::string("/Resources").size());
    std::string plistPath = contents + "/Info.plist";
    std::ifstream f(cecon::to_native_path(plistPath), std::ios::binary);
    if (!f) {
      check(false, "packaged bundle has a readable Info.plist at " + plistPath);
    } else {
      std::ostringstream ss;
      ss << f.rdbuf();
      std::string plist = ss.str();
      check(plistStringForKey(plist, "piet_version") == compiledPiet,
            "Info.plist PIETReleaseTuple piet_version == compiled (" + compiledPiet + ")");
      check(plistStringForKey(plist, "contract_version_A") == compiledA,
            "Info.plist PIETReleaseTuple contract_version_A == compiled (" + compiledA + ")");
      check(plistStringForKey(plist, "contract_version_B") == compiledB,
            "Info.plist PIETReleaseTuple contract_version_B == compiled (" + compiledB + ")");
      check(plistStringForKey(plist, "CFBundleShortVersionString") == compiledPiet,
            "Info.plist CFBundleShortVersionString == compiled (" + compiledPiet + ")");

      // (b-2) the 4th tuple key. piet_version/contract_A/contract_B above are
      // compiled-in macros, so they are asserted against the binary itself;
      // cobra_version has no macro — its truth lives in the bundled ENGINE, so
      // this leg asks that engine directly. REDTEAM-P5 §3j found the gap: the
      // packaging script reads the key out of the actual pinned binary
      // (package_native_app.sh:111) precisely so it can never be hand-typed,
      // but nothing ever read it BACK — a 1-char seeded cobra_version drift
      // (0.34.0 -> 0.34.9) passed `--selftest` with ALL CHECKS PASSED while a
      // seed in any of the other four keys was caught. Deliberately execs the
      // bundle's own Resources/cobra instead of resolveCobraBin(): $COBRA_BIN
      // (which the headless gates routinely set) would let a foreign engine
      // answer for the one this bundle actually ships.
      std::string plistCobra = plistStringForKey(plist, "cobra_version");
      std::string bundledCobra = *resources + "/cobra";
      std::error_code bcec;
      if (!std::filesystem::exists(bundledCobra, bcec)) {
        check(false, "packaged bundle ships Resources/cobra (expected at " + bundledCobra + ")");
      } else {
        auto [cobraRc, cobraOut] =
            runCommandCapture(commandPathQuote(bundledCobra) + " version --json 2>&1");
        std::string reported;
        if (cobraRc == 0) {
          try {
            reported = nlohmann::json::parse(cobraOut).value("cobra_version", std::string());
          } catch (const std::exception&) {
            reported.clear();  // fall into the named launch/parse failure below
          }
        }
        if (reported.empty()) {
          check(false, "bundled Resources/cobra answers `version --json` (exit " +
                           std::to_string(cobraRc) + ", output: " + cobraOut.substr(0, 160) + ")");
        } else {
          check(reported == plistCobra,
                "Info.plist PIETReleaseTuple cobra_version == bundled cobra's own `version --json` "
                "(plist " + plistCobra + " / bundled " + reported + ")");
        }
      }
    }
  } else {
    skip("Info.plist tuple assert", "not running from inside a packaged .app bundle");
  }
#endif

  // (c) the private coordination tree's releases/CURRENT matches the
  //     compiled tuple.
  //
  // BUG-209 (an internal bug ledger entry): this used to derive its root
  // from __FILE__ and append a FIXED ".." hop to a sibling directory, which
  // assumed the checkout sits exactly one level under the workspace. A
  // worktree pool one level deeper made that fixed hop silently resolve to a
  // sibling that doesn't exist — and skip() being pass-by-default meant the
  // run still printed ALL CHECKS PASSED while this leg never actually
  // compared anything. Fixed by searching upward for the coordination tree
  // (piet::findCoordRoot: a sibling directory carrying releases/CURRENT and
  // claims/) instead of counting hops — see piet/coord_root.h.
  //
  // Two distinct outcomes on top of that search, told apart deliberately:
  //   - the root genuinely isn't found anywhere above this binary's source
  //     tree (any standalone or public checkout) ->
  //     that is a real environmental limit, so this still SKIPs, honestly.
  //   - the root IS found but releases/CURRENT isn't readable inside it ->
  //     that is NOT an environmental limit, it is a file that exists but
  //     was being looked for in the wrong place -> FAIL, not skip. skip()
  //     on a path-assumption precondition mislabels a bug as an honest
  //     limit (the Info.plist skip above IS honest — there is no bundle in
  //     a dev build; this leg, before this fix, was not).
  {
    std::string root = std::string(selftestSourceFile());
    auto pos = root.rfind("/piet/native/src/selftest.cpp");
    root = (pos == std::string::npos) ? "." : root.substr(0, pos);
    piet::CoordRootResult coord = piet::findCoordRoot(root);
    if (!coord.found) {
      skip("releases/CURRENT tuple assert",
           "no private coordination tree (releases/CURRENT + claims/) above this checkout (" +
               coord.explain() + ")");
    } else {
      std::string currentPath = coord.path + "/releases/CURRENT";
      std::ifstream f(cecon::to_native_path(currentPath));
      if (!f) {
        check(false, "coordination tree found at " + coord.path +
                          " but releases/CURRENT is not readable at " + currentPath +
                          " (the coordination root resolved correctly — the file itself is "
                          "missing or unreadable, which is a real bug, not a path-assumption miss)");
      } else {
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        std::string curPiet = currentFileValue(text, "piet_version");
        std::string curA = currentFileValue(text, "contract_A");
        std::string curB = currentFileValue(text, "contract_B");
        int compiledV[4], currentV[4];
        if (!parsePietVersion(compiledPiet, compiledV) || !parsePietVersion(curPiet, currentV)) {
          check(false, "parse compiled (" + compiledPiet + ") and released-CURRENT (" + curPiet +
                           ") piet versions (numeric or B/A letter line)");
        } else {
          int cmp = cmpPietVersion(compiledV, currentV);
          if (cmp == 0) {
            check(true, "releases/CURRENT piet_version == compiled (" + compiledPiet + ")");
            check(curA == compiledA && curB == compiledB,
                  "releases/CURRENT contract_A/contract_B == compiled at matching piet_version (" +
                      curA + " / " + curB + ")");
          } else if (cmp > 0) {
            skip("releases/CURRENT tuple assert",
                 "in-branch pre-release: compiled " + compiledPiet +
                     " ahead of released CURRENT " + curPiet +
                     " — becomes a hard equality PASS once the release is stamped");
          } else {
            check(false, "compiled piet_version " + compiledPiet + " is BEHIND released CURRENT " +
                             curPiet + " (stale binary — rebuild before gating)");
          }
        }
      }
    }
  }
}


// --- 7. (WI-5, 2.0.3) legal.notices route ------------------------------------
//
// The About/Legal panel's data source must never silently vanish: the route
// serves the packaged Resources/THIRD_PARTY_NOTICES.txt (bundle runs) or a
// live concatenation of piet/packaging/licenses/ (dev runs) — this leg
// asserts, per artifact, that the served text is non-empty and carries both
// the StataCorp non-affiliation disclaimer (WI-6) and the MIT notices the
// bundled components require to travel with the binary.
void testLegalNoticesRoute() {
  std::printf("[selftest] --- legal.notices (third-party notices / WI-5) ---\n");

  auto r = piet::routes::legalNotices("{}");
  check(r.ok, "legal.notices route returns ok=true");
  if (!r.ok) {
    std::printf("[selftest]        legal.notices FAILED: %s\n", r.json.c_str());
    return;
  }
  auto j = nlohmann::json::parse(r.json);
  const std::string text = j.value("text", std::string());
  check(!text.empty(), "legal.notices text is non-empty");
  check(j.value("source", std::string()) == "bundle" || j.value("source", std::string()) == "dev",
        "legal.notices source is 'bundle' or 'dev'");
  check(text.find("StataCorp LLC") != std::string::npos,
        "notices carry the StataCorp non-affiliation disclaimer (WI-6)");
  check(text.find("MIT License") != std::string::npos,
        "notices carry the MIT license text (webview / nlohmann json)");
  check(text.find("Niels Lohmann") != std::string::npos,
        "notices carry the nlohmann/json copyright line");
}


// --- 8. (WS-N1, native-paths stabilization item 2) piet::resolveUserPath
// invariants ------------------------------------------------------------
//
// piet/native has no ctest harness — path-resolution invariants live here.
// resolveUserPath (fileio/user_paths.cpp) is the ONE shared implementation of
// path resolution; it replaced THREE independent forks (resolvePath,
// resolvePathG, resolveShimPath) plus several inline session.cwd-or-workdir()
// joins (save/append/merge, postclose in fileio/postfile.cpp); this leg is the
// one place that consolidation now needs covering.
void testResolveUserPath() {
  std::printf("[selftest] --- native-paths: piet::resolveUserPath invariants ---\n");

  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_resolveuserpath").u8string();
  std::error_code ec;
  fs::create_directories(workdir, ec);

  // quoted + relative + seeded cwd (both quote styles dequote identically).
  {
    piet::SessionState session(workdir);
    session.cwd = workdir;
    check(piet::resolveUserPath(session, "\"fatalities.csv\"") == workdir + "/fatalities.csv",
          "resolveUserPath: double-quoted relative path dequotes then joins against seeded cwd");
    check(piet::resolveUserPath(session, "'fatalities.csv'") == workdir + "/fatalities.csv",
          "resolveUserPath: single-quoted relative path dequotes the same way");
  }

  // `~` expansion.
  {
    piet::SessionState session(workdir);
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");  // resolveUserPath uses USERPROFILE on Windows
#else
    const char* home = std::getenv("HOME");
#endif
    if (home == nullptr || *home == '\0') {
      skip("resolveUserPath: ~ expansion", "home dir env var unset in this environment");
    } else {
      check(piet::resolveUserPath(session, "~/foo.dta") == std::string(home) + "/foo.dta",
            "resolveUserPath: `~/...` expands against $HOME");
    }
  }

  // absolute passthrough — unaffected by cwd.
  {
    piet::SessionState session(workdir);
    session.cwd = workdir;
    check(piet::resolveUserPath(session, "/etc/hosts") == "/etc/hosts",
          "resolveUserPath: absolute path passes through unchanged (cwd ignored)");
  }

  // root cwd — the EXACT reported bug shape (Finder launch -> getcwd()=="/"
  // -> `use "fatalities.csv"` used to resolve to "/fatalities.csv"). The
  // fix is item 1 (seed cwd away from "/"), but the resolver itself must
  // still produce exactly ONE leading slash if cwd genuinely is root, never
  // "//fatalities.csv" (the old pop_back-to-empty-string collapse).
  {
    piet::SessionState session(workdir);
    session.cwd = "/";
    check(piet::resolveUserPath(session, "fatalities.csv") == "/fatalities.csv",
          "resolveUserPath: cwd==\"/\" joins to a single-slash \"/fatalities.csv\", never "
          "\"//fatalities.csv\"");
  }

  // last-resort getcwd() — headless drivers (--run-block's fresh-session-
  // per-cell harness) never seed session.cwd at all.
  {
    piet::SessionState session(workdir);
    check(!session.cwd.has_value(), "resolveUserPath fixture: a fresh SessionState has no seeded cwd");
    std::string procCwd = fs::current_path(ec).u8string();
    check(!ec && piet::resolveUserPath(session, "x.csv") == procCwd + "/x.csv",
          "resolveUserPath: unseeded session.cwd falls back to the process cwd");
  }

  // `use`-shaped and `save`-shaped relative paths resolve against the SAME
  // base — a structural guarantee (one shared resolver) rather than a
  // numeric coincidence, since runUse and runSaveData both call THIS
  // function now instead of two independently-maintained joins.
  {
    piet::SessionState session(workdir);
    session.cwd = workdir;
    std::string usePath = piet::resolveUserPath(session, "fatalities.csv");
    std::string savePath = piet::resolveUserPath(session, "out.dta");
    auto dirOf = [](const std::string& p) { return p.substr(0, p.find_last_of('/')); };
    check(dirOf(usePath) == workdir && dirOf(savePath) == workdir,
          "resolveUserPath: `use`-shaped and `save`-shaped relative paths resolve against the "
          "SAME seeded cwd base (single shared resolver, not two independent joins)");
  }

  fs::remove_all(workdir, ec);
}


// --- 9. (WS-N1, native-paths stabilization item 1) cwd seeded at startup ---
//
// The actual bug: a Finder/dock-launched app's getcwd() is "/", and
// SessionState::cwd was never seeded anywhere, so the very first relative
// `use "fatalities.csv"` resolved to "/fatalities.csv" instead of anywhere
// the user's own files live. Asserts the REAL seeding function
// runRealFrontend() calls (piet::defaultUserCwd(), forward-declared above
// from main.cpp — not a paraphrase) returns something sane on THIS
// machine: non-empty, not bare root, and an existing directory.
void testCwdSeededAtStartup() {
  std::printf("[selftest] --- native-paths: cwd seeded at startup ---\n");

  std::string seeded = piet::defaultUserCwd();
  check(!seeded.empty(), "defaultUserCwd: returns a non-empty path");
  check(seeded != "/",
        "defaultUserCwd: never seeds the bare root \"/\" on this machine (the exact "
        "Finder-launch bug this consolidation fixes)");
  std::error_code ec;
  check(std::filesystem::is_directory(seeded, ec) && !ec,
        "defaultUserCwd: the seeded path (" + seeded + ") is an existing, real directory");
}


// --- 10. (WS-F1 scope addendum) file.read bridge route ---------------------
//
// `file.read` backs the frontend's new editor-first Run.do flow (WS-F1 is
// coding the client side against this exact shape in parallel). Since the
// route itself is bound as an inline lambda in main.cpp's bindAllRoutes()
// (piet::Shell needs a real window to invoke through), the resolve+
// validate+read logic lives in the free function piet::readDoFileForBridge()
// instead so it's reachable here headlessly. Covers: happy path (relative
// .do fixture resolved against a seeded cwd — same resolver as `use`/
// `save`) and the two named rejections (missing file, wrong extension).
void testFileReadRoute() {
  std::printf("[selftest] --- WS-F1 addendum: file.read bridge route ---\n");

  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_fileread").u8string();
  std::error_code ec;
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  session.cwd = workdir;

  // happy path: relative .do filename, resolved against the seeded cwd —
  // the SAME resolver `use`/`save` now share (item 2 of this wave).
  {
    std::string fixturePath = workdir + "/fixture.do";
    {
      std::ofstream f(cecon::to_native_path(fixturePath), std::ios::binary);
      f << "display \"hello from file.read\"\n";
    }
    piet::FileReadResult r = piet::readDoFileForBridge(session, "fixture.do");
    check(r.ok, "file.read: happy path (relative .do filename against seeded cwd) succeeds");
    check(r.resolvedPath == fixturePath,
          "file.read: resolved_path is the cwd-joined absolute path");
    check(r.source.find("hello from file.read") != std::string::npos,
          "file.read: source carries the fixture file's actual content");
  }

  // rejection: file does not exist.
  {
    piet::FileReadResult r = piet::readDoFileForBridge(session, "does_not_exist.do");
    check(!r.ok, "file.read: a missing file is rejected (ok=false)");
    check(!r.errorMessage.empty(), "file.read: missing-file rejection carries a message");
  }

  // rejection: wrong extension — a real, existing, small file, just not .do.
  {
    std::string txtPath = workdir + "/notes.txt";
    {
      std::ofstream f(cecon::to_native_path(txtPath), std::ios::binary);
      f << "not a do file\n";
    }
    piet::FileReadResult r = piet::readDoFileForBridge(session, "notes.txt");
    check(!r.ok, "file.read: a non-.do extension is rejected even though the file exists");
  }

  fs::remove_all(workdir, ec);
}


void testBS041VersionColonPrefix() {
  std::printf("[selftest] --- BS-041 `version #: command` remainder re-dispatch ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs041").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  auto blocks = piet::dispatch("version 14: count", session);
  bool sawDisplay = false;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "display") sawDisplay = true;
  }
  check(sawDisplay, "BS-041: `version 14: count` runs the nested command (today silently no-ops)");

  auto blocks2 = piet::dispatch("version 14", session);
  check(blocks2.size() == 1,
        "BS-041 regression guard: bare `version 14` (no colon) is still a silent no-op (echo only)");

  fs::remove_all(workdir, ec);
}


// BUG-1939: every dev-tree path (sysuse sample, frontend index.html,
// c(sysdir_sample), the notices route) was anchored on __FILE__ alone. A build
// with -ffile-prefix-map — the public build script — makes __FILE__ relative,
// so each anchor fell back to "../.." against the cwd and `sysuse fatalities`
// answered "file not found" in a public clone (and BUG-155's arm then crashed
// on the empty column). The shared resolver must find the tree from the
// executable's own location, with no __FILE__ involved.
void testBUG1939DevTreeRootResolution() {
  std::printf("[selftest] --- BUG-1939 dev-tree root resolves from the executable, not __FILE__ ---\n");
  if (piet::bundleResourcesDir().has_value()) {
    skip("BUG-1939 (all legs)", "running from inside a packaged bundle; the dev tree does not apply");
    return;
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  std::optional<std::string> exe = piet::executablePath();
  check(exe.has_value(), "BUG-1939: executablePath() resolves for a dev-tree binary");
  std::optional<std::string> fromExe;
  if (exe.has_value()) {
    auto sep = exe->find_last_of("/\\");
    if (sep != std::string::npos) fromExe = piet::devTreeRootFrom(exe->substr(0, sep));
  }
  check(fromExe.has_value(),
        "BUG-1939: the checkout root is found by walking up from the executable's directory "
        "(no __FILE__ in that path) -- the arm that a -ffile-prefix-map build failed");
  check(fromExe.has_value() &&
            fs::is_regular_file(cecon::to_native_path(*fromExe + "/piet/packaging/sample/fatalities.csv"), ec),
        "BUG-1939: ...and that root holds piet/packaging/sample/fatalities.csv");
  std::optional<std::string> root = piet::devTreeRoot();
  check(root.has_value() &&
            fs::is_regular_file(cecon::to_native_path(*root + "/piet/frontend/index.html"), ec),
        "BUG-1939: devTreeRoot() (the production resolver) reaches piet/frontend/index.html");
  check(piet::devTreePath("/piet/packaging/sample") == (root.has_value() ? *root : std::string("../..")) + "/piet/packaging/sample",
        "BUG-1939: devTreePath() is root + suffix (the \"../..\" guess only when nothing resolves)");
  check(!piet::devTreeRootFrom("/").has_value(),
        "BUG-1939: a start point with no checkout above it resolves to nothing, not to a guess");
}
