// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: language control.
#include "selftest_support.h"
#include "selftest_cases.h"


void testTempfileReapedOnProgramReturnBug360() {
  std::printf("[selftest] --- BUG-360 tempfile lifetime = the program ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug360").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  // Driven as ONE do-file source, not as a sequence of single dispatch() calls:
  // `program define ... end` is captured pre-tokenizer by doexec's
  // captureProgams over a WHOLE file's lines (dispatcher.cpp says so, and the
  // BS-044 leg above repeats it). Fed line-by-line, the body EXECUTES at define
  // time instead of being captured — which is exactly what the preconditions
  // below caught on the first draft of this leg.
  //
  // And note `export delimited`, not `save`. A `save` to a MINTED tempfile is
  // routed to the engine's in-RAM virtual-file table when the resident plane is
  // live (fileio/dataset_save.cpp's isMintedTempfile arm) and writes NO bytes,
  // so a disk-existence assertion against it would pass whether or not this fix
  // works — a false green. `export delimited` always writes real bytes.
  // (`confirm file` is no help either: it consults the filesystem and is blind
  // to the virtual table — BUG-361.)
  const std::string src =
      "clear\n"
      "set obs 5\n"
      "gen x = _n\n"
      "program define b360\n"
      "  tempfile t\n"
      "  export delimited using \"`t'\", replace\n"
      "  global B360PATH \"`t'\"\n"
      "end\n"
      "b360\n";
  const long seqBefore = session.tempSeq;
  (void)piet::doexec::executeDoSource(src, session, piet::dispatch);
  const long seqAfter = session.tempSeq;
  const std::string path = session.eval.globals.get("B360PATH", "");

  // PRECONDITIONS — without these the leg asserts the absence of a file that
  // never existed, which is how the first draft passed for the wrong reason.
  check(seqAfter > seqBefore,
        "BUG-360 precondition: the program actually minted a tempfile (tempSeq advanced)");
  check(!path.empty(),
        "BUG-360 precondition: the program published the minted path");

  check(!path.empty() && !fs::exists(cecon::to_native_path(path)),
        "BUG-360: the tempfile is ERASED when the program returns (Stata's lifetime)");
  bool anyProgDepthLeft = false;
  for (const auto& mt : session.mintedTempfiles)
    if (mt.first > 0) anyProgDepthLeft = true;
  check(!anyProgDepthLeft,
        "BUG-360: no program-depth entry survives in the minted-tempfile ledger");

  // A do-file-level tempfile is NOT the program's to take — its lifetime is the
  // run, still owned by doexec's TempdirReapGuard.
  piet::SessionState session2(workdir);
  const std::string src2 =
      "clear\n"
      "set obs 5\n"
      "gen x = _n\n"
      "program define b360b\n"
      "  tempfile inner\n"
      "  export delimited using \"`inner'\", replace\n"
      "end\n"
      "tempfile outer\n"
      "export delimited using \"`outer'\", replace\n"
      "global B360OUTER \"`outer'\"\n"
      "b360b\n";
  (void)piet::doexec::executeDoSource(src2, session2, piet::dispatch);
  const std::string outerPath = session2.eval.globals.get("B360OUTER", "");
  check(!outerPath.empty(),
        "BUG-360 precondition: the do-file-level tempfile published its path");
  check(!outerPath.empty() && fs::exists(cecon::to_native_path(outerPath)),
        "BUG-360: a do-file-level tempfile SURVIVES an unrelated program's return");

  fs::remove_all(workdir, ec);
}


void testBS020EststoColonPrefix() {
  std::printf("[selftest] --- BS-020 `eststo NAME: command` prefix re-dispatch ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs020").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  // A stale prior model in memory — the fix must NOT store this one.
  (void)piet::dispatch("regress fatal beertax", session);

  auto blocks = piet::dispatch("eststo mBS020: regress fatal spirits", session);

  bool sawEstimation = false;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "estimation") sawEstimation = true;
  }
  check(sawEstimation,
        "BS-020: `eststo mBS020: regress fatal spirits` runs the nested regress "
        "(today the nested command never executes)");

  const auto* stored = session.estimatesGet("mBS020");
  check(stored != nullptr, "BS-020: stored under the bare name `mBS020` (colon correctly stripped)");
  check(session.estimatesGet("mBS020:") == nullptr,
        "BS-020: NOT stored under the corrupted `mBS020:` key");

  if (stored != nullptr) {
    bool hasSpirits = false;
    bool hasBeertax = false;
    for (const auto& row : stored->value("coeftable", nlohmann::ordered_json::array())) {
      std::string nm = row.value("name", std::string());
      if (nm == "spirits") hasSpirits = true;
      if (nm == "beertax") hasBeertax = true;
    }
    check(hasSpirits && !hasBeertax,
          "BS-020: the stored snapshot reflects the FRESH `regress fatal spirits`, "
          "not the stale `regress fatal beertax`");
  }

  fs::remove_all(workdir, ec);
}


void testBS040QuietlyNoisilyInnerWins() {
  std::printf("[selftest] --- BS-040 quietly/noisily inner-wins nesting ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs040").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto sawDisplayBlock = [](const nlohmann::ordered_json& blocks) {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") return true;
    }
    return false;
  };

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);

  check(sawDisplayBlock(piet::dispatch("quietly noisily count", session)),
        "BS-040: `quietly noisily count` shows output (inner noisily wins)");
  check(!sawDisplayBlock(piet::dispatch("quietly count", session)),
        "BS-040 regression guard: plain `quietly count` (no nesting) still hides its output");
  check(sawDisplayBlock(piet::dispatch("noisily count", session)),
        "BS-040 regression guard: plain `noisily count` still shows its output");
  check(!sawDisplayBlock(piet::dispatch("noisily quietly count", session)),
        "BS-040: `noisily quietly count` still hides output (inner quietly wins)");

  fs::remove_all(workdir, ec);
}


void testBS014ProgramBareFormsPrepass() {
  std::printf("[selftest] --- BS-014 `program dir`/`program list`/bare `program drop` pre-pass ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs014").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // `program dir` / bare `program drop NAME` ARE handled at dispatch time
  // (dispatcher.cpp's own pre-pass) — with BS-014 fixed, the rest of the
  // do-file must run normally after them instead of being silently eaten.
  // BUG-923: the drop of an absent program is r(111) (Stata), so it is
  // captured here; the arm is about the capture pass not eating the file.
  for (const std::string& bareLine :
       {std::string("capture program drop nosuchprogram014"), std::string("program dir")}) {
    piet::SessionState session(workdir);
    std::string src = bareLine + "\nsysuse fatalities, clear\ngen bs014mark = 1\n";
    (void)piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(session.hasData() && session.datastore()->has("bs014mark"),
          "BS-014: `" + bareLine + "` no longer swallows the rest of the do-file as a bogus "
          "program body (sysuse+gen after it still ran)");
  }

  // `program list` has NO dispatcher handler (an honest gap) — with BS-014
  // fixed, it reaches dispatch and is honestly rejected right there, instead
  // of the capture-pass silently eating the rest of the file with NO error
  // at all and no later line ever getting a chance to run.
  {
    piet::SessionState session(workdir);
    std::string src = std::string("program list") + "\nsysuse fatalities, clear\ngen bs014mark2 = 1\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(result.stoppedAtReason.has_value(),
          "BS-014: `program list` reaches dispatch and honestly halts there "
          "(not a silent no-op over the whole rest of the file)");
    check(session.programs.find("list") == session.programs.end(),
          "BS-014: `program list` was NOT captured as a bogus program named `list`");
  }

  fs::remove_all(workdir, ec);
}


void testBS015CaptureMultilineBlock() {
  std::printf("[selftest] --- BS-015 `capture { ... }` multi-line block ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs015").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Success case: every line in the block runs, in order, and _rc == 0.
  {
    piet::SessionState session(workdir);
    std::string src =
        "capture {\n"
        "    sysuse fatalities, clear\n"
        "    gen bs015a = 1\n"
        "    gen bs015b = 2\n"
        "}\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(session.hasData() && session.datastore()->has("bs015a") && session.datastore()->has("bs015b"),
          "BS-015: every line inside `capture { ... }` actually ran (today: runs uncaptured, "
          "which happens to still work for an all-success body, but see the failure case below)");
    check(session.eval.locals.get("_rc", "") == "0",
          "BS-015: `capture { ... }` (fully successful body) sets _rc=0");
    check(!result.stoppedAtReason.has_value(),
          "BS-015: a successful `capture { ... }` block does not halt the enclosing do-file");
  }

  // Failure case: a bad line inside the block is swallowed (does NOT halt
  // the do-file), _rc == 111, lines after the failure inside the SAME block
  // do not run, and a line AFTER the closing brace still runs.
  // Mechanism (BUG-546 routes capture-block errors through eval::coarseRc):
  // the 111 comes from errors.cpp's "variable" + "not found" substring rule
  // matching the engine message `column not found: nosuchvariable_bs015`
  // ONLY because this identifier contains the word "variable". The engine's
  // generic `column not found: X` still maps to rc 1 (pinned by BS-100 and
  // the C1/N3 guard below) -- a REFUSAL-rc gap, not fixed here.
  {
    piet::SessionState session(workdir);
    std::string src =
        "capture {\n"
        "    sysuse fatalities, clear\n"
        "    regress nosuchvariable_bs015 spirits\n"
        "    gen bs015_never_reached = 1\n"
        "}\n"
        "gen bs015_after_block = 1\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(session.eval.locals.get("_rc", "") == "111",
          "BS-015: an error inside `capture { ... }` sets _rc=111 (variable not found; oracle "
          "w2_openbugs fe_unknown / bug483_484_control_oracle C1)");
    check(session.hasData(), "BS-015: lines BEFORE the failing line inside the block still ran");
    check(session.hasData() && !session.datastore()->has("bs015_never_reached"),
          "BS-015: lines AFTER the failing line inside the SAME block did not run");
    check(session.hasData() && session.datastore()->has("bs015_after_block"),
          "BS-015: the do-file CONTINUES past the capture block after it swallows the error "
          "(today, this error incorrectly runs uncaptured and halts the whole do-file instead)");
    check(!result.stoppedAtReason.has_value(),
          "BS-015: the enclosing do-file did not halt because of the captured error");
  }

  fs::remove_all(workdir, ec);
}


