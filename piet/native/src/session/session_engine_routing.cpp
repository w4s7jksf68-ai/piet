// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: engine-call routing (resident frame-run vs stateless vs one-shot), worker creation, failure recovery and rollback.
#include "piet/session_state.h"
#ifndef _WIN32
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>   // C4-L1 window trace (stderr fprintf, env-gated)
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>  // std::greater<> (BUG-381 census sort)
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "piet/bcal.h"  // bcal::clearCache (resetProcessCaches)
#include "piet/cclass.h"  // BUG-1006: resolveCValue (macroCValue)
#include "piet/counters.h"
#include "piet/eval/errors.h"  // eval::coarseRc (the r(N) of a logged error)
#include "piet/eval/macros.h"  // eScalarKey (eNameOrderFor)
#include "piet/display_hardening.h"  // BUG-718: readVarObservation (macroVarObs)
#include "piet/scratch_seq.h"  // BUG-389: scratchMintSeq (sweep throttle)
#include "piet/storage_type.h"  // stataDefaultFormat (displayFormat)
#include "session_resident_internal.h"

namespace piet {
using namespace session_detail;

// ===========================================================================
// MAXPERF Stage 1b: persistent `cobra serve` worker intercept + resident-frame
// data plane. See session_state.h for the field contract and cobra_session.h
// for the wire protocol. All of this is inert (residentToken_ stays empty,
// tryResidentMutationCard returns nullopt) until the first successful frame op,
// so a §25-only engine, PIET_NO_SERVE, or any non-dispatch call path stays
// byte-identical to the pre-Stage-1b one-shot behavior.
// ===========================================================================
namespace session_detail {
bool serveDisabled() {
  static const bool off = [] {
    const char* e = std::getenv("PIET_NO_SERVE");
    return e && *e;
  }();
  return off;
}
}  // namespace session_detail

// C4-L2A escape hatch (doc in session_state.h). Per-call getenv on purpose —
// the selftest A/B leg setenv/unsetenv's it around single dispatches, which a
// serveDisabled()-style static cache would silently ignore.
bool residentReadDisabled() {
  const char* e = std::getenv("PIET_NO_RESIDENT_READ");
  return e && *e;
}

CobraSession* SessionState::ensureCobraSession() {
  if (!cobraSession_) cobraSession_ = std::make_unique<CobraSession>(resolveCobraBin());
  return cobraSession_.get();
}

namespace session_detail {
// ROUTING / MATERIALIZATION CENSUS (PIET_ROUTELOG). One-time env read: both call
// sites are hot (952 eval routings on the reference slice alone), so the gate must
// be a predictable branch, not a getenv per call.
const char* routelog_path() {
  static const char* p = [] {
    const char* e = std::getenv("PIET_ROUTELOG");
    return (e && *e) ? e : nullptr;
  }();
  return p;
}
}  // namespace session_detail

CobraRunResult SessionState::rollbackToFloor(const std::vector<std::string>& argv) {
  (void)argv;
  releaseResidentToken("rollback-floor");
  shadowValid_ = false;
  pendingDeltaValid_ = false;
  datastore_.reset();
  if (!lastMaterializedHead_.empty()) currentHbin_ = lastMaterializedHead_;
  residentComputedColClear();  // BUG-916: the floor predates the cached column.
  CobraRunResult r;
  r.exit_code = 3;  // data_io convention — surfaces as an engine error, never a silent wrong result.
  r.ok = false;
  nlohmann::json err;
  err["error"] = {{"code", "engine_error"},
                  {"message", "engine session restarted; state = last materialized snapshot"}};
  r.stderr_text = err.dump();
  return r;
}

CobraRunResult SessionState::finishSessionOp(CobraRunResult r, const std::vector<std::string>& argv) {
  if (!r.error.empty() && r.exit_code < 0) {
    // Hard worker failure (unavailable / died twice). Preserve correctness: a
    // real head can be reproduced by a one-shot spawn; a lost virtual head
    // rolls back to the last on-disk snapshot with an honest restart error.
    if (residentDirty_) return rollbackToFloor(argv);
    releaseResidentToken("hard-failure-oneshot");
    return runCobraOneShot(argv);
  }
  if (cobraSession_ && cobraSession_->consumeRespawned()) {
    // Transient death + respawn mid-op: the resident frame is gone. NOTE
    // (BUG-384 audit): if the head was a dirty VIRTUAL token, its bytes died
    // with the worker — clearing here leaves an unbacked head, which the
    // dispatch-exit audit reports. Recovery policy is a separate decision;
    // this site only stops being silent.
    releaseResidentToken("worker-respawn");
    shadowValid_ = false;
    if (!r.ok) return rollbackToFloor(argv);
  }
  return r;
}

CobraRunResult SessionState::runViaCobraSession(const std::vector<std::string>& argv) {
  // lastRunResident_ describes THIS call, so every exit below must set it —
  // including the two early one-shot exits and the two stateless fallbacks a
  // resident-routed call can still take. A stale `true` would let an opted-in
  // caller skip its disk-full guard on a run that really did write a file.
  lastRunResident_ = false;
  if (serveDisabled()) return runCobraOneShot(argv);
  CobraSession* cs = ensureCobraSession();
  if (cs->serveUnavailable()) return runCobraOneShot(argv);

  // Settings-change respawn (CECON_NUM_THREADS): persist resident state before
  // the thread-count respawn, then let the next frame op resync.
  if (cs->epochStale()) {
    ensureMaterializedHead("epoch-respawn");
    cs->respawnForEpoch();
    releaseResidentToken("epoch-respawn");
    shadowValid_ = false;
  }

  // Locate --data / --out and detect --keepif.
  std::optional<std::string> dataPath, outPath;
  int dataIdx = -1, outIdx = -1;
  bool hasKeepif = false;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--keepif") {
      hasKeepif = true;
    } else if (i + 1 < argv.size() && argv[i] == "--data") {
      dataPath = argv[i + 1];
      dataIdx = static_cast<int>(i);
    } else if (i + 1 < argv.size() && argv[i] == "--out") {
      outPath = argv[i + 1];
      outIdx = static_cast<int>(i);
    }
  }

  // e(sample) T2: a read-only replay of the frame a fit read (LastModel::dataPath:
  // test/lincom/margins/estat) while the head is still that fit's mask-install
  // frame -- the same rows and values plus the hidden column -- reads the
  // resident head instead. Without it the replay resynced the worker to the base
  // file, writing the install frame to disk first ("resident-resync"), once per
  // fit. A --keepif or a by-group read keeps its stateless route (both read the
  // file); a mutation keeps its own --data.
  if (dataIdx >= 0 && !outPath.has_value() && !hasKeepif && statelessRead_ == 0 &&
      currentHbin_.has_value() && residentToken_ == *currentHbin_ &&
      esampleHeadIsInstallOf(*dataPath)) {
    std::vector<std::string> aliased = argv;
    aliased[static_cast<size_t>(dataIdx) + 1] = *currentHbin_;
    return runViaCobraSession(aliased);
  }
  // ... and the converse: a read-only call that will run STATELESS on that
  // install frame (a --keepif fit -- iv, xtreg, newey, prais, a TS regress --
  // or a by-group read) would write the whole frame to disk first
  // ("stateless-data"); it reads the on-disk base instead. Not when an argument
  // names the mask column, which only the install frame carries.
  if (dataIdx >= 0 && !outPath.has_value() && (hasKeepif || statelessRead_ > 0) &&
      currentHbin_.has_value() && *dataPath == *currentHbin_ && residentDirty_ &&
      residentToken_ == *currentHbin_ && esampleHeadIsInstallOf(esampleLastInstallBase)) {
    bool namesMask = false;
    for (const auto& s : argv) namesMask = namesMask || namesHiddenColumn(s);
    std::error_code bec;
    if (!namesMask &&
        std::filesystem::exists(cecon::to_native_path(esampleLastInstallBase), bec)) {
      std::vector<std::string> aliased = argv;
      aliased[static_cast<size_t>(dataIdx) + 1] = esampleLastInstallBase;
      return runViaCobraSession(aliased);
    }
  }

  CobraSession::Frames fr = cs->framesState();  // lazy-spawns the worker on first use.
  if (cs->serveUnavailable()) return runCobraOneShot(argv);  // old binary / spawn failed.
  const bool isMutation = outPath.has_value();

  // Route to the resident frame plane ONLY for read-only calls (no --out — no
  // temp file is created, so nothing can be read back as a stale/virtual file)
  // and for mutations explicitly opted in (gen/replace/egen/drop/keep — and, as
  // of S2, the if-only row filter). Everything else runs STATELESS (byte-equal
  // one-shot, real --out files):
  //   • no --data (version/convert/import-dta/postclose);
  //   • a §25-only engine (frames No);
  //   • every non-opted-in mutation (append/merge — whose exact stderr status
  //     counts are thus preserved — collapse/reshape/sort/duplicates/winsor/
  //     the egen-if/in + applyIfAndInFilter `in`-range chains, whose --out temps
  //     are read back AS FILES and so must stay real);
  //   • a READ-ONLY --keepif, which keeps its historical stateless routing.
  //
  // fix/verb-overhead-sweep S2: --keepif used to disqualify EVERY call, on the
  // strength of a §26 carve-out that says a resident frame-run cannot filter.
  // That text is stale — measured against the shipped 0.33.0 binary, bare
  // --keepif, --gen+--keepif and --drop+--keepif all filter correctly on a
  // resident frame and return in 58-96 ms against 527 ms stateless (the
  // stateless leg pays a full re-read of the on-disk frame plus a full --out
  // write; `drop if`/`keep if` on a 164 MB / 192-column frame was the single
  // largest verb cost in the campaign's attribution). The disqualification is
  // therefore lifted for the OPTED-IN MUTATION path only, and deliberately not
  // for read-only calls: a read-only --keepif writes no --out, so residency
  // would buy it nothing, while flipping ~9 call sites (summarize/regress/
  // tabulate/ttest/… via applyIfAndInFilter) onto a plane none of them asked
  // for. Opting in stays exactly what it has always meant — "this --out becomes
  // the session head and is never read back as a file" — so the caller, not
  // this router, remains the one place that fact is stated.
  // P2-PERF ARC-C C1-④: statelessRead_ > 0 is a caller saying "the --data I am
  // about to read is NOT my head, and I am about to say that G times" — see
  // SessionState::StatelessRead. Without it, a by-group loop's per-group reads
  // evict and resync the resident frame once per group.
  const bool goResident = fr != CobraSession::Frames::No && dataPath.has_value() &&
                          (isMutation ? residentEligible_ > 0 : (!hasKeepif && statelessRead_ == 0));

  // ROUTING CENSUS (env-gated, PIET_ROUTELOG). The resident plane's whole value is
  // that an opted-in mutation writes no full-frame --out: measured on the reference
  // slice, turning it OFF costs +28 such writes, +4.5 GB and +47 s of wall. But 196
  // writes REMAIN, so most calls are still leaving this router by the stateless
  // door and no existing counter says which door or why. One line per verb, with
  // the reason spelled the way the condition above reads.
  if (const char* rp = routelog_path()) {
    const char* why = goResident            ? "resident"
                    : fr == CobraSession::Frames::No ? "no-frames"
                    : !dataPath.has_value() ? "no-data"
                    : isMutation            ? "mutation-not-eligible"
                    : hasKeepif             ? "read-with-keepif"
                                            : "read-stateless-scope";
    if (std::FILE* f = std::fopen(rp, "a")) {
      std::string shape;
      for (size_t i = 1; i < argv.size(); ++i)
        if (argv[i].size() > 2 && argv[i][0] == '-' && argv[i][1] == '-')
          shape += (shape.empty() ? "" : ",") + argv[i];
      std::fprintf(f, "%s\t%s\t%s\n", argv.empty() ? "?" : argv[0].c_str(), why, shape.c_str());
      std::fclose(f);
    }
  }

  lastRunResident_ = goResident;
  if (!goResident) {
    // MUTRES-WIRE follow-up, 2026-08-22 — a claim of mine that measurement
    // refuted. The plan asserted pendingDelta_ could never be stale because it
    // is "overwritten wholesale at every mutation". It is not: it is written
    // ONLY on the resident leg (`r.ok && isMutation` below), so a STATELESS verb
    // left the previous resident run's varlist sitting there, and
    // mutationResult's wire branch would rebuild this frame's card from another
    // frame's columns. The completeness guard caught every one of them —
    // measured 115 wire-shape-mismatch events on a single corpus run, all of
    // them refusals that fell back correctly — but a guard catching a wrong
    // input is not a reason to keep feeding it one, and a stale list that
    // happened to have the RIGHT COLUMN COUNT would have passed.
    //
    // Cleared here, at the one place that knows this run is not producing a
    // delta: an empty vars is exactly what pendingWireDelta() reports as "no
    // wire available", so the caller falls back to the datastore path it always
    // used before.
    pendingDelta_.vars.clear();
    // Stateless. If --data is the CURRENT virtual head, snapshot it first — the
    // stateless worker reads --data from DISK (contract_A §25). (A read-only
    // --keepif, and every non-opted-in mutation, still resolve here: materialize
    // so the file reflects RAM, then run stateless with the original
    // --data/--out; clearing residentToken on an --out makes the next resident
    // verb frame-use the new real head.)
    if (dataPath.has_value() && *dataPath == residentToken_ && residentDirty_) {
      ensureMaterializedHead("stateless-data");
    }
    CobraRunResult r = cs->sendStateless(argv);
    // BUG-349 (ONESHOT-LASTFLUSH): this invalidation used to fire on EVERY stateless
    // mutation. Its stated reason — "a real --out was written, so the resident frame
    // is now stale" — only holds when the mutation READ the head: that is the case
    // where the branch above just materialized it and the new --out supersedes it.
    // A stateless mutation over some OTHER file (a §38 projection, a by-group slice)
    // leaves the worker's resident frame completely untouched; §25 stateless requests
    // never enter the frame session at all. Clearing the token there is not
    // conservative, it is destructive: it declares a DIRTY VIRTUAL head clean, and
    // the bytes that only ever existed in the worker are then unreachable — the next
    // reader gets `data_io: cannot open <the virtual head>`. Latent until now only
    // because every caller happened to filter the head itself.
    const bool consumedHead =
        dataPath.has_value() && !residentToken_.empty() && *dataPath == residentToken_;
    if (isMutation && consumedHead) {
      releaseResidentToken("stateless-consumed");  // a real --out was written; frame now stale.
    }
    return finishSessionOp(r, argv);
  }

  // Resident frame path. Resync (frame-use the REAL file) when the head moved
  // off the resident frame (fresh use/restore/import, or after a stateless
  // mutation). `*dataPath` is always a real file here.
  if (*dataPath != residentToken_) {
    // The worker's resident frame is about to be replaced by *dataPath. If the
    // OUTGOING resident frame is a dirty VIRTUAL head (its bytes exist only in
    // the worker), snapshot it to its currentHbin_ path FIRST — otherwise it is
    // lost, and a later verb whose --data is that (now orphaned) virtual head
    // frame-uses a file that was never written → data_io. This is the
    // post-estimation case: `gen x` (virtual head) then `test`/`margins`/
    // `lincom` re-run the base model with --data=lastModel.dataPath (a DIFFERENT
    // real file), resyncing away from the virtual head; the next data verb reads
    // currentHbin_ (still the gen head) and must find it on disk.
    ensureMaterializedHead("resident-resync");
    CobraFrameMeta useMeta;
    CobraRunResult ur = cs->frameUse(*dataPath, &useMeta);
    if (cs->framesState() == CobraSession::Frames::No) {
      lastRunResident_ = false;
      return finishSessionOp(cs->sendStateless(argv), argv);  // engine is §25-only.
    }
    if (!ur.ok) return finishSessionOp(ur, argv);  // bad file / dead worker.
    ++residentLiveCount_;  // worker accepted a frame — see residentLiveCount()
    residentToken_ = *dataPath;
    residentDirty_ = false;
    seedShadowFromFrameVars(useMeta);
    (void)cs->consumeRespawned();  // frameUse self-heals a transient respawn.
  }

  // frameRun with --data/--out stripped (the engine mutates the resident frame
  // in place, or reads it for a read-only verb; no scratch file is written).
  std::vector<std::string> inner;
  inner.reserve(argv.size());
  for (size_t i = 0; i < argv.size(); ++i) {
    int ii = static_cast<int>(i);
    // Guard on >= 0: a sentinel of -1 (flag absent) must NOT match index 0 via
    // `-1 + 1 == 0` — that would strip argv[0], the subcommand itself. Every
    // read verb has no --out (outIdx == -1), so the unguarded form silently
    // dropped the subcommand and the engine saw a headless argv (exit 2).
    if (dataIdx >= 0 && (ii == dataIdx || ii == dataIdx + 1)) continue;
    if (outIdx >= 0 && (ii == outIdx || ii == outIdx + 1)) continue;
    inner.push_back(argv[i]);
  }
  CobraFrameMeta runMeta;
  CobraRunResult r = cs->frameRun(inner, &runMeta);
  if (cs->framesState() == CobraSession::Frames::No) {
    lastRunResident_ = false;
    return finishSessionOp(cs->sendStateless(argv), argv);
  }
  if (r.ok && isMutation) {
    // Opted-in mutation succeeded: `out` is now a VIRTUAL head (no file). The
    // handler's imminent session.load(out) records the virtual chain; the
    // mutationResult shadow-card branch folds this delta.
    pendingDelta_ = runMeta;
    pendingDeltaValid_ = runMeta.valid;
    // S1b: pair the delta with what THIS argv asked the engine to do to the
    // column set/order (Kind::None for every verb but order/drop/keep). Kept
    // adjacent to the pendingDelta_ write on purpose — the two are consumed
    // together and must never drift apart.
    capturePendingCardPlan(argv);
    residentToken_ = *outPath;
    residentDirty_ = true;
    if (scratchTraceOn()) scratchTrace("MINT", "mutation-out", *outPath);
  }
  // Read-only (no --out) or exit!=0: resident token/dirty unchanged (the engine
  // guarantees no state advance on a non-zero frame run).
  return finishSessionOp(r, argv);
}

}  // namespace piet
