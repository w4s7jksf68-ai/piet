// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: file io.
#include "selftest_support.h"
#include "selftest_cases.h"


// `tempfile p` → `save "\`p'"` → `use "\`p'"` was broken by the 07-09
// "honest .dta save" fix (@ad23151), which over-blocked PIET's OWN `.tmp`
// suffix (runTempfile mints `<tempdir>/NAME.tmp`). This leg drives the full
// idiom through the command layer (which resolves the `\`p'` local) and
// asserts the round-trip restores the data — the save-side fix accepts
// `.tmp` as a native-hbin target; `use` closes it by content-sniffing.
void testTempfileSaveUseRoundtripBug085() {
  std::printf("[selftest] --- BUG-085 tempfile->save->use round-trip ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug085").u8string();
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

  piet::SessionState session(workdir);
  (void)runCmd(session, "clear");
  (void)runCmd(session, "set obs 5");
  (void)runCmd(session, "gen x = _n");
  (void)runCmd(session, "gen y = _n * 2");
  check(session.hasData() && session.datastore()->has("x") && session.datastore()->has("y"),
        "BUG-085 setup: 5-row 2-var dataset built in memory");

  (void)runCmd(session, "tempfile p");
  auto jSave = runCmd(session, "save \"`p'\", replace");
  check(firstBlockOfType(jSave, "error").is_null(),
        "BUG-085: `save \"`p'\"` (a `.tmp` native target) succeeds, not an engine_error");

  // Drop the in-memory copy so the `use` genuinely re-reads from disk.
  (void)runCmd(session, "clear");
  check(!session.hasData(), "BUG-085: `clear` emptied memory before the round-trip `use`");

  auto jUse = runCmd(session, "use \"`p'\", clear");
  check(firstBlockOfType(jUse, "error").is_null(),
        "BUG-085: `use \"`p'\"` re-opens the `.tmp` file (content-sniffed as hbin)");
  check(session.hasData() && session.datastore()->has("x") && session.datastore()->has("y"),
        "BUG-085: the round-trip restored the dataset (both vars back in memory)");

  // BUG-085 UX residual: `use`-ing PIET's OWN `.tmp` tempfile must emit NO
  // hbin-sniff rename NOTE — `.tmp` is the engine's minted suffix, not a user
  // misnaming (dispatcher lifts the note into a prepended `echo` block whose
  // distinctive text is "Rename it to end in `.hbin`"). Assert no such block.
  bool sawRenameNote = false;
  for (const auto& blk : jUse.value("blocks", nlohmann::json::array())) {
    if (blk.value("type", std::string()) == "echo" &&
        blk.value("text", std::string()).find("Rename it to end in") != std::string::npos) {
      sawRenameNote = true;
    }
  }
  check(!sawRenameNote,
        "BUG-085 UX: `use \"`p'\"` on the engine's own `.tmp` emits NO rename NOTE");

  // Regression guard: a foreign extension is STILL rejected — the `.tmp`
  // carve-out of @ad23151 must not have relaxed the extension rule generally.
  // P1-A3 moved the EXEMPLAR, not the rule: `.dta` stopped being foreign the
  // day cobra's `export-dta` writer landed (PIET now writes real Stata bytes
  // for it), so the guard asks a format this build still genuinely cannot
  // write. `.csv` is that format — grepped, there is no CSV-export verb in
  // this tree, exactly as the `runSaveData` BUG A note records.
  (void)runCmd(session, "set obs 3");
  (void)runCmd(session, "gen z = _n");
  auto jForeign = runCmd(session, "save \"myfile.csv\", replace");
  check(!firstBlockOfType(jForeign, "error").is_null(),
        "BUG-085 over-fix guard: a foreign extension (`.csv`) is STILL rejected");

  fs::remove_all(workdir, ec);
}


// BUG-341: `display var[1]` after a resident mutation answered `matrix var not
// found` — a variable read falling into the matrix namespace. Pre-existing since
// subscript support arrived (before it, B.6.2 echoed `ynum[1]` literally), and the
// acceptance corpus never uses the form, which is why eight waves missed it.
//
// Every expectation below is a StataBE measurement taken 2026-08-14, not a guess:
// in-range → the row's value, out-of-range and index 0 → missing, string variable
// → the string. The undefined-matrix leg guards the arm ABOVE it staying reachable.
// BUG-343: the `file` family (text mode). The registry had 162 rows and none of
// them was `file`, so a do-file that wrote its own log stopped dead. These legs
// drive the whole round trip through the user-visible path, because the three
// things that can silently go wrong here are all end-to-end: the bytes actually
// on disk, `r(eof)` actually reaching a read loop, and the refusals actually
// naming what they refuse.
void testBug343FileFamily() {
  std::printf("[selftest] --- BUG-343 `file open|write|read|close` (text mode) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug343").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  const std::string out = (fs::path(workdir) / "out.txt").u8string();

  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  auto errorText = [](const auto& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error")
        return b.value("message", std::string());
    return std::string();
  };

  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    std::string src = "file open fh using \"" + out + "\", write replace\n"
                      "file write fh \"hello\" _n \"a,b\" _n\n"
                      "file close fh\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(errorText(r.blocks).empty(), "BUG-343: open/write/close raises no error");
    std::ifstream f(out, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    check(got == "hello\na,b\n",
          "BUG-343: the bytes on disk are exactly what was written (`_n` is a newline, "
          "quoted items keep their spaces) — got " + std::to_string(got.size()) + "B");
  }
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    std::string src = "file open rh using \"" + out + "\", read\n"
                      "file read rh l1\ndisplay \"`l1'\"\n"
                      "file read rh l2\ndisplay \"`l2'\"\n"
                      "file read rh l3\ndisplay r(eof)\n"
                      "file close rh\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    auto d = displays(r.blocks);
    check(d.size() == 3 && d[0] == "hello" && d[1] == "a,b",
          "BUG-343: `file read` returns one LINE per call into the named local");
    check(d.size() == 3 && d[2] == "1",
          "BUG-343: r(eof) is 1 once the reads run past the end — the value a read loop tests");
  }
  {  // BUG-1526: `binary` mode reads the file's bytes (BUG-343 refused it by name).
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    auto r = piet::doexec::executeDoSource("file open bh using \"" + out + "\", binary\n"
                                           "file read bh %5s w %1bu nl\n"
                                           "display \"`w'|\" nl\n"
                                           "file close bh\n",
                                           session, piet::dispatch);
    auto d = displays(r.blocks);
    check(errorText(r.blocks).empty() && d.size() == 1 && d[0] == "hello|10",
          "BUG-1526: a binary read gives the file's bytes (%5s `hello`, %1bu 10 for the newline)");
  }
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    auto r = piet::doexec::executeDoSource("file close nosuch\n", session, piet::dispatch);
    // EM2: Stata's wording and code, "file handle NAME not found" r(111)
    // (w2af_macro_text_file.log:2130-2136 mt_fwrite_closed).
    check(errorText(r.blocks).find("not found") != std::string::npos,
          "BUG-343: closing a handle that was never opened is an honest error");
  }
  {  // No-clobber: a plain `write` over an existing file must refuse without `replace`,
     // in Stata's words: `file X already exists` r(602), the file as typed
     // (StataBE 19.5; this arm used to pin PIET's own "(specify `replace`)").
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    auto r = piet::doexec::executeDoSource(
        "file open ch using \"" + out + "\", write\n", session, piet::dispatch);
    std::string reason;
    for (const auto& b : r.blocks)
      if (b.is_object() && b.value("type", std::string()) == "error")
        reason = b.value("reason", std::string());
    check(errorText(r.blocks) == "file " + out + " already exists" && reason == "r(602)",
          "BUG-343: opening an existing file for write without `replace` refuses and says so "
          "(got `" + errorText(r.blocks) + "` " + reason + ")");
  }
}


