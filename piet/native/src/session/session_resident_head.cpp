// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: resident head transitions (worker sync, preserve/restore stash, virtual save/use, materialisation to disk).
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

// -- MAXPERF Stage 2 (§27 session snapshots) client wiring -------------------
// Shared precondition: the resident plane must be usable AND the worker must be
// holding the CURRENT head. Ensures both, returning false when the plane is
// unavailable (caller falls back to the file plane). A dirty virtual head is
// materialized first (crash-recovery floor — see residentPreserve's doc); this
// is a no-op on the common clean/real head, where the snapshot stays RAM-only.
bool SessionState::residentSyncWorkerToHead() {
  if (serveDisabled() || !currentHbin_.has_value()) return false;
  CobraSession* cs = ensureCobraSession();
  if (!cs || cs->serveUnavailable()) return false;
  if (cs->framesState() != CobraSession::Frames::Yes) return false;
  ensureMaterializedHead("resident-sync");  // dirty virtual head -> on-disk floor (else a no-op).
  if (residentToken_ != *currentHbin_) {
    // Worker isn't holding this head (fresh `use` before any compute, or a
    // prior stateless mutation). Frame-use the real file so the stash/save
    // snapshots exactly the current data.
    CobraFrameMeta useMeta;
    CobraRunResult ur = cs->frameUse(*currentHbin_, &useMeta);
    if (cs->framesState() == CobraSession::Frames::No || !ur.ok) return false;
    ++residentLiveCount_;  // worker accepted a frame — see residentLiveCount()
    residentToken_ = *currentHbin_;
    residentDirty_ = false;
    seedShadowFromFrameVars(useMeta);
    (void)cs->consumeRespawned();
  }
  return true;
}

std::optional<std::string> SessionState::residentPreserve() {
  if (!residentSyncWorkerToHead()) return std::nullopt;
  // BUG-384 (root, 2026-09-01). runPreserve's resident arm records
  // frame.hbinPath = currentHbin_ on the strength of this function's old
  // contract comment — "materialized the head just before stashing". That was
  // FALSE on the common path: residentSyncWorkerToHead materializes only in its
  // DIVERGENT branch (token != head, where a resync forces it); a dirty virtual
  // head whose token equals the head — the normal state right after any opted-in
  // mutation — passed straight through, and the preserve recorded a twin that
  // did not exist. The trace that convicted it, on the internal-volume corpus:
  //     MINT mutation-out strmut_19478
  //     PRESERVE(twin-MISSING) resident strmut_19478
  //     ...
  //     RESTORE(twin-MISSING) -> fallback mints restore_N -> cascade -> ENOENT
  // Externally the hole was invisible because the NEXT command's datastore()
  // render usually materialized the head a moment later, making the twin real
  // retroactively; when the card-fold fast path skips that render — a timing-
  // and machine-dependent choice — the twin never comes into existence. That is
  // why four external acceptance runs passed 47/47 while five internal runs
  // died at artifact 35-39.
  //
  // The twin is this function's promise, so this function keeps it: snapshot
  // the head to disk BEFORE stashing. A no-op when the head is already real; on
  // snapshot failure dirty stays set and we fall back to the clone plane, which
  // has its own materialize-and-verify and reports honestly (BUG-124 shape).
  ensureMaterializedHead("preserve-floor");
  if (residentDirty_) return std::nullopt;  // could not write the twin -> clone fallback
  std::string name = "__preserve_" + std::to_string(stashSeq_++) + "__";
  CobraFrameMeta meta;
  CobraRunResult r = cobraSession_->frameStash(name, &meta);
  if (!r.ok) return std::nullopt;  // §27-absent engine / worker death -> clone fallback.
  (void)cobraSession_->consumeRespawned();
  return name;
}

// PP-L2B / K1b: stash the current head under an EXPLICIT, stable name (the
// caller's useStashName(src)) for later cache restore. Mirrors residentPreserve
// but names the stash itself (re-stashing an unchanged name overwrites the
// worker-RAM frame in place — how a changed source frees its predecessor
// without a drop op). residentSyncWorkerToHead() also materializes a dirty
// virtual head first, so the hbinPath the caller records as the recovery floor
// is a real on-disk twin. false => resident plane unavailable: the caller does
// not memoize and the next `use` pays the ordinary disk path.
bool SessionState::useStashSnapshot(const std::string& name) {
  if (!residentSyncWorkerToHead()) return false;
  CobraFrameMeta meta;
  CobraRunResult r = cobraSession_->frameStash(name, &meta);
  if (!r.ok) return false;
  (void)cobraSession_->consumeRespawned();
  return true;
}

