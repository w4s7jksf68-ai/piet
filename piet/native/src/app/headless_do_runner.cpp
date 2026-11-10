// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// --do: headless do-file runner, transcript JSON, env-gated counters/timing emit.
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

// --do <file> [args...] — headless acceptance runner (Eval / GA acceptance).
// Runs a full .do file through the SAME native do-executor the live UI's Run
// button uses (doexec::executeDo -> executeDoSource, progressOwner=true), with
// NO window and NO worker thread — synchronous, deterministic. Emits one JSON
// object {blocks, stopped_at_line, stopped_at_reason, resolved_path} to stdout
// (redirect to a file; blocks can be large), plus a trailing `counters` key
// ONLY when $PIET_EMIT_COUNTERS is set non-empty (see the emit site below).
// session.cwd is seeded to the
// INVOKING SHELL's own process cwd (below) so its relative paths (`use
// "foo.dta"`, `cd`, tempfiles) resolve exactly as a real run would — a
// kitchensink run invoked as `piet_native --do debug_all.do`
// from the repo root keeps resolving `use "fatalities.csv"`-style relative
// paths against the repo root, not the .do file's own directory. Exit 0 iff
// the file ran to completion with no stopped_at_reason, OR stopped on a
// clean Stata `exit`/`exit, clear` (BUG-139 — `exit` halts via the same
// untagged-error channel a real failure uses, but it isn't one; a bare
// `exit`/`exit, clear` returns 0, `exit n` returns n). Every other
// stopped_at_reason still returns 1. This is the FAST + FAITHFUL GA-target
// path: it exercises the shipped C++ core (in-memory strmutate, no per-op
// dataset round-trip), unlike the legacy Python-backend accept_run.py.
int runDoFileHeadless(const std::string& filepath, const std::vector<std::string>& args) {
  // Scratch workdir = an internal mkdtemp (makeWorkdir honours $PIET_WORKDIR,
  // else /tmp) — the SAME choice the real .app makes (runRealFrontend), NOT
  // the invoking shell's cwd. Data/result paths resolve against session.cwd
  // (seeded just below) + the .do file's own `cd`, independent of this
  // scratch dir; the per-mutation hbin round-trips (loaded.hbin +
  // gen_*/egen_*/...) therefore land on fast local storage instead of
  // wherever the script happens to sit.
  // BUG-346 — the user's own settings apply here too. A worker parked under a
  // parent scratch dir on a cramped boot volume is how a corpus run loses two
  // thirds of its estimations to "disk full" while reporting success.
  (void)applyPersistedSettings();

  std::string workdir = makeWorkdir();
  std::fprintf(stderr, "[piet_native] --do scratch workdir: %s\n", workdir.c_str());

  ScratchDirGuard scratchGuard{workdir};  // reaped at process exit (PIET-TMP/BUG-092)
  AppState app(workdir);
  // Seed cwd = getcwd() of the invoking shell (NOT defaultUserCwd() —
  // --do is a CLI/CI entry point, not a Finder launch, so the caller's own
  // cwd is the right base; keeps kitchensink runs invoked from the repo
  // root working exactly as they did before cwd was ever seeded).
  {
    std::error_code ec;
    std::filesystem::path invokerCwd = std::filesystem::current_path(ec);
    if (!ec) {
      app.session.cwd = invokerCwd.u8string();
      app.session.eval.cwd = *app.session.cwd;
    }
  }
  nlohmann::ordered_json out = nlohmann::ordered_json::object();
  // BUG-139: set only when the run stopped on a clean `exit`/`exit N`
  // (never for a real failure) — see doexec.h's stoppedAtExitCode doc.
  std::optional<int> exitRequestedCode;
  try {
    piet::doexec::ExecuteFileResult fr =
        piet::doexec::executeDo(filepath, app.session, piet::dispatch, args);
    out["blocks"] = fr.result.blocks;
    if (fr.result.stoppedAtLine.has_value()) out["stopped_at_line"] = *fr.result.stoppedAtLine;
    if (fr.result.stoppedAtReason.has_value()) out["stopped_at_reason"] = *fr.result.stoppedAtReason;
    out["resolved_path"] = fr.resolvedPath;
    exitRequestedCode = fr.result.stoppedAtExitCode;
  } catch (const std::exception& e) {
    out["blocks"] = nlohmann::ordered_json::array();
    nlohmann::ordered_json errBlock;
    errBlock["type"] = "error";
    errBlock["code"] = "engine_error";
    errBlock["message"] = std::string("run failed: ") + e.what();
    out["blocks"].push_back(errBlock);
    out["stopped_at_reason"] = std::string("cpp_exception: ") + e.what();
  }
  // BUG-350: say plainly, on STDERR, that this run hid something. A plain
  // `capture` is supposed to suppress the block — that is Stata — but a run that
  // swallowed a disk-full and then reported 47/47 artifacts and exit 0 is the
  // count-passes-while-output-degrades family (BUG-340's sibling), and the only
  // trace was one line inside a 23-minute log.
  //
  // stderr, not the JSON: stdout is compared byte-for-byte by 163 freeze
  // fixtures, and a new key there would diff every one of them for a message
  // that concerns the OPERATOR, not the transcript. A clean run prints nothing.
  if (piet::counters::swallowedErrors() > 0) {
    std::fprintf(stderr,
                 "[piet_native] NOTE: %lld error(s) were captured and hidden by `capture` during "
                 "this run — the run's own output cannot show them. First: %s\n",
                 static_cast<long long>(piet::counters::swallowedErrors()),
                 piet::counters::firstSwallowedError().c_str());
  }
  // OVERHEAD-ZERO S5 (per-command floor gate): the mechanism counters the gate
  // reads — "did this verb re-open the dataset / rebuild its card at all" —
  // are process-wide statics whose only consumer was selftest.cpp, i.e. not
  // reachable from outside this process. Surface them, but ONLY under an
  // explicit opt-in: an unconditional key would change --do stdout for EVERY
  // run, diffing all captured freeze fixtures at once (freeze_matrix.py
  // --check). Appended last, after the try/catch, so key order is unchanged
  // and a run that threw still reports its counters.
  // T6 PIET-SIDE-SPLIT: per-verb {count, wall_ms, engine_ms} for one --do run.
  // Its OWN env gate (not PIET_EMIT_COUNTERS) so no existing consumer of the
  // counters key — floor_gate, freeze captures, the campaign's own A/B arms —
  // sees a single new byte. piet_ms = wall_ms - engine_ms is the number the
  // campaign was missing: the ~12 minutes that were not the engine.
  if (const char* et = std::getenv("PIET_EMIT_TIMING"); et && *et) {
    nlohmann::ordered_json timing = nlohmann::ordered_json::object();
    for (const auto& [verb, v] : piet::counters::verbTimings()) {
      nlohmann::ordered_json row = nlohmann::ordered_json::object();
      row["n"] = v.count;
      row["wall_ms"] = v.wallNs / 1e6;
      row["engine_ms"] = v.engineNs / 1e6;
      row["piet_ms"] = (v.wallNs - v.engineNs) / 1e6;
      timing[verb] = row;
    }
    out["timing"] = timing;
    out["engine_wait_ms_total"] = piet::counters::engineWaitNs() / 1e6;
    // T7: is the row evaluator's SLOT-COMPILED fast path actually engaging?
    // A nonzero count means rows went through the legacy per-row namespace
    // rebind instead (datamgmt/row_evaluation.cpp) — the only signal that distinguishes the
    // two, since both produce identical answers. Same report-only env gate.
    out["evalrows_map_writes"] = piet::strmutate::evalRowsMapWriteCount();
    // T7b ROWLOOP-VECTOR-SCOPING: the PIET-side half of the string gen/replace
    // path, split four ways (counters.h StrmutPhases). §T7 left exactly this
    // open — "compiled row loop vs the 108k-row CSV hand-off, which dominates
    // the ~45 ms/call" — and could not answer it because piet_ms was one lump.
    // Appended last on the same env gate, so no already-captured fixture and no
    // existing timing consumer sees a byte.
    {
      const auto& sp = piet::counters::strmutPhases();
      nlohmann::ordered_json ph = nlohmann::ordered_json::object();
      ph["evalrows_ms"] = sp.evalrowsNs / 1e6;
      ph["outbuild_ms"] = sp.outbuildNs / 1e6;
      ph["csvwrite_ms"] = sp.csvwriteNs / 1e6;
      ph["reload_ms"] = sp.reloadNs / 1e6;
      // Partition of reload_ms — three unrelated costs with three unrelated
      // fixes; the first probe put ~55% of the PIET half in reload alone.
      ph["rl_load_ms"] = sp.rlLoadNs / 1e6;
      ph["rl_nmiss_ms"] = sp.rlNmissNs / 1e6;
      ph["rl_varlist_ms"] = sp.rlVarlistNs / 1e6;
      // ROWLOOP-RLLOAD-SUBSPLIT: partition of rl_load_ms itself — the dominant
      // ~53% term, opened. Three named owners plus the previous frame's munmap;
      // rl_load - (these four) is the residual completeness check.
      ph["rll_materialize_ms"] = sp.rllMaterializeNs / 1e6;
      ph["rll_open_ms"] = sp.rllOpenNs / 1e6;
      ph["rll_reclaim_ms"] = sp.rllReclaimNs / 1e6;
      ph["rll_close_ms"] = sp.rllCloseNs / 1e6;
      ph["rll_materialize_calls"] = sp.rllMaterializeCalls;
      ph["rll_open_calls"] = sp.rllOpenCalls;
      ph["writenew_calls"] = sp.calls;
      ph["evalrows_calls"] = sp.evalrowsCalls;
      ph["rows"] = sp.rows;
      out["strmutate_phases"] = ph;
    }
    // ATTR-20260812. `timing` above mixes nesting levels (a loop header's row
    // and its body's rows are both present, and the header's wall INCLUDES the
    // body), which is why p0_landings §T6 had to write "행 합산 금지" beside it.
    // These two keys separate the levels instead of warning about them, and are
    // appended LAST on higher levels of the same gate so nothing already
    // captured moves.
    const int __lvl = piet::counters::timingLevel();
    if (__lvl >= 2) {
      // Depth-1 items, in execution order. THESE SUM: each root item appears
      // once, its wall inclusive of whatever it ran. `line` is the item index
      // within the root do-source (1-based, brace blocks count as one item).
      nlohmann::ordered_json items = nlohmann::ordered_json::array();
      for (const auto& r : piet::counters::itemRecords()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["line"] = r.line;
        row["verb"] = r.verb;
        row["text"] = r.text;
        row["wall_ms"] = r.wallNs / 1e6;
        row["engine_ms"] = r.engineNs / 1e6;
        items.push_back(row);
      }
      out["timing_items"] = items;
    }
    if (__lvl >= 3) {
      // Depth>=2 items — inside loop bodies, program bodies and replayed do
      // files — verb-summed. Never add these to timing_items: they are the same
      // seconds, told one level down.
      nlohmann::ordered_json nested = nlohmann::ordered_json::object();
      for (const auto& [verb, v] : piet::counters::nestedTimings()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["n"] = v.count;
        row["wall_ms"] = v.wallNs / 1e6;
        row["engine_ms"] = v.engineNs / 1e6;
        row["piet_ms"] = (v.wallNs - v.engineNs) / 1e6;
        nested[verb] = row;
      }
      out["timing_nested"] = nested;
      // Exclusive (self) time per verb: this item's wall minus its children's.
      // The one view whose rows SUM to the run, and the only one that
      // decomposes a corpus whose entire top level is a single program call.
      nlohmann::ordered_json excl = nlohmann::ordered_json::object();
      for (const auto& [verb, v] : piet::counters::exclusiveTimings()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["n"] = v.count;
        row["self_wall_ms"] = v.wallNs / 1e6;
        row["self_engine_ms"] = v.engineNs / 1e6;
        row["self_piet_ms"] = (v.wallNs - v.engineNs) / 1e6;
        excl[verb] = row;
      }
      out["timing_exclusive"] = excl;
    }
    // INSPECT-2608, level 4: the per-FORM table that pairs wall with BYTES.
    // Every row is EXCLUSIVE (self) — the child-bucket subtraction the level-3
    // block above uses — so the rows SUM to the run. `sums_to_run` announces
    // that property rather than leaving a reader to guess which of the timing
    // views may be added up; the completeness invariant is asserted in selftest.
    if (__lvl >= 4) {
      const auto& acc = piet::counters::foldableAccumulators();
      nlohmann::ordered_json rows = nlohmann::ordered_json::object();
      for (const auto& [form, r] : piet::counters::percmdRows()) {
        nlohmann::ordered_json row = nlohmann::ordered_json::object();
        row["n"] = r.n;
        row["self_wall_ms"] = r.selfWallNs / 1e6;
        row["self_engine_ms"] = r.selfEngineNs / 1e6;
        for (size_t i = 0; i < acc.size(); ++i)
          row[acc[i].first] = i < r.self.size() ? r.self[i] : 0;
        rows[form] = row;
      }
      nlohmann::ordered_json pc = nlohmann::ordered_json::object();
      pc["rows"] = rows;
      pc["sums_to_run"] = true;
      out["percmd"] = pc;
    }
  }
  // BUG-381: the scratch census, taken HERE — the last moment the workdir still
  // exists (ScratchDirGuard reaps it on the way out). Both peak runs put their
  // PEAK sample at the very END of the run, so a census at exit is a census of
  // the peak's composition. Its own env gate, like the two above it, so no
  // captured freeze fixture and no existing counters consumer sees a byte.
  if (const char* sc = std::getenv("PIET_SCRATCH_CENSUS"); sc && *sc) {
    out["scratch_census"] = nlohmann::ordered_json::parse(app.session.scratchCensusJson());
  }
  if (const char* emit = std::getenv("PIET_EMIT_COUNTERS"); emit && *emit) {
    nlohmann::ordered_json counters = nlohmann::ordered_json::object();
    counters["datastore_constructs"] = piet::Datastore::constructCount();
    counters["card_folds"] = piet::SessionState::cardFoldCount();
    counters["postest_hits"] = piet::CobraSession::postestHitCount();
    counters["resident_live_count"] = piet::SessionState::residentLiveCount();
    // FLOOR metric ① (A1-METRICS-RATIFICATION §1.5/§7) — the byte half. Three
    // more deterministic integers on the SAME opt-in seam, added last so key
    // order is append-only and every already-captured freeze fixture stays
    // byte-identical (the emit is env-gated, so with the var absent nothing
    // here reaches stdout at all).
    //   touched_bytes  — payload bytes the resident plane moved (counters.h's
    //                    four site classes). Numerator of amp = touched/semantic.
    //   snapshot_count — direction `max`: snapshot is efficient PER OP, so its
    //                    defect is the count, not the ratio (A1 §1.3).
    //   setcol_bytes   — the `semantic_zero` class: a ratio against a zero
    //                    denominator is undefined, not infinite, so setcol-shaped
    //                    verbs leave ① and get an ABSOLUTE byte budget here.
    counters["touched_bytes"] = piet::counters::touchedBytes();
    counters["snapshot_count"] = piet::counters::snapshotCount();
    counters["setcol_bytes"] = piet::counters::setcolBytes();
    // FLOOR metric ② DENOMINATOR (A1-METRICS §2.2 · B2 NAMED-OWED ③ ·
    // PHASE-B-DECISION-MEMO §6-3) — the honest "live named artifacts" byte
    // count a2_seqbench's ws ratio needs, replacing its input-.dta-size
    // stand-in. Report-only (NO floors.json budget — Eval pen): a SNAPSHOT of
    // end-of-run session state, appended last so key order stays append-only
    // and, being inside this env-gated block, every already-captured freeze
    // fixture stays byte-identical.
    // FIX-C4COUNTER-SAMPLERSPAN/BUG-142: through the NEUTRAL wrapper, not bare
    // liveNamedBytes(). This walk may materialise a dirty head (a real
    // frameSnapshot — desirable, the value stays accurate), and several
    // counters above (datastore_constructs/touched_bytes/snapshot_count) are
    // captured BEFORE this line while snapshot_bytes is captured AFTER it —
    // a bare walk half-records its own closure into whichever keys are read
    // later (measured: snapshot_bytes +67968 with snapshot_count 0, and a
    // trace line attributed to the last dispatched verb). The neutral wrapper
    // makes capture order irrelevant and keeps trace==snapshot_count exact.
    const int64_t endNamedBytes = app.session.liveNamedBytesEndSnapshot();
    counters["live_named_bytes"] = endNamedBytes;
    // FLOOR metric ② DENOMINATOR — the RATIFIED peak-vs-peak value
    // (PHASE-B-DECISION-MEMO §6-3 ADDENDUM, 2026-07-23): the running high-water
    // max of liveNamedBytes() over the run's top-level items (doexec.cpp),
    // folded with the end snapshot so a run that never dropped its frame still
    // reports its own end state as the peak. THIS — not `live_named_bytes`
    // above — is a2_seqbench's ② denominator: a corpus that ends `exit, clear`
    // (a2_mini) drops the frame before this emit, so the end snapshot
    // under-counts while peak RSS occurred with the frame resident. Appended
    // last (append-only key order → captured freeze fixtures stay byte-identical
    // env-off). Report-only (NO floors.json budget — Eval pen).
    piet::counters::updateLiveNamedBytesPeak(endNamedBytes);
    counters["live_named_bytes_peak"] = piet::counters::liveNamedBytesPeak();
    // C4-L1 + PP-L1 (C4-RESIDENT-READ-SCOPE-brief §6 · PINGPONG-SCOPE-brief
    // §5) — four more deterministic integers, appended last on the same
    // env-gated seam (append-only key order; env-off stdout unchanged, so all
    // captured freeze fixtures stay byte-identical).
    //   snapshot_bytes   — byte half of the SNAPSHOT class: settles the §2.2
    //                      upper bound (95 × peak-frame) into a measured share
    //                      of touched_bytes.
    //   use_disk_loads / use_virtual_hits / dta_converts — runUse branch split:
    //                      closes the PINGPONG §1 disk-vs-virtual contradiction
    //                      and prices K1 (use conversion/load cache) honestly.
    //   use_stash_hits    — PP-L2B / K1b: re-`use`s that restored the worker's
    //                      resident stash (no disk open at all). Complements
    //                      use_disk_loads: the win is disk-loads falling as this
    //                      rises across a sectioning corpus's repeat `use`s.
    counters["snapshot_bytes"] = piet::counters::snapshotBytes();
    counters["use_disk_loads"] = piet::counters::useDiskLoads();
    counters["use_virtual_hits"] = piet::counters::useVirtualHits();
    counters["dta_converts"] = piet::counters::dtaConverts();
    counters["use_stash_hits"] = piet::counters::useStashHits();
    // W0 (P2PERF-WAVE-PLAN §W0) — the ENGINE ROUND-TRIP ledger, split by request
    // shape. Appended last, same env-gated seam (append-only key order → every
    // captured freeze fixture stays byte-identical with the var absent).
    //
    // The campaign's opening hypothesis was that by-group work paid a process
    // SPAWN per group; the code says otherwise (a persistent worker is the
    // default plane). These keys make that checkable instead of argued: a
    // stateless round trip re-reads --data from disk and writes --out, a
    // frame_run touches no file at all, and `one_shot_spawns` is 0 unless the
    // worker plane was unavailable. A rung that claims to remove engine work
    // must move one of these integers.
    nlohmann::ordered_json ereq = nlohmann::ordered_json::object();
    ereq["stateless"] = piet::counters::engineReqStateless();
    ereq["frame_use"] = piet::counters::engineReqFrameUse();
    ereq["frame_run"] = piet::counters::engineReqFrameRun();
    ereq["snapshot"] = piet::counters::engineReqSnapshot();
    ereq["postest"] = piet::counters::engineReqPostest();
    ereq["other"] = piet::counters::engineReqOther();
    ereq["total"] = piet::counters::engineReqTotal();
    counters["engine_requests"] = ereq;
    counters["one_shot_spawns"] = piet::counters::oneShotSpawns();
    counters["serve_spawns"] = piet::counters::serveSpawns();
    // Self-reload witness (counters.h) — a `use` that re-materialized off disk
    // a file THIS session had just written with `save`. Appended last, same
    // env-gated seam (append-only key order → every captured freeze fixture
    // stays byte-identical with the var absent).
    counters["datastore_reloads"] = piet::counters::datastoreReloads();
    // The engine's own mechanism counters, fetched from the live worker with one
    // observational request (see CobraSession::frameCounters). Absent — not
    // zero — when this run never spawned a worker or the engine predates the op:
    // an all-zero block would read as "no full rewrites happened", which is a
    // different and much more flattering claim than "not measured".
    if (piet::CobraSession* cs = app.session.cobraSessionIfLive()) {
      piet::CobraRunResult ec = cs->frameCounters();
      if (ec.ok && !ec.stdout_text.empty()) {
        try {
          counters["engine"] = nlohmann::ordered_json::parse(ec.stdout_text);
        } catch (const nlohmann::json::exception&) {
          counters["engine"] = "unparseable";
        }
      } else {
        counters["engine"] = "unavailable";
      }
    } else {
      counters["engine"] = "no-worker";
    }
    // ATTR-20260812 coverage fingerprint. The 08-12 bar-grade record had to
    // grep an append-mode log that held every run since July for these two, and
    // the graph number could not be re-derived from it afterwards. Counted per
    // run at the emission point instead. Appended last, after `engine`, so no
    // captured counters block moves.
    counters["graph_emissions"] = piet::counters::graphEmissions();
    counters["error_blocks"] = piet::counters::errorBlocks();
    out["counters"] = counters;
  }
  // BUG-1372: a split UTF-8 sequence from a byte-based string function is
  // written as U+FFFD, as the GUI path sends it (runDoInBackground above); so
  // is a byte that is not UTF-8 (`type` of a Latin-1 file, BUG-1131).
  std::printf("%s\n",
              out.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace).c_str());
  // BUG-124 (silent-death mode): with stdout redirected to a file on a FULL
  // disk, the buffered transcript is lost at exit-time flush and the run ends
  // exit 1 with an EMPTY stdout and no diagnostic anywhere (observed on the
  // fair-run: 8.4s, empty transcript, only the workdir line on stderr). Flush
  // NOW, and if the transcript could not be written, say so on stderr (tiny,
  // unbuffered — lands even when the big transcript can't) and fail the run:
  // a lost transcript must never look like a clean exit to CI.
  if (std::fflush(stdout) != 0 || std::ferror(stdout)) {
    std::fprintf(stderr,
                 "[piet_native] FATAL: transcript write to stdout failed "
                 "(disk full or I/O error) — run output lost\n");
    return 1;
  }
  // BUG-139: a clean `exit`/`exit N` halts via the same untagged-error
  // channel a real failure uses — honor the requested code instead of
  // collapsing every halt to rc 1.
  if (exitRequestedCode.has_value()) return *exitRequestedCode;
  return out.contains("stopped_at_reason") ? 1 : 0;
}

}  // namespace app_entry_detail