// AER-SCOPE1 bundle D (ENV): the nineteen preamble verbs.
//
// Sibling of testBug343FileFamily above, and for the same reason: these verbs
// act on the FILESYSTEM and on SESSION STATE, so neither the eval goldens nor
// the per-command floor gate can see whether they did anything. The floors.json
// rows say so explicitly (state no_fixture, "a budget would be measuring
// nothing"). That makes THIS function the only place the work itself is
// checked, and the checks below are written against the effect rather than
// against the message wherever an effect exists: `shell rm` is verified by
// asking the filesystem whether the file is gone, not by reading the note.
//
// The note texts ARE asserted verbatim, in one table, because for the accepted
// verbs the note is the entire observable behaviour -- a `net install` that
// went quiet and returned 0 is indistinguishable from one that installed. Each
// message prints what came back beside what was expected, so a display-layer
// drift reads as a drift at a glance instead of as a mystery.
void testAerEnvBundle() {
  std::printf("[selftest] --- AER-SCOPE1 bundle D (ENV): shell / net / ado / adopath / sysdir / "
              "findfile / error / frames / tset / sleep ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerenv").u8string();
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
  auto errField = [](const auto& blocks, const char* field) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error")
        return b.value(field, std::string());
    return std::string();
  };
  auto writeFile = [](const std::string& p, const std::string& body) {
    std::ofstream f(p, std::ios::binary);
    f << body;
  };
  auto readFile = [](const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  // Absolute paths throughout: the shell arg parser's quote handling is not
  // what is under test here, and the temp dir has no spaces to need it.
  auto at = [&](const std::string& rel) { return (fs::path(workdir) / rel).u8string(); };

  // --- shell: the translation is judged by the DISK, not by its own note. ---
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    const std::string victim = at("victim.txt");
    writeFile(victim, "gone soon\n");
    auto r = piet::doexec::executeDoSource("shell rm -f " + victim + "\n", session, piet::dispatch);
    check(!fs::exists(victim), "ENV/shell: `shell rm` actually unlinks the file -- the note is the "
                               "report, not the work");
    bool saidSo = false;
    for (const auto& line : displays(r.blocks))
      if (line.find("translated ->") != std::string::npos) saidSo = true;
    check(saidSo, "ENV/shell: the translation is VISIBLE (`translated -> <verb> <paths>`); a silent "
                  "accept would leave the user believing a real shell ran");
  }
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    const std::string deep = at("a/b/c");
    (void)piet::doexec::executeDoSource("shell mkdir -p " + deep + "\n", session, piet::dispatch);
    check(fs::is_directory(deep),
          "ENV/shell: `mkdir -p` creates the WHOLE chain, not just the leaf's parent");
  }
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    const std::string payload = "one,two\nthree\n";
    const std::string a = at("cp_src.txt"), b = at("cp_mid.txt"), c = at("cp_dst.txt");
    writeFile(a, payload);
    (void)piet::doexec::executeDoSource(
        "shell cp " + a + " " + b + "\nshell mv " + b + " " + c + "\n", session, piet::dispatch);
    check(fs::exists(a) && readFile(a) == payload,
          "ENV/shell: `cp` COPIED -- the source is still there with its bytes");
    check(!fs::exists(b), "ENV/shell: `mv` moved rather than copied -- the intermediate is gone");
    check(fs::exists(c) && readFile(c) == payload,
          "ENV/shell: cp-then-mv leaves the bytes unchanged at the destination");
  }
  {  // A glob is the shape the corpus actually writes (`shell rm -f tmp/*.txt`).
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    const std::string g1 = at("g_1.txt"), g2 = at("g_2.txt"), keep = at("g_keep.dat");
    writeFile(g1, "1");
    writeFile(g2, "2");
    writeFile(keep, "k");
    (void)piet::doexec::executeDoSource("shell rm -f " + at("g_*.txt") + "\n", session,
                                        piet::dispatch);
    check(!fs::exists(g1) && !fs::exists(g2),
          "ENV/shell: a glob removes BOTH matches (a first-match-only expansion is the silent "
          "half-done class)");
    check(fs::exists(keep), "ENV/shell: and nothing that did not match the glob");
  }
  // The two refusals, each naming its own cause AND landing on r(199).
  //
  // The rc leg is not decoration. eval::coarseRc (errors.cpp) answers 199 for
  // exactly two shapes: code == "unsupported_command", or a `reason` spelled
  // "r(NNN)". A refusal block built as a bare engine_error with reason
  // "refused:<cause>" matches NEITHER, so it falls through to the generic 1 --
  // and a do-file branching on `if _rc == 199` then takes the wrong branch off
  // an error that refused perfectly well. The code check and the _rc check are
  // therefore both here: the first says which shape was built, the second says
  // what the user actually sees.
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    auto r = piet::doexec::executeDoSource("shell chmod 777 " + workdir + "\n", session,
                                           piet::dispatch);
    check(errField(r.blocks, "code") == "unsupported_command",
          "ENV/shell: an off-allowlist verb is an honest unsupported_command, got code `" +
              errField(r.blocks, "code") + "`");
    check(errField(r.blocks, "reason") == "refused:shell_allowlist",
          "ENV/shell: ...and names the allowlist as the cause, got reason `" +
              errField(r.blocks, "reason") + "`");
  }
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    auto r = piet::doexec::executeDoSource("shell rm -f " + at("nothing.txt") + " | cat\n", session,
                                           piet::dispatch);
    check(errField(r.blocks, "reason") == "refused:shell_operator",
          "ENV/shell: a shell metacharacter refuses as an OPERATOR, distinct from the allowlist "
          "refusal -- PIET has no shell to interpret it, got reason `" +
              errField(r.blocks, "reason") + "`");
  }
  {
    const std::vector<std::pair<std::string, std::string>> kRefusals = {
        {"shell chmod 777 " + workdir, "off-allowlist shell verb"},
        {"shell rm -f " + at("nothing.txt") + " | cat", "shell metacharacter"},
        {"winexec notepad.exe", "winexec"},
        // twoway__histogram_gen left this list when gfx-histogram-kdensity-family
        // served it (w2aq_histogram_family hk_thg_*); selftest_graphics_histogram_kdensity pins it.
    };
    for (const auto& [src, what] : kRefusals) {
      piet::SessionState session(workdir);
      session.eval.cwd = workdir;
      (void)piet::doexec::executeDoSource("capture " + src + "\n", session, piet::dispatch);
      check(session.eval.locals.get("_rc", "") == "199",
            "ENV/refusal: " + what + " leaves _rc 199, the code Stata gives an unrecognized "
            "capability -- got _rc=" + session.eval.locals.get("_rc", "<unset>"));
    }
  }
  {  // `!cmd` must reach the same code, not a second and looser one.
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    const std::string bang = at("bang.txt");
    writeFile(bang, "b");
    (void)piet::doexec::executeDoSource("!rm -f " + bang + "\n", session, piet::dispatch);
    check(!fs::exists(bang),
          "ENV/shell: `!cmd` is rewritten to `shell cmd` before tokenizing -- same file verb, same "
          "effect");
    auto r = piet::doexec::executeDoSource("!chmod 777 " + workdir + "\n", session, piet::dispatch);
    check(errField(r.blocks, "reason") == "refused:shell_allowlist",
          "ENV/shell: `!` inherits the SAME allowlist -- a bang that refused less than `shell` "
          "would be a hole with a shorter spelling, got reason `" +
              errField(r.blocks, "reason") + "`");
  }

  // --- error: the argument IS the return code, and 0 is not an error. ---
  {
    piet::SessionState session(workdir);
    (void)piet::doexec::executeDoSource("capture error 199\n", session, piet::dispatch);
    check(session.eval.locals.get("_rc", "") == "199",
          "ENV/error: `capture error 199` leaves _rc 199 (not the generic captured 1), got _rc=" +
              session.eval.locals.get("_rc", "<unset>"));
  }
  {
    piet::SessionState session(workdir);
    auto r = piet::doexec::executeDoSource("error 0\n", session, piet::dispatch);
    // Every dispatched line emits its own echo block, so "no block at all" is
    // not the observable -- "no ERROR block" is. Asserting emptiness measured
    // the echo, not `error 0`.
    size_t errBlocks = 0;
    for (const auto& b : r.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") ++errBlocks;
    }
    check(errBlocks == 0, "ENV/error: `error 0` raises NO error -- r(0) is Stata's success code, "
                          "so raising it would invent a failure, got " +
                              std::to_string(errBlocks) + " error block(s)");
    check(!r.stoppedAtReason.has_value(),
          "ENV/error: `error 0` does not stop the do-file, stop=" +
              r.stoppedAtReason.value_or("<none>"));
  }

  // --- the accept-notes, verbatim. For these verbs the note IS the behaviour.
  {
    // adopath and sysdir RECORD the resolved path but ECHO the argument as it
    // was written (environment/ado_paths_and_packages.cpp says why: a resolved echo would bake
    // the capture machine's cwd into every golden that pins the note). These
    // two probes pass an ABSOLUTE path anyway, so as-written and as-resolved
    // are the same string and the expectation stays exact whichever way a
    // later edit takes the handler -- this test is not the place that decides
    // it. The read-back leg below is what actually pins the recorded form.
    const std::string apDir = at("99_ado");
    const std::string sdDir = at("plus_ado");
    const std::vector<std::pair<std::string, std::string>> kNotes = {
        {"net install reghdfe", "(net install reghdfe: PIET is offline -- skipped)"},
        // The corpus's real spelling carries options. The note is built off the
        // varlist, so the option tail must not reach it -- a `(net install
        // reghdfe,: ...)` would be invisible to the bare probe above.
        {"net install reghdfe, from(\"https://example.invalid/src/\") replace",
         "(net install reghdfe: PIET is offline -- skipped)"},
        {"ado uninstall moremata", "(ado uninstall moremata: PIET has no ado directory -- skipped)"},
        {"adopath + " + apDir,
         "(adopath + " + apDir + ": recorded for findfile and ado autoload)"},
        {"sysdir set PLUS " + sdDir,
         "(sysdir set PLUS " + sdDir + ": recorded for findfile)"},
        {"pause on", "(pause on: PIET is non-interactive -- no effect)"},
        {"gr_edit .plotregion1.style.editstyle margin(medium) editcopy",
         "(gr_edit: graph-editor recording, appearance only -- ignored)"},
    };
    for (const auto& [src, note] : kNotes) {
      piet::SessionState session(workdir);
      session.eval.cwd = workdir;
      auto r = piet::doexec::executeDoSource(src + "\n", session, piet::dispatch);
      auto d = displays(r.blocks);
      bool exact = false;
      for (const auto& line : d)
        if (line == note) exact = true;
      check(exact, "ENV/note: `" + src + "` prints its accept-note verbatim -- expected `" + note +
                       "`, got " + (d.empty() ? std::string("no display at all") :
                                                std::string("`") + d.front() + "`"));
    }
  }

  // --- adopath -> findfile: the recorded path is what findfile is FOR. ---
  const std::string adodir = at("ado");
  fs::create_directories(adodir, ec);
  const std::string adofile = (fs::path(adodir) / "piet_env_probe.ado").u8string();
  writeFile(adofile, "* probe\n");
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    (void)piet::doexec::executeDoSource(
        "adopath + " + adodir + "\nfindfile piet_env_probe.ado\nlocal fn `r(fn)'\n", session,
        piet::dispatch);
    check(session.eval.locals.get("fn", "") == adofile,
          "ENV/findfile: searches the RECORDED adopath and publishes r(fn) as the absolute path, "
          "got `" + session.eval.locals.get("fn", "<unset>") + "`");
  }
  {
    // [P] findfile (p.pdf pp.246-247): each path directory is searched, then its
    // letter subdirectory, unless nodescend -- where an installed ado lives
    // (w2t_boottest_family.log:131 A6 `.../ado/plus/b/boottest.ado`). bin/main
    // tested `dir/name` only: r(601).
    const std::string letterDir = (fs::path(adodir) / "p").u8string();
    fs::create_directories(letterDir, ec);
    const std::string letterFile = (fs::path(letterDir) / "piet_env_letter.ado").u8string();
    writeFile(letterFile, "* probe\n");
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    (void)piet::doexec::executeDoSource(
        "findfile piet_env_letter.ado, path(\"" + adodir + "\")\nlocal fn `r(fn)'\n"
        "capture findfile piet_env_letter.ado, path(\"" + adodir + "\") nodescend\n"
        "local rc2 = _rc\n",
        session, piet::dispatch);
    check(session.eval.locals.get("fn", "") == letterFile &&
              session.eval.locals.get("rc2", "") == "601",
          "ENV/findfile: descends into the letter subdirectory (r(fn) `" + letterFile +
              "`), and nodescend does not (r(601)); got `" +
              session.eval.locals.get("fn", "<unset>") + "` rc2 " +
              session.eval.locals.get("rc2", "<unset>"));
  }
  {
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    auto r = piet::doexec::executeDoSource("findfile piet_env_no_such.ado\n", session,
                                           piet::dispatch);
    check(errField(r.blocks, "reason") == "r(601)",
          "ENV/findfile: a miss is r(601) -- an empty r(fn) with rc 0 would be the silent-wrong "
          "answer, got reason `" + errField(r.blocks, "reason") + "`");
  }
  {  // A `set` nobody can read back is a write-only setting.
    //
    // The set and the read-back are TWO executeDoSource calls on one session,
    // not one two-line source: the `sysdir set` accept-note itself contains
    // both "PLUS" and the directory, so a single-call scan is satisfied by the
    // note alone and stays GREEN even if bare `sysdir` prints nothing at all.
    // Only the second call's blocks are scanned, so the listing has to answer.
    piet::SessionState session(workdir);
    session.eval.cwd = workdir;
    (void)piet::doexec::executeDoSource("sysdir set PLUS " + adodir + "\n", session,
                                        piet::dispatch);
    auto r = piet::doexec::executeDoSource("sysdir\n", session, piet::dispatch);
    bool reflected = false;
    for (const auto& line : displays(r.blocks))
      if (line.find("PLUS") != std::string::npos && line.find(adodir) != std::string::npos)
        reflected = true;
    check(reflected, "ENV/sysdir: bare `sysdir` reports the directory `sysdir set` recorded");
  }

  // --- frames reset: the roster, not just the current frame. ---
  {
    piet::SessionState session(workdir);
    (void)piet::doexec::executeDoSource("sysuse fatalities, clear\n"
                                        "frame put state beertax, into(envf1)\n"
                                        "frame put state year, into(envf2)\n",
                                        session, piet::dispatch);
    check(session.frames.size() >= 2,
          "ENV/frames: two frames exist before the reset (preamble sanity), have " +
              std::to_string(session.frames.size()));
    (void)piet::doexec::executeDoSource("frames reset\n", session, piet::dispatch);
    // Counted rather than `frames.empty()`: runFramesReset skips the name
    // "default" by design (that frame IS the session), so an empty-map
    // assertion would be testing whether the roster happens to carry a
    // "default" row instead of testing the drop.
    size_t survivors = 0;
    std::string surviving;
    for (const auto& [name, path] : session.frames) {
      (void)path;
      if (name == "default") continue;
      ++survivors;
      surviving += (surviving.empty() ? "" : " ") + name;
    }
    check(survivors == 0, "ENV/frames: `frames reset` drops EVERY non-default frame, left " +
                              std::to_string(survivors) + " (" + surviving + ")");
  }

  // --- sleep: milliseconds. Both bounds matter -- a seconds reading would
  // still pass a >= 5ms floor while turning `sleep 200` into three minutes.
  {
    piet::SessionState session(workdir);
    auto t0 = std::chrono::steady_clock::now();
    (void)piet::dispatch("sleep 5", session);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    check(ms >= 5, "ENV/sleep: `sleep 5` actually waits -- waited " + std::to_string(ms) + "ms");
    check(ms < 2000, "ENV/sleep: ...in MILLISECONDS, not seconds -- waited " +
                         std::to_string(ms) + "ms");
  }

  // --- tset: the pre-Stata-7 spelling, both arities. ---
  {
    piet::SessionState session(workdir);
    (void)piet::doexec::executeDoSource(
        "sysuse fatalities, clear\negen long envpid = group(state)\ntset envpid year\n", session,
        piet::dispatch);
    check(session.panelvar.value_or("<unset>") == "envpid" &&
              session.timevar.value_or("<unset>") == "year",
          "ENV/tset: `tset panelvar timevar` is xtset -- it seeds BOTH, got panelvar=" +
              session.panelvar.value_or("<unset>") + " timevar=" +
              session.timevar.value_or("<unset>"));
  }
  {
    piet::SessionState session(workdir);
    (void)piet::doexec::executeDoSource(
        "sysuse fatalities, clear\nkeep if state == \"al\"\ntset year\n", session, piet::dispatch);
    check(!session.panelvar.has_value() && session.timevar.value_or("<unset>") == "year",
          "ENV/tset: `tset timevar` is tsset -- time only, no panel invented, got panelvar=" +
              session.panelvar.value_or("<unset>") + " timevar=" +
              session.timevar.value_or("<unset>"));
  }

  fs::remove_all(workdir, ec);
}