bool SessionState::residentRestore(const std::string& stashName,
                                   const std::string& preserveHbinPath, bool keepStash) {
  if (serveDisabled() || !cobraSession_) return false;
  CobraSession* cs = cobraSession_.get();
  if (cs->serveUnavailable() || cs->framesState() != CobraSession::Frames::Yes) return false;
  CobraFrameMeta meta;
  // keepStash=true (PP-L2B / K1b) retains the stash for the next re-`use`; the
  // engine hands back a CoW copy (columns shared with the retained snapshot
  // until a later mutation), so a restored-then-mutated dataset never corrupts
  // the cache. keepStash=false (preserve/restore LIFO) consumes it.
  CobraRunResult r = cs->frameUnstash(stashName, /*keep=*/keepStash, &meta);
  if (!r.ok || !meta.valid) return false;  // stash lost to a worker restart / §27-absent.
  // Either restore ABANDONS whatever scratch was the live head (a mutation
  // output made after the preserve). The disk path's load() reclaims its
  // superseded predecessor; this resident path has no load(), so it must say so
  // itself or the file is stranded for the life of the process.
  //
  // BUG-381: this was `keepStash && ...`, i.e. the cache-`use` path only. The
  // reason given was that "preserve/restore keeps its old head as part of the
  // LIFO" — and that is not so. The LIFO holds the preserve SNAPSHOT
  // (frame.hbinPath); the head at restore time is a mutation output made AFTER
  // the preserve, which nothing keeps. Measured cost of the gap on the
  // 47-artifact corpus: 1145 stranded files, 4.538 GiB, 96.7% of a scratch
  // high-water that no budget setting could touch (that is what "orphan" means
  // — a file no ledger knows about is one no reap can reach). The shape is
  // `preserve` -> mutate -> `save` -> `restore` in a loop, which run_one_depvar
  // does three times per dependent variable; 160 depvars gave exactly 480
  // stranded `rename_*` files.
  //
  // Safe for both values of keepStash for the reason the old comment already
  // gave: reclaimSupersededScratch's own exclusions spare a pinned twin, a live
  // preserve snapshot (including an OUTER frame's, under nesting), a frame, and
  // a user file — and the `!= preserveHbinPath` guard below spares the exact
  // file re-adopted as the head immediately after. A virtual (tokenized) head
  // names no file and is dropped by reclaim's is_regular_file test.
  if (currentHbin_.has_value() && *currentHbin_ != preserveHbinPath) {
    reclaimSupersededScratch(*currentHbin_, /*abandoningHead=*/true);
  }
  // The engine restored the snapshot as the resident. residentPreserve() took
  // the stash IMMEDIATELY after materializing the head, so the preserve-time
  // file it recorded is byte-identical to the restored frame by construction.
  // Re-adopt that file as a CLEAN real head: the card build after restore
  // re-mmaps the existing file with ZERO writes. (Minting a dirty virtual token
  // here instead forced datastore() to re-materialize the full frame on every
  // restore — a control-confirmed +7% batch regression vs the clone plane,
  // which re-mmapped its already-on-disk clonefile.) The file is GC-protected
  // while the preserve frame is live (reclaimSupersededScratch skips paths on
  // preserveStack), so intra-session it exists; the exists() check below is a
  // belt-and-braces fallback, never an error.
  std::error_code ec;
  if (!preserveHbinPath.empty() &&
      std::filesystem::exists(cecon::to_native_path(preserveHbinPath), ec)) {
    currentHbin_ = preserveHbinPath;
    residentToken_ = preserveHbinPath;
    residentDirty_ = false;
    lastMaterializedHead_ = preserveHbinPath;  // real on-disk head — the new crash floor
    // NOTE: deliberately no flushDeferredReclaim() here — under an eager scratch
    // budget it could unlink the head just adopted. The next materialization (or
    // real-file load) flushes as usual, exactly like the dirty-token path below.
  } else {
    // No on-disk twin survives (scratch GC'd — shouldn't happen intra-session).
    // Fall back: adopt the resident as a VIRTUAL head under a fresh token, dirty
    // so the first PIET-side datastore() snapshots it to disk on demand (exactly
    // the gen/replace virtual-head discipline).
    std::string token = workdir_ + "/restore_" + std::to_string(stashSeq_++) + ".hbin";
    currentHbin_ = token;
    residentToken_ = token;
    residentDirty_ = true;
    if (scratchTraceOn()) scratchTrace("MINT", "restore-fallback", token);
  }
  datastore_.reset();
  // BUG-916: the computed-column cache describes the chain this restore just
  // abandoned (a tostring/destring made after the preserve, or the frame a
  // cache `use` replaced). load() clears it on a real-file head; this path has
  // no load(), so it must clear it itself, or the next setcol verb and every
  // NAME[i] read are served the abandoned column (w2b_dataio.log:90-96).
  residentComputedColClear();
  datasetChanged = true;  // restore replaces the in-memory data (matches load()).
  seedShadowFromFrameVars(meta);
  (void)cs->consumeRespawned();
  return true;
}

