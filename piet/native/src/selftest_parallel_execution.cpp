// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: parallel execution.
#include "selftest_support.h"
#include "selftest_cases.h"


// P9 arc①/② — `parallel, workers(N) { ... }` block form.
//
// The VERIFY probe (tests/eval/verify_ledger/probes/piet-plevel) is the real
// gate for this feature: it forks actual workers and byte-compares stdout, the
// log, and every artifact. It is NOT part of the standard battery, so these
// legs are what a routine `--selftest` run has to notice a regression with.
// They stay in-process on purpose (workers(1) takes the serial leg), so they
// are fast and cannot be flaky about process scheduling.
// P9 automode · the recognizer's approval predicate.
//
// The predicate is the only thing standing between "run these in parallel" and
// wrong output, so its REFUSALS are as load-bearing as its approvals and are
// tested here in both directions. These legs never spawn a worker — they ask
// planRegion directly, which is where every decision is actually made.
void testP9AutoRecognizer() {
  std::printf("[selftest] --- P9 automode recognizer (approval predicate) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p9auto").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Default OFF is a User ruling, not a preference: assert it holds with no env.
  check(!piet::parallel::autoModeEnabled(),
        "P9 automode: OFF unless PIET_PARALLEL_AUTO is set");

  auto plan = [&](const std::vector<std::string>& stmts,
                  const std::vector<std::string>& after) {
    piet::SessionState session(workdir);
    return piet::parallel::planRegion(stmts, after, session);
  };

  // Approves: three self-seeding, effect-free-outside tasks with nothing after.
  {
    auto p = plan({"clear", "clear", "clear"}, {});
    check(p.approved, std::string("P9 automode: a run of self-seeding statements is approved") +
                          (p.approved ? "" : " — got: " + p.refusalReason));
  }
  // Refuses: below the minimum region size (a 2-task region cannot pay for itself).
  {
    auto p = plan({"clear", "clear"}, {});
    check(!p.approved, "P9 automode: a run shorter than kAutoMinRegion is refused");
  }
  // Session-globals are decided BY NAME, not by the fact of writing one. With
  // nothing downstream reading `g`, the region is safe and is approved — the
  // blanket refusal this replaced rejected every real program.
  {
    auto p = plan({"clear", "global g 1", "clear"}, {});
    check(p.approved,
          std::string("P9 automode: a session-global nobody reads afterwards is approved") +
              (p.approved ? "" : " — got: " + p.refusalReason));
  }
  // ...and refused, by name, the moment something after the region reads it.
  {
    auto p = plan({"clear", "global g 1", "clear"}, {"display \"$g\""});
    check(!p.approved && p.refusalReason.find("g") != std::string::npos,
          "P9 automode: a session-global READ after the region refuses it, by name");
  }
  // A write whose target name is minted at run time cannot be reasoned about at
  // all, so it refuses regardless of what follows.
  {
    auto p = plan({"clear", "global `nm' 1", "clear"}, {});
    check(!p.approved && p.refusalReason.find("macro") != std::string::npos,
          "P9 automode: a macro-named session-global write refuses");
  }
  // Refuses: a top-level `capture` (writes _rc and changes which lines run).
  {
    auto p = plan({"clear", "capture summarize x", "clear"}, {});
    check(!p.approved && p.refusalReason.find("capture") != std::string::npos,
          "P9 automode: a top-level `capture` is refused by name");
  }
  // Refuses: an unknown command — the whole safety axiom in one case.
  {
    auto p = plan({"clear", "no_such_command_at_all", "clear"}, {});
    check(!p.approved && p.refusalReason.find("effect table") != std::string::npos,
          "P9 automode: an unclassified command refuses the region");
  }
  // Refuses: the region leaves a dataset that a later statement reads.
  {
    auto p = plan({"clear", "clear", "clear"}, {"summarize x"});
    check(!p.approved, "P9 automode: a successor that reads the region's dataset refuses it");
  }
  // The failure index is what lets the scan resume after a bad statement instead
  // of writing off everything behind it — measured on the acceptance corpus,
  // that difference is the whole batch being reachable or not.
  {
    auto p = plan({"capture summarize x", "clear", "clear", "clear"}, {});
    check(!p.approved && p.failedAt == 0,
          "P9 automode: the refusal names WHICH statement failed (failedAt)");
  }
  // User ruling 2026-09-02 (supersedes #14-b's floor of one and #25): the auto
  // worker count is DERIVED from the budgets and the work — cores (performance
  // cores when auto), memory (budget / a dataset-scaled per-worker cost), tasks
  // (the block's own count) and disk (half the free scratch space / a
  // dataset-scaled per-task scratch peak). The smallest term binds; nothing is
  // a constant floor. Pinned through the pure derivation so no live machine
  // state enters.
  {
    using piet::parallel::DerivationInputs;
    auto mk = [] {
      DerivationInputs in;
      in.perfCores = 4;
      in.memoryBudgetMb = 4096;
      in.residentBytes = 0;
      in.taskCount = 8;
      in.scratchFreeBytes = int64_t(100) << 30;  // 100 GiB: disk never binds here
      return in;
    };
    auto d = piet::parallel::deriveDefaultWorkers(mk());
    check(d.n == 4 && d.coresTerm == 4 && d.memTerm == 5,
          "P9 N (09-02): auto/4 perf cores/4096 MB/8 tasks/plenty of disk -> 4 (cores bind; memory 5)");
    check(d.reason.find("cores 4") != std::string::npos && d.reason.find("-> 4") != std::string::npos,
          "P9 N (09-02): the banner names every term and the result");
    { auto in = mk(); in.taskCount = 2; check(piet::parallel::deriveDefaultWorkers(in).n == 2,
          "P9 N (09-02): a 2-task block gets 2 workers (the work binds)"); }
    { auto in = mk(); in.taskCount = 1; check(piet::parallel::deriveDefaultWorkers(in).n == 1,
          "P9 N (09-02): a 1-task block gets 1 worker"); }
    { auto in = mk(); in.taskCount = 0; check(piet::parallel::deriveDefaultWorkers(in).n == 4,
          "P9 N (09-02): unknown task count is no constraint"); }
    { auto in = mk(); in.computeCores = 2; check(piet::parallel::deriveDefaultWorkers(in).n == 2,
          "P9 N (09-02): an explicit compute_cores=2 replaces the perf-core width"); }
    { auto in = mk(); in.perfCores = 8; in.memoryBudgetMb = 8192;
      check(piet::parallel::deriveDefaultWorkers(in).n == 4,
          "P9 N (09-02): the measured ceiling (kMemoryCapWorkers) still caps a generous machine at 4"); }
    { auto in = mk(); in.memoryBudgetMb = 1000;
      auto dd = piet::parallel::deriveDefaultWorkers(in);
      check(dd.n == 1 && dd.perWorkerMb == 700,
          "P9 N (09-02): resident unknown -> the 700 MB constant; 1000 MB budget -> 1 worker"); }
    { auto in = mk(); in.residentBytes = int64_t(164) << 20;
      auto dd = piet::parallel::deriveDefaultWorkers(in);
      check(dd.perWorkerMb == 200 + 2 * 164 && dd.memTerm == 7 && dd.n == 4,
          "P9 N (09-02): per-worker cost scales with the resident data (200 + 2 x 164 = 528 MB)"); }
    { auto in = mk(); in.residentBytes = int64_t(164) << 20; in.scratchFreeBytes = int64_t(1) << 30;
      auto dd = piet::parallel::deriveDefaultWorkers(in);
      check(dd.perTaskScratchMb == 4 * 164 && dd.n == 1,
          "P9 N (09-02): 1 GiB free scratch / (4 x 164 MB per task, half headroom) -> 1 worker (disk binds)"); }
    { auto in = mk(); in.residentBytes = int64_t(164) << 20; in.scratchFreeBytes = int64_t(4) << 30;
      check(piet::parallel::deriveDefaultWorkers(in).n == 3,
          "P9 N (09-02): 4 GiB free -> 2048 / 656 = 3 workers (disk binds below cores)"); }
    { auto in = mk(); in.scratchFreeBytes = -1; check(piet::parallel::deriveDefaultWorkers(in).n == 4,
          "P9 N (09-02): unknown free space is no constraint"); }
    { auto in = mk(); in.residentBytes = 1 << 20; in.scratchFreeBytes = int64_t(1) << 30;
      auto dd = piet::parallel::deriveDefaultWorkers(in);
      check(dd.perTaskScratchMb == 64 && dd.n == 4,
          "P9 N (09-02): a tiny dataset still budgets the 64 MB per-task floor"); }
    check(piet::parallel::perfCoreCount() >= 1, "P9 N (09-02): perfCoreCount() is never 0");
    check(piet::parallel::scratchFreeBytes("") > 0,
          "P9 N (09-02): scratchFreeBytes() reads the scratch volume (>0 on a live machine)");
  }

  // ONESHOT-P9N4 C: the N² guard's divisor. resolveWorkers reads the live
  // settings file, so these legs pin the arithmetic through the ONE input a
  // selftest can hold fixed — the requested N — and assert the invariant that
  // holds for every machine rather than a number that only holds on this one.
  // (compute_cores is the User's setting, not ours to write from a test.)
  {
    const piet::parallel::WorkerBudget b1 = piet::parallel::resolveWorkers(1);
    const piet::parallel::WorkerBudget b4 = piet::parallel::resolveWorkers(4);
    check(b1.engineThreads >= 1 && b4.engineThreads >= 1,
          "P9 threads: every worker gets at least one engine thread (never 0)");
    check(b4.n == 4 || b4.pressure >= 4,
          "P9 threads: an explicit workers(4) is honoured unless the kernel is critical");
    // The whole point of the guard: N workers must not each take the full width.
    check(b4.engineThreads <= b1.engineThreads,
          "P9 threads: four workers each get no more engine threads than one worker does");
    check(b4.n * b4.engineThreads <= b1.engineThreads * b1.n + b4.n,
          "P9 threads: total engine threads stay within the core budget (+rounding floor)");
    // 2026-09-02: the memory budget divides the same way, with a 64 MB floor.
    check(b1.engineMemoryMb >= 64 && b4.engineMemoryMb >= 64,
          "P9 memory: every worker gets at least the 64 MB floor");
    check(b4.engineMemoryMb <= b1.engineMemoryMb,
          "P9 memory: four workers each get no more memory than one worker does");
    check(b4.n * b4.engineMemoryMb <= b1.engineMemoryMb + 64 * b4.n,
          "P9 memory: the workers' shares stay within the memory budget (+floor)");
  }

  // Coverage is a progress metric, not a correctness one — but an empty table
  // would make every region refuse and every test above pass vacuously.
  check(piet::parallel::classifiedCommandCount() > 50,
        "P9 automode: the effect table is populated (" +
            std::to_string(piet::parallel::classifiedCommandCount()) + " commands)");
}