// AER-SCOPE1 bundle G (IO-TEXT). The older text I/O verbs plus `putexcel`.
//
// WHAT MAKES THIS BUNDLE DIFFERENT FROM EVERY OTHER SELFTEST IN THIS FILE:
// nine of the thirteen rows exist to put BYTES ON DISK. `rc == 0` is therefore
// the weakest possible assertion here -- a handler that parsed the line, opened
// no file and returned a clean mutation result passes an rc check on every one
// of them. Every leg below reads the artifact back: the separator byte out of
// `outsheet`, the absence of a header line out of `outfile`, the exact echo
// sentence out of `saveold`, and -- leg (vii) -- the CELL VALUES out of the
// workbook `putexcel` wrote, through `xlsx::read`, which is the only check in
// the tree that can tell "wrote a workbook" from "wrote a workbook Excel and
// our own reader agree about".
//
// The other standing trap this file has paid for twice (bundle E, then F):
// `resolveUserPath` (fileio/user_paths.cpp) reads `SessionState::cwd`, NOT
// `SessionState::eval.cwd`. A session seeded with only the latter writes every
// `using` file into the PROCESS cwd -- the repo root -- and each leg then reads
// a file it never wrote. `seed()` below sets both, and every session in this
// function goes through it.
void testAerIoText() {
  std::printf("[selftest] --- AER-SCOPE1 bundle G: insheet/outsheet/infile/infix/outfile + "
              "saveold/putexcel/filefilter/copy + the four refusals ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aeriotext").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto exec = [](const std::string& src, piet::SessionState& session) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  // The one place `cwd` is seeded. See the header: eval.cwd alone is the
  // sixteen-failures-for-one-reason bug this bundle cannot afford to repeat,
  // because unlike E and F almost every leg here names a file.
  auto seed = [&](piet::SessionState& s) {
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (fileio/user_paths.cpp).
  };
  // NULL rather than a throw when there is no error block -- the BUG-129 trap:
  // check()'s message argument is built EAGERLY, on the passing path too, so a
  // `value()` on nothing would abort the whole run instead of failing one leg.
  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json();
  };
  auto errText = [](const nlohmann::ordered_json& e) {
    return e.is_object() ? e.value("message", std::string()) : std::string("<none>");
  };
  auto errField = [](const nlohmann::ordered_json& e, const std::string& f) {
    return e.is_object() ? e.value(f, std::string()) : std::string("<none>");
  };
  auto rcOf = [](const nlohmann::ordered_json& e) {
    return e.is_object() ? piet::eval::coarseRc(e) : std::string("<none>");
  };
  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  // %.17g, never std::to_string: six decimals renders two doubles that differ
  // in the 8th digit as the same text, and a failure report that cannot be
  // read is worse than no report (testAerTables' own note).
  auto fullPrec = [](double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };
  auto slurp = [](const std::string& p) {
    std::ifstream f(cecon::to_native_path(p), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  auto spit = [](const std::string& p, const std::string& bytes) {
    std::ofstream f(cecon::to_native_path(p), std::ios::binary | std::ios::trunc);
    f << bytes;
  };
  auto at = [&](const std::string& rel) { return (fs::path(workdir) / rel).u8string(); };
  auto varNames = [](piet::SessionState& s) {
    std::vector<std::string> out;
    const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
        for (const auto& v : b["variables"]) out.push_back(v.value("name", std::string()));
    return out;
  };
  auto hasName = [](const std::vector<std::string>& v, const std::string& n) {
    return std::find(v.begin(), v.end(), n) != v.end();
  };
  auto joinNames = [](const std::vector<std::string>& v) {
    std::string s;
    for (const auto& n : v) { if (!s.empty()) s += " "; s += n; }
    return s.empty() ? std::string("<none>") : s;
  };
  // describe's per-variable `dtype` IS varMeta.stataType when one is recorded
  // and the "str#"/"float" default otherwise -- which is exactly what the
  // declared-type halves of legs (iii) and (iv) need.
  auto dtypeOf = [](piet::SessionState& s, const std::string& name) -> std::string {
    const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
        for (const auto& v : b["variables"])
          if (v.value("name", std::string()) == name) return v.value("dtype", std::string());
    return std::string("<absent>");
  };
  // r() is REPLACED wholesale by the next r-class command (macros.cpp's
  // captureRFromBlock), so every read happens immediately after the command
  // that published it -- never two commands later.
  auto rOf = [](piet::SessionState& s, const char* key) -> double {
    const double* p = s.eval.rValues.find(key);
    return p ? *p : std::nan("");
  };
  auto runR = [&](piet::SessionState& s, const std::string& cmd, const char* key) -> double {
    (void)piet::dispatch(cmd, s);
    return rOf(s, key);
  };
  auto nRows = [&](piet::SessionState& s) { return runR(s, "count", "N"); };
  auto countIf = [&](piet::SessionState& s, const std::string& expr) {
    return runR(s, "count if " + expr, "N");
  };
  // Bounds-safe cell read: xlsx::read pads rows rectangular, but a defective
  // writer can produce a SHORT sheet, and an out-of-range [] there would abort
  // the run instead of failing the one check that noticed.
  auto cellAt = [](const piet::xlsx::Sheet& sh, size_t r0, size_t c0) -> piet::xlsx::Cell {
    if (r0 >= sh.rows.size() || c0 >= sh.rows[r0].size()) return piet::xlsx::Cell();
    return sh.rows[r0][c0];
  };
  auto readBook = [](const std::string& p, piet::xlsx::Sheet& out) -> bool {
    try { out = piet::xlsx::read(p, /*firstRowIsHeader=*/false); }
    catch (const std::exception&) { return false; }
    return true;
  };

  // ── (i) insheet applies STATA'S naming rules, which is the whole reason it
  // is not a plain alias for `import delimited`. ─────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(at("is_case.csv"), "ID,Val\n1,2\n3,4\n");
    auto r = exec("insheet using \"is_case.csv\", clear\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (i): `insheet using \"is_case.csv\", clear` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "id") && hasName(vn, "val"),
          "AER-IOTEXT (i): the header `ID,Val` lands as LOWERCASE {id,val}. This is the leg that "
          "separates a real insheet from a one-line alias to `import delimited`: the loader keeps "
          "the header's case, Stata's insheet lowercases it, and every downstream `summarize val` "
          "in a ported do-file then fails with `val not found` while the load itself reported rc "
          "0. Got {" + joinNames(vn) + "}");
    check(nRows(s) == 2.0,
          "AER-IOTEXT (i): ...over 2 data rows, so row 1 was consumed as the HEADER and not as "
          "data. A shim that passed `nonames` through (or failed to pass `names`) reads 3 rows "
          "with the header sitting in them as text. Got " + fullPrec(nRows(s)));
    check(countIf(s, "val == 4") == 1.0,
          "AER-IOTEXT (i): ...and the VALUES arrived: exactly one row has val == 4. Names and a "
          "row count are both true of a load that renamed the columns of the wrong file. Got " +
              fullPrec(countIf(s, "val == 4")));
  }
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("insheet using \"is_case.csv\", clear case\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (i/case): `insheet ..., case` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "ID") && hasName(vn, "Val"),
          "AER-IOTEXT (i/case): `case` PRESERVES the header's capitalisation -- {ID,Val}. Without "
          "this check the option can be accepted and dropped, which is rc 0 with the names the "
          "author explicitly asked to keep silently folded away. Got {" + joinNames(vn) + "}");
  }
  {
    // Stata's own auto-detection: if every field of the first row parses as a
    // number, that row is DATA and the variables are v1..vk.
    piet::SessionState s(workdir);
    seed(s);
    spit(at("is_noh.csv"), "1,2\n3,4\n");
    auto r = exec("insheet using \"is_noh.csv\", clear\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (i/auto-nonames): a headerless numeric file loads clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "v1") && hasName(vn, "v2"),
          "AER-IOTEXT (i/auto-nonames): a first row whose every field is a NUMBER is data, not a "
          "header, so the variables are {v1,v2}. Got {" + joinNames(vn) + "}");
    check(nRows(s) == 2.0,
          "AER-IOTEXT (i/auto-nonames): ...and BOTH rows survive. This is the half that matters: "
          "without the auto-detection the first row is eaten as a header, the frame silently "
          "loses an observation, and every mean computed from it is wrong by 1/N with no error "
          "anywhere. Got " + fullPrec(nRows(s)));
  }
  {
    // Plan edge case: a RAGGED file. This leg used to assert an error, which
    // pinned the defect: StataBE's insheet WIDENS (BUG-414 -- `a,b,c` over
    // `4,5` and `6,7,8,9,10` reads a b c v4 v5, 3 obs, rc 0). The unacceptable
    // outcome is still the quiet one -- the extra field dropped.
    piet::SessionState s(workdir);
    seed(s);
    spit(at("is_ragged.csv"), "a,b\n1,2\n3,4,5\n");
    auto r = exec("insheet using \"is_ragged.csv\", clear\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (i/ragged): a row with MORE fields than the header loads, as StataBE's "
          "insheet does (BUG-414). Got `" + errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(vn.size() == 3 && hasName(vn, "a") && hasName(vn, "b") && hasName(vn, "v3"),
          "AER-IOTEXT (i/ragged): ...widened by the extra field into v3 -- {a,b,v3}. Got {" +
              joinNames(vn) + "}");
    check(nRows(s) == 2.0 && countIf(s, "v3 == 5") == 1.0 && countIf(s, "missing(v3)") == 1.0,
          "AER-IOTEXT (i/ragged): ...over 2 rows, the extra 5 kept in v3 and the short row's v3 "
          "missing. Dropping the 5 is the frame that is quietly wrong. Got N " +
              fullPrec(nRows(s)) + ", v3==5 in " + fullPrec(countIf(s, "v3 == 5")));
  }

  // ── (ii) outsheet's default separator is a TAB, and that is a byte. ───────
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("clear\nset obs 2\ngen double a = _n\ngen double b = _n*2\n"
                  "outsheet using \"os.tsv\", replace\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (ii): `outsheet using \"os.tsv\", replace` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::string tsv = slurp(at("os.tsv"));
    check(!tsv.empty(),
          "AER-IOTEXT (ii): ...and left BYTES on disk. A handler that parsed the line, produced a "
          "mutation result and opened no file returns rc 0 and writes nothing -- the defect this "
          "whole bundle's tests exist for");
    check(has(tsv, "a\tb"),
          "AER-IOTEXT (ii): the header row is TAB-separated (`a\\tb`). outsheet's default "
          "delimiter is a tab and `export delimited`'s is a comma, so a shim that forwarded "
          "without synthesising `delimiter(\"\\t\")` writes a comma file under a .tsv name: every "
          "value is correct, every field boundary is wrong, and nothing returns an error. Got `" +
              tsv.substr(0, 32) + "`");
    check(!has(tsv, ","),
          "AER-IOTEXT (ii): ...and no comma appears ANYWHERE in the file, so the default was not "
          "merely tab-ish in the header. Got `" + tsv.substr(0, 32) + "`");
    check(has(tsv, "a") && has(tsv, "b"),
          "AER-IOTEXT (ii): ...and the variable NAMES are present: outsheet writes a header by "
          "default (`nonames` is the refusal in this build, not the default)");
  }
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("clear\nset obs 2\ngen double a = _n\ngen double b = _n*2\n"
                  "outsheet using \"os.csv\", replace comma\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (ii/comma): `outsheet ..., comma` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::string csv = slurp(at("os.csv"));
    check(has(csv, "a,b"),
          "AER-IOTEXT (ii/comma): `comma` overrides the tab default (`a,b`). Paired with the leg "
          "above this is what proves the separator is CHOSEN rather than hardcoded either way: "
          "one of the two checks fails for any handler that ignores the option. Got `" +
              csv.substr(0, 32) + "`");
    check(!has(csv, "\t"),
          "AER-IOTEXT (ii/comma): ...and no tab survives into the comma file. Got `" +
              csv.substr(0, 32) + "`");
  }

  // ── (iii) infix reads FIXED COLUMNS, 1-based and inclusive. ───────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(at("ix.dat"), "1990abc\n1991def\n");
    auto r = exec("infix int year 1-4 str3 code 5-7 using \"ix.dat\", clear\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (iii): `infix int year 1-4 str3 code 5-7 using \"ix.dat\", clear` runs "
          "clean. Got `" + errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 2.0,
          "AER-IOTEXT (iii): two records, one per line. Got " + fullPrec(nRows(s)));
    check(countIf(s, "year == 1990") == 1.0 && countIf(s, "year == 1991") == 1.0,
          "AER-IOTEXT (iii): the column range 1-4 is 1-BASED and INCLUSIVE, so year is {1990,1991} "
          "and not {199,199} (0-based read) or {990,991} (exclusive lower bound). Both off-by-one "
          "readings produce a perfectly plausible integer column that no rc, no row count and no "
          "missing-value check can distinguish from the right one. Got " +
              fullPrec(countIf(s, "year == 1990")) + " rows at 1990, " +
              fullPrec(countIf(s, "year == 1991")) + " at 1991");
    check(countIf(s, "code == \"abc\"") == 1.0,
          "AER-IOTEXT (iii): ...and the STRING field at 5-7 is \"abc\", so the second range was "
          "measured from the line rather than from the remainder after the first field. Got " +
              fullPrec(countIf(s, "code == \"abc\"")));
    check(dtypeOf(s, "code").rfind("str", 0) == 0,
          "AER-IOTEXT (iii): ...carrying its DECLARED type (str3). A reader that ignored the type "
          "prefix and let the sniffer decide reads `abc` as numeric, silently produces a column "
          "of missings, and reports two rows loaded. Got dtype `" + dtypeOf(s, "code") + "`");
  }
  {
    // Edge case from the plan: a range past the end of the line is MISSING,
    // not an error and not garbage from the next record.
    piet::SessionState s(workdir);
    seed(s);
    spit(at("ix_short.dat"), "12\n34\n");
    auto r = exec("infix int a 1-2 int b 5-6 using \"ix_short.dat\", clear\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (iii/short): a range past the end of the line is not an ERROR. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(countIf(s, "missing(b)") == 2.0,
          "AER-IOTEXT (iii/short): ...it is MISSING, in both rows. The alternative a naive "
          "substring reader produces is a wrapped read that pulls bytes from the NEXT record, "
          "which fabricates data and looks entirely healthy. Got " +
              fullPrec(countIf(s, "missing(b)")) + " missing");
  }

  // ── (iv) infile is FREE FORMAT: the record boundary is the varlist length,
  // not the line break. ─────────────────────────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(at("if.raw"), "\"a\" 1 \"b\" 2\n");
    auto r = exec("infile str3 s x using \"if.raw\", clear\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (iv): `infile str3 s x using \"if.raw\", clear` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(nRows(s) == 2.0,
          "AER-IOTEXT (iv): FOUR values over a two-variable varlist are TWO observations, even "
          "though the source is one line. Stata's free-format reader ignores line breaks entirely "
          "and wraps every `varlist.size()` values; a reader that treated one line as one record "
          "loads a single row and drops half the file with rc 0. Got " + fullPrec(nRows(s)));
    check(countIf(s, "x == 2") == 1.0,
          "AER-IOTEXT (iv): ...and the second record's numeric field is 2, so the wrap put the "
          "values in the right slots rather than merely counting to the right total. Got " +
              fullPrec(countIf(s, "x == 2")));
    check(countIf(s, "s == \"b\"") == 1.0,
          "AER-IOTEXT (iv): ...and the QUOTED string token arrived without its quotes as \"b\". A "
          "tokenizer that split on whitespace alone stores `\"b\"` with the delimiters embedded, "
          "which compares unequal to every string literal the user will ever write. Got " +
              fullPrec(countIf(s, "s == \"b\"")));
    check(dtypeOf(s, "s").rfind("str", 0) == 0,
          "AER-IOTEXT (iv): ...under its declared type. Got dtype `" + dtypeOf(s, "s") + "`");
  }

  // ── (v) outfile has NO HEADER and quotes its strings. ────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    const std::string kMake =
        "clear\nset obs 2\ngen double a = _n\ngen t = \"ab\"\n";
    auto r = exec(kMake + "outfile using \"of.raw\", replace\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (v): `outfile using \"of.raw\", replace` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::string raw = slurp(at("of.raw"));
    check(!raw.empty(), "AER-IOTEXT (v): ...and left BYTES on disk");
    const std::string firstLine = raw.substr(0, raw.find('\n'));
    // Leading whitespace is skipped, not asserted away: outfile formats numbers
    // with %10.0g and a right-justified field would put spaces in front of the
    // digit. What is being asserted is that the first thing on the line is a
    // VALUE, not a name.
    const size_t firstNonSpace = firstLine.find_first_not_of(" \t");
    check(firstNonSpace != std::string::npos && firstLine[firstNonSpace] >= '0' &&
              firstLine[firstNonSpace] <= '9',
          "AER-IOTEXT (v): the file carries NO HEADER ROW -- the first non-blank character of the "
          "file is a DIGIT (the first observation's `a`), not the letter that starts the variable "
          "name `a`. outfile is the one export in this bundle that never writes names, and a "
          "handler that reused outsheet's writer emits one anyway: the receiving program then "
          "reads the header as an observation and every column shifts by a row. Got first line `" +
              firstLine + "`");
    check(has(raw, "\"ab\""),
          "AER-IOTEXT (v): ...and the string value is QUOTED (`\"ab\"`), which is outfile's "
          "default and what makes a value containing the separator survive the round trip. Got `" +
              raw.substr(0, 24) + "`");
    auto r2 = exec(kMake + "outfile using \"of_nq.raw\", replace noquote\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-IOTEXT (v/noquote): `outfile ..., noquote` runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    const std::string nq = slurp(at("of_nq.raw"));
    check(!nq.empty() && !has(nq, "\""),
          "AER-IOTEXT (v/noquote): ...and NOT ONE double quote is left in the file. Paired with "
          "the leg above, this is the check that says the quoting is a decision rather than a "
          "constant: an accepted-and-dropped `noquote` leaves the bytes identical to the default "
          "and returns rc 0. Got `" + nq.substr(0, 24) + "`");
    check(has(nq, "ab"),
          "AER-IOTEXT (v/noquote): ...while the VALUE itself survives -- `noquote` drops the "
          "delimiters, not the string. Got `" + nq.substr(0, 24) + "`");
    auto r3 = exec(kMake + "outfile using \"of_c.raw\", replace comma noquote\n", s);
    check(firstError(r3.blocks).is_null(),
          "AER-IOTEXT (v/comma): `outfile ..., comma noquote` runs clean. Got `" +
              errText(firstError(r3.blocks)) + "`");
    const std::string cm = slurp(at("of_c.raw"));
    check(has(cm, ","),
          "AER-IOTEXT (v/comma): ...and `comma` really changed the separator byte. The default "
          "form above is space-separated, so a dropped `comma` produces a file with no comma in "
          "it at all. Got `" + cm.substr(0, 24) + "`");
  }

  // ── (vi) saveold: the release follows version(). ─────────────────────────
  // version(11|12) writes format 115, 13 (and the bare form) 117, 14 format
  // 118 -- genuine bytes of that release (w2ar_data_io_family.log
  // dx_svo_017_v12 first byte 115, dx_svo_019 release 117), and the echo says
  // which one was written.
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("clear\nset obs 1\ngen double x = 1\n"
                  "saveold \"so\", replace version(12)\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (vi): `saveold \"so\", replace version(12)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(fs::exists(cecon::to_native_path(at("so.dta"))),
          "AER-IOTEXT (vi): the extension defaults to .dta, so `saveold \"so\"` writes so.dta. A "
          "handler that passed the bare token through writes an extensionless file that `use "
          "\"so\"` -- which appends .dta -- can never find again");
    const std::vector<std::string> d = displays(r.blocks);
    const std::string kEcho = "(saveold: PIET writes Stata format 115)";
    bool sawEcho = false;
    for (const auto& t : d) if (t == kEcho) sawEcho = true;
    std::string dumped;
    for (const auto& t : d) { if (!dumped.empty()) dumped += " | "; dumped += t; }
    check(sawEcho,
          "AER-IOTEXT (vi): the echo is EXACTLY `" + kEcho + "` -- the release actually written. "
          "Got `" + (dumped.empty() ? std::string("<no display lines>") : dumped) + "`");
    unsigned char first = 0;
    {
      std::ifstream fin(cecon::to_native_path(at("so.dta")), std::ios::binary);
      char c0 = 0;
      if (fin.get(c0)) first = static_cast<unsigned char>(c0);
    }
    check(first == 115,
          "AER-IOTEXT (vi): ...and the file IS format 115 (first byte 115, dx_svo_017_v12 "
          "`b1=[s]`). Got " + std::to_string(static_cast<int>(first)));
    auto r2 = exec("use \"so.dta\", clear\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-IOTEXT (vi/reload): the file saveold wrote is READABLE by this build's own `use`. "
          "Got `" + errText(firstError(r2.blocks)) + "`");
    check(nRows(s) == 1.0 && countIf(s, "x == 1") == 1.0,
          "AER-IOTEXT (vi/reload): ...and carries the data back: one row with x == 1. A saveold "
          "that wrote a header and no observations still passes `fs::exists`. Got " +
              fullPrec(nRows(s)) + " rows");
  }
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("clear\nset obs 1\ngen double x = 1\nsaveold \"so2\", replace\n", s);
    const std::vector<std::string> d = displays(r.blocks);
    // Bare saveold writes release 117 (Stata 13), not the current 118:
    // w2ar_data_io_family.log:4081 `PREC|dx_svo_019|0|rel=[117]|`.
    const std::string kEchoBare = "(saveold: PIET writes Stata format 117)";
    bool sawEcho = false;
    for (const auto& t : d) if (t == kEchoBare) sawEcho = true;
    std::string dumped;
    for (const auto& t : d) { if (!dumped.empty()) dumped += " | "; dumped += t; }
    check(sawEcho,
          "AER-IOTEXT (vi/no-version): with no `version()` the echo is exactly `" + kEchoBare +
              "` -- the clause about a recorded note is ABSENT, because nothing was recorded. A "
              "single hardcoded sentence that always names version 12 would pass the leg above "
              "and lie here. Got `" +
              (dumped.empty() ? std::string("<no display lines>") : dumped) + "`");
  }

  // ── (vii) putexcel: the ROUND TRIP. The decisive leg of the bundle. ──────
  // Everything else in this function reads back a text file this process also
  // wrote. A .xlsx is a ZIP of XML parts, and "the file exists and is 4 kB" is
  // true of a workbook no reader on earth will open. So the values go out
  // through the writer and come back through `xlsx::read` -- the same reader
  // `import excel` uses on Stata's own workbooks (testP1XlsxLayer leg ②).
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("putexcel set \"pe.xlsx\", replace bold\n"
                  "putexcel A1 = \"n\"\n"
                  "putexcel B1 = 2.5\n"
                  "putexcel A2 = (2+3)\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (vii): `putexcel set ... replace bold` + three assignments run clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    int notes = 0;
    for (const auto& t : displays(r.blocks))
      if (has(t, "cell formatting options are accepted and ignored")) ++notes;
    check(notes == 1,
          "AER-IOTEXT (vii): the formatting-options note is shown EXACTLY ONCE per `set`, not "
          "once per assignment and not never. Never means `bold` was silently swallowed and the "
          "user believes the cell is bold; once-per-cell means a real putexcel script (the corpus "
          "has one with 1,464 calls) buries its own output under the same sentence repeated "
          "thousands of times. Got " + std::to_string(notes) + " occurrences");

    piet::xlsx::Sheet sh;
    const bool ok = readBook(at("pe.xlsx"), sh);
    check(ok,
          "AER-IOTEXT (vii): the workbook putexcel wrote is READABLE by this build's own xlsx "
          "reader. If this fails the defect is in the WRITER and the plan's instruction applies: "
          "stop and report, do not relax the check");
    if (ok) {
      const piet::xlsx::Cell a1 = cellAt(sh, 0, 0);
      const piet::xlsx::Cell b1 = cellAt(sh, 0, 1);
      const piet::xlsx::Cell a2 = cellAt(sh, 1, 0);
      check(!a1.isNumber && a1.text == "n",
            "AER-IOTEXT (vii): A1 came back as the STRING \"n\". Got isNumber=" +
                std::string(a1.isNumber ? "true" : "false") + " text=`" + a1.text + "`");
      check(b1.isNumber && b1.number == 2.5,
            "AER-IOTEXT (vii): B1 came back as the NUMBER 2.5 -- in column B, which is what says "
            "the cell ADDRESS was honoured. A writer that appended every assignment to the next "
            "free cell produces a workbook that opens fine and puts every value in the wrong "
            "place. Got isNumber=" + std::string(b1.isNumber ? "true" : "false") + " number=" +
                fullPrec(b1.number) + " text=`" + b1.text + "`");
      check(a2.isNumber && a2.number == 5.0,
            "AER-IOTEXT (vii): A2 == 5, so `A1 = (2+3)` was EVALUATED. A handler that stored the "
            "parenthesised text verbatim writes the string `(2+3)` into the cell: the workbook is "
            "valid, the command returned rc 0, and the spreadsheet is wrong. Got isNumber=" +
                std::string(a2.isNumber ? "true" : "false") + " number=" + fullPrec(a2.number) +
                " text=`" + a2.text + "`");
    }

    // The matrix forms, appended to the same open workbook.
    auto rm = exec("matrix M = (1,2\\3,4)\n"
                   "putexcel A4 = matrix(M)\n"
                   "putexcel A7 = matrix(M), names\n",
                   s);
    check(firstError(rm.blocks).is_null(),
          "AER-IOTEXT (vii/matrix): the two matrix assignments run clean. Got `" +
              errText(firstError(rm.blocks)) + "`");
    piet::xlsx::Sheet sm;
    const bool okm = readBook(at("pe.xlsx"), sm);
    check(okm, "AER-IOTEXT (vii/matrix): the rewritten workbook is still readable");
    if (okm) {
      check(cellAt(sm, 3, 0).isNumber && cellAt(sm, 3, 0).number == 1.0 &&
                cellAt(sm, 3, 1).isNumber && cellAt(sm, 3, 1).number == 2.0 &&
                cellAt(sm, 4, 0).isNumber && cellAt(sm, 4, 0).number == 3.0 &&
                cellAt(sm, 4, 1).isNumber && cellAt(sm, 4, 1).number == 4.0,
            "AER-IOTEXT (vii/matrix): `A4 = matrix(M)` writes the 2x2 block at rows 4-5, columns "
            "A-B, in ROW-MAJOR order: 1 2 / 3 4. A transposed write (2 1 / 4 3) is the classic "
            "silent defect here -- same cells, same count, every number in the wrong place. Got "
            "A4=" + fullPrec(cellAt(sm, 3, 0).number) + " B4=" + fullPrec(cellAt(sm, 3, 1).number) +
                " A5=" + fullPrec(cellAt(sm, 4, 0).number) + " B5=" +
                fullPrec(cellAt(sm, 4, 1).number));
      check(cellAt(sm, 0, 0).text == "n" && cellAt(sm, 1, 0).number == 5.0,
            "AER-IOTEXT (vii/matrix): ...and the cells written BEFORE the matrix are still there. "
            "The design rewrites the whole workbook on every assignment, so an accumulated cell "
            "map that was not carried forward loses everything above the newest write while the "
            "file still opens");
      // `names` is Stata's row-AND-column-name layout: a blank corner, the
      // column names across the first row of the block, the row names down its
      // first column, and the numbers shifted one right and one down.
      check(!cellAt(sm, 6, 1).isNumber && !cellAt(sm, 6, 1).text.empty(),
            "AER-IOTEXT (vii/names): with `names`, the FIRST row of the block (row 7) carries the "
            "matrix's column NAMES as text, not its numbers. Got isNumber=" +
                std::string(cellAt(sm, 6, 1).isNumber ? "true" : "false") + " text=`" +
                cellAt(sm, 6, 1).text + "`");
      check(cellAt(sm, 7, 1).isNumber && cellAt(sm, 7, 1).number == 1.0,
            "AER-IOTEXT (vii/names): ...and the matrix body is displaced one row down and one "
            "column right by the labels, putting M[1,1] at B8. An accepted-and-dropped `names` "
            "writes the bare numbers at A7 instead: rc 0, a readable workbook, and a table whose "
            "rows and columns are unlabelled in exactly the report the author wanted them in. "
            "Got B8 isNumber=" + std::string(cellAt(sm, 7, 1).isNumber ? "true" : "false") +
                " number=" + fullPrec(cellAt(sm, 7, 1).number));
    }
    check(!has(slurp(at("pe.xlsx")), "<mergeCells"),
          "AER-IOTEXT (vii/merge): a workbook with NO merges emits no <mergeCells> element at "
          "all. xlsx.h states this as a byte-identity guarantee against the pre-putexcel writer, "
          "and an always-emitted empty element (count=\"0\") quietly breaks it -- and is rejected "
          "outright by some readers");
  }
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("putexcel set \"pm.xlsx\", replace\n"
                  "putexcel A1 = \"x\"\n"
                  "putexcel B1 = \"y\"\n"
                  "putexcel A1:B1, merge\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (vii/merge): the range form `A1:B1, merge` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(has(slurp(at("pm.xlsx")), "<mergeCells"),
          "AER-IOTEXT (vii/merge): ...and a <mergeCells> element reaches the sheet XML. The "
          "writer emits STORED (uncompressed) ZIP entries, so this is a literal byte search in "
          "the file -- the merge either is in the workbook or it is not. A `merge` parsed and "
          "dropped returns rc 0 over an unmerged sheet, which is a layout defect the author only "
          "discovers in Excel");
  }
  {
    // `modify` PRELOADS: the classic defect is a modify that opened the path
    // with truncation and silently emptied a workbook the user asked to extend.
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("putexcel set \"pe.xlsx\", modify\nputexcel C1 = \"z\"\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (vii/modify): `putexcel set \"pe.xlsx\", modify` + one write runs clean. "
          "Got `" + errText(firstError(r.blocks)) + "`");
    piet::xlsx::Sheet sh;
    const bool ok = readBook(at("pe.xlsx"), sh);
    check(ok, "AER-IOTEXT (vii/modify): the modified workbook is still readable");
    if (ok) {
      check(cellAt(sh, 0, 2).text == "z",
            "AER-IOTEXT (vii/modify): the new cell C1 is there. Got `" + cellAt(sh, 0, 2).text +
                "`");
      check(cellAt(sh, 0, 0).text == "n" && cellAt(sh, 3, 0).number == 1.0,
            "AER-IOTEXT (vii/modify): ...and EVERYTHING written by the earlier session survived "
            "-- A1 is still \"n\" and the matrix block is still at A4. `modify` that truncated "
            "instead of preloading destroys a workbook the user explicitly asked to keep, "
            "reports rc 0, and leaves a file that opens perfectly with one cell in it. Got A1=`" +
                cellAt(sh, 0, 0).text + "` A4=" + fullPrec(cellAt(sh, 3, 0).number));
    }
  }
  {
    // Plan edge case: `modify` pointed at something that is NOT a workbook.
    // The failure mode this guards is the destructive one -- a modify that
    // cannot preload and therefore starts from an empty cell map, then
    // rewrites the path on the first assignment and destroys whatever the file
    // actually was.
    piet::SessionState s(workdir);
    seed(s);
    spit(at("notabook.xlsx"), "this is plainly not a zip archive");
    auto r = exec("putexcel set \"notabook.xlsx\", modify\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(),
          "AER-IOTEXT (vii/not-a-workbook): `set ..., modify` over a file this build cannot read "
          "is an ERROR at the `set`, before any cell map exists");
    check(has(errText(e), "workbook") || has(errText(e), "putexcel"),
          "AER-IOTEXT (vii/not-a-workbook): ...and the message says which file and what is wrong "
          "with it, rather than surfacing the ZIP reader's complaint. Got `" + errText(e) + "`");
    auto r2 = exec("putexcel A1 = \"x\"\n", s);
    check(!firstError(r2.blocks).is_null(),
          "AER-IOTEXT (vii/not-a-workbook): ...and no workbook was opened, so the next assignment "
          "is the no-open-workbook error rather than a write");
    check(slurp(at("notabook.xlsx")) == "this is plainly not a zip archive",
          "AER-IOTEXT (vii/not-a-workbook): ...and the FILE IS INTACT. This is the whole point of "
          "the leg: a `modify` that swallowed the read failure and continued with an empty map "
          "would overwrite the user's file with a one-cell workbook on the very next line, having "
          "reported nothing. Got `" + slurp(at("notabook.xlsx")) + "`");
  }
  {
    // The round trip through PIET's own `import excel` -- the second reader.
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("putexcel set \"pr.xlsx\", replace\n"
                  "putexcel A1 = \"k\"\n"
                  "putexcel B1 = \"v\"\n"
                  "putexcel A2 = 7\n"
                  "putexcel B2 = 8\n"
                  "import excel using \"pr.xlsx\", clear firstrow\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (vii/import): putexcel writes a workbook and `import excel ... firstrow` "
          "reads it back in the same session. Got `" + errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "k") && hasName(vn, "v"),
          "AER-IOTEXT (vii/import): the row-1 strings became the VARIABLE NAMES {k,v}, so the "
          "cells putexcel addressed as A1/B1 really are row 1 columns A and B and not, say, one "
          "string crammed into a single cell. Got {" + joinNames(vn) + "}");
    check(nRows(s) == 1.0,
          "AER-IOTEXT (vii/import): ...over exactly one data row. Got " + fullPrec(nRows(s)));
    check(countIf(s, "k == 7") == 1.0 && countIf(s, "v == 8") == 1.0,
          "AER-IOTEXT (vii/import): ...whose values are 7 and 8, as NUMBERS. This is the end of "
          "the loop the plan calls the STOP condition: a mismatch here means our writer and our "
          "reader disagree about the format, which no single-sided check can see. Got k-count " +
              fullPrec(countIf(s, "k == 7")) + ", v-count " + fullPrec(countIf(s, "v == 8")));
  }
  {
    // Writing with no open workbook, and writing after `clear`.
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("putexcel A1 = \"x\"\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(),
          "AER-IOTEXT (vii/no-set): an assignment with no open workbook is an ERROR. A handler "
          "that invented a default filename writes a workbook nobody asked for, in a directory "
          "nobody chose, and reports success");
    check(has(errText(e), "putexcel set"),
          "AER-IOTEXT (vii/no-set): ...and the message names `putexcel set`, so the fix is in the "
          "message rather than in the manual. Got `" + errText(e) + "`");

    piet::SessionState s2(workdir);
    seed(s2);
    auto r2 = exec("putexcel set \"pc.xlsx\", replace\nputexcel A1 = \"x\"\nputexcel clear\n", s2);
    check(firstError(r2.blocks).is_null(),
          "AER-IOTEXT (vii/clear): set -> write -> clear runs clean. Got `" +
              errText(firstError(r2.blocks)) + "`");
    check(s2.putexcel.empty(),
          "AER-IOTEXT (vii/clear): `putexcel clear` empties SessionState::putexcel. Got " +
              s2.putexcel.dump());
    auto r3 = exec("putexcel B1 = \"y\"\n", s2);
    check(!firstError(r3.blocks).is_null(),
          "AER-IOTEXT (vii/clear): ...and a write AFTER clear is an error again. A `clear` that "
          "printed something and left the state behind lets the next assignment silently reopen "
          "and rewrite a workbook the author had finished with");
  }

  // ── (viii) filefilter: the ESCAPES are decoded, and r(occurrences) counts
  // REPLACEMENTS. ───────────────────────────────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(at("ff_in.txt"), "x\"y\"z");
    // `\Q` is filefilter's escape for a double quote. `to(Q)` is a plain
    // letter on purpose: this call isolates the FROM-side escape decode from
    // any question about quoting the TO argument, which the next call covers.
    auto r = exec("filefilter \"ff_in.txt\" \"ff_out.txt\", from(\\Q) to(Q) replace\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (viii): `filefilter ... from(\\Q) to(Q) replace` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::string out = slurp(at("ff_out.txt"));
    check(out == "xQyQz",
          "AER-IOTEXT (viii): the output is EXACTLY `xQyQz`, byte for byte. `\\Q` must be decoded "
          "to a double quote before the search: taken literally it matches the two-character "
          "sequence backslash-Q, which appears nowhere, so the file is copied through unchanged "
          "and the command reports success over a filter that filtered nothing. Got `" + out + "`");
    check(rOf(s, "occurrences") == 2.0,
          "AER-IOTEXT (viii): ...and r(occurrences) == 2, the number of REPLACEMENTS. Not 1 (one "
          "line touched) and not 0 (published as a name with no value): do-files branch on this "
          "number, and a line count agrees with the replacement count on exactly the one-hit "
          "input that would hide the bug. Got " + fullPrec(rOf(s, "occurrences")));
    check(slurp(at("ff_in.txt")) == "x\"y\"z",
          "AER-IOTEXT (viii): ...and the INPUT file is untouched. filefilter writes a new file; "
          "an implementation that opened the source for writing en route destroys the original");
  }
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(at("ff_in2.txt"), "x\"y\"z");
    // The plan's own literal spelling, apostrophe and all.
    auto r = exec("filefilter \"ff_in2.txt\" \"ff_out2.txt\", from(\\Q) to(') replace\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (viii/apostrophe): the plan's literal `to(')` form parses. A lone "
          "apostrophe is the closing half of Stata's local-macro delimiter, so an option reader "
          "that scans for macro syntax before it splits options can swallow the rest of the line. "
          "Got `" + errText(firstError(r.blocks)) + "`");
    check(slurp(at("ff_out2.txt")) == "x'y'z",
          "AER-IOTEXT (viii/apostrophe): ...and produces `x'y'z`. Got `" +
              slurp(at("ff_out2.txt")) + "`");
  }
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(at("ff_same.txt"), "aaa");
    auto r = exec("filefilter \"ff_same.txt\" \"ff_same.txt\", from(a) to(b) replace\n", s);
    check(!firstError(r.blocks).is_null(),
          "AER-IOTEXT (viii/same): input == output is an ERROR, refused BEFORE anything is "
          "written. This is the dest-==-source class (BUG-138): a filter that streamed through "
          "its own source truncates the file it is reading and the surviving bytes are whatever "
          "the buffer happened to hold");
    check(slurp(at("ff_same.txt")) == "aaa",
          "AER-IOTEXT (viii/same): ...and the file is INTACT. An error raised after the truncate "
          "is the worst version of this defect -- it reports failure and did the damage. Got `" +
              slurp(at("ff_same.txt")) + "`");
  }

  // ── (ix) copy: a real byte copy, an honest already-exists, and a URL that
  // is refused BY NAME. ─────────────────────────────────────────────────────
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(at("cp_a.txt"), "hello");
    auto r = exec("copy \"cp_a.txt\" \"cp_b.txt\"\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-IOTEXT (ix): `copy \"cp_a.txt\" \"cp_b.txt\"` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    check(slurp(at("cp_b.txt")) == "hello",
          "AER-IOTEXT (ix): ...and the destination holds the source's BYTES. `fs::exists` is true "
          "of the zero-length file a create-then-fail leaves behind. Got `" +
              slurp(at("cp_b.txt")) + "`");
    auto r2 = exec("copy \"cp_a.txt\" \"cp_b.txt\"\n", s);
    const nlohmann::ordered_json e2 = firstError(r2.blocks);
    check(!e2.is_null(),
          "AER-IOTEXT (ix/exists): copying onto an existing destination WITHOUT `replace` is an "
          "error. A copy that overwrote silently is data loss with a clean return code");
    check(rcOf(e2) == "602",
          "AER-IOTEXT (ix/exists): ...at r(602), Stata's own `file already exists`, so a do-file "
          "branching on `if _rc == 602` takes the branch its author wrote. Got rc " + rcOf(e2) +
              " from `" + errText(e2) + "`");
    auto r3 = exec("copy \"cp_a.txt\" \"cp_b.txt\", replace\n", s);
    check(firstError(r3.blocks).is_null(),
          "AER-IOTEXT (ix/replace): ...and `replace` makes the same call succeed, so 602 above "
          "was the guard and not a broken path. Got `" + errText(firstError(r3.blocks)) + "`");
    auto r4 = exec("copy \"cp_a.txt\" \"cp_a.txt\", replace\n", s);
    check(!firstError(r4.blocks).is_null() && slurp(at("cp_a.txt")) == "hello",
          "AER-IOTEXT (ix/self): src == dst is refused and the file is INTACT (dest-==-source, "
          "BUG-138: copy_file with overwrite_existing onto itself is where a file goes to be "
          "emptied). Got `" + slurp(at("cp_a.txt")) + "`");
  }
  {
    piet::SessionState s(workdir);
    seed(s);
    auto r = exec("copy \"http://example.invalid/x.dta\" \"cp_url.dta\"\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(), "AER-IOTEXT (ix/url): a URL source is refused");
    check(errField(e, "reason") == "refused:no_network",
          "AER-IOTEXT (ix/url): ...with the cause `refused:no_network` in `reason`. The wrong "
          "implementation this catches is the one that treats `http://example.invalid/x.dta` as a "
          "relative FILENAME and answers r(601) `file not found`: the user then spends an "
          "afternoon looking for a file, when the true answer is that this build makes no network "
          "calls from Stata verbs and never will. Got reason `" + errField(e, "reason") + "`");
    check(has(errText(e), "URL") || has(errText(e), "url"),
          "AER-IOTEXT (ix/url): ...and the sentence says so in words, not only in a machine "
          "field. Got `" + errText(e) + "`");
    check(!fs::exists(cecon::to_native_path(at("cp_url.dta"))),
          "AER-IOTEXT (ix/url): ...and NOTHING was created at the destination. A refusal that "
          "already opened the output leaves an empty file that the next `use` finds and fails on "
          "for a completely different reason");
  }

  // ── (x) the refusals, each naming its OWN cause. ─────────────────────────
  // A registered row that refuses BY REASON tells the user what is missing; an
  // unregistered name answers a generic r(199) that reads like a typo. What
  // this leg really guards is the second half: rows sharing one cause string is
  // a user typing `odbc` and being told there is no shapefile reader.
  // AER tier-1 (2026-10-28): unzipfile and zipfile left this table -- they run
  // now (zip_io.h), and selftest_file_io_zip_commands.cpp checks them.
  {
    struct Refusal { const char* cmd; const char* reason; };
    const std::vector<Refusal> kRefusals = {
        {"odbc list", "refused:no_odbc_driver"},
        {"spshape2dta \"shp\"", "refused:no_shapefile_reader"},
    };
    for (const auto& rf : kRefusals) {
      piet::SessionState s(workdir);
      seed(s);
      auto r = exec(std::string(rf.cmd) + "\n", s);
      const nlohmann::ordered_json e = firstError(r.blocks);
      check(!e.is_null(),
            std::string("AER-IOTEXT (x): `") + rf.cmd + "` is refused rather than silently "
            "accepted");
      check(errField(e, "reason") == rf.reason,
            std::string("AER-IOTEXT (x): `") + rf.cmd + "` refuses with its OWN cause `" +
                rf.reason + "`. Got `" + errField(e, "reason") + "` -- a shared cause string " +
                "across these rows tells an `odbc` user about a missing shapefile reader, and a " +
                "bare `piet_gap` tells them their spelling is wrong");
      check(rcOf(e) == "199",
            std::string("AER-IOTEXT (x): ...at r(199), so `capture` + `_rc` in a do-file sees the "
            "same code Stata gives an unimplemented command. Got rc ") + rcOf(e) + " for `" +
                rf.cmd + "`");
    }
  }

  fs::remove_all(workdir, ec);
}