void SessionState::discardTopPreserve() {
  if (preserveStack.empty()) return;
  const PreserveFrame& top = preserveStack.back();
  if (!top.stashName.empty() && cobraSession_ && cobraSession_->hasWorker())
    (void)cobraSession_->frameStashDrop(top.stashName);
  preserveStack.pop_back();
}

const PreserveFrame* SessionState::ownPreserveFrame() const {
  if (preserveStack.empty() || preserveStack.back().level != preserveLevel()) return nullptr;
  return &preserveStack.back();
}

bool SessionState::residentSaveVirtual(const std::string& path) {
  if (!residentSyncWorkerToHead()) return false;
  CobraFrameMeta meta;
  CobraRunResult r = cobraSession_->frameSaveVirtual(path, &meta);
  if (!r.ok) return false;
  (void)cobraSession_->consumeRespawned();
  virtualSaves_.insert(path);
  return true;
}

bool SessionState::loadFromVirtual(const std::string& path) {
  if (!isVirtualSave(path)) return false;
  if (serveDisabled() || !cobraSession_) return false;
  CobraSession* cs = cobraSession_.get();
  if (cs->serveUnavailable() || cs->framesState() != CobraSession::Frames::Yes) return false;
  CobraFrameMeta meta;
  // §27: `use` checks the virtual-file table FIRST — a registered path restores
  // from RAM in O(columns). The client calls frameUse(path) unchanged; the hit
  // is transparent (a MISS would fall through to a disk open, but this path is
  // only reached for a registered entry, so a MISS means the worker restarted
  // and lost the table -> return false, and runUse surfaces "File not found").
  CobraRunResult r = cs->frameUse(path, &meta);
  if (cs->framesState() == CobraSession::Frames::No || !r.ok || !meta.valid) return false;
  ++residentLiveCount_;  // worker accepted a frame — see residentLiveCount()
  // BUG-381: this fast path replaces the head WITHOUT going through load(), so
  // it must reclaim the predecessor itself — load()'s reclaim is the only one
  // there is, and a `use` that hits the §27 virtual table never reaches it.
  // Measured on one campaign group: `append using <tempfile>` made the head,
  // the next `use <tempfile>` came through here, and the append output was
  // stranded for the life of the process. Same class as the restore gap above:
  // every DIRECT `currentHbin_ =` is a place load()'s bookkeeping does not run.
  if (currentHbin_.has_value() && *currentHbin_ != path) {
    reclaimSupersededScratch(*currentHbin_, /*abandoningHead=*/true);
  }
  currentHbin_ = path;
  residentToken_ = path;
  residentDirty_ = true;  // no disk file yet; datastore() materializes on demand.
  if (scratchTraceOn()) scratchTrace("MINT", "resident-use", path);
  datastore_.reset();
  residentComputedColClear();  // BUG-916: a new frame — the old chain's cache is over.
  datasetChanged = false;  // a fresh `use` leaves c(changed) false (matches load+setCurrentFile).
  seedShadowFromFrameVars(meta);
  (void)cs->consumeRespawned();
  return true;
}