void testBUG116NestedCaptureBlock() {
  std::printf("[selftest] --- BUG-116 nested `capture` inside `capture { ... }` ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug116").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // A NESTED `capture` failing inside an outer `capture { ... }` block must
  // NOT be treated as an error escaping to the outer scope: the dispatcher
  // already tagged that error captured:true (BS-127, display-only), so the
  // outer block's own scan (runCaptureBlock) must key off the SAME halt
  // authority runBody/doexec use (stoppedAtLine), not a bare "any
  // type==error block" scan — the pre-fix bug. Correct behavior: the outer
  // block keeps running past the nested capture's line, and outer _rc stays
  // "0" (nothing actually escaped to this scope). BUG-1434: a bare capture
  // block prints none of its ordinary output (StataBE 19.5), so the marker is
  // a `noisily display`, which StataBE does print inside one.
  piet::SessionState session(workdir);
  std::string src =
      "capture {\n"
      "    sysuse fatalities, clear\n"
      "    capture noisily regress nosuchvariable_bug116 spirits\n"
      "    noisily display \"bug116_after_nested_marker\"\n"
      "}\n"
      "display \"bug116_after_block_marker\"\n";
  auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

  check(session.eval.locals.get("_rc", "") == "0",
        "BUG-116: outer `capture { ... }` block's _rc stays \"0\" — an already-captured NESTED "
        "capture's error must not pollute the outer scope's _rc, got _rc=" +
            session.eval.locals.get("_rc", "<unset>"));

  bool sawAfterNested = false;
  bool sawAfterBlock = false;
  for (const auto& b : result.blocks) {
    if (!b.is_object() || b.value("type", std::string()) != "display") continue;
    const std::string text = b.value("text", std::string());
    if (text == "bug116_after_nested_marker") sawAfterNested = true;
    if (text == "bug116_after_block_marker") sawAfterBlock = true;
  }
  check(sawAfterNested,
        "BUG-116: the line AFTER the nested capture, inside the SAME outer capture block, still "
        "ran and its output reached the transcript (pre-fix: the outer block wrongly halted "
        "right at the nested capture's already-caught error)");
  check(sawAfterBlock,
        "BUG-116: the do-file continues normally past the whole outer capture block");
  check(!result.stoppedAtReason.has_value(),
        "BUG-116: the do-file never halted — the nested capture's error was genuinely caught, "
        "twice over");

  fs::remove_all(workdir, ec);
}


void testBUG116NestedCaptureSingleLine() {
  std::printf("[selftest] --- BUG-116 class sweep: nested single-line `capture capture ...` ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug116_singleline").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // Oracle (StataBE 19.5, live-verified this session): `capture capture
  // noisily regress mpg nosuchvar` leaves _rc==0 — the wrapped statement is
  // itself a `capture`, which always completes without an error escaping
  // it (that is capture's entire point), regardless of what _rc the INNER
  // capture set as a side effect. dispatcher.cpp's Pre-0 `capture` arm had
  // the same un-guarded "any error block" scan as BUG-116's runCaptureBlock
  // — fixed by the same !captured guard.
  (void)piet::dispatch("capture capture noisily regress fatal nosuchvar_bug116", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "BUG-116 class: `capture capture noisily <failing cmd>` (single-line double capture) "
        "leaves outer _rc==0 (oracle: StataBE 19.5), got _rc=" +
            session.eval.locals.get("_rc", "<unset>"));

  // Same oracle-confirmed result holds when the OUTER capture is itself
  // noisily-rooted (noisily only affects display, not rc capture).
  (void)piet::dispatch("capture noisily capture noisily regress fatal nosuchvar_bug116", session);
  check(session.eval.locals.get("_rc", "") == "0",
        "BUG-116 class: `capture noisily capture noisily <failing cmd>` also leaves outer "
        "_rc==0 (oracle: StataBE 19.5), got _rc=" +
            session.eval.locals.get("_rc", "<unset>"));

  fs::remove_all(workdir, ec);
}


void testBUG132QuietlyNoisilyBlock() {
  std::printf("[selftest] --- BUG-132 `quietly { ... }` / `noisily { ... }` block syntax ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug132").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Pre-fix (BUG-132), `quietly {`/`noisily {` were never in loopIfKeywords,
  // so aggregateBraces never recognised the block: the bare `{` dispatched as
  // an unknown command → "unsupported command" halt at the opening brace, body
  // 0 lines run. These legs lock the fixed behaviour: block parses, body runs,
  // quietly suppresses / noisily shows, and (unlike capture) an uncaught error
  // still HALTS.
  auto sawDisplay = [](const auto& blocks, const std::string& text) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display" &&
          b.value("text", std::string()) == text)
        return true;
    return false;
  };
  auto sawSummarizeOf = [](const auto& blocks, const std::string& var) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "summarize" && b.contains("stats") &&
          b["stats"].is_object() && b["stats"].contains(var))
        return true;
    return false;
  };
  auto sawUncapturedError = [](const auto& blocks) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error" &&
          !b.value("captured", false))
        return true;
    return false;
  };

  // --- Case 1: quietly { ... } with a CLEAN body: body RUNS, output HIDDEN.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "quietly {\n"
        "    summarize spirits\n"
        "    gen bug132qmark = year\n"
        "    display \"bug132_quietly_hidden\"\n"
        "}\n"
        "summarize bug132qmark\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

    check(!result.stoppedAtReason.has_value(),
          "BUG-132: a clean `quietly { ... }` block does NOT halt (pre-fix it died on the bare "
          "`{`), stop=" + result.stoppedAtReason.value_or("<none>"));
    check(!sawDisplay(result.blocks, "bug132_quietly_hidden"),
          "BUG-132: `display` INSIDE the quietly block is suppressed");
    check(!sawSummarizeOf(result.blocks, "spirits"),
          "BUG-132: `summarize spirits` INSIDE the quietly block is suppressed");
    check(sawSummarizeOf(result.blocks, "bug132qmark"),
          "BUG-132: the body actually RAN — `gen bug132qmark` inside the quietly block created the "
          "variable, proven by the post-block `summarize bug132qmark` (outside quietly) showing it");
  }

  // --- Case 2: quietly { ... } with an ERROR: HALTS (quietly != capture).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "quietly {\n"
        "    summarize nosuchvar_bug132\n"
        "    display \"bug132_after_error_in_block\"\n"
        "}\n"
        "display \"bug132_after_error_block\"\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

    check(result.stoppedAtReason.has_value(),
          "BUG-132: an uncaught error inside `quietly { ... }` HALTS the do-file (quietly is "
          "output-only, it does NOT catch errors the way capture does)");
    check(sawUncapturedError(result.blocks),
          "BUG-132: the error message is still surfaced under quietly (quietly suppresses normal "
          "output, not errors)");
    check(!sawDisplay(result.blocks, "bug132_after_error_in_block"),
          "BUG-132: the line AFTER the error, inside the block, never ran (block halted)");
    check(!sawDisplay(result.blocks, "bug132_after_error_block"),
          "BUG-132: the do-file halted — nothing past the whole quietly block ran");
  }

  // --- Case 3: noisily { ... } with a CLEAN body: output SHOWN (transparent).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "noisily {\n"
        "    summarize spirits\n"
        "    display \"bug132_noisily_shown\"\n"
        "}\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

    check(!result.stoppedAtReason.has_value(), "BUG-132: a clean `noisily { ... }` block does NOT halt");
    check(sawSummarizeOf(result.blocks, "spirits"),
          "BUG-132: `summarize spirits` inside `noisily { ... }` IS shown (noisily is transparent)");
    check(sawDisplay(result.blocks, "bug132_noisily_shown"),
          "BUG-132: `display` inside `noisily { ... }` IS shown");
  }

  // --- Case 4: noisily { ... } with an ERROR: also HALTS (class parity).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "noisily {\n"
        "    summarize nosuchvar_bug132b\n"
        "    display \"bug132_noisily_after_error\"\n"
        "}\n"
        "display \"bug132_noisily_after_block\"\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

    check(result.stoppedAtReason.has_value(),
          "BUG-132: an uncaught error inside `noisily { ... }` also HALTS the do-file");
    check(!sawDisplay(result.blocks, "bug132_noisily_after_error"),
          "BUG-132: the line after the error inside the noisily block never ran");
    check(!sawDisplay(result.blocks, "bug132_noisily_after_block"),
          "BUG-132: the do-file halted past the noisily block");
  }

  // --- Case 5: abbreviation routing (A11 class sweep) — `qui {` / `noi {`.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "qui {\n"
        "    gen bug132abbrev = 1\n"
        "    summarize spirits\n"
        "}\n"
        "noi {\n"
        "    summarize spirits\n"
        "}\n"
        "summarize bug132abbrev\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

    check(!result.stoppedAtReason.has_value(),
          "BUG-132 A11: abbreviated `qui {`/`noi {` blocks parse + run (no halt on `{`)");
    check(sawSummarizeOf(result.blocks, "spirits"),
          "BUG-132 A11: `noi { summarize spirits }` shows its output (abbrev routes to noisily)");
    check(sawSummarizeOf(result.blocks, "bug132abbrev"),
          "BUG-132 A11: `gen` inside `qui { ... }` ran (abbrev routes to quietly, body executed)");
  }

  fs::remove_all(workdir, ec);
}