// BETA campaign Phase B (B-log cluster): `log using FILE [, replace|append]
// [text]` really opens a file and tees subsequent command output to it as a
// plain-text transcript (BUG-028 / BS-006 — previously a pure no-op shim
// that only tracked session.logPath). Real coverage, not just a golden
// diff: open -> run commands -> close -> read the file back off disk and
// assert real content, plus append-vs-replace, the double-open / file-
// exists / smcl honest-rejects, and log off/on pausing.
void testBS006LogUsingRealFileWrite() {
  std::printf("[selftest] --- BS-006 `log using`/`log close` real file write (BUG-028) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs006").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  std::string logPath = workdir + "/test.log";

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  // 1. `log using FILE, replace text` opens a REAL file on disk (no error).
  auto openBlocks = piet::dispatch("log using \"" + logPath + "\", replace text", session);
  bool openErr = false;
  for (const auto& b : openBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") openErr = true;
  }
  check(!openErr, "BS-006: `log using FILE, replace text` does not error");
  check(fs::exists(logPath, ec), "BS-006: `log using ... replace` creates the file on disk");

  // 2. Run a couple of commands while the log is open.
  (void)piet::dispatch("count", session);
  (void)piet::dispatch("summarize year", session);

  // 3. `log close` flushes + closes.
  (void)piet::dispatch("log close", session);

  {
    std::ifstream in(cecon::to_native_path(logPath));
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    check(!content.empty(), "BS-006: the log file has non-empty content after `log close`");
    check(content.find("count") != std::string::npos,
          "BS-006: the log transcript contains the echoed `count` command");
    check(content.find("summarize year") != std::string::npos,
          "BS-006: the log transcript contains the echoed `summarize year` command");
    // `summarize` has no pre-rendered "text" field anywhere in this codebase
    // (the webview renders it from raw JSON) -- logTee()'s generic
    // JSON->text fallback should still surface its own fields, headed by a
    // "[summarize]" marker line, not a stub placeholder.
    check(content.find("[summarize]") != std::string::npos,
          "BS-006: the summarize block's OWN fields reached the log via the generic "
          "JSON->text fallback (not just its echo line)");
  }

  // 4. `append` mode preserves prior content instead of truncating.
  auto appendOpenBlocks = piet::dispatch("log using \"" + logPath + "\", append text", session);
  bool appendOpenErr = false;
  for (const auto& b : appendOpenBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") appendOpenErr = true;
  }
  check(!appendOpenErr, "BS-006: re-opening the SAME file with `append text` does not error");
  (void)piet::dispatch("display 2+2", session);
  (void)piet::dispatch("log close", session);
  {
    std::ifstream in(cecon::to_native_path(logPath));
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    check(content.find("summarize year") != std::string::npos,
          "BS-006: `append` preserves the PRIOR session's content (not truncated)");
    check(content.find("display 2+2") != std::string::npos,
          "BS-006: `append` also contains the NEW content written after re-opening");
  }

  // 5. `replace` truncates -- prior content is gone.
  (void)piet::dispatch("log using \"" + logPath + "\", replace text", session);
  (void)piet::dispatch("display 1+1", session);
  (void)piet::dispatch("log close", session);
  {
    std::ifstream in(cecon::to_native_path(logPath));
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    check(content.find("summarize") == std::string::npos,
          "BS-006: `replace` truncates -- the earlier session's content is gone");
    check(content.find("display 1+1") != std::string::npos,
          "BS-006: `replace` still contains the fresh content written after re-opening");
  }

  // 6. Bare `log using` (no replace/append) on an EXISTING file is an honest
  // reject (Stata's real "file already exists" r(602) idiom), not a silent
  // overwrite or silent append.
  {
    auto rejBlocks = piet::dispatch("log using \"" + logPath + "\"", session);
    bool sawErr = false;
    for (const auto& b : rejBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawErr = true;
    }
    check(sawErr, "BS-006: bare `log using` on an EXISTING file (no replace/append) is an "
                  "honest reject, not silently accepted");
    // errors.cpp's coarseRc() maps this engine_error (message mentions
    // "file" + "already exists") to Stata's real r(602) with no new mapping
    // rule needed -- verify `capture`'s `_rc` actually lands there.
    (void)piet::dispatch("capture log using \"" + logPath + "\"", session);
    std::string rc = session.eval.locals.get("_rc");
    check(rc == "602",
          "BS-006: `capture log using` (existing file, no replace/append) sets _rc=602 (got '" +
              rc + "')");
  }

  // 7. A second `log using` while one is already open is ALSO an honest
  // reject (real Stata requires `log close` first) -- not a silent
  // discard-and-reopen.
  {
    std::string logPath2 = workdir + "/second.log";
    auto firstOpen = piet::dispatch("log using \"" + logPath2 + "\", replace text", session);
    bool firstOpenErr = false;
    for (const auto& b : firstOpen) {
      if (b.is_object() && b.value("type", std::string()) == "error") firstOpenErr = true;
    }
    check(!firstOpenErr, "BS-006: opening a fresh second log succeeds");
    std::string logPath3 = workdir + "/third.log";
    auto dupBlocks = piet::dispatch("log using \"" + logPath3 + "\", replace text", session);
    bool sawErr = false;
    for (const auto& b : dupBlocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") sawErr = true;
    }
    check(sawErr, "BS-006: a second `log using` while one is already open is an honest reject");
    check(!fs::exists(logPath3, ec),
          "BS-006: the rejected second `log using` did not create third.log");
    (void)piet::dispatch("log close", session);
  }

  // 8. SMCL logs (`log using x.smcl`, or `log using FILE, smcl`) are written for
  // real; a file that claims SMCL is SMCL. The layout pinned below -- the
  // {smcl} marker, header, tagged echo and display lines, the log off / log on /
  // log close blocks, the final marker with no newline, the `append` run-on --
  // is what StataBE 19.5 wrote for the same do-file lines (captures of
  // `log using x.smcl` with display lines, `name()`, `append`, `log off` and
  // `log on`). Only the date stamps vary, so they are masked. The text arm at the
  // end pins the plain-text transcript byte for byte, captured from this build
  // BEFORE the SMCL writer existed, so the text path cannot drift unseen.
  {
    auto slurp8 = [](const std::string& p) {
      std::ifstream in(cecon::to_native_path(p), std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    };
    // "...on:  {res}28 Oct 2026, 02:46:04\n" -> "...on:  {res}<STAMP>\n"
    auto maskStamps = [](const std::string& s) {
      static const std::regex re(
          R"(on:  \{res\}[ 0-9]{2} [A-Z][a-z]{2} [0-9]{4}, [0-9]{2}:[0-9]{2}:[0-9]{2}\n)");
      return std::regex_replace(s, re, "on:  {res}<STAMP>\n");
    };
    auto fill = [](std::string s, const std::string& token, const std::string& value) {
      for (size_t at = s.find(token); at != std::string::npos; at = s.find(token, at + value.size())) {
        s.replace(at, token.size(), value);
      }
      return s;
    };
    // check() prints its label on a pass too, so the observed text is shown only
    // when the check fails.
    auto checkGot = [](bool ok, const std::string& label, const std::string& got) {
      check(ok, label);
      if (!ok) std::printf("[selftest]        got:\n%s\n[selftest]        --\n", got.c_str());
    };
    auto sawError = [](const nlohmann::ordered_json& blocks) {
      for (const auto& b : blocks) {
        if (b.is_object() && b.value("type", std::string()) == "error") return true;
      }
      return false;
    };

    // 8a. An unnamed .smcl log: display lines, a literal-brace command line,
    // log off / log on, log close.
    {
      const std::string P = workdir + "/x.smcl";
      const bool openErr = sawError(piet::dispatch("log using \"" + P + "\", replace", session));
      check(!openErr, "BS-006: `log using x.smcl, replace` opens (no unsupported_option reject)");
      (void)piet::dispatch("display \"plain\"", session);
      (void)piet::dispatch("display \"a {c -(} b {c )-} c\"", session);
      (void)piet::dispatch("display", session);
      (void)piet::dispatch("log off", session);
      (void)piet::dispatch("display \"not logged\"", session);
      (void)piet::dispatch("log on", session);
      (void)piet::dispatch("display \"after on\"", session);
      (void)piet::dispatch("log close", session);
      const std::string expected = fill(
          R"SMCL({smcl}
{txt}{sf}{ul off}{.-}
      name:  {res}<unnamed>
       {txt}log:  {res}@P@
  {txt}log type:  {res}smcl
 {txt}opened on:  {res}<STAMP>
{txt}
{com}. display "plain"
{res}plain
{txt}
{com}. display "a {c -(}c -({c )-} b {c -(}c )-{c )-} c"
{res}a {c -(} b {c )-} c
{txt}
{com}. display
{res}
{txt}
{com}. log off
      {txt}name:  {res}<unnamed>
       {txt}log:  {res}@P@
  {txt}log type:  {res}smcl
 {txt}paused on:  {res}<STAMP>
{txt}{.-}
{smcl}
{txt}{sf}{ul off}{smcl}
{com}{sf}{ul off}{txt}{.-}
      name:  {res}<unnamed>
       {txt}log:  {res}@P@
  {txt}log type:  {res}smcl
{txt}resumed on:  {res}<STAMP>
{txt}
{com}. display "after on"
{res}after on
{txt}
{com}. log close
      {txt}name:  {res}<unnamed>
       {txt}log:  {res}@P@
  {txt}log type:  {res}smcl
 {txt}closed on:  {res}<STAMP>
{txt}{.-}
{smcl}
{txt}{sf}{ul off})SMCL",
          "@P@", P);
      const std::string got = maskStamps(slurp8(P));
      checkGot(got == expected,
               "BS-006: the SMCL log is byte-for-byte StataBE's layout (header, tagged echo and "
               "display lines, escaped braces, log off/on blocks, closing marker)",
               got);
    }

    // 8b. A named log, closed and re-opened with `append`: the second segment
    // runs on from the first's closing marker with the continuation header.
    {
      const std::string Q = workdir + "/nm.smcl";
      (void)piet::dispatch("log using \"" + Q + "\", replace name(nm)", session);
      (void)piet::dispatch("display \"named hello\"", session);
      (void)piet::dispatch("log close nm", session);
      (void)piet::dispatch("log using \"" + Q + "\", append name(nm)", session);
      (void)piet::dispatch("display \"appended\"", session);
      (void)piet::dispatch("log close nm", session);
      const std::string expected = fill(
          R"SMCL({smcl}
{txt}{sf}{ul off}{.-}
      name:  {res}nm
       {txt}log:  {res}@Q@
  {txt}log type:  {res}smcl
 {txt}opened on:  {res}<STAMP>
{txt}
{com}. display "named hello"
{res}named hello
{txt}
{com}. log close nm
      {txt}name:  {res}nm
       {txt}log:  {res}@Q@
  {txt}log type:  {res}smcl
 {txt}closed on:  {res}<STAMP>
{txt}{.-}
{smcl}
{txt}{sf}{ul off}{smcl}
{com}{sf}{ul off}{txt}{.-}
      name:  {res}nm
       {txt}log:  {res}@Q@
  {txt}log type:  {res}smcl
 {txt}opened on:  {res}<STAMP>
{txt}
{com}. display "appended"
{res}appended
{txt}
{com}. log close nm
      {txt}name:  {res}nm
       {txt}log:  {res}@Q@
  {txt}log type:  {res}smcl
 {txt}closed on:  {res}<STAMP>
{txt}{.-}
{smcl}
{txt}{sf}{ul off})SMCL",
          "@Q@", Q);
      const std::string got = maskStamps(slurp8(Q));
      checkGot(got == expected,
               "BS-006: a named SMCL log, closed and re-opened with `append`, matches StataBE's "
               "layout (continuation header after the first closing marker)",
               got);
    }

    // 8c. Which format a file gets, and the extension it is given. StataBE:
    // `.smcl` -> SMCL, `.log` -> text, no extension -> `set logtype` (SMCL) and
    // ".smcl" appended, or ".log" when `text` is given; `text`/`smcl` decide
    // regardless of the extension. A dot in a directory name is not an extension.
    {
      const std::string dotted = workdir + "/d.v2";
      fs::create_directories(dotted, ec);
      auto runLogCase = [&](const std::string& typed, const std::string& opts) {
        (void)piet::dispatch("log using \"" + typed + "\", replace" + opts, session);
        (void)piet::dispatch("display \"body\"", session);
        (void)piet::dispatch("log close", session);
      };
      auto startsSmcl = [&](const std::string& p) {
        return slurp8(p).compare(0, 7, "{smcl}\n") == 0;
      };

      runLogCase(workdir + "/noext", "");
      check(fs::exists(workdir + "/noext.smcl", ec) && !fs::exists(workdir + "/noext", ec),
            "BS-006: no extension and no format option -> SMCL, \".smcl\" appended");
      check(startsSmcl(workdir + "/noext.smcl"), "BS-006: the extensionless default is SMCL");

      runLogCase(workdir + "/noext2", ", text");
      check(fs::exists(workdir + "/noext2.log", ec) && !fs::exists(workdir + "/noext2", ec),
            "BS-006: no extension with `text` -> \".log\" appended");
      check(!startsSmcl(workdir + "/noext2.log"), "BS-006: `text` writes a text log");

      runLogCase(dotted + "/noext3", "");
      check(fs::exists(dotted + "/noext3.smcl", ec) && !fs::exists(dotted + "/noext3", ec),
            "BS-006: a dot in the directory name is not a file extension");

      runLogCase(workdir + "/ext.log", "");
      check(fs::exists(workdir + "/ext.log", ec) && !startsSmcl(workdir + "/ext.log"),
            "BS-006: a .log file with no format option is a text log");

      runLogCase(workdir + "/ext2.smcl", ", text");
      check(fs::exists(workdir + "/ext2.smcl", ec) && !startsSmcl(workdir + "/ext2.smcl"),
            "BS-006: `text` wins over a .smcl extension");

      runLogCase(workdir + "/ext3.log", ", smcl");
      check(fs::exists(workdir + "/ext3.log", ec) && startsSmcl(workdir + "/ext3.log"),
            "BS-006: `smcl` wins over a .log extension");

      runLogCase(workdir + "/UPPER.SMCL", "");
      check(startsSmcl(workdir + "/UPPER.SMCL"), "BS-006: the .smcl extension is case-blind");

      check(sawError(piet::dispatch("log using \"" + workdir + "/both.log\", replace text smcl",
                                    session)),
            "BS-006: `text` and `smcl` together are a syntax error");
      check(!fs::exists(workdir + "/both.log", ec), "BS-006: the rejected open left no file");
    }

    // 8d. Non-display blocks and errors in an SMCL log: tagged, and literal
    // braces in PIET-made text escaped. A captured (noisily) error has no
    // r() line; an uncaptured one links its return code.
    {
      const std::string R = workdir + "/blocks.smcl";
      (void)piet::dispatch("log using \"" + R + "\", replace", session);
      (void)piet::dispatch("summarize year", session);
      (void)piet::dispatch("capture noisily confirm variable nosuchvar", session);
      const std::string braceFile = workdir + "/no{such}.dta";
      (void)piet::dispatch("use \"" + braceFile + "\"", session);
      (void)piet::dispatch("confirm variable nosuchvar2", session);
      // Text a command other than `display` prints is plain: `type` of a file
      // with SMCL-looking braces must not turn them into directives.
      const std::string typed = workdir + "/typed.txt";
      {
        std::ofstream f(cecon::to_native_path(typed), std::ios::binary);
        f << "type {bf:me} {x}\n";
      }
      (void)piet::dispatch("type \"" + typed + "\"", session);
      (void)piet::dispatch("log close", session);
      const std::string got = slurp8(R);
      check(got.find("{res}type {c -(}bf:me{c )-} {c -(}x{c )-}\n") != std::string::npos &&
                got.find("type {bf:me}") == std::string::npos,
            "BS-006: text a command other than `display` prints has its braces escaped");
      check(got.find("{com}. summarize year\n{txt}[summarize]\n") != std::string::npos,
            "BS-006: a result block follows its command line in {txt} mode");
      check(got.find("{com}. capture noisily confirm variable nosuchvar\n{err}") != std::string::npos,
            "BS-006: an error line is in {err} mode");
      check(got.find("{txt}{search r(111), local:r(111);}\n") != std::string::npos,
            "BS-006: an uncaptured error is followed by its linked return code");
      check(got.find("{search r(", got.find("{search r(111)") + 1) == std::string::npos,
            "BS-006: `capture noisily` prints no return-code line (only the uncaptured error does)");
      check(got.find("no{c -(}such{c )-}.dta") != std::string::npos && got.find("no{such}") == std::string::npos,
            "BS-006: literal braces in logged non-display text are escaped as {c -(} / {c )-}");
    }

    // 8e. A text log and an SMCL log open together each keep their own format.
    {
      const std::string T = workdir + "/both_t.log";
      const std::string S = workdir + "/both_s.smcl";
      (void)piet::dispatch("log using \"" + T + "\", replace text", session);
      (void)piet::dispatch("log using \"" + S + "\", replace name(s)", session);
      (void)piet::dispatch("display \"two formats\"", session);
      (void)piet::dispatch("log close s", session);
      (void)piet::dispatch("log close", session);
      const std::string t = slurp8(T);
      const std::string s = slurp8(S);
      check(t.compare(0, 1, "\n") == 0 && t.find("two formats\n") != std::string::npos &&
                t.find("{smcl}") == std::string::npos && t.find("{res}") == std::string::npos,
            "BS-006: the text log stays plain text while an SMCL log is open beside it");
      check(s.compare(0, 7, "{smcl}\n") == 0 && s.find("{res}two formats\n") != std::string::npos &&
                s.find("{com}. display \"two formats\"\n") != std::string::npos,
            "BS-006: the SMCL log beside a text log carries the tagged lines");
    }

    // 8g. Under `quietly` / `capture` the banner and footer text are suppressed
    // output, so StataBE writes only its structural markers where they would
    // stand ("{smcl}\n{com}{sf}{ul off}", no newline); `capture noisily` keeps
    // the full text. (Captures: `qui log off`, `qui log on`, `cap log close`,
    // `cap noisily log off`.)
    {
      const std::string K = workdir + "/quiet.smcl";
      (void)piet::dispatch("log using \"" + K + "\", replace", session);
      (void)piet::dispatch("display \"A\"", session);
      (void)piet::dispatch("qui log off", session);
      (void)piet::dispatch("display \"hidden\"", session);
      (void)piet::dispatch("qui log on", session);
      (void)piet::dispatch("display \"C\"", session);
      (void)piet::dispatch("cap noisily log off", session);
      (void)piet::dispatch("display \"hidden too\"", session);
      (void)piet::dispatch("cap noisily log on", session);
      (void)piet::dispatch("cap log close", session);
      const std::string expected = fill(
          R"SMCL({smcl}
{txt}{sf}{ul off}{.-}
      name:  {res}<unnamed>
       {txt}log:  {res}@K@
  {txt}log type:  {res}smcl
 {txt}opened on:  {res}<STAMP>
{txt}
{com}. display "A"
{res}A
{txt}
{com}. qui log off
{smcl}
{com}{sf}{ul off}{smcl}
{com}{sf}{ul off}{txt}
{com}. display "C"
{res}C
{txt}
{com}. cap noisily log off
      {txt}name:  {res}<unnamed>
       {txt}log:  {res}@K@
  {txt}log type:  {res}smcl
 {txt}paused on:  {res}<STAMP>
{txt}{.-}
{smcl}
{txt}{sf}{ul off}{smcl}
{com}{sf}{ul off}{txt}{.-}
      name:  {res}<unnamed>
       {txt}log:  {res}@K@
  {txt}log type:  {res}smcl
{txt}resumed on:  {res}<STAMP>
{txt}
{com}. cap log close
{smcl}
{com}{sf}{ul off})SMCL",
          "@K@", K);
      const std::string got = maskStamps(slurp8(K));
      checkGot(got == expected,
               "BS-006: quiet/capture `log off` / `log on` / `log close` write StataBE's short "
               "markers, `cap noisily` the full blocks",
               got);
    }

    // 8h. `log off` / `log on` inside a nested `do` file: the frame that carries
    // the whole file's blocks is written when it ends, and the pause takes effect
    // at its own line -- the lines between are not recorded and the `log on`
    // line is not either. (StataBE n1.smcl capture, same sub-do.)
    {
      const std::string N = workdir + "/nested.smcl";
      const std::string sub = workdir + "/nested_sub.do";
      {
        std::ofstream f(cecon::to_native_path(sub), std::ios::binary);
        f << "display \"sub A\"\ncap log off nest\ndisplay \"sub while off\"\n"
             "cap log on nest\ndisplay \"sub C\"\n";
      }
      (void)piet::dispatch("log using \"" + N + "\", replace name(nest)", session);
      (void)piet::dispatch("display \"master before\"", session);
      (void)piet::dispatch("do \"" + sub + "\"", session);
      (void)piet::dispatch("display \"master after\"", session);
      (void)piet::dispatch("log close nest", session);
      const std::string got = maskStamps(slurp8(N));
      const size_t offAt = got.find("{com}. cap log off nest\n{smcl}\n{com}{sf}{ul off}{smcl}\n"
                                    "{com}{sf}{ul off}{txt}\n{com}. display \"sub C\"\n");
      checkGot(offAt != std::string::npos &&
                   got.find("{com}. display \"sub A\"\n{res}sub A\n{txt}\n{com}. cap log off nest\n") !=
                       std::string::npos,
               "BS-006: a pause inside a nested do file takes effect at its own line, "
               "with the quiet markers",
               got);
      check(got.find("sub while off") == std::string::npos &&
                got.find("cap log on nest") == std::string::npos,
            "BS-006: lines between the nested `log off` and `log on`, and the `log on` line "
            "itself, are not recorded");
      const std::string tail = "{txt}{.-}\n{smcl}\n{txt}{sf}{ul off}";
      check(got.find("{com}. display \"master after\"\n{res}master after\n{txt}\n"
                     "{com}. log close nest\n      {txt}name:  {res}nest\n") != std::string::npos &&
                got.size() > tail.size() &&
                got.compare(got.size() - tail.size(), tail.size(), tail) == 0,
            "BS-006: the log closed after the nested file still gets its footer, in order");
    }

    // 8f. The text transcript, byte for byte, as this build wrote it before the
    // SMCL writer existed (captured from the pre-change binary on the same
    // lines): `log off` / `log on` echo skipping, a second named text log, an
    // error line, and a `.log` file with no format option. The extensionless
    // `text` log is the one intended change: it now gets ".log" appended.
    {
      const std::string A = workdir + "/t8a.log";
      const std::string B = workdir + "/t8b.txt";
      const std::string C = workdir + "/t8c.log";
      const std::string D = workdir + "/t8d";
      (void)piet::dispatch("log using \"" + A + "\", replace text", session);
      (void)piet::dispatch("display \"plain\"", session);
      (void)piet::dispatch("display \"brace {c -(} x\"", session);
      (void)piet::dispatch("count", session);
      (void)piet::dispatch("capture noisily confirm variable nosuchvar", session);
      (void)piet::dispatch("log using \"" + B + "\", replace text name(two)", session);
      (void)piet::dispatch("display \"both\"", session);
      (void)piet::dispatch("log off two", session);
      (void)piet::dispatch("display \"only first\"", session);
      (void)piet::dispatch("log on two", session);
      (void)piet::dispatch("display \"both again\"", session);
      (void)piet::dispatch("log close two", session);
      (void)piet::dispatch("display \"only first again\"", session);
      (void)piet::dispatch("log off", session);
      (void)piet::dispatch("display \"hidden\"", session);
      (void)piet::dispatch("log on", session);
      (void)piet::dispatch("display \"after\"", session);
      (void)piet::dispatch("log close", session);
      (void)piet::dispatch("log using \"" + C + "\", replace", session);
      (void)piet::dispatch("display \"ext log\"", session);
      (void)piet::dispatch("log close", session);
      (void)piet::dispatch("log using \"" + D + "\", replace text", session);
      (void)piet::dispatch("display \"noext text\"", session);
      (void)piet::dispatch("log close", session);
      const std::string expectA =
          "\n.  log using \"" + A + "\", replace text\n"
          "\n.  display \"plain\"\nplain\n"
          "\n.  display \"brace {c -(} x\"\nbrace {c -(} x\n"
          "\n.  count\n336\n"
          "\n.  capture noisily confirm variable nosuchvar\n"
          "variable nosuchvar not found\n(engine_error)\n"
          "\n.  log using \"" + B + "\", replace text name(two)\n"
          "\n.  display \"both\"\nboth\n"
          "\n.  log off two\n"
          "\n.  display \"only first\"\nonly first\n"
          "\n.  log on two\n"
          "\n.  display \"both again\"\nboth again\n"
          "\n.  log close two\n"
          "\n.  display \"only first again\"\nonly first again\n"
          "\n.  log on\n"
          "\n.  display \"after\"\nafter\n";
      const std::string expectB =
          "\n.  log using \"" + B + "\", replace text name(two)\n"
          "\n.  display \"both\"\nboth\n"
          "\n.  log on two\n"
          "\n.  display \"both again\"\nboth again\n";
      const std::string expectC =
          "\n.  log using \"" + C + "\", replace\n\n.  display \"ext log\"\next log\n";
      const std::string expectD =
          "\n.  log using \"" + D + "\", replace text\n\n.  display \"noext text\"\nnoext text\n";
      checkGot(slurp8(A) == expectA,
               "BS-006: the unnamed text log is byte-identical to the pre-SMCL transcript", slurp8(A));
      checkGot(slurp8(B) == expectB,
               "BS-006: the named text log (.txt, `text`) is byte-identical to the pre-SMCL "
               "transcript",
               slurp8(B));
      checkGot(slurp8(C) == expectC,
               "BS-006: a .log file with no format option is byte-identical to the pre-SMCL "
               "text transcript",
               slurp8(C));
      checkGot(slurp8(D + ".log") == expectD && !fs::exists(D, ec),
               "BS-006: an extensionless `text` log is the same transcript, now in \"<name>.log\"",
               slurp8(D + ".log"));
    }
  }

  // 9. `log off` suspends writes (stream stays open); `log on` resumes.
  {
    std::string pausePath = workdir + "/pause.log";
    (void)piet::dispatch("log using \"" + pausePath + "\", replace text", session);
    (void)piet::dispatch("display 111222333", session);
    (void)piet::dispatch("log off", session);
    (void)piet::dispatch("display 444555666", session);
    (void)piet::dispatch("log on", session);
    (void)piet::dispatch("display 777888999", session);
    (void)piet::dispatch("log close", session);
    std::ifstream in(cecon::to_native_path(pausePath));
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    check(content.find("111222333") != std::string::npos,
          "BS-006: content written BEFORE `log off` reaches the file");
    check(content.find("444555666") == std::string::npos,
          "BS-006: `log off` suspends writes -- paused-period content is absent");
    check(content.find("777888999") != std::string::npos,
          "BS-006: `log on` resumes writes after a pause");
  }

  fs::remove_all(workdir, ec);
}