// BUG-125 (Wave-γ save_virtual follow-up, found during the BUG-124 sweep): a
// save_virtual'd tempfile lives ONLY in the worker's RAM table. `use` restores
// it via the virtual-first fast path, but the second-file readers (merge/
// joinby/append `using`) read the path from DISK — shipped B.5.0 answered
// "file not found" for a tempfile the script had just saved (observed live:
// analysis.do's opponent_map joinby, 2 captured error blocks on a clean-disk
// run of the group-01 slice). On-demand materialization: the first time a disk
// reader asks, restore the entry as the resident (frameUse hits the virtual
// table) and snapshot it to its real path — byte-equal by the §27 parity law.
// The worker is left holding the TEMPFILE frame, so residentToken_ is cleared
// and the next resident verb re-frame-uses the real head (ordinary resync).
bool SessionState::materializeVirtualSave(const std::string& path) {
  if (!isVirtualSave(path)) return false;
  std::error_code ec;
  if (std::filesystem::exists(cecon::to_native_path(path), ec)) return true;  // already real
  if (serveDisabled() || !cobraSession_) return false;
  CobraSession* cs = cobraSession_.get();
  if (cs->serveUnavailable() || cs->framesState() != CobraSession::Frames::Yes) return false;
  // Anchor the CURRENT head on disk first — the frameUse below replaces the
  // resident frame, and a dirty virtual head would otherwise orphan (BUG-123's
  // materialize-before-resync law).
  ensureMaterializedHead("vsave-anchor");
  // BUG-384: frameUse(path) below REPLACES the worker's resident frame. If the
  // ensure above could not put the current head on disk (snapshot failure, or a
  // cleared token making it skip), proceeding would destroy the only copy of
  // the head's bytes — the exact mechanism the first traced repro caught
  // (CLEAR(dirty-head) vsave-anchor on a restore-fallback token). Refuse
  // instead: the caller's false-path falls back to the stateless/disk route,
  // which is slower and honest rather than fast and destructive.
  if (residentDirty_ && currentHbin_.has_value() && residentToken_ == *currentHbin_) {
    if (scratchTraceOn()) scratchTrace("VSAVE-REFUSE(dirty-head)", "vsave-anchor", *currentHbin_);
    return false;
  }
  CobraFrameMeta meta;
  CobraRunResult ur = cs->frameUse(path, &meta);  // virtual-table hit -> resident
  if (cs->framesState() == CobraSession::Frames::No || !ur.ok || !meta.valid) return false;
  CobraRunResult sr = cs->frameSnapshot(path);  // resident -> real file at `path`
  releaseResidentToken("vsave-anchor");  // worker holds the tempfile frame now, not the head
  shadowValid_ = false;  // the next head resync reseeds the shadow card
  (void)cs->consumeRespawned();
  if (!sr.ok) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(path), rmEc);  // BUG-124: never leave a torn partial
    return false;
  }
  return true;
}

SessionState::SaveHeadPath SessionState::trySaveHeadDirect(const std::string& destTmp) {
  // Same guard as ensureMaterializedHead: only a dirty head whose resident frame
  // still IS currentHbin_ can be written straight to the temp. A clean head, a
  // divergent intermediate temp (currentHbin_ already real), or an engine with
  // frames unused all fall to the caller's copy-from-currentHbin_ path.
  if (!residentDirty_ || !cobraSession_ || !currentHbin_.has_value() ||
      residentToken_ != *currentHbin_) {
    return SaveHeadPath::CopyFromCurrent;
  }
  CobraRunResult r = cobraSession_->frameSnapshot(destTmp);
  if (!r.ok) {
    // A failed snapshot can leave a TORN temp (the engine's writer creates the
    // file before it can detect the failure — same hazard ensureMaterializedHead
    // guards). Remove it so the caller's copy path starts from a clean temp, and
    // leave the head dirty so that path's ensureMaterializedHead retries and, if
    // it too fails, reports honestly ("file not saved … unchanged").
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(destTmp), rmEc);
    return SaveHeadPath::CopyFromCurrent;
  }
  // Head intentionally left VIRTUAL (residentDirty_ untouched, no addSnapshot
  // Bytes) — see the header. `save` no longer anchors the head as a side effect.
  return SaveHeadPath::WroteDirect;
}