// BUG-167: a brace INSIDE a prefixed command's arguments was read as a block
// opener. `capture noisily nl (y = {b0=2}+{b1=.5}*x1)` has `capture` first and
// a `{` later, which is all the old predicate
// (`isLoopHeader(line) && line.find('{') != npos`) required — so the aggregator
// built a block headed `capture noisily nl (y = {`, ran `b0=2` as its body, and
// DROPPED `+{b1=.5}*x1)` entirely. The command the user typed never reached the
// dispatcher: no "nl is unsupported" error, `_rc` set to 1 by an unrelated
// statement. A command silently replaced by a fragment of its own arguments is
// the silent-wrong class, so these legs are the RED certificate for the fix
// (each Case-1 leg FAILS against the pre-fix predicate).
void testBUG167BraceInsidePrefixedArgs() {
  std::printf("[selftest] --- BUG-167 brace inside a prefixed command's arguments ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug167").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto sawEchoContaining = [](const auto& blocks, const std::string& needle) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "echo" &&
          b.value("text", std::string()).find(needle) != std::string::npos)
        return true;
    return false;
  };
  auto sawDisplay = [](const auto& blocks, const std::string& text) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display" &&
          b.value("text", std::string()) == text)
        return true;
    return false;
  };

  // --- Case 1: the defect itself. The braces belong to nl's parameter syntax.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    // Uses an UNIMPLEMENTED command deliberately. The defect was found with
    // `nl`, but `nl` landed later in this same wave, and pinning the rc to 199
    // against a command that now works would make this leg assert the
    // implementation calendar instead of the parse. The invariant under test is
    // the brace handling, which is command-independent.
    std::string src =
        "capture noisily nosuchverb_bug167 (y = {b0=2}+{b1=.5}*spirits)\n"
        "display \"bug167_rc=\" _rc\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

    check(!sawEchoContaining(result.blocks, ".  b0=2"),
          "BUG-167: `b0=2` is NOT executed as a statement of its own (pre-fix it was the block "
          "body)");
    check(sawEchoContaining(result.blocks, "nosuchverb_bug167 (y ="),
          "BUG-167: the whole braced line reaches the dispatcher intact (pre-fix it was consumed "
          "as a block header and never dispatched)");
    check(sawDisplay(result.blocks, "bug167_rc=199"),
          "BUG-167: the captured rc is the honest unsupported-command 199 (pre-fix: 1, set by the "
          "smuggled `b0=2`)");
  }

  // --- Case 2: genuine inline blocks still parse (the fix must not overreach).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        // BUG-1434: a bare capture block hides ordinary output; its noisily
        // line shows (StataBE 19.5), which is what proves the block ran.
        "capture { noisily display \"bug167_capture_inline\" }\n"
        "quietly { display \"bug167_quietly_inline\" }\n"
        "capture noisily { display \"bug167_capnoi_inline\" }\n"
        "forvalues i=1/3 { display \"bug167_loop\" }\n"
        "if 1==1 { display \"bug167_if_inline\" }\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);

    check(!result.stoppedAtReason.has_value(),
          "BUG-167: the legal inline brace forms still parse, stop=" +
              result.stoppedAtReason.value_or("<none>"));
    check(sawDisplay(result.blocks, "bug167_capture_inline"), "BUG-167: `capture { ... }` inline");
    check(!sawDisplay(result.blocks, "bug167_quietly_inline"),
          "BUG-167: `quietly { ... }` inline still suppresses");
    check(sawDisplay(result.blocks, "bug167_capnoi_inline"),
          "BUG-167: `capture noisily { ... }` inline (multi-keyword prefix header)");
    check(sawDisplay(result.blocks, "bug167_loop"), "BUG-167: `forvalues ... { ... }` inline");
    check(sawDisplay(result.blocks, "bug167_if_inline"), "BUG-167: `if COND { ... }` inline");
  }

  // --- Case 3: the control class keeps its permissive rule — a `{` after a
  // list/condition still opens a block even though the header is not a bare
  // keyword. This is the asymmetry the fix deliberately encodes.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src = "foreach v in spirits beertax { display \"bug167_foreach\" }\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(sawDisplay(result.blocks, "bug167_foreach"),
          "BUG-167: `foreach v in ... { ... }` — header carries the list, brace still opens");
  }

  fs::remove_all(workdir, ec);
}


// BUG-154: a taken `exit [N]` inside a brace block was silently IGNORED — the
// inner executeDoSource consumed AND ERASED the clean-exit halt sentinel, so
// nothing bubbled up to re-halt the outer level: `if (cond) { exit 7 }` fell
// through and every statement after the block still ran (failure guards were
// dead — silent-wrong class). The fix re-emits the sentinel from runBody and
// carries the exact code to capture's _rc. Oracle for every leg: StataBE 19.5
// (capture{exit 5} → _rc 5, verified live 2026-08-04).
void testBUG154ExitInBlock() {
  std::printf("[selftest] --- BUG-154 `exit` inside brace blocks halts the do-file ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug154").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto sawDisplay = [](const auto& blocks, const std::string& text) {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display" &&
          b.value("text", std::string()) == text)
        return true;
    return false;
  };

  // --- ① RED-cert arm (the pre-fix silent-wrong): taken guard HALTS with the
  //        requested code and nothing after the block runs. Pre-fix this arm
  //        fails exactly here: "bug154_after" printed and no exit code was set.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "local rc = 7\n"
        "if `rc' != 0 {\n"
        "    exit `rc'\n"
        "}\n"
        "display \"bug154_after\"\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(result.stoppedAtExitCode.has_value() && *result.stoppedAtExitCode == 7,
          "BUG-154 ①: taken `if { exit 7 }` halts the do-file with code 7, got " +
              (result.stoppedAtExitCode.has_value() ? std::to_string(*result.stoppedAtExitCode)
                                                    : std::string("<none>")));
    check(!sawDisplay(result.blocks, "bug154_after"),
          "BUG-154 ①: nothing after a taken exit-guard runs (pre-fix it printed)");
  }

  // --- ② false guard: body SKIPPED, execution continues past the block.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "if 0 != 0 {\n"
        "    exit 9\n"
        "}\n"
        "display \"bug154_false_ok\"\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!result.stoppedAtExitCode.has_value() && !result.stoppedAtReason.has_value(),
          "BUG-154 ②: false guard does not halt");
    check(sawDisplay(result.blocks, "bug154_false_ok"),
          "BUG-154 ②: execution continues after a false guard");
  }

  // --- ③ nested if-in-if: the halt propagates through EVERY level.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "if 1 {\n"
        "    if 2 > 1 {\n"
        "        exit 5\n"
        "    }\n"
        "    display \"bug154_inner_after\"\n"
        "}\n"
        "display \"bug154_outer_after\"\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(result.stoppedAtExitCode.has_value() && *result.stoppedAtExitCode == 5,
          "BUG-154 ③: nested `exit 5` halts with code 5 through both levels");
    check(!sawDisplay(result.blocks, "bug154_inner_after") &&
              !sawDisplay(result.blocks, "bug154_outer_after"),
          "BUG-154 ③: neither inner-after nor outer-after ran");
  }

  // --- ④ foreach body: `exit` stops the WHOLE file mid-iteration.
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "local n = 0\n"
        "foreach k in 1 2 3 {\n"
        "    if `k' == 2 {\n"
        "        exit 3\n"
        "    }\n"
        "    local n = `n' + 1\n"
        "}\n"
        "display \"bug154_post_loop\"\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(result.stoppedAtExitCode.has_value() && *result.stoppedAtExitCode == 3,
          "BUG-154 ④: exit inside a foreach body halts the file with code 3");
    check(!sawDisplay(result.blocks, "bug154_post_loop"),
          "BUG-154 ④: nothing after the loop runs");
    check(session.eval.locals.get("n", "") == "1",
          "BUG-154 ④: iteration 2 stopped mid-body (n stayed 1), got n=" +
              session.eval.locals.get("n", "<unset>"));
  }

  // --- ⑤ capture traps a clean exit with its EXACT code and continues
  //        (StataBE 19.5: `capture { exit 5 }` → _rc 5, file continues).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    std::string src =
        "capture {\n"
        "    exit 5\n"
        "}\n"
        "display \"bug154_continued\"\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!result.stoppedAtExitCode.has_value() && !result.stoppedAtReason.has_value(),
          "BUG-154 ⑤: capture traps the exit — file does not halt");
    check(session.eval.locals.get("_rc", "") == "5",
          "BUG-154 ⑤: _rc carries the exact exit code 5 (oracle: StataBE 19.5), got _rc=" +
              session.eval.locals.get("_rc", "<unset>"));
    check(sawDisplay(result.blocks, "bug154_continued"),
          "BUG-154 ⑤: execution continues after the captured block");
  }

  fs::remove_all(workdir, ec);
}


