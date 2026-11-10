// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/stata_format.h"  // BUG-1395: numlist element text is %15.0g
#include "piet/strutil.h"
#include "core/expr_blanks.h"  // FU-875: Stata's blank squeeze and r(132) pairing rule
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "data/missing.h"  // BUG-921: the extended-missing kind of a cell read by the if condition
#include "dispatch/dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

#include "language/loops_internal.h"

namespace piet::loops {

namespace loops_detail {

// PLEVEL-IMPL-WAVE arc ① — `parallel[, workers(N)] { … }`, the SKELETON leg.
// Design authority + arc boundary: include/piet/parallel.h. What lands here is
// header parsing (honest errors), the §3.4 task decomposition, the resolved
// worker budget, the §2.4 task spec, the stderr banner — and SERIAL execution
// of the decomposed tasks in this process. Arc ② replaces the execution loop
// below with spawn/splice over `--parallel-worker` children; everything around
// it (what a task is, what a child inherits, what the parent learns) is already
// what that arc will hand to them.
//
// Two deliberate properties of this leg:
//  · **Per-TASK dispatch, not one whole-body run.** Running the body as a
//    single doexec pass would be one line shorter, but it would make a failing
//    task halt the rest of the block — the opposite of §2.6's failure
//    isolation, which is a documented semantic of the construct and not merely
//    an artefact of running in parallel. Dispatching per task means the
//    isolation rule holds identically at N=1 and at N>1, which is exactly what
//    makes the N=1 leg a legitimate reference arm for G-1.
//  · **The banner goes to stderr.** stdout is the byte-pinned surface (§2.5;
//    freeze/run_eval/floor_gate are all byte-exact), so a stdout banner would
//    diff every fixture at once. Precedent: main.cpp's `--do scratch workdir`
//    line is stderr for the same reason.
ojson runParallelBlock(const std::string& header, const std::vector<std::string>& bodyLines,
                       SessionState& session, const DispatchFn& dispatchFn) {
  parallel::HeaderOptions opt = parallel::parseHeader(header);
  if (opt.isError) return ojson::array({opt.error});

  std::vector<std::string> tasks = parallel::decomposeTasks(bodyLines);
  // 2026-09-02: the derivation gets the WORK it is sizing for — this block's
  // task count, the dataset the workers will inherit, and the scratch volume.
  parallel::ResolveContext rctx;
  rctx.taskCount = static_cast<int64_t>(tasks.size());
  rctx.residentBytes = parallel::residentBytesEstimate(session);
  rctx.scratchDir = session.workdir();
  parallel::WorkerBudget budget = parallel::resolveWorkers(opt.workers, rctx);

  std::string banner = "[piet_native] parallel: " + std::to_string(tasks.size()) + " tasks / " +
                       std::to_string(budget.n) + " workers";
  if (!budget.reason.empty()) banner += " (" + budget.reason + ")";
  // arc ② decides here whether this block actually fans out. Three reasons not
  // to, each NAMED in the banner rather than silently degrading (§10.9: a block
  // that quietly runs serially is how "why isn't it faster" becomes unanswerable):
  //   - budget resolved to one worker (nothing to fan out to);
  //   - we ARE a worker (PIET_PARALLEL_WORKER) — one generation only;
  //   - fan-out could not be started at all, decided below.
  const bool inWorker = [] {
    const char* e = std::getenv("PIET_PARALLEL_WORKER");
    return e != nullptr && *e != '\0';
  }();
  bool fanOut = budget.n > 1 && !inWorker && tasks.size() > 1;
  if (inWorker && budget.n > 1) banner += " (nested: serial)";

  // §2.4 — the dataset travels BY PATH. datastore() materializes a virtual or
  // dirty resident head to a real file first, so what the children open is the
  // parent's actual current data rather than whatever the last save left on
  // disk. The parent executes nothing while the block runs, so the scratch head
  // cannot be reclaimed out from under them.
  std::string parentHead;
  if (fanOut && session.hasData()) {
    try {
      session.datastore();
      if (session.currentHbin().has_value()) parentHead = *session.currentHbin();
    } catch (const std::exception&) {
      parentHead.clear();
    }
    if (parentHead.empty()) {
      fanOut = false;
      banner += " (resident head could not be snapshotted: serial)";
    }
  }

  std::vector<parallel::TaskOutcome> outcomes;
  if (fanOut) {
    static std::atomic<unsigned long long> blockSeq{0};
    const std::string scratchDir = session.workdir() + "/parallel_" +
                                   std::to_string(blockSeq.fetch_add(1));
    outcomes = parallel::runTasksInWorkers(tasks, session, budget, parentHead, scratchDir);
    if (outcomes.empty()) {
      fanOut = false;
      banner += " (fan-out unavailable: serial)";
    }
  }
  if (!fanOut) banner += " [serial]";
  std::fprintf(stderr, "%s\n", banner.c_str());

  // Opt-in spec dump — kept as the schema-pinning hook selftest uses.
  parallel::dumpTaskSpecIfRequested(
      parallel::buildTaskSpec(tasks, session, budget, parentHead));

  ojson out = ojson::array();
  bool truncated = false;  // P7 storage cap — same discipline as every loop body
  int nFailed = 0;
  std::optional<std::string> control;
  // ---- arc ② replay: SOURCE order, whatever order the workers finished in ----
  if (fanOut) {
    std::optional<int> exitCode;
    size_t replayEnd = tasks.size();
    for (size_t i = 0; i < outcomes.size(); ++i) {
      if (outcomes[i].hasExitCode) {
        // §2.6 exit promotion: the earliest task (in SOURCE order) that exited
        // decides. Later tasks may already have finished; their OUTPUT is
        // discarded, which is what "the do-file stopped there" has to mean.
        exitCode = outcomes[i].exitCode;
        replayEnd = i + 1;
        break;
      }
    }
    for (size_t i = 0; i < replayEnd; ++i) {
      parallel::TaskOutcome& o = outcomes[i];
      if (!o.delivered) {
        // A worker that died owes an honest block, never silence (§2.6).
        ojson eb = errBlock("parallel: task " + std::to_string(i) + " did not complete (" +
                            (o.driverError.empty() ? std::string("worker died") : o.driverError) +
                            ")");
        eb["captured"] = true;
        pushLoopBlock(out, truncated, eb);
        ++nFailed;
        continue;
      }
      // Same per-task varlist thinning the serial leg does — a task's output is
      // what it would have been as a top-level statement.
      doexec::coalesceMutationVarlists(o.blocks);
      // G-4: fold this task's additive counters into the parent's own
      // accumulators BEFORE its blocks are replayed, so the env-gated emit at
      // the end of the run reports the whole run's work rather than only the
      // part that happened in this address space. Only the additive set folds
      // (counters.h::foldableAccumulators explains what is excluded and why);
      // error/graph counts are left to the parent's own replay so they are
      // counted exactly once.
      for (auto it = o.countersDelta.begin(); it != o.countersDelta.end(); ++it) {
        if (!it.value().is_number_integer()) continue;
        for (const auto& [name, slot] : piet::counters::foldableAccumulators()) {
          if (it.key() == name) { *slot += it.value().get<int64_t>(); break; }
        }
      }
      // G-2: the log. In every other path a statement's blocks reach an open log
      // because the OUTERMOST dispatch tees them on the way back. Replayed
      // blocks never pass through dispatch — they were produced in another
      // process, and a child is never given the parent's log (log state is
      // control, and two writers on one file is not a thing to attempt). So the
      // replay has to tee them itself, once per task, in source order. Without
      // this line the parallel run's log is missing every task's output while
      // its stdout is byte-perfect — which is precisely the split G-2 exists to
      // catch.
      session.logTee(o.blocks, 1);
      for (auto& b : o.blocks) {
        if (o.failed && b.value("type", "") == "error" && !b.value("captured", false)) {
          ojson tagged = b;
          tagged["captured"] = true;
          pushLoopBlock(out, truncated, tagged);
          continue;
        }
        pushLoopBlock(out, truncated, b);
      }
      if (o.failed) ++nFailed;
    }
    session.eval.locals.set("_rc", nFailed > 0 ? "1" : "0");
    session.eval.rValues.set("n_failed", static_cast<double>(nFailed));
    if (exitCode.has_value()) {
      // Re-raise the halt through the SAME sentinel the serial paths use, so
      // doexec's uncaptured-error rule promotes it to a clean parent halt with
      // this code (BUG-139's message shape).
      out.push_back(errBlock(*exitCode == 0 ? "exit" : "exit " + std::to_string(*exitCode)));
      if (replayEnd < tasks.size())
        std::fprintf(stderr,
                     "[piet_native] parallel: exit at task %zu — output of tasks %zu..%zu "
                     "discarded; completed tasks' file side effects were not rolled back\n",
                     replayEnd - 1, replayEnd, tasks.size() - 1);
    }
    return out;
  }

  for (const auto& task : tasks) {
    std::vector<std::string> taskLines;
    {
      std::istringstream iss(task);
      std::string l;
      while (std::getline(iss, l)) taskLines.push_back(l);
    }
    BodyRunResult r = runBody(taskLines, session, dispatchFn);
    // §2.5 byte law — one `varlist_after` card per TASK, not per block.
    // executeDoSource applies this thinning per top-level item, and a task is
    // exactly what a top-level item would have been had the wrapper not been
    // written; without this call the whole block shares one card and tasks
    // 1..K-1 silently lose theirs. cmp(A1,AN) cannot see that (both arms lose
    // them identically) — the probe's wrapper-less A0 arm is what caught it.
    doexec::coalesceMutationVarlists(r.blocks);
    for (auto& b : r.blocks) {
      // §2.6 failure isolation, the half that per-task dispatch alone does NOT
      // buy: a task's uncaught error block would otherwise escape the block
      // untagged and trip doexec's top-level halt rule (type=="error" &&
      // !captured), so the do-file would stop at the parallel block even though
      // the remaining tasks already ran. Measured on the first build: task 2
      // ran, and `stopped_at_line` was still set.
      //
      // The error stays VISIBLE (honest-error doctrine — it is pushed, not
      // swallowed); `captured` is this codebase's existing "display-only, does
      // not halt" tag, the same one the dispatcher's capture arm sets (BS-127),
      // and it is the right one here because a parallel block reports failure
      // the way capture does: through `_rc` (+ r(n_failed) below), not by
      // halting. A caller that wants the old halt behaviour writes
      // `if _rc { exit _rc }` after the block.
      //
      // ⚠ ARC ② OWED — `exit` inside a task. §2.6 requires a task's clean
      // `exit`/`exit N` to be PROMOTED to a parent halt (stop dispatching, let
      // running tasks finish, replay in source order, then halt). runBody drops
      // ExecuteResult::stoppedAtExitCode, so this arc cannot distinguish a
      // clean exit from a failure and treats both as a non-halting task
      // failure. Arc ② must widen BodyRunResult with that field — the child
      // protocol already carries it (`exit_code` in parallel.cpp's worker
      // result), so the parent half is what is missing, not the wire.
      if (r.hasError && b.value("type", "") == "error" && !b.value("captured", false)) {
        ojson tagged = b;
        tagged["captured"] = true;
        pushLoopBlock(out, truncated, tagged);
        continue;
      }
      pushLoopBlock(out, truncated, b);
    }
    if (r.hasError) ++nFailed;  // §2.6: counted, but the block keeps going
    if (r.control.has_value()) {
      // A break/continue that bubbled out of a task belongs to an ENCLOSING
      // loop, so it must not be swallowed — and no further task may run once
      // it fired (that is what break/continue mean).
      control = r.control;
      break;
    }
  }

  // §8 #4 ruling (c): the parent learns the failure count and a coarse _rc.
  // "1 on any failure, 0 otherwise" is the same convention runCaptureBlock uses
  // — an exact numeric rc taxonomy is BS-100/A4 everywhere in this codebase,
  // not something this block invents for itself.
  session.eval.locals.set("_rc", nFailed > 0 ? "1" : "0");
  session.eval.rValues.set("n_failed", static_cast<double>(nFailed));

  if (control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *control;
    out.push_back(ctrl);
  }
  return out;
}

}  // namespace loops_detail

}  // namespace piet::loops