// BUG-180 (P2-PREP): `rmdir DIR` — mkdir's teardown twin, which PIET simply
// did not have (no registry row, no dispatch arm), so it fell through to the
// generic unsupported_command fallback. Every expectation below is
// live-probed against StataNow 19.5, not guessed: rc 0 on an empty
// directory; the SAME "could not remove directory" / r(693) for a non-empty
// directory, a missing path, AND a plain file.
void testP2PrepRmdir() {
  std::printf("[selftest] --- BUG-180 `rmdir` removes an empty directory (and refuses the rest) "
              "---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p2prep_rmdir").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // --- the happy path: mkdir then rmdir round-trips -----------------------
  std::string emptyDir = workdir + "/empty_dir";
  (void)piet::dispatch("capture mkdir \"" + emptyDir + "\"", session);
  check(fs::is_directory(emptyDir), "BUG-180 setup: `mkdir` created the directory");
  (void)piet::dispatch("capture rmdir \"" + emptyDir + "\"", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "BUG-180: `rmdir` on an EMPTY directory sets _rc=0 (was unsupported_command, r(199) — "
        "silently swallowed under `capture`)");
  check(!fs::exists(emptyDir), "BUG-180: the directory is really gone from disk");

  // The whole point of the fix: mkdir after rmdir must now succeed, which is
  // exactly what the benchmark harness needed and never got.
  (void)piet::dispatch("capture mkdir \"" + emptyDir + "\"", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "BUG-180: `mkdir` after `rmdir` succeeds — the r(693) the benchmark harness kept "
        "hitting was stale directories, not a broken mkdir");

  // --- non-empty directory: refused, not recursively erased ---------------
  std::string fullDir = workdir + "/full_dir";
  fs::create_directories(fullDir, ec);
  { std::ofstream f(fullDir + "/keep.txt"); f << "x"; }
  (void)piet::dispatch("capture rmdir \"" + fullDir + "\"", session);
  check(session.eval.locals.get("_rc", "") == "693",
        "BUG-180: `rmdir` on a NON-EMPTY directory sets _rc=693 (live-probed)");
  check(fs::exists(fullDir + "/keep.txt"),
        "BUG-180: the refusal is real — rmdir did NOT recursively erase the contents");

  // --- missing path -------------------------------------------------------
  (void)piet::dispatch("capture rmdir \"" + workdir + "/no_such_dir\"", session);
  check(session.eval.locals.get("_rc", "") == "693",
        "BUG-180: `rmdir` on a MISSING path sets _rc=693 (same message/rc as non-empty — "
        "real Stata does not distinguish them)");

  // --- a plain FILE: refused. Without the is_directory() guard,
  // std::filesystem::remove() would have silently deleted it.
  std::string loneFile = workdir + "/notes.txt";
  { std::ofstream f(loneFile); f << "do not delete me"; }
  (void)piet::dispatch("capture rmdir \"" + loneFile + "\"", session);
  check(session.eval.locals.get("_rc", "") == "693",
        "BUG-180: `rmdir` on a plain FILE sets _rc=693");
  check(fs::exists(loneFile),
        "BUG-180: the file SURVIVES — rmdir never deletes a non-directory");

  // --- syntax / options ---------------------------------------------------
  (void)piet::dispatch("capture rmdir", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "BUG-180: bare `rmdir` (no directory name) sets _rc=198");
  (void)piet::dispatch("capture rmdir \"" + fullDir + "\", bogusopt", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "BUG-180: `rmdir DIR, bogusopt` is rejected via the shared option net, _rc=198 "
        "(real Stata says r(101) \"options not allowed\" — a bucket this codebase has "
        "nowhere else; deliberate, reported deviation)");

  fs::remove_all(workdir, ec);
}