void testP9ParallelBlock() {
  std::printf("[selftest] --- P9 `parallel, workers(N) { ... }` block form ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p9par").u8string();
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
  auto errorCount = [](const auto& blocks) {
    int n = 0;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") ++n;
    return n;
  };

  // --- Case 1: workers(1) runs every task, in source order, and does not halt.
  {
    piet::SessionState session(workdir);
    std::string src =
        "parallel, workers(1) {\n"
        "display \"p9_task_a\"\n"
        "display \"p9_task_b\"\n"
        "}\n"
        "display \"p9_after\"\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!r.stoppedAtReason.has_value(),
          "P9: a clean `parallel, workers(1)` block does not halt, stop=" +
              r.stoppedAtReason.value_or("<none>"));
    check(sawDisplay(r.blocks, "p9_task_a") && sawDisplay(r.blocks, "p9_task_b"),
          "P9: every task in the block ran");
    check(sawDisplay(r.blocks, "p9_after"), "P9: execution continues after the block");
  }

  // --- Case 2: honest option errors. Each STOPS rather than being ignored --
  // a misspelt option that silently runs with the default is how a user ends up
  // believing they configured something they did not.
  {
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"parallel, wokers(4) {\ndisplay \"x\"\n}\n", "misspelt option name"},
        {"parallel, workers(0) {\ndisplay \"x\"\n}\n", "workers(0)"},
        {"parallel workers(4) {\ndisplay \"x\"\n}\n", "options without the comma"},
    };
    for (const auto& [src, what] : bad) {
      piet::SessionState session(workdir);
      auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
      check(errorCount(r.blocks) > 0, "P9: " + what + " is an honest error, not a silent default");
    }
  }

  // --- Case 3: failure isolation (2.6) -- a failing task neither halts the
  // block nor hides itself, siblings still run, and _rc / r(n_failed) report it.
  {
    piet::SessionState session(workdir);
    std::string src =
        "parallel, workers(1) {\n"
        "display \"p9_iso_1\"\n"
        "summarize p9_no_such_variable\n"
        "display \"p9_iso_3\"\n"
        "}\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    check(!r.stoppedAtReason.has_value(), "P9: a failing task does not halt the do-file");
    check(sawDisplay(r.blocks, "p9_iso_1") && sawDisplay(r.blocks, "p9_iso_3"),
          "P9: tasks on both sides of a failing task still ran");
    check(errorCount(r.blocks) > 0, "P9: the failing task's error is VISIBLE, not swallowed");
    check(session.eval.locals.get("_rc") == "1",
          "P9: _rc is 1 after a task failed");
  }

  // --- Case 4: `capture, {` must NOT be parsed as a block head. The parallel
  // keyword is the first block head whose keyword may be followed by an option
  // comma, and the comma-stripping that allows it is deliberately scoped to
  // `parallel` alone -- widening it would silently turn `capture, {` into a
  // block and change what a pre-existing do-file means.
  {
    piet::SessionState session(workdir);
    auto r = piet::doexec::executeDoSource("capture, {\n", session, piet::dispatch);
    check(errorCount(r.blocks) > 0 || r.stoppedAtReason.has_value(),
          "P9: `capture, {` is NOT silently promoted to a brace block");
  }

  // --- Case 5: the task spec is the worker contract; pin its schema and the
  // inheritance set, since a child that silently loses `programs` fails as an
  // unknown command and reports something else entirely (BUG-342's second layer).
  {
    piet::SessionState session(workdir);
    (void)piet::dispatch("global p9g hello", session);
    piet::parallel::WorkerBudget budget = piet::parallel::resolveWorkers(2);
    auto spec = piet::parallel::buildTaskSpec({"display \"t\""}, session, budget, "");
    check(spec.value("schema", std::string()) == "piet.parallel.task_spec/2",
          "P9: task spec schema is task_spec/2");
    for (const char* key : {"locals", "globals", "scalars", "programs", "engine_threads",
                            "engine_memory_mb", "cwd", "parent_head", "set_state", "tasks"})
      check(spec.contains(key), std::string("P9: task spec carries `") + key + "`");
    // BUG-347: the `set` state must travel, and it must travel by VALUE — a
    // spec that carries the key with the parent's default in it would pass the
    // presence check above while still shipping the wrong flag.
    session.graphicsOff = true;
    session.level = 90;
    auto spec2 = piet::parallel::buildTaskSpec({"display \"t\""}, session, budget, "");
    check(spec2["set_state"].value("graphics_off", false) == true &&
              spec2["set_state"].value("level", 95) == 90,
          "BUG-347: task spec carries the parent's actual `set graphics off` / `set level`");
  }

  // --- Case 6: the BARE form. `parallel {` (and `parallel, {`) omits workers()
  // and asks the settings-derived pool size instead. The derivation itself is
  // pinned by the unit legs above; what was never executed end to end is the
  // PARSE branch that reaches it -- until this leg, the string `parallel {`
  // appeared nowhere in the repo, so a regression in `parseHeader`'s empty-tail
  // handling would have surfaced first in a user's do-file. Deliberately makes
  // no claim about N: the derived count depends on this machine's settings, and
  // every N >= 1 must run every task.
  {
    for (const char* head : {"parallel {\n", "parallel, {\n"}) {
      piet::SessionState session(workdir);
      std::string src = std::string(head) +
                        "display \"p9_bare_a\"\n"
                        "display \"p9_bare_b\"\n"
                        "}\n"
                        "display \"p9_bare_after\"\n";
      auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
      std::string what = std::string("`") + (head[8] == ',' ? "parallel, {" : "parallel {") + "`";
      check(!r.stoppedAtReason.has_value(),
            "P9 bare: " + what + " does not halt, stop=" + r.stoppedAtReason.value_or("<none>"));
      check(errorCount(r.blocks) == 0, "P9 bare: " + what + " raises no error");
      check(sawDisplay(r.blocks, "p9_bare_a") && sawDisplay(r.blocks, "p9_bare_b"),
            "P9 bare: every task ran under " + what);
      check(sawDisplay(r.blocks, "p9_bare_after"),
            "P9 bare: execution continues after " + what);
    }
    // The bare form's budget is the derived one, and it must satisfy the same
    // floor/cap the formula legs pin -- read here through the resolve path the
    // block actually uses, not through deriveDefaultWorkers directly.
    piet::parallel::WorkerBudget derived = piet::parallel::resolveWorkers(0);
    check(derived.n >= 1 && derived.n <= 4,
          "P9 bare: the derived worker count is inside [1, 4], got " + std::to_string(derived.n));
    check(!derived.reason.empty(),
          "P9 bare: the derived budget names its reason (the banner prints it)");
  }

  // --- Case 7 (BUG-1131): a local holding a byte that is not UTF-8 (a line
  // `file read` brought back from a Latin-1 file) reaches the workers with its
  // bytes intact. The spec and result were JSON text, whose dump threw
  // type_error.316 and ended the whole run. Each task generates a column that
  // never comes back to the parent, which is how the arm knows it fanned out.
  {
    piet::SessionState session(workdir);
    session.eval.locals.set("p9l", std::string("caf\xE9 12"));
    std::string src =
        "clear\n"
        "set obs 1\n"
        "parallel, workers(2) {\n"
        "generate p9u1 = 1\n"
        "display \"p9_latin1 \" strlen(`\"`macval(p9l)'\"')\n"
        "}\n"
        "display \"p9_latin1_after\"\n";
    auto r = piet::doexec::executeDoSource(src, session, piet::dispatch);
    piet::Datastore* ds = session.datastore();
    const std::string label = "BUG-1131 parallel: a Latin-1 local reaches the workers byte for byte";
    if (ds == nullptr || ds->has("p9u1")) {
      skip(label, "the block ran serially (budget 1 or no sibling piet_native)");
    } else {
      check(!r.stoppedAtReason.has_value() && sawDisplay(r.blocks, "p9_latin1 7") &&
                sawDisplay(r.blocks, "p9_latin1_after"),
            label + " (strlen 7, the run goes on), stop=" + r.stoppedAtReason.value_or("<none>"));
    }
  }
}