void testBS097OptionNetAndBS018DoNostop() {
  std::printf(
      "[selftest] --- BS-097 option consume-or-reject net + BS-018 do/run nostop ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bs097").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto firstBlockOfType = [](const nlohmann::ordered_json& blocks,
                             const std::string& type) -> nlohmann::ordered_json {
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == type) return b;
    }
    return nlohmann::ordered_json();
  };
  auto isUnknownOptionReject = [&](const nlohmann::ordered_json& blocks, const std::string& token) {
    auto err = firstBlockOfType(blocks, "error");
    return !err.is_null() && err.value("code", std::string()) == "unsupported_option" &&
           err.value("message", std::string()).find(token) != std::string::npos;
  };

  piet::SessionState session(workdir);
  auto setup1 = piet::dispatch("sysuse fatalities, clear", session);
  check(firstBlockOfType(setup1, "error").is_null(), "BS-097 setup: `sysuse fatalities, clear` succeeds");
  auto setup2 = piet::dispatch("regress fatal beertax spirits", session);
  check(firstBlockOfType(setup2, "error").is_null(),
        "BS-097 setup: `regress fatal beertax spirits` succeeds (seeds lastModel for "
        "margins/test/lincom/nlcom/estat below)");

  // margins — the B6 headline (previously ZERO net).
  {
    auto rej = piet::dispatch("margins, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"),
          "BS-097 margins: `margins, bogusopt` honestly rejects, naming `bogusopt`");
    auto ok = piet::dispatch("margins, at(beertax=0.5)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 margins over-rejection guard: `margins, at(beertax=0.5)` "
          "(a valid option in the net's allow-list) still succeeds — no over-rejection");
  }

  // AER-B4 margins-dydx-OLS — `margins, dydx(x)` after a linear model.
  // This is the arm BUG-479 is about, restored WITH the fix its entry asks for:
  // the abort it records was this lambda reading `.value("blocks", ...)` off a
  // block ARRAY, in a draft that had no catch yet — not a throw crossing a
  // `noexcept` boundary inside dispatch. With the catch in place the same throw
  // is CAUGHT and reported (measured: `FAIL ... threw instead of reporting:
  // [json.exception.type_error.306] cannot use value() with array`), which is the
  // refutation of that theory. The second half of the fix is the arm's own
  // session, below. Until 2026-09-09 this
  // combination was refused outright (the NB that used to sit above called it
  // a known Stata-divergence); the oracle
  // tests/eval/golden/aer_b4/pins_oracle_r.do:75-79 says Stata answers it
  // rc 0, with dy/dx, std. err., t, P>|t| and the CI ALL equal to the fitted
  // regression's own row for that variable — a linear model's dy/dx IS its
  // coefficient.
  //
  // That identity is the pin, and it is the discriminating one: cobra's
  // cmd_margins AME path has no OLS branch and defaults to `--model logit`
  // (apps/cmds/cmds_postest.cpp:929), so the failure mode this closure had to
  // avoid is a LOGISTIC average marginal effect silently standing in for the
  // coefficient — a number that differs by orders of magnitude, not by
  // rounding. Asserted at EXACT equality (not a tolerance): estimation_m4c's
  // buildLinearAmeRawM4 copies the fitted row, it does not recompute it, so
  // any future re-route through the engine breaks this bit-for-bit.
  try {
    // Its OWN session and scratch dir, not the shared `session` above. The LPM
    // leg has to FIT a second model, and lastModel/currentHbin are exactly what
    // every arm BELOW (test/lincom/nlcom/estat) replays — measured: sharing the
    // session turned three of those RED for reasons that had nothing to do with
    // what they test, and re-seeding the original fit afterwards did NOT
    // reliably put it back. An arm that needs different state gets its own.
    const std::string wd478 = (fs::path(workdir) / "bug478").string();
    std::error_code ec478;
    fs::create_directories(wd478, ec478);
    piet::SessionState session(wd478);
    (void)piet::dispatch("sysuse fatalities, clear", session);
    auto setup2 = piet::dispatch("regress fatal beertax spirits", session);
    // NB: `piet::dispatch` hands this scope the BLOCK ARRAY itself, not a
    // {"blocks": [...]} envelope — same shape firstBlockOfType above iterates.
    auto coefRow = [](const nlohmann::ordered_json& blocks, const char* want) {
      for (const auto& b : blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
        for (const auto& row : b.value("coeftable", nlohmann::ordered_json::array()))
          if (row.is_object() && row.value("name", std::string()) == want) return row;
      }
      return nlohmann::ordered_json();
    };
    const auto fitted = coefRow(setup2, "beertax");
    check(!fitted.is_null() && fitted.contains("b"),
          "AER-B4 margins-dydx-OLS setup: the seeding `regress fatal beertax spirits` has a beertax row");

    auto ame = piet::dispatch("margins, dydx(beertax)", session);
    check(firstBlockOfType(ame, "error").is_null(),
          "AER-B4 margins-dydx-OLS: `margins, dydx(beertax)` after `regress` is answered, not refused");
    const auto got = coefRow(ame, "beertax");
    check(!got.is_null(), "AER-B4 margins-dydx-OLS: the margins block carries a `beertax` dy/dx row");
    if (!fitted.is_null() && !got.is_null()) {
      bool same = true;
      for (const char* k : {"b", "se", "t", "p", "ci_lo", "ci_hi"})
        same = same && fitted.contains(k) && got.contains(k) && got[k] == fitted[k];
      check(same,
            "AER-B4 margins-dydx-OLS: every column of the dy/dx row (b/se/t/p/ci) is EXACTLY the fitted "
            "coefficient's — not a logistic AME computed from a defaulted --model");
      // The margins table is the REQUESTED effect only: one row, no intercept,
      // no second covariate. `spirits` is in the model and must not appear.
      bool onlyBeertax = true;
      for (const auto& b : ame) {
        if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
        onlyBeertax = onlyBeertax && b.value("coeftable", nlohmann::ordered_json::array()).size() == 1;
      }
      check(onlyBeertax,
            "AER-B4 margins-dydx-OLS: dydx(beertax) publishes that one effect — no `spirits`, no `(intercept)`");
    }
    // The refusal narrowed, it did not vanish: a name the model has no
    // coefficient for is still an honest named error, never an empty table.
    auto miss = piet::dispatch("margins, dydx(nosuchvarBUG478)", session);
    const auto missErr = firstBlockOfType(miss, "error");
    const std::string missMsg =
        missErr.is_object() ? missErr.value("message", std::string()) : std::string();
    check(missMsg.find("nosuchvarBUG478") != std::string::npos,
          "AER-B4 margins-dydx-OLS: `margins, dydx(<not a covariate>)` refuses and names the variable");

    // The SILENT half, pinned on its own. Seeding the naive fix (empty
    // kLinearAmeCmds + the five OLS names dropped from kLinearCmds) shows the
    // hazard is narrower than "any linear model" and much sharper where it
    // bites: for a CONTINUOUS outcome cobra's margins(logit) refuses out loud
    // ("outcome 'price' is not binary (found 4099)"), but for a LINEAR
    // PROBABILITY MODEL — a 0/1 outcome, an everyday Stata idiom — it answers
    // rc 0 with the LOGIT average marginal effect and says nothing. Measured
    // on `regress foreign mpg weight` (sysuse auto) under that seed: the LPM
    // coefficient is -.0194295 (se .0126701) and margins returned
    // -.0197187 (se .0096987) — which is, to every digit, what `logit foreign
    // mpg weight` -> `margins, dydx(mpg)` returns. A 31% error in the standard
    // error, from a different estimator, with no return code between them.
    // Nothing above catches that: the arms so far run on a continuous outcome,
    // where the seed trips the engine's binary guard instead.
    (void)piet::dispatch("gen byte ybinBUG478 = spirits > 2", session);
    auto lpmFit = piet::dispatch("regress ybinBUG478 beertax spirits", session);
    const auto lpmRow = coefRow(lpmFit, "beertax");
    check(!lpmRow.is_null(), "AER-B4 margins-dydx-OLS LPM setup: the 0/1-outcome regress has a beertax row");
    auto lpmAme = piet::dispatch("margins, dydx(beertax)", session);
    const auto lpmGot = coefRow(lpmAme, "beertax");
    check(!lpmGot.is_null() && !lpmRow.is_null() && lpmGot.contains("b") &&
              lpmGot["b"] == lpmRow["b"] && lpmGot["se"] == lpmRow["se"],
          "AER-B4 margins-dydx-OLS: after a LINEAR PROBABILITY MODEL the dy/dx is the LPM coefficient "
          "exactly — not the logit AME the engine would compute unasked");
  } catch (const std::exception& e) {
    // A throwing probe is a probe with no verdict: fail loudly rather than
    // abort the whole binary (which would report nothing at all).
    check(false, std::string("AER-B4 margins-dydx-OLS arm threw instead of reporting: ") + e.what());
  }

  // corr/pwcorr/correlate.
  {
    auto rej = piet::dispatch("pwcorr fatal beertax spirits, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"),
          "BS-097 pwcorr: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("pwcorr fatal beertax spirits, sig", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 pwcorr over-rejection guard: `pwcorr ..., sig` "
          "(freeze corpus shape: `pwcorr y x1 x2, sig`) still succeeds");
  }

  // ttest.
  {
    (void)piet::dispatch("gen ybinBS097 = spirits > 2", session);
    auto rej = piet::dispatch("ttest beertax, by(ybinBS097) bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 ttest: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("ttest beertax, by(ybinBS097)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 ttest over-rejection guard: `ttest ..., by(...)` "
          "(freeze corpus shape: `ttest y, by(female)`) still succeeds");
  }

  // test / lincom / nlcom — zero pre-existing consumption (the dispatcher
  // arm forwards only the joined varlist string; parsed.opts was never even
  // passed to the handler before this phase).
  {
    auto rejTest = piet::dispatch("test beertax spirits, bogusopt", session);
    check(isUnknownOptionReject(rejTest, "bogusopt"), "BS-097 test: unknown option honestly rejects, naming it");
    auto okTest = piet::dispatch("test beertax spirits", session);
    check(firstBlockOfType(okTest, "error").is_null(),
          "BS-097 test over-rejection guard: plain `test varlist` (freeze corpus shape) still succeeds");

    auto rejLincom = piet::dispatch("lincom beertax - spirits, bogusopt", session);
    check(isUnknownOptionReject(rejLincom, "bogusopt"),
          "BS-097 lincom: unknown option honestly rejects, naming it");
    auto okLincom = piet::dispatch("lincom beertax - spirits", session);
    check(firstBlockOfType(okLincom, "error").is_null(),
          "BS-097 lincom over-rejection guard: plain `lincom expr` (freeze corpus shape) still succeeds");

    auto rejNlcom = piet::dispatch("nlcom _b[beertax]/_b[spirits], bogusopt", session);
    check(isUnknownOptionReject(rejNlcom, "bogusopt"),
          "BS-097 nlcom: unknown option honestly rejects, naming it");
    auto okNlcom = piet::dispatch("nlcom _b[beertax]/_b[spirits]", session);
    check(firstBlockOfType(okNlcom, "error").is_null(),
          "BS-097 nlcom over-rejection guard: plain `nlcom expr` (freeze corpus shape) still succeeds");
  }

  // eststo/estimates/est — zero pre-existing consumption.
  {
    auto rej = piet::dispatch("eststo mBS097a, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 eststo: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("eststo mBS097a", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 eststo over-rejection guard: plain `eststo NAME` (freeze corpus shape) still succeeds");
  }

  // esttab.
  {
    auto setup3 = piet::dispatch("regress fatal spirits", session);
    check(firstBlockOfType(setup3, "error").is_null(), "BS-097 setup: `regress fatal spirits` succeeds");
    (void)piet::dispatch("eststo mBS097b", session);
    auto rej = piet::dispatch("esttab mBS097a mBS097b, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 esttab: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("esttab mBS097a mBS097b, se star(* 0.10 ** 0.05 *** 0.01)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 esttab over-rejection guard: `esttab ..., se star(...)` "
          "(freeze corpus shape: `esttab m1 m2, se star(* 0.10 ** 0.05 *** 0.01)`) still succeeds");
  }

  // estat — diag family (hettest/ovtest/vif/dwatson/bgodfrey/archlm/durbinalt).
  {
    auto setup4 = piet::dispatch("regress fatal beertax spirits", session);
    check(firstBlockOfType(setup4, "error").is_null(), "BS-097 setup: re-`regress fatal beertax spirits` succeeds");
    auto rej = piet::dispatch("estat vif, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 estat vif: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("estat vif", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 estat vif over-rejection guard: plain `estat vif` (freeze corpus shape) still succeeds");

    // estat's csdid sub-family net is branch-scoped BY `sub` (event has its
    // own {window, post} known-set; simple/calendar/group/pretrend read
    // none) — this leg does not set up a PIET-native csdid model, so it only
    // proves the net did not get reordered AHEAD of the pre-existing "no
    // csdid result in memory" guard (which must still fire first, unchanged).
    auto noCsdid = piet::dispatch("estat event, bogusopt", session);
    auto errNoCsdid = firstBlockOfType(noCsdid, "error");
    check(!errNoCsdid.is_null() &&
              errNoCsdid.value("message", std::string()).find("no csdid result in memory") !=
                  std::string::npos,
          "BS-097 estat event: without a prior csdid, still reports the pre-existing "
          "\"no csdid result in memory\" guard (the branch-scoped option net does not "
          "run ahead of it)");
  }

  // levelsof.
  {
    auto rej = piet::dispatch("levelsof year, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 levelsof: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("levelsof year, clean", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 levelsof over-rejection guard: `levelsof ..., clean` still succeeds");
  }

  // winsor2.
  {
    auto rej = piet::dispatch("winsor2 beertax, cuts(1 99) bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 winsor2: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("winsor2 beertax, cuts(1 99)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 winsor2 over-rejection guard: `winsor2 ..., cuts(1 99)` "
          "(freeze corpus shape: `winsor2 y, cuts(1 99)`) still succeeds");

    // BUG-112(4) PIET-half: cuts() must strict-consume each number (mirrors the
    // engine-side collapse pNN fix @9f86577) — a partial-consume token like "5x"
    // used to silently parse as 5.0, so `winsor2 fatal, cuts(5x 95)` succeeded.
    auto badCuts = piet::dispatch("winsor2 beertax, cuts(5x 95)", session);
    auto errBadCuts = firstBlockOfType(badCuts, "error");
    check(!errBadCuts.is_null() &&
              errBadCuts.value("message", std::string()).find("cuts(5x 95)") != std::string::npos,
          "BUG-112(4): winsor2 cuts(5x 95) — partial-consume token \"5x\" is honestly "
          "rejected, not silently read as 5");
  }

  // tabulate/tab/tab1/tab2.
  {
    auto rej = piet::dispatch("tabulate year, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 tabulate: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("tabulate year, sum(beertax)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 tabulate over-rejection guard: `tabulate ..., sum(...)` "
          "(freeze corpus shape: `tabulate region, sum(y)`) still succeeds");
  }

  // table.
  {
    auto rej = piet::dispatch("table year, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 table: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("table year", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 table over-rejection guard: plain `table rowvar` (freeze corpus shape) still succeeds");
  }

  // duplicates.
  {
    auto rej = piet::dispatch("duplicates report year, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"),
          "BS-097 duplicates: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("duplicates report year", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 duplicates over-rejection guard: `duplicates report varlist` "
          "(freeze corpus shape) still succeeds");
  }

  // encode — `state` is the bundled fatalities dataset's one real string
  // column (2-letter state abbreviation), the same role `grp` plays in the
  // kitchensink corpus's own `encode grp, gen(grpn)` frozen fixture.
  {
    auto rej = piet::dispatch("encode state, gen(state_nBS097) bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 encode: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("encode state, gen(state_nBS097ok)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 encode over-rejection guard: `encode ..., gen(...)` "
          "(freeze corpus shape: `encode grp, gen(grpn)`) still succeeds");
  }

  // count.
  {
    auto rej = piet::dispatch("count, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 count: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("count", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 count over-rejection guard: plain `count` (freeze corpus shape) still succeeds");
  }

  // drop / keep — zero pre-existing consumption. `drop` round-trips a
  // disposable var it creates itself so the reject probe (which must NOT
  // actually drop anything) is verifiable by the accept probe succeeding
  // right after. `keep` only gets the reject probe: shrinking the shared
  // session's varlist here would break every later leg that reuses it.
  {
    (void)piet::dispatch("gen dropProbeBS097 = 1", session);
    auto rej = piet::dispatch("drop dropProbeBS097, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 drop: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("drop dropProbeBS097", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 drop over-rejection guard: plain `drop varlist` still succeeds "
          "(also proves the reject probe just above did NOT actually drop the variable)");

    auto rejKeep = piet::dispatch("keep fatal beertax spirits, bogusopt", session);
    check(isUnknownOptionReject(rejKeep, "bogusopt"), "BS-097 keep: unknown option honestly rejects, naming it");
  }

  // append — zero pre-existing consumption (the arm never reads
  // parsed.opts at all; only a regex over parsed.raw + parsed.varlist). The
  // net is the FIRST line in the arm, so it fires before the `using`
  // filename is even parsed — a nonexistent path is enough to prove it,
  // no second on-disk dataset needed for this leg.
  {
    auto rej =
        piet::dispatch("append using \"/nonexistent/piet_bs097_probe.hbin\", bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"),
          "BS-097 append: unknown option honestly rejects, naming it, even when the `using` "
          "file does not exist (the net runs before file resolution)");
  }

  // B5-D10: `append using F, generate(NAME)` -- src marks which rows came
  // from `using` (0 for master, 1 for using). Measured B4REC|append|generate|0|
  // (tests/eval/golden/aer_b4/pins_oracle_r.log:96-97). append is in
  // kOptMinComplete; its GENerate minimum was measured later: `gen(src)` rc 0
  // (aer_w2 w2_data_io.log app9, w2b_dataio append_gen(src)) and `ge(src)`
  // r(198) (w2b_dataio append_ge) -- dispatcher.cpp kOptMinimumsLater.
  {
    std::string agWd = workdir + "/append_gen";
    fs::create_directories(agWd, ec);
    const std::string usingPath = agWd + "/using.hbin";
    {
      piet::SessionState setup(agWd);
      (void)piet::dispatch("clear", setup);
      (void)piet::dispatch("set obs 2", setup);
      (void)piet::dispatch("gen double gid = _n + 10", setup);
      (void)piet::dispatch("save \"" + usingPath + "\", replace", setup);
    }
    piet::SessionState ag(agWd);
    (void)piet::dispatch("clear", ag);
    (void)piet::dispatch("set obs 2", ag);
    (void)piet::dispatch("gen double gid = _n", ag);
    auto genBlocks =
        piet::dispatch("append using \"" + usingPath + "\", generate(srcBS097)", ag);
    check(firstBlockOfType(genBlocks, "error").is_null(),
          "B5-D10: `append using F, generate(NAME)` is accepted (rc 0)");
    piet::Datastore* agDs = ag.datastore();
    bool genOk = agDs != nullptr && agDs->has("srcBS097") && agDs->nRows() == 4;
    check(genOk, "B5-D10: the generate() column is created and the row count is nMaster+nUsing");
    if (genOk) {
      check(agDs->col("srcBS097").f64()[0] == 0.0 && agDs->col("srcBS097").f64()[1] == 0.0 &&
                agDs->col("srcBS097").f64()[2] == 1.0 && agDs->col("srcBS097").f64()[3] == 1.0,
            "B5-D10: generate() marks 0 for the 2 master rows, 1 for the 2 appended using rows");
    }
    // append.ado declares GENerate(name); the abbreviation is measured now
    // (w2aw_data_combine_family.log dc_a_gen `gen(src)` rc 0).
    auto genAbbrev = piet::dispatch("append using \"" + usingPath + "\", gen(srcBS097abbrev)", ag);
    check(firstBlockOfType(genAbbrev, "error").is_null(),
          "B5-D10: `gen(...)` is rc 0 on `append` (w2_data_io.log app9)");
    auto geAbbrev = piet::dispatch("append using \"" + usingPath + "\", ge(srcBS097ge)", ag);
    check(!firstBlockOfType(geAbbrev, "error").is_null(),
          "B5-D10: `ge(...)` stays r(198) on `append` (w2b_dataio append_ge)");
  }

  // hausman (BUG-105 remainder) — previously had NO opts parameter at all
  // (real Stata's constant/sigmamore/sigmaless/force/alleqs are none of them
  // implemented by this handler, which always recomputes both sides from
  // raw data via cobra's own --tidy hausman — a knob-less call), so every
  // one of them silently no-op'd instead of an honest reject.
  {
    (void)piet::dispatch("xtset state_nBS097ok year", session);
    (void)piet::dispatch("xtreg fatal beertax spirits, fe", session);
    (void)piet::dispatch("eststo feHausBS097", session);
    (void)piet::dispatch("xtreg fatal beertax spirits, re", session);
    (void)piet::dispatch("eststo reHausBS097", session);
    auto rej = piet::dispatch("hausman feHausBS097 reHausBS097, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"),
          "BS-097 hausman: unknown option honestly rejects, naming it (previously had no opts "
          "channel at all)");
    auto ok = piet::dispatch("hausman feHausBS097 reHausBS097", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 hausman over-rejection guard: plain `hausman A B` (D5e oracle shape) still "
          "succeeds");

    // BUG-105 secondary ⑤: `alleqs` is real Stata `hausman` syntax ([R]
    // hausman) this handler doesn't implement — distinct wording from the
    // `bogusopt` typo case just above, same "not allowed" code both ways.
    auto rejReal = piet::dispatch("hausman feHausBS097 reHausBS097, alleqs", session);
    auto realErr = firstBlockOfType(rejReal, "error");
    check(!realErr.is_null() &&
              realErr.value("code", std::string()) == "unsupported_option" &&
              realErr.value("message", std::string())
                      .find("is a valid Stata option that this version of PIET does not yet "
                            "implement") != std::string::npos,
          "BUG-105 secondary ⑤: hausman's `alleqs` (a real Stata option this build doesn't "
          "implement) is named as such, not lumped with a bogus typo");
  }

  // BUG-1940: hausman when V_b-V_B is not positive definite. Stata inverts
  // the difference with a generalized inverse and reports the statistic with
  // the note "(V_b-V_B is not positive definite)"; PIET's PSD-clip fallback
  // answered chi2 ~ 2.6e16 (near-singular clipped matrix) or refused with an
  // engine_error. Oracle: StataBE 19.5, 2026-11-06, the bundled fatalities
  // sample imported asdouble, frate = fatal/pop*10000, encode state, xtset:
  //   beertax unemp         -> r(chi2) 1.15382326678694  r(df) 2  r(p) .5616302108923181  (not PD)
  //   beertax spirits unemp -> r(chi2) 70.30558871723755 r(df) 3  r(p) 3.67130877062e-15  (not PD)
  //   beertax spirits       -> r(chi2) 30.02004414363056 r(df) 2  r(p) 3.02851856992e-07  (PD, no note)
  {
    piet::SessionState hs(workdir + "/bug1940");
    std::filesystem::create_directories(workdir + "/bug1940");
    (void)piet::dispatch("sysuse fatalities, clear", hs);
    if (!hs.hasData()) {
      skip("BUG-1940 (all legs)", "`sysuse fatalities` setup returned no data");
    } else {
      (void)piet::dispatch("generate double frate = fatal/pop*10000", hs);
      (void)piet::dispatch("encode state, gen(snum)", hs);
      (void)piet::dispatch("xtset snum year", hs);
      struct Arm { const char* xs; double chi2; int df; double p; bool notpd; };
      const Arm arms[] = {
          {"beertax unemp", 1.15382326678694, 2, 0.5616302108923181, true},
          {"beertax spirits unemp", 70.30558871723755, 3, 3.67130877062e-15, true},
          {"beertax spirits", 30.02004414363056, 2, 3.02851856992e-07, false},
      };
      int armNo = 0;
      for (const Arm& a : arms) {
        ++armNo;
        std::string tag = "h1940_" + std::to_string(armNo);
        (void)piet::dispatch(std::string("xtreg frate ") + a.xs + ", fe", hs);
        (void)piet::dispatch("estimates store " + tag + "f", hs);
        (void)piet::dispatch(std::string("xtreg frate ") + a.xs + ", re", hs);
        (void)piet::dispatch("estimates store " + tag + "r", hs);
        auto blocks = piet::dispatch("hausman " + tag + "f " + tag + "r", hs);
        auto err = firstBlockOfType(blocks, "error");
        nlohmann::ordered_json hb;
        for (const auto& b : blocks)
          if (b.is_object() && b.value("estimator", std::string()) == "hausman") { hb = b; break; }
        std::string label = std::string("BUG-1940 hausman `") + a.xs + "`";
        check(err.is_null() && hb.is_object(),
              label + ": answers with a hausman block, no engine_error (was: refused or chi2 ~ 1e16)");
        if (!hb.is_object()) continue;
        double chi2 = hb.value("chi2", 0.0);
        double p = hb.contains("p") && hb["p"].is_number() ? hb["p"].get<double>() : -1.0;
        check(std::fabs(chi2 - a.chi2) <= 1e-9 * std::fabs(a.chi2),
              label + ": chi2 matches StataBE r(chi2) " + std::to_string(a.chi2) + " at rtol 1e-9, got " +
                  std::to_string(chi2));
        check(hb.value("dof", -1) == a.df,
              label + ": dof is Stata's r(df) " + std::to_string(a.df) + " (the rank of V_b-V_B)");
        check(std::fabs(p - a.p) <= 1e-9 * std::fabs(a.p) + 1e-300,
              label + ": p matches StataBE r(p) at rtol 1e-9, got " + std::to_string(p));
        bool noted = false;
        if (hb.contains("notes"))
          for (const auto& n : hb["notes"])
            if (n.is_string() && n.get<std::string>() == "(V_b-V_B is not positive definite)") noted = true;
        check(hb.value("not_pd", !a.notpd) == a.notpd && noted == a.notpd,
              label + (a.notpd ? ": carries Stata's note \"(V_b-V_B is not positive definite)\""
                               : ": positive definite, so no note"));
      }
    }
  }

  // tabstat (BUG-105 remainder) — `stat`/`stats`/`statistics` are the only
  // options this arm reads; the singular `stat(...)` spelling had to be
  // recognized in the SAME change as the reject net (freeze corpus proof:
  // internal script debug_all.do:92 / class_demo.do:246 both use
  // `stat(mean sd n)` — a net keyed only on {by,stats,statistics} would have
  // newly rejected that exact frozen line instead of preserving it).
  {
    auto rej = piet::dispatch("tabstat beertax spirits, bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"),
          "BS-097 tabstat: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("tabstat beertax spirits, stat(mean sd n)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 tabstat over-rejection guard: `tabstat ..., stat(mean sd n)` (freeze corpus "
          "shape: debug_all.do:92 / class_demo.do:246, singular `stat`) still succeeds");
  }

  // egen (BUG-105 remainder) — cobra's OWN `--egen` CLI parsing never
  // validates its comma-suffix tail at all (live-verified against the
  // compiled engine: `--egen "x=mean(v),bogusopt(5)"` -> exit 0, x computed,
  // bogusopt silently dropped), so verbatim passthrough via buildOptsSuffix
  // reached NO rejecting net anywhere until egen.cpp's own PIET-layer check
  // added this phase (the judgment call this BUG-105 sub-item required).
  {
    auto rej = piet::dispatch("egen meanBS097 = mean(beertax), bogusopt", session);
    check(isUnknownOptionReject(rej, "bogusopt"), "BS-097 egen: unknown option honestly rejects, naming it");
    auto ok = piet::dispatch("egen meanBS097ok = mean(beertax), by(state_nBS097ok)", session);
    check(firstBlockOfType(ok, "error").is_null(),
          "BS-097 egen over-rejection guard: `egen ... = mean(v), by(g)` still succeeds");
  }

  // --- BS-018: `do`/`run FILE [args] [, nostop]` option parse ---
  //
  // Previously there was no comma handling in the do/run pre-classify step
  // at all: `do file.do, nostop` let the comma glue onto the end of the
  // filename token (scanOneToken stops at whitespace, not ','), so the path
  // never resolved; `do file.do , nostop` (space before the comma) instead
  // shredded ","/"nostop" into the positional-args vector. `nostop` itself
  // is now parsed/consumed (so it hits neither failure mode); its "continue
  // past an error instead of stopping the do-file" semantic is a SEPARATE
  // follow-up — doexec's stop-on-first-error loop is intentionally
  // unchanged here (out of BS-018's parsing-bug scope; see the return
  // summary).
  {
    std::string doFile = workdir + "/bs018_probe.do";
    {
      std::ofstream f(cecon::to_native_path(doFile));
      f << "gen bs018Marker = 1\n";
    }

    // (a) `do FILE, nostop` resolves FILE correctly.
    piet::SessionState s1(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", s1);
    auto r1 = piet::dispatch("do \"" + doFile + "\", nostop", s1);
    check(s1.hasData() && s1.datastore() != nullptr && s1.datastore()->has("bs018Marker"),
          "BS-018: `do FILE, nostop` resolves FILE correctly and runs it (previously the "
          "comma glued onto the filename, breaking resolution outright)");
    bool sawFileError = false;
    for (const auto& b : r1) {
      if (b.is_object() && b.value("type", std::string()) == "error" &&
          b.value("message", std::string()).find("not found") != std::string::npos) {
        sawFileError = true;
      }
    }
    check(!sawFileError, "BS-018: no \"file not found\"-shaped error for `do FILE, nostop`");

    // (b) a space before the comma — the OTHER shape the bug report called
    // out (","/"nostop" shredded into positional args, corrupting `1'`/`2'`).
    piet::SessionState s2(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", s2);
    (void)piet::dispatch("do \"" + doFile + "\" , nostop", s2);
    check(s2.hasData() && s2.datastore() != nullptr && s2.datastore()->has("bs018Marker"),
          "BS-018: `do FILE , nostop` (space before the comma) also resolves FILE correctly");

    // (c) a genuinely-unknown do/run option is still an honest reject
    // (BS-097 synergy: the do/run options tail now goes through the SAME
    // shared net as every other arm).
    piet::SessionState s3(workdir);
    auto r3 = piet::dispatch("do \"" + doFile + "\", bogusopt", s3);
    check(isUnknownOptionReject(r3, "bogusopt"),
          "BS-018/BS-097: `do FILE, bogusopt` honestly rejects, naming it");

    // (d) plain `do FILE` (no comma at all) is completely unaffected — the
    // common case, and the one every existing do-file invocation relies on.
    piet::SessionState s4(workdir);
    (void)piet::dispatch("sysuse fatalities, clear", s4);
    (void)piet::dispatch("do \"" + doFile + "\"", s4);
    check(s4.hasData() && s4.datastore() != nullptr && s4.datastore()->has("bs018Marker"),
          "BS-018 regression guard: plain `do FILE` (no options at all) is unaffected");
  }

  fs::remove_all(workdir, ec);
}


void testBUG127PredictTypePrefix() {
  std::printf("[selftest] --- BUG-127 `predict <type> newvar` storage-type prefix ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug127").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("regress fatal beertax", session);

  // --- `double` prefix: accepted, stripped, the REAL name is used ---
  auto dblBlocks = piet::dispatch("predict double yh, xb", session);
  bool dblErr = false;
  for (const auto& b : dblBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") dblErr = true;
  }
  check(!dblErr, "BUG-127: `predict double yh, xb` succeeds (double = PIET's own f64 storage)");
  piet::Datastore* ds = session.datastore();
  check(ds != nullptr && ds->has("yh") && ds->dtype("yh") == "f64",
        "BUG-127: the REAL name `yh` exists as an f64 column (was: silently discarded — the "
        "type token became the variable's name)");
  check(ds != nullptr && !ds->has("double"),
        "BUG-127: no column literally named `double` is created (the old misparse)");

  // --- `float` prefix: served, the column holds float values (BUG-924) ---
  // StataBE serves `predict float pf, xb` rc 0, typed float, values float-rounded
  // (w2aa_predict_type_restricted.log:133 a_pf_float_xb). The engine rounds on
  // store (--store-type), so every stored value is exactly a float.
  auto fltBlocks = piet::dispatch("predict float yf, xb", session);
  bool fltErr = false;
  for (const auto& b : fltBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") fltErr = true;
  }
  check(!fltErr,
        "BUG-127/BUG-924: `predict float yf, xb` succeeds, as StataBE does "
        "(w2aa_predict_type_restricted.log:133) -- it was refused r(198)");
  piet::Datastore* ds2 = session.datastore();
  check(ds2 != nullptr && ds2->has("yf") && !ds2->has("float"),
        "BUG-127: the REAL name `yf` exists; no column literally named `float`");
  check(session.varMeta.count("yf") != 0 && session.varMeta["yf"].stataType.has_value() &&
            *session.varMeta["yf"].stataType == "float",
        "BUG-924: `yf` is recorded as float (a_pf_float_xb `: type` float)");
  if (ds2 != nullptr && ds2->has("yf")) {
    const double* yf = ds2->col("yf").f64();
    const double* yh = ds2->has("yh") ? ds2->col("yh").f64() : nullptr;
    bool allFloat = true, anyDiffers = false;
    for (int64_t i = 0; i < ds2->nRows(); ++i) {
      if (!std::isfinite(yf[i])) continue;
      if (static_cast<double>(static_cast<float>(yf[i])) != yf[i]) allFloat = false;
      if (yh != nullptr && yf[i] != yh[i]) anyDiffers = true;
      if (yh != nullptr && static_cast<double>(static_cast<float>(yh[i])) != yf[i])
        allFloat = false;
    }
    check(allFloat,
          "BUG-924: every `yf` value is the float rounding of the double prediction "
          "(a_pf_float_xb %21x pf[1] +1.dda8fe0000000X+003: a float's mantissa)");
    check(yh == nullptr || anyDiffers,
          "BUG-924: ...and `yf` is NOT the double column (a_eq_p_pd: 0 rows equal)");
  }

  fs::remove_all(workdir, ec);
}


void testC3BS034LoopHeaderForms() {
  std::printf("[selftest] --- C3/BS-034 loop header forms (foreach of newlist, forvalues #1 #2 "
              "to/: #3) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c3_bs034").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // (a) foreach v of newlist na nb nc -- oracle: newvar na / newvar nb /
  // newvar nc.
  {
    piet::SessionState session(workdir);
    std::string src = "foreach v of newlist na nb nc {\ndisplay \"newvar `v'\"\n}\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!result.stoppedAtReason.has_value(),
          "C3/BS-034(a): `foreach v of newlist na nb nc` no longer errors \"unrecognised loop "
          "header\"");
    std::vector<std::string> texts;
    for (const auto& b : result.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        texts.push_back(b.value("text", std::string()));
      }
    }
    std::vector<std::string> expected = {"newvar na", "newvar nb", "newvar nc"};
    check(texts == expected, "C3/BS-034(a): iterates exactly na, nb, nc in order (oracle-pinned)");
  }

  // (b) forvalues i = 1 3 to 9 -- oracle: 1 3 5 7 9 (step = 3-1 = 2).
  {
    piet::SessionState session(workdir);
    std::string src = "forvalues i = 1 3 to 9 {\ndisplay \"fv `i'\"\n}\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!result.stoppedAtReason.has_value(),
          "C3/BS-034(b): `forvalues i = 1 3 to 9` no longer errors \"unrecognised loop header\"");
    std::vector<std::string> texts;
    for (const auto& b : result.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        texts.push_back(b.value("text", std::string()));
      }
    }
    std::vector<std::string> expected = {"fv 1", "fv 3", "fv 5", "fv 7", "fv 9"};
    check(texts == expected, "C3/BS-034(b): iterates 1 3 5 7 9 (oracle-pinned: step=3-1=2)");
  }

  // (c) forvalues i = 1 3 : 9 -- same semantics, colon separator.
  {
    piet::SessionState session(workdir);
    std::string src = "forvalues i = 1 3 : 9 {\ndisplay \"fv `i'\"\n}\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!result.stoppedAtReason.has_value(),
          "C3/BS-034(c): `forvalues i = 1 3 : 9` (colon form) no longer errors");
    std::vector<std::string> texts;
    for (const auto& b : result.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        texts.push_back(b.value("text", std::string()));
      }
    }
    std::vector<std::string> expected = {"fv 1", "fv 3", "fv 5", "fv 7", "fv 9"};
    check(texts == expected, "C3/BS-034(c): colon form gives the same 1 3 5 7 9 sequence");
  }

  // Regression guard: the already-working `1(3)9` form is untouched.
  {
    piet::SessionState session(workdir);
    std::string src = "forvalues j = 1(3)9 {\ndisplay \"fvp `j'\"\n}\n";
    auto result = piet::doexec::executeDoSource(src, session, piet::dispatch);
    std::vector<std::string> texts;
    for (const auto& b : result.blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display") {
        texts.push_back(b.value("text", std::string()));
      }
    }
    std::vector<std::string> expected = {"fvp 1", "fvp 4", "fvp 7"};
    check(texts == expected,
          "C3/BS-034 regression guard: `forvalues j = 1(3)9` (existing form) is untouched");
  }

  fs::remove_all(workdir, ec);
}