// C4-L1 window trace (C4-RESIDENT-READ-SCOPE-brief §6): stderr-only, opt-in
// via $PIET_C4_TRACE. Permanentizes the C3 scratch instrumentation: one line
// per dirty-window CLOSURE (an EMH that actually snapshotted), naming the
// call site, the verb whose dispatch closed the window, and the 1-based window
// sequence number (== snapshot_count after the bump — EMH's addSnapshotBytes
// is the counter's only site, so the two are the same series by construction).
// This is what settles moved-vs-died for the D1/D2 legs: a window whose closer
// goes resident doesn't disappear from this trace unless NO later consumer in
// the same window materializes. With the env var absent nothing here runs, and
// the emit itself is stderr — stdout stays byte-identical either way (the
// freeze-fixture safety property).
static bool c4TraceEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("PIET_C4_TRACE");
    return e && *e;
  }();
  return on;
}

void SessionState::noteC4TraceVerb(const std::string& rawCmd) {
  if (!c4TraceEnabled()) return;
  size_t b = rawCmd.find_first_not_of(" \t");
  if (b == std::string::npos) return;  // blank line — keep the previous verb
  size_t e = rawCmd.find_first_of(" \t,", b);
  c4TraceVerb_.assign(rawCmd, b, (e == std::string::npos ? rawCmd.size() : e) - b);
}