// ── AER scope 2, bundle K1c: `parallel`, answered sequentially ──────────────
//
// The package CANNOT BE RUN on this machine -- `parallel setclusters 2` answers
// r(601) even with an explicit statapath(), so $PLL_STATA_PATH never gets set
// and no child process can start. Every rule asserted below therefore comes
// from parallel.ado's OWN SOURCE rather than from a live capture, and the two
// forms whose answer depends on how the data is split are REFUSED rather than
// approximated, because there is no oracle to check a reproduction against.
//
// What the source says, and what these legs pin:
//   · `parallel do` requires $PLL_CLUSTERS and exits 198 without it.
//   · with `nodata` the children get no data and nothing is pasted back, so
//     the parent's dataset must be UNTOUCHED afterwards. That is the leg that
//     fails if the sequential run is allowed to leak into the parent.
//   · without `nodata` the rows are SPLIT across clusters and the results are
//     pasted back, so with more than one cluster the answer depends on the
//     split -- refused, named.
//   · `initialize` is not a subcommand of the installed 1.15.8 (measured
//     r(198)) but IS what the corpus writes. It is accepted as a spelling of
//     `setclusters`: the ACCEPTED-NAMED exception for a third-party ado whose
//     own versions disagree. This leg is the record of that decision, so
//     reversing it is a visible change and not a quiet one.
void testAerK1cParallel() {
  std::printf("[selftest] --- AER scope-2 K1c: parallel (sequential) ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_pll").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  const std::string child = workdir + "/pll_child.do";
  {
    std::ofstream f(child);
    f << "clear\nset obs 4\ngen fromchild = 99\n";
  }

  auto run = [&](const std::string& src) {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;
    return piet::doexec::executeDoSource(src, s, piet::dispatch);
  };
  auto lastDisplay = [](const nlohmann::ordered_json& blocks) {
    std::string out;
    for (const auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        out = b.value("text", std::string());
    }
    return out;
  };
  auto rcOfSrc = [&](const std::string& body) {
    return lastDisplay(run(body + "\nlocal rc = _rc\ndisplay \"`rc'\"\n").blocks);
  };
  const std::string kData = "clear\nset obs 5\ngen parentvar = _n\n";

  check(rcOfSrc(kData + "capture parallel do \"" + child + "\", nodata processors(1)") == "198",
        "K1c parallel: `do` with no cluster count set is r(198) -- parallel.ado's own first check");
  check(lastDisplay(run(kData + "parallel initialize 4\ndisplay \"${PLL_CLUSTERS}\"\n").blocks) ==
            "4",
        "K1c parallel: `initialize` records the cluster count in $PLL_CLUSTERS, which is the "
        "global the package itself uses. It is NOT a subcommand of the installed 1.15.8 (r(198), "
        "measured) and is accepted anyway, because it is what the corpus writes");
  check(lastDisplay(run(kData + "parallel setclusters 2\ndisplay \"${PLL_CLUSTERS}\"\n").blocks) ==
            "2",
        "K1c parallel: `setclusters` is the same arm under the spelling this version documents");
  check(rcOfSrc(kData + "parallel initialize 1\ncapture parallel do \"" + child +
                "\", nodata processors(1)") == "0",
        "K1c parallel: `do ... , nodata` RUNS the file");
  check(lastDisplay(run(kData + "parallel initialize 1\nparallel do \"" + child +
                        "\", nodata processors(1)\nds\n")
                        .blocks) == "parentvar",
        "K1c parallel: ...and the parent's dataset is UNTOUCHED. With `nodata` the children get "
        "no data and nothing is pasted back, so a sequential run that leaked into the parent "
        "would be a different dataset at rc 0 -- this is the leg that catches it");
  check(rcOfSrc(kData + "parallel initialize 4\ncapture parallel do \"" + child + "\"") == "199",
        "K1c parallel: WITHOUT `nodata` the package splits the rows across clusters and pastes "
        "the results back, so the answer depends on the split. Refused, named -- the package "
        "cannot be run here to check a reproduction against");
  check(rcOfSrc(kData + "parallel initialize 1\ncapture parallel do \"" + child +
                "\", nodata seeds(random)") == "198",
        "K1c parallel: an option that changes what a cluster RECEIVES is refused rather than "
        "accepted-and-ignored");
  check(rcOfSrc(kData + "capture parallel bogus") == "198",
        "K1c parallel: an unknown subcommand is r(198) with parallel.ado's own wording");
  check(rcOfSrc(kData + "capture parallel clean") == "0",
        "K1c parallel: `clean` is rc 0 -- measured in Stata, where it is rc 0 even with no "
        "cluster ever started");
  check(rcOfSrc(kData + "capture parallel append , do(ds) e(\"x_%g.dta, 1/3\")") == "199",
        "K1c parallel: `append` goes through the cluster machinery this build cannot start, so "
        "the whole verb is refused rather than half-done");
}