// BETA campaign C4 batch: session/other family (BS-042/BS-043/BS-044/
// BS-045/BS-124). Every rc/message pinned below was live-probed against
// StataNow 19.5 (not guessed) — see the C4 agent's task-return report for
// the full probe transcript this batch was built from.

void testC4BS042ConfirmFamily() {
  std::printf("[selftest] --- C4/BS-042 confirm silent-success family (new variable/scalar/"
              "number/names/existence) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c4_bs042").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);
  (void)piet::dispatch("scalar sca1_c4 = 42", session);

  // `confirm new variable <existing>` -> rc=110 (oracle: confirm-new-exists).
  (void)piet::dispatch("capture confirm new variable income", session);
  check(session.eval.locals.get("_rc", "") == "110",
        "C4/BS-042: `confirm new variable income` (income exists) sets _rc=110, was silently "
        "succeeding");

  // `confirm new variable <nonexisting>` -> silent success, rc=0.
  (void)piet::dispatch("capture confirm new variable brandnew_c4", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-042: `confirm new variable <fresh name>` still succeeds (rc=0)");

  // Multi-name form: ALL must be new; reports the FIRST existing conflict
  // (live-probed: `confirm new variable newvar1 x newvar2` with x existing
  // -> "variable x already defined", stopping before newvar2).
  (void)piet::dispatch("capture confirm new variable brandnewA_c4 income brandnewB_c4", session);
  check(session.eval.locals.get("_rc", "") == "110",
        "C4/BS-042: multi-name `confirm new variable A income B` catches the middle conflict");
  (void)piet::dispatch("capture confirm new variable brandnewA_c4 brandnewB_c4", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-042: multi-name `confirm new variable A B` (all fresh) succeeds");

  // `confirm scalar` — live-probed (`confirm scalar notascalar` -> "scalar
  // notascalar not found", rc=111; existing scalar -> rc=0).
  (void)piet::dispatch("capture confirm scalar sca1_c4", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-042: `confirm scalar <existing>` succeeds (was an always-succeed stub before, "
        "still succeeds here, but now for the RIGHT reason)");
  (void)piet::dispatch("capture confirm scalar notascalar_c4", session);
  check(session.eval.locals.get("_rc", "") == "111",
        "C4/BS-042: `confirm scalar <missing>` now sets _rc=111, was silently succeeding");

  // `confirm number` — live-probed (`confirm number abc` -> "'abc' found
  // where number expected", rc=7; any real number, integer or not -> rc=0).
  (void)piet::dispatch("capture confirm number 5.5", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-042: `confirm number 5.5` (non-integer, no `integer` keyword) succeeds");
  (void)piet::dispatch("capture confirm number abc", session);
  check(session.eval.locals.get("_rc", "") == "7",
        "C4/BS-042: `confirm number abc` now sets _rc=7, was silently succeeding");

  // `confirm names`/`confirm name` — live-probed (`confirm names 9def` ->
  // "9def invalid name", rc=7; legal identifier -> rc=0).
  (void)piet::dispatch("capture confirm names goodname_c4", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-042: `confirm names <legal identifier>` succeeds");
  (void)piet::dispatch("capture confirm names 9badname_c4", session);
  check(session.eval.locals.get("_rc", "") == "7",
        "C4/BS-042: `confirm names 9badname` (leading digit) now sets _rc=7, was silently "
        "succeeding");
  (void)piet::dispatch("capture confirm name 9badname_c4", session);
  check(session.eval.locals.get("_rc", "") == "7",
        "C4/BS-042: singular `confirm name` alias behaves identically");

  // `confirm existence` — non-blank text always succeeds (Stata's real
  // semantics here are "is there anything to confirm", not a name-format
  // check — see runConfirm's own doc comment for the honest partial-rc
  // note on the blank-arg corner case, not exercised here).
  (void)piet::dispatch("capture confirm existence something_c4", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-042: `confirm existence <non-blank>` succeeds");

  fs::remove_all(workdir, ec);
}


void testC4BS043ConfirmIntegerNumber() {
  std::printf("[selftest] --- C4/BS-043 `confirm integer number N` misparse fix ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c4_bs043").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // Oracle-pinned (c-oracle-stata.log): `confirm integer number 5` -> rc=0.
  // Before this fix, the "number " keyword leaked into the parsed value
  // ("'number 5' is not an integer value"), erroring on objectively valid
  // Stata code.
  (void)piet::dispatch("capture confirm integer number 5", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-043: `confirm integer number 5` now succeeds (was misparsing 'number 5' as the "
        "value and erroring)");

  // Oracle-pinned: `confirm integer number 5.5` -> rc=7 (non-integer).
  (void)piet::dispatch("capture confirm integer number 5.5", session);
  check(session.eval.locals.get("_rc", "") == "7",
        "C4/BS-043: `confirm integer number 5.5` correctly rejects (rc=7), value parsed as 5.5 "
        "not 'number 5.5'");

  fs::remove_all(workdir, ec);
}


void testC4BS045RmMkdir() {
  std::printf("[selftest] --- C4/BS-045 `rm` + mkdir option cleanup ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c4_bs045").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  std::string freshDir = workdir + "/fresh_dir_c4";
  (void)piet::dispatch("capture mkdir \"" + freshDir + "\"", session);
  check(session.eval.locals.get("_rc", "") == "0", "C4/BS-045: `mkdir` on a fresh path succeeds");
  check(fs::is_directory(freshDir), "C4/BS-045: the directory really exists on disk now");

  // Oracle-pinned: mkdir-again -> rc=693 ("could not create directory").
  (void)piet::dispatch("capture mkdir \"" + freshDir + "\"", session);
  check(session.eval.locals.get("_rc", "") == "693",
        "C4/BS-045: re-`mkdir`'ing the SAME directory sets _rc=693 (oracle-pinned)");

  // Live-probed: missing parent -> the SAME rc=693, not a silent `mkdir -p`
  // bridge (the old invented `,p` behavior).
  (void)piet::dispatch(
      "capture mkdir \"" + workdir + "/no_such_parent_c4/child_dir\"", session);
  check(session.eval.locals.get("_rc", "") == "693",
        "C4/BS-045: `mkdir` under a MISSING parent also sets _rc=693 (real Stata has no "
        "parents-creation option; the old invented `,p` silently bridged this)");

  // `,p` is no longer a recognized option (it was invented) -> unknown-
  // option reject via the shared net, rc=198.
  std::string pOptDir = workdir + "/p_opt_dir_c4";
  (void)piet::dispatch("capture mkdir \"" + pOptDir + "\", p", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "C4/BS-045: `mkdir DIR, p` now rejects (`,p` was an invented option, not real Stata)");

  // `,public` IS a real, honored option.
  std::string pubDir = workdir + "/pub_dir_c4";
  (void)piet::dispatch("capture mkdir \"" + pubDir + "\", public", session);
  check(session.eval.locals.get("_rc", "") == "0", "C4/BS-045: `mkdir DIR, public` succeeds");
  check(fs::is_directory(pubDir), "C4/BS-045: the `,public` directory really exists");

  // Unknown option (anything else) -> rc=198 via the shared net.
  std::string bogusDir = workdir + "/bogus_opt_dir_c4";
  (void)piet::dispatch("capture mkdir \"" + bogusDir + "\", bogusopt", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "C4/BS-045: `mkdir DIR, bogusopt` rejects an unrecognized option (rc=198)");

  // `rm` — oracle-pinned: nonexistent file -> rc=601, "file X not found".
  std::string missingFile = workdir + "/definitely_not_here_c4.txt";
  auto rmBlocks = piet::dispatch("capture rm \"" + missingFile + "\"", session);
  check(session.eval.locals.get("_rc", "") == "601",
        "C4/BS-045: `rm <nonexistent>` sets _rc=601 (oracle-pinned), was `unsupported_command` "
        "before this fix");

  // `rm` on a real file actually deletes it (rm was entirely unrecognized
  // before this fix — bs045b probe).
  std::string realFile = workdir + "/real_file_c4.txt";
  {
    std::ofstream f(cecon::to_native_path(realFile));
    f << "hello\n";
  }
  check(fs::exists(realFile), "C4/BS-045 setup: the test file exists before `rm`");
  (void)piet::dispatch("capture rm \"" + realFile + "\"", session);
  check(session.eval.locals.get("_rc", "") == "0", "C4/BS-045: `rm <existing file>` succeeds");
  check(!fs::exists(realFile), "C4/BS-045: `rm` really deleted the file");

  // `erase` (the pre-existing spelling) still works unchanged.
  std::string eraseFile = workdir + "/erase_file_c4.txt";
  {
    std::ofstream f(cecon::to_native_path(eraseFile));
    f << "hello\n";
  }
  (void)piet::dispatch("capture erase \"" + eraseFile + "\"", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "C4/BS-045 regression guard: `erase` (pre-existing spelling) still succeeds");
  check(!fs::exists(eraseFile), "C4/BS-045 regression guard: `erase` still really deletes");

  fs::remove_all(workdir, ec);
}


// W:P8. `export delimited` cell spellings, each pinned to Stata's own export of
// the same values (StataBE 19.5, live-probed).
void testP8ExportDelimitedSpelling() {
  std::printf("[selftest] --- W:P8 export delimited cell spellings ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p8_csv").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  auto run = [&](const std::string& cmd) { return piet::dispatch(cmd, session); };
  run("clear");
  run("set obs 3");
  run("generate double n = _n");                 // 1, 2, 3 -> integral
  run("generate double frac = 1/2");             // .5 -> no leading zero
  run("generate double miss = .");               // system missing -> EMPTY
  run("replace miss = .a in 2");                 // extended missing -> `.a`
  run("generate double d = 14624 + _n");         // %td -> 16jan2000, ...
  run("format d %td");

  const std::string csv = workdir + "/out.csv";
  run("export delimited using \"" + csv + "\", replace");
  std::ifstream f(cecon::to_native_path(csv));
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  check(lines.size() == 4, "W:P8 csv: header + 3 data rows");
  if (lines.size() != 4) return;
  check(lines[0] == "n,frac,miss,d", "W:P8 csv: header names");
  check(lines[1] == "1,.5,,16jan2000",
        "W:P8 csv row 1: integral `1` (not `1.0`), `.5` (no leading zero), system missing EMPTY "
        "(not `.`), and the %td column as a formatted date (not 14625)");
  check(lines[2] == "2,.5,.a,17jan2000",
        "W:P8 csv row 2: an EXTENDED missing keeps its `.a` code");
  check(lines[3] == "3,.5,,18jan2000", "W:P8 csv row 3");
}