void SessionState::ensureMaterializedHead(const char* c4Site) {
  // MATERIALIZATION CENSUS (env-gated, PIET_ROUTELOG). The scratch census says the
  // reference slice writes 45.7 GB, 31.7 GB of it under the `strmut` prefix — but
  // the routing census then showed all 224 setcol calls DO go resident, so those
  // bytes are not the setcol --out. The hypothesis this line tests is that they are
  // the VIRTUAL HEAD being flushed here, once per stateless op that needs it on
  // disk. Logged with the call site so the answer names a caller, not a guess.
  if (const char* rp = routelog_path()) {
    if (std::FILE* f = std::fopen(rp, "a")) {
      // On a FLUSH, name the caller. `datastore` accounts for 294 of 350 flushes
      // and that label is a site inside SessionState, not the thing that asked —
      // so the useful question ("who still needs a real Datastore when a shadow
      // card already carries the delta?") needs the frames above it. Symbolized
      // in-process with dladdr: an offline atos guesses the ASLR slide and names
      // the wrong function (measured, RAMDIET-CENSUS section 7.1).
      std::string chain;
#ifndef _WIN32  // no backtrace/dladdr there; this diagnostic just leaves the caller blank
      if (residentDirty_) {
        void* bt[10];
        const int n = ::backtrace(bt, 10);
        for (int i = 2; i < n && i < 7; ++i) {
          Dl_info info{};
          const char* sym = (dladdr(bt[i], &info) && info.dli_sname) ? info.dli_sname : "?";
          chain += (chain.empty() ? "" : "<") + std::string(sym);
        }
      }
#endif
      std::fprintf(f, "MATERIALIZE\t%s\t%s\t%s\n", residentDirty_ ? "flush" : "clean",
                   c4Site ? c4Site : "?", chain.c_str());
      std::fclose(f);
    }
  }
  if (!residentDirty_) return;  // clean head (or frames never used) — nothing to flush.
  if (!cobraSession_) {
    // BUG-384: no worker => the virtual bytes never existed anywhere reachable;
    // declaring the head clean here leaves it unbacked, and the audit says so.
    if (scratchTraceOn()) scratchTrace("CLEAR(no-worker)", "materialize", residentToken_);
    residentDirty_ = false;
    return;
  }
  if (!currentHbin_.has_value() || residentToken_ != *currentHbin_) {
    // The resident frame no longer corresponds to currentHbin_ — a divergent
    // intermediate temp (egen if/in, applyIfAndInFilter) advanced the frame
    // after currentHbin_ was already materialized. Snapshotting now would write
    // the WRONG frame over a real file, so skip. NOTE (BUG-384): the old tail
    // of this comment claimed "currentHbin_ is already real" — that is the
    // NORMAL case, not an invariant, and when a clearing site has already
    // dropped the token out from under a virtual head this skip is where the
    // head's last chance of materializing dies. The dispatch-exit audit is the
    // detector for that state; this trace names the site that skipped.
    if (scratchTraceOn())
      scratchTrace("MAT-SKIP(divergent)", c4Site ? c4Site : "?",
                   currentHbin_.value_or(std::string("-")));
    return;
  }
  CobraRunResult r = cobraSession_->frameSnapshot(*currentHbin_);
  // BUG-384: a snapshot failure was SILENT here ("the next real op reports it")
  // — and the very next op at one call site (materializeVirtualSave) DESTROYS
  // the worker frame, so nothing was left to report from. Say it when it fails.
  if (!r.ok) {
    std::fprintf(stderr,
                 "[piet_native] WARN snapshot-failed at %s: could not write %s (%s) — the head "
                 "stays a dirty virtual frame (BUG-384)\n",
                 c4Site ? c4Site : "?", currentHbin_->c_str(),
                 !r.error.empty() ? r.error.c_str()
                 : !r.stderr_text.empty()
                     ? r.stderr_text.c_str()  // the serve protocol carries the engine's
                                              // message in "stderr"; resultFromResponse
                                              // never mapped it into .error (BUG-384)
                     : "no engine detail");
  }
  if (r.ok) {
    // FLOOR ① site class SNAPSHOT (counters.h). Per op this is amp ≡ 1 — the
    // copy IS the snapshot's semantic work — so the number that can be wrong
    // here is the COUNT, not the ratio (32 snapshots where the semantics need
    // ~2). addSnapshotBytes bumps snapshot_count alongside the bytes for
    // exactly that reason; the count is the assertable half (A1 §1.3).
    {
      std::error_code ec;
      auto sz = std::filesystem::file_size(cecon::to_native_path(*currentHbin_), ec);
      counters::addSnapshotBytes(ec ? 0 : static_cast<int64_t>(sz));
      // GA sweep 2026-08-22: the SAME size, also billed to the caller that forced
      // this flush. The routelog already named the caller and the verb-diag ledger
      // already carried the bytes, but in two files with no join key — so the
      // materialization census could only rank callers by COUNT, and count is the
      // wrong unit: measured on this corpus the top 10% of snapshots carry 51% of
      // the bytes while the bottom half carry 3.3%. Ranking by count sent one fix
      // at 320 flushes worth 2 GB and ~0 s. One line, one join key, no new stat().
      if (const char* rp = routelog_path()) {
        if (std::FILE* f = std::fopen(rp, "a")) {
          std::fprintf(f, "MATBYTES\t%s\t%lld\n", c4Site ? c4Site : "?",
                       static_cast<long long>(ec ? 0 : sz));
          std::fclose(f);
        }
      }
    }
    // C4-L1 window trace: this snapshot IS a dirty-window closure — say who
    // closed it. Seq = snapshot_count post-bump (same series, see c4TraceEnabled).
    // FIX-C4COUNTER-SAMPLERSPAN/BUG-142: a closure fired from inside a
    // counter-neutral diagnostic walk (c4TraceSuppress_) has its count
    // restored, so no line prints — else trace and snapshot_count desync and
    // the line names an innocent verb (c4TraceVerb_ is stale post-dispatch).
    if (c4TraceEnabled() && !c4TraceSuppress_) {
      std::fprintf(stderr, "[c4-trace] %s|%s|%lld\n", c4Site,
                   c4TraceVerb_.empty() ? "-" : c4TraceVerb_.c_str(),
                   static_cast<long long>(counters::snapshotCount()));
    }
    residentDirty_ = false;
    lastMaterializedHead_ = *currentHbin_;
    flushDeferredReclaim();  // the virtual chain is now anchored on disk.
  } else {
    // BUG-124 sibling: a failed snapshot (ENOSPC, worker death mid-write) can
    // leave a TORN file at the token path — complete header, short blobs (the
    // engine's writer creates the file before it can detect the failure). Any
    // later reader (datastore()'s mmap, a stateless resync's --data) would
    // otherwise find it via an existence check. Remove it so the token path
    // truthfully returns to its "no file" state; dirty stays set and the next
    // real op reports the failure honestly (unchanged).
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(*currentHbin_), rmEc);
  }
  // A failed snapshot (dead worker) leaves dirty set; the next real op reports it.
}

}  // namespace piet