void testC3BS036DoRunEcho() {
  std::printf("[selftest] --- C3/BS-036 `do` keeps echoing, `run` doesn't ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c3_bs036").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  std::string doPath = workdir + "/bs036.do";
  {
    std::ofstream f(cecon::to_native_path(doPath));
    f << "display \"hello from bs036\"\n";
  }

  auto doBlocks = piet::dispatch("do \"" + doPath + "\"", session);
  int doEchoCount = 0;
  bool doSawDisplay = false;
  for (const auto& b : doBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "echo") ++doEchoCount;
    if (b.is_object() && b.value("type", std::string()) == "display" &&
        b.value("text", std::string()) == "hello from bs036") {
      doSawDisplay = true;
    }
  }
  // doEchoCount includes the OUTER "do FILE" echo (the console-level echo of
  // the do/run command itself) PLUS one inner echo per replayed line -- `do`
  // must show BOTH.
  check(doEchoCount >= 2,
        "C3/BS-036: `do FILE` echoes both the do-command itself AND the file's own `display "
        "...` line (>= 2 echo blocks), got " + std::to_string(doEchoCount));
  check(doSawDisplay, "C3/BS-036: `do FILE`'s display output still shows");

  auto runBlocks = piet::dispatch("run \"" + doPath + "\"", session);
  int runEchoCount = 0;
  bool runSawDisplay = false;
  for (const auto& b : runBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "echo") ++runEchoCount;
    if (b.is_object() && b.value("type", std::string()) == "display" &&
        b.value("text", std::string()) == "hello from bs036") {
      runSawDisplay = true;
    }
  }
  // runEchoCount should be exactly 1: only the OUTER "run FILE" echo (the
  // console-level echo of the run command line itself, which is not part of
  // the FILE's own commands) -- the inner `display` line's own echo is
  // suppressed.
  check(runEchoCount == 1,
        "C3/BS-036: `run FILE` echoes ONLY the outer run-command line, not the file's own "
        "commands (previously byte-identical to `do`), got " + std::to_string(runEchoCount) +
            " echo blocks");
  check(runSawDisplay,
        "C3/BS-036: `run FILE`'s display OUTPUT still shows (only the command echo is "
        "suppressed, not the command's own output)");

  fs::remove_all(workdir, ec);
}


void testC3BS037Tempvar() {
  std::printf("[selftest] --- C3/BS-037 tempvar mints a legal, collision-free new variable name "
              "---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c3_bs037").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);

  auto tvBlocks = piet::dispatch("tempvar tv", session);
  bool sawTvError = false;
  for (const auto& b : tvBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawTvError = true;
  }
  check(!sawTvError,
        "C3/BS-037: `tempvar tv` no longer trips the macro-reference tripwire (was \"Macro "
        "reference `tempvar` detected\")");
  std::string minted = session.eval.locals.get("tv", "");
  check(!minted.empty(), "C3/BS-037: `tv' resolves to a non-empty minted name");
  check(minted.size() >= 2 && minted[0] == '_' && minted[1] == '_',
        "C3/BS-037: the minted name follows Stata's own __NNNNNN pattern, got: " + minted);

  // The minted name must be a LEGAL, usable variable name -- prove it by
  // actually generating a column under it.
  auto genBlocks = piet::dispatch("gen `tv' = 1", session);
  bool sawGenError = false;
  for (const auto& b : genBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawGenError = true;
  }
  check(!sawGenError, "C3/BS-037: `gen `tv'' (using the minted name) succeeds");
  check(session.hasData() && session.datastore()->has(minted),
        "C3/BS-037: the minted name is now a real column in the dataset");

  // Two tempvar names in the same call must be distinct (and neither may
  // collide with an existing column).
  auto twoBlocks = piet::dispatch("tempvar tva tvb", session);
  bool sawTwoError = false;
  for (const auto& b : twoBlocks) {
    if (b.is_object() && b.value("type", std::string()) == "error") sawTwoError = true;
  }
  check(!sawTwoError, "C3/BS-037: `tempvar tva tvb` (two names) succeeds");
  std::string mintedA = session.eval.locals.get("tva", "");
  std::string mintedB = session.eval.locals.get("tvb", "");
  check(!mintedA.empty() && !mintedB.empty() && mintedA != mintedB,
        "C3/BS-037: two tempvar names in one call mint two DISTINCT names");

  fs::remove_all(workdir, ec);
}


void testP8DoRelativeToSessionCwd() {
  std::printf("[selftest] --- W:P8 BUG-174 `do` resolves against the session cwd ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p8_docwd").u8string();
  std::string scriptDir = workdir + "/scripts";
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(scriptDir, ec);
  {
    std::ofstream f(cecon::to_native_path(scriptDir + "/inner.do"));
    f << "display \"BUG174-INNER-RAN\"\n";
  }

  // The session starts somewhere ELSE entirely, so a process-cwd resolution
  // cannot accidentally succeed.
  piet::SessionState session(workdir);
  auto run = [&](const std::string& cmd) { return piet::dispatch(cmd, session); };
  run("cd \"" + scriptDir + "\"");

  nlohmann::ordered_json res = run("do \"inner.do\"");
  bool ran = false, notFound = false;
  for (const auto& b : res) {
    if (!b.is_object()) continue;
    const std::string text = b.value("text", std::string());
    if (text.find("BUG174-INNER-RAN") != std::string::npos) ran = true;
    if (b.value("type", std::string()) == "error" &&
        b.value("message", std::string()).find("file not found") != std::string::npos) {
      notFound = true;
    }
  }
  check(ran, "BUG-174: `cd DIR` then `do \"inner.do\"` finds and runs the sibling do-file");
  check(!notFound, "BUG-174: no \"file not found\" — the relative path resolved against session.cwd");

  // An ABSOLUTE path keeps working from any cwd (non-regression).
  nlohmann::ordered_json abs = run("do \"" + scriptDir + "/inner.do\"");
  bool absRan = false;
  for (const auto& b : abs) {
    if (b.is_object() && b.value("text", std::string()).find("BUG174-INNER-RAN") != std::string::npos) {
      absRan = true;
    }
  }
  check(absRan, "BUG-174 non-regression: an absolute `do` path still runs");
}


void testBUG112DoexecRaiiReap() {
  std::printf("[selftest] --- BUG-112(7) doexec tempdir reap survives a throw (RAII) ---\n");
  namespace fs = std::filesystem;
  std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_bug112_7").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  std::string doPath = workdir + "/bug112_7.do";
  {
    std::ofstream f(cecon::to_native_path(doPath));
    f << "display 1\n";
  }

  // Mirrors runTempfile's own minting shape (language/temporary_names.cpp) so the guard
  // is exercised against a realistic session.runTempdir value, not a stand-in.
  std::string mintedTempdir = workdir + "/tmp_bug112_7";
  fs::create_directories(mintedTempdir, ec);
  check(fs::exists(mintedTempdir), "BUG-112(7) setup: the per-run tempdir exists on disk before the run");

  piet::SessionState session(workdir);
  session.runTempdir = mintedTempdir;
  check(session.doExecDepth == 0, "BUG-112(7) setup: doExecDepth starts at 0 (outermost call)");

  piet::doexec::DispatchFn throwingDispatch =
      [](const std::string&, piet::SessionState&) -> nlohmann::ordered_json {
        throw std::runtime_error("BUG-112(7) selftest forced throw");
      };

  bool threw = false;
  try {
    (void)piet::doexec::executeDo(doPath, session, throwingDispatch, {});
  } catch (const std::runtime_error&) {
    threw = true;
  }
  check(threw, "BUG-112(7): the forced throw inside executeDoSource propagates out of executeDo unchanged "
               "(dispatchFn is called unguarded — confirms the reap sits on a real, reachable throw path)");
  check(session.doExecDepth == 0, "BUG-112(7): doExecDepth correctly unwinds back to 0 across the throw");
  check(!session.runTempdir.has_value(),
        "BUG-112(7): session.runTempdir is reaped (reset) even though executeDoSource threw");
  check(!fs::exists(mintedTempdir),
        "BUG-112(7): the minted tempdir is actually removed from disk on the throw path (not just the optional cleared)");

  fs::remove_all(workdir, ec);
}


// BUG-483/484/508 BEGIN -- control flow out of context, the throw that ate a run,
// and the varlist that was dropped in silence.
// Kept as its own function at the end of the file rather than folded into a
// neighbouring bundle: both defects are about what happens to the run AROUND a
// statement, not about a command's output, and the two W1 bundles next door are
// being edited by another session.
//
// Oracle for every expectation here: tests/eval/golden/bug483_484_control_oracle.log
// (top level) and tests/eval/golden/bug483b_loop_oracle.log (inside loops),
// captured on local StataBE 2026-09-18.
void testControlFlowOutOfContext() {
  std::printf("[selftest] --- BUG-483/484/508: out-of-context control flow, tabulate's throw, list's dropped varlist ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_ctrlflow").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };
  auto run = [&](const std::string& src) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  auto textOf = [](const piet::doexec::ExecuteResult& r) {
    std::string all;
    for (const auto& b : r.blocks) {
      const std::string t = b.value("type", std::string());
      if (t == "display" || t == "error") {
        all += b.value("text", b.value("message", std::string()));
        all += "\n";
      }
    }
    return all;
  };
  const std::string kFrame =
      "clear\nset obs 5\ngenerate double a = mod(_n,3)\ngenerate double b = _n\n";

  // (1) BUG-483. A `continue` with no loop around it is r(119) and the file
  // GOES ON. The marker is the whole point: before the fix the run ended at the
  // `continue` with no error, no stoppedAtLine and exit 0, so an rc-only check
  // would have passed while the rest of the file was being discarded.
  {
    auto r = run(kFrame + "capture noisily continue\ndisplay \"AFTER-CONTINUE\"\n");
    check(has(textOf(r), "AFTER-CONTINUE"),
          "BUG-483: a top-level `continue` must not end the do-file -- the statement after it "
          "runs (Stata: r(119) and execution continues). Got: [" + textOf(r) + "]");
    check(session.eval.locals.get("_rc", "") == "119",
          "BUG-483: ...and `capture` sees r(119), the rc Stata sets (oracle A1). Got _rc=" +
              session.eval.locals.get("_rc", "<unset>"));
    check(has(textOf(r), "statement out of context"),
          "BUG-483: ...with Stata's own wording. Got: [" + textOf(r) + "]");
    check(!r.stoppedAtLine.has_value(),
          "BUG-483: ...and nothing halted -- a captured error is display-only (BS-127)");
  }
  // (2) `continue, break` outside a loop is the same r(119) (oracle A3).
  {
    auto r = run(kFrame + "capture noisily continue, break\ndisplay \"AFTER-CB\"\n");
    check(has(textOf(r), "AFTER-CB") && session.eval.locals.get("_rc", "") == "119",
          "BUG-483: `continue, break` outside a loop is r(119) too, and the file goes on. Got "
          "_rc=" + session.eval.locals.get("_rc", "<unset>") + " text=[" + textOf(r) + "]");
  }
  // (3) `if` is TRANSPARENT: a `continue` inside a top-level `if` block is
  // still out of context (oracle B5). This is why the depth counter is on the
  // three loop constructs and NOT on runBody, which `if` shares with them.
  {
    auto r = run(kFrame + "capture noisily {\nif 1 == 1 {\ncontinue\n}\n}\n" +
                 "display \"AFTER-IF\"\n");
    check(has(textOf(r), "AFTER-IF"),
          "BUG-483: an `if` block does not make a `continue` in-context -- the file goes on "
          "(oracle B5). Got: [" + textOf(r) + "]");
  }
  // (4) The control arm. Inside a loop `continue` still skips, so the fix
  // cannot have been "make continue an error".
  {
    auto r = run(kFrame + "local hits = 0\nforvalues i = 1/4 {\nif `i' == 2 continue\n" +
                 "local hits = `hits' + 1\n}\ndisplay \"HITS=`hits'\"\n");
    check(has(textOf(r), "HITS=3"),
          "BUG-483 control: inside a loop `continue` still advances the iteration (3 of 4). "
          "Got: [" + textOf(r) + "]");
  }
  // (5) `break` is a NO-OP, everywhere, at rc 0 -- measured, not assumed
  // (oracle A2 top level, B1/B3/B4 inside forvalues/while/foreach, where it
  // does NOT exit: 5 of 5 iterations). Before the fix a bare `break` never
  // matched the control arm at all and came back "No dataset is loaded".
  {
    auto r = run(kFrame + "capture noisily break\ndisplay \"AFTER-BREAK\"\n");
    check(has(textOf(r), "AFTER-BREAK") && session.eval.locals.get("_rc", "") == "0",
          "BUG-483: a bare `break` is a no-op at rc 0 and prints nothing. Got _rc=" +
              session.eval.locals.get("_rc", "<unset>") + " text=[" + textOf(r) + "]");
    check(!has(textOf(r), "No dataset"),
          "BUG-483: ...and specifically not the old fall-through error. Got: [" + textOf(r) + "]");
  }
  {
    auto r = run(kFrame + "local n = 0\nforvalues i = 1/5 {\nlocal n = `n' + 1\n" +
                 "if `i' == 3 break\n}\ndisplay \"ITERS=`n'\"\n");
    check(has(textOf(r), "ITERS=5"),
          "BUG-483: a bare `break` inside forvalues does NOT exit the loop -- Stata runs all 5 "
          "(oracle B1); `continue, break` is the exit. Got: [" + textOf(r) + "]");
  }
  // (6) BUG-484. The blocks that already succeeded must SURVIVE. `BEFORE-TAB`
  // is the half that a forward-looking check would miss: the defect replaced
  // the whole array with one `run failed:` error, so everything printed before
  // the statement disappeared too.
  {
    auto r = run(kFrame + "display \"BEFORE-TAB\"\ncapture noisily tabulate nosuchcol\n" +
                 "display \"AFTER-TAB\"\n");
    const std::string t = textOf(r);
    check(has(t, "BEFORE-TAB") && has(t, "AFTER-TAB"),
          "BUG-484: `tabulate` on a missing column must not discard the run -- the blocks before "
          "it survive and the file goes on. Got: [" + t + "]");
    check(session.eval.locals.get("_rc", "") == "111",
          "BUG-484: ...at r(111), the same rc `summarize`/`list` give for the same column "
          "(oracle B1/C1/C2). Got _rc=" + session.eval.locals.get("_rc", "<unset>"));
    check(has(t, "variable nosuchcol not found"),
          "BUG-484: ...with Stata's wording, which is also what eval::coarseRc maps to 111. "
          "Got: [" + t + "]");
    check(!has(t, "run failed:"),
          "BUG-484: ...and no exception reached main() (`run failed:` is that signature). "
          "Got: [" + t + "]");
  }
  // (7) Two-way and the option form take the same path (oracle B2/B3).
  {
    auto r = run(kFrame + "capture noisily tab a nosuchcol\ndisplay \"AFTER-TAB2\"\n");
    check(has(textOf(r), "AFTER-TAB2") && session.eval.locals.get("_rc", "") == "111",
          "BUG-484: the two-way form is r(111) as well. Got _rc=" +
              session.eval.locals.get("_rc", "<unset>"));
  }
  {
    auto r = run(kFrame + "capture noisily tab nosuchcol, missing\ndisplay \"AFTER-TAB3\"\n");
    check(has(textOf(r), "AFTER-TAB3") && session.eval.locals.get("_rc", "") == "111",
          "BUG-484: and so is `, missing`. Got _rc=" +
              session.eval.locals.get("_rc", "<unset>"));
  }
  // (8) The control: a column that EXISTS still tabulates.
  {
    auto r = run(kFrame + "capture noisily tabulate a\ndisplay \"AFTER-GOOD\"\n");
    check(has(textOf(r), "AFTER-GOOD") && session.eval.locals.get("_rc", "") == "0",
          "BUG-484 control: `tabulate a` on a real column is still rc 0 -- the guard rejects "
          "only what is absent. Got _rc=" + session.eval.locals.get("_rc", "<unset>"));
  }
  // (9) BUG-508. A varlist naming a variable the frame does not have was
  // DROPPED in silence: `list a nosuchcol` rendered a one-column table at rc 0.
  // The oracle (bug508_varlist_oracle.log) is why this is not a plain has()
  // loop — `alpha*` and `alpha-beta` are rc 0, and an unmatched `zz*` is r(111)
  // naming the token the user typed.
  {
    auto nBrowse = [](const piet::doexec::ExecuteResult& r) {
      int n = 0;
      for (const auto& b : r.blocks) {
        if (b.value("type", std::string()) == "browse") ++n;
      }
      return n;
    };
    const std::string f2 =
        "clear\nset obs 5\ngenerate double alpha = _n\ngenerate double beta = 2*_n\n";
    auto r1 = run(f2 + "capture noisily list alpha nosuchcol\ndisplay \"AFTER-L1\"\n");
    check(has(textOf(r1), "variable nosuchcol not found") &&
              session.eval.locals.get("_rc", "") == "111",
          "BUG-508: `list alpha nosuchcol` is r(111) and NAMES the variable (oracle L1). Got "
          "_rc=" + session.eval.locals.get("_rc", "<unset>") + " [" + textOf(r1) + "]");
    check(nBrowse(r1) == 0,
          "BUG-508: ...and renders NO partial table -- dropping the unknown column and showing "
          "the rest is the silent-wrong-answer this fixes");
    check(has(textOf(r1), "AFTER-L1"),
          "BUG-508: ...and the error is captured, so the file goes on");

    auto r2 = run(f2 + "capture noisily list alpha*\ndisplay \"AFTER-L3\"\n");
    check(session.eval.locals.get("_rc", "") == "0" && nBrowse(r2) == 1,
          "BUG-508 control: a wildcard that MATCHES is still rc 0 with its table (oracle L3). "
          "Got _rc=" + session.eval.locals.get("_rc", "<unset>"));
    auto r3 = run(f2 + "capture noisily list alpha-beta\ndisplay \"AFTER-L4\"\n");
    check(session.eval.locals.get("_rc", "") == "0" && nBrowse(r3) == 1,
          "BUG-508 control: an `a-b` RANGE is still rc 0 (oracle L4) -- the guard reuses "
          "loops::expandVarlist so a range means here what it means to `foreach of varlist`. "
          "Got _rc=" + session.eval.locals.get("_rc", "<unset>"));
    auto r4 = run(f2 + "capture noisily list\ndisplay \"AFTER-L0\"\n");
    check(session.eval.locals.get("_rc", "") == "0" && nBrowse(r4) == 1,
          "BUG-508 control: a bare `list` still shows every column");

    auto r5 = run(f2 + "capture noisily list zz*\ndisplay \"AFTER-L5\"\n");
    check(has(textOf(r5), "variable zz* not found") &&
              session.eval.locals.get("_rc", "") == "111",
          "BUG-508: a wildcard matching NOTHING is r(111) naming the token as typed (oracle L5) "
          "-- not the expander's own message. Got _rc=" +
              session.eval.locals.get("_rc", "<unset>") + " [" + textOf(r5) + "]");
  }
  fs::remove_all(workdir, ec);
}
