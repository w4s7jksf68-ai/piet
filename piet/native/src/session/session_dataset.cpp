// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// session dataset. Method bodies retain their original ordering.
#include "piet/session_state.h"

namespace piet {

void SessionState::releaseCarryPrev() {
    if (!carryPrev_) {
      carrySrc_ = {};
      return;
    }
    counters::RlPhaseTimer _rllClose(&counters::strmutPhases().rllCloseNs, nullptr);
    carryPrev_.reset();
    carrySrc_ = {};
  }

void SessionState::load(const std::string& path, LoadIntent intent ) {
    // e(sample) T2: the mask install's own load keeps the pre-install head
    // pinned; any later head move releases it (pinPreInstallHead below).
    const bool installLoad = preInstallLoadPending_;
    preInstallLoadPending_ = false;
    struct ReleaseAfter {
      SessionState& s;
      bool release;
      ~ReleaseAfter() {
        if (release) s.releasePreInstallPins();
      }
    } releaseAfter{*this, !installLoad && !preInstallPins_.empty()};
    if (intent == LoadIntent::FreshRead) residentDropToken();
    std::optional<std::string> superseded = currentHbin_;
    // BUG-381 DIAGNOSTIC (PIET_SCRATCH_TRACE): head transitions, so an orphan
    // found by scratchCensusJson can be traced to the moment it stopped being
    // the head -- or shown never to have been one. Report-only, env-gated.
    // The gate is tested BEFORE the arguments are built: load() is the row-loop
    // hot path, and value_or() would construct a std::string on every call with
    // tracing off. An off-by-default diagnostic that costs an allocation per
    // mutation is not off.
    if (scratchTraceOn()) scratchTrace("LOAD", path, superseded.value_or(std::string("-")));
    currentHbin_ = path;
    ++headLoadCount;
    {
      // ROWLOOP-RLLOAD-SUBSPLIT: ~Datastore = the munmap of the PREVIOUS frame.
      counters::RlPhaseTimer _rllClose(&counters::strmutPhases().rllCloseNs, nullptr);
      datastore_.reset();
    }
    // BETA A4 (BS-091): every mutating verb (gen/replace/drop/sort/egen/...)
    // funnels through this SAME choke point to reload its rewritten scratch
    // hbin, so this is where c(changed) flips true. A fresh `use` ALSO calls
    // load() (before setCurrentFile()'s reset below runs), so the net effect
    // after `use` is still false — only a genuine mutation leaves it true.
    datasetChanged = true;
    // Stage 1b resident-frame bookkeeping. The runCobra intercept sets
    // residentToken_ to the (virtual) --out just before a mutation handler
    // calls load(out), so `path == residentToken_` uniquely marks "this load
    // continues the resident chain — the head is a VIRTUAL frame, no file
    // exists". Every OTHER load (use/restore/import, or a legacy real --out
    // from append/merge stateless) moved the head onto a REAL file the worker
    // is now out of sync with. When frames are unused, residentToken_ is
    // always empty => always the else branch => byte-identical to before.
    if (!residentToken_.empty() && path == residentToken_) {
      // Virtual head: defer reclaiming the (possibly real) predecessor — it is
      // the crash-recovery floor until the next materialization (§3).
      if (superseded.has_value() && *superseded != path) {
        // ROWLOOP-RLLOAD-SUBSPLIT: the scratch-GC half (virtual-head arm — a
        // push_back, expected ~0; billed anyway so the arm is not assumed free).
        counters::RlPhaseTimer _rllRec(&counters::strmutPhases().rllReclaimNs, nullptr);
        deferredReclaim_.push_back(*superseded);
      }
    } else {
      releaseResidentToken("load-real");  // worker holds a different frame -> next verb frame-uses `path`
      lastMaterializedHead_ = path;  // real file (use/restore/import/legacy --out) — the new floor
      {
        // ROWLOOP-RLLOAD-SUBSPLIT: the scratch-GC half (real-head arm — this is
        // the one that can do deletion I/O once the scratch budget is exceeded).
        counters::RlPhaseTimer _rllRec(&counters::strmutPhases().rllReclaimNs, nullptr);
        flushDeferredReclaim();      // moved off the virtual chain — reclaim what it pinned
      }
      residentComputedColClear();    // C4-L4: the chain the cache described is over
      if (superseded.has_value() && *superseded != path) {
        counters::RlPhaseTimer _rllRec2(&counters::strmutPhases().rllReclaimNs, nullptr);
        reclaimSupersededScratch(*superseded);
      }
    }
  }

Datastore* SessionState::datastore() {
    {
      // ROWLOOP-RLLOAD-SUBSPLIT (report-only, armed only inside writeNew's
      // rl_load scope — counters.h RlArm). The snapshot-write half.
      counters::RlPhaseTimer _rllMat(&counters::strmutPhases().rllMaterializeNs,
                                     &counters::strmutPhases().rllMaterializeCalls);
      ensureMaterializedHead("datastore");  // Stage 1b: a virtual resident head becomes a real file before mmap.
    }
    // BUG-124: a FAILED materialization (snapshot ENOSPC / dead worker) leaves
    // the head a dirty virtual token with NO file (the failed snapshot's
    // partial is unlinked). Opening it would throw a bare "cannot open <token>"
    // that hides the real cause — surface it honestly instead. Only the
    // token==head case is the unmaterialized-virtual-head state; the divergent
    // skip path (token != head) leaves dirty set with a REAL head on disk.
    if (residentDirty_ && currentHbin_.has_value() && residentToken_ == *currentHbin_) {
      throw std::runtime_error(
          "could not materialize the working dataset to disk (disk full or I/O error)");
    }
    if (!datastore_ && currentHbin_.has_value()) {
      {
        // ROWLOOP-RLLOAD-SUBSPLIT: the mmap + header-parse half.
        counters::RlPhaseTimer _rllOpen(&counters::strmutPhases().rllOpenNs,
                                        &counters::strmutPhases().rllOpenCalls);
        // ALLOC-C3A-ARC1-CARRY: consume the carry only for the exact file the arm
        // was raised for, and only on the stateless plane. A resident head is a
        // virtual frame whose materialization this descriptor never described
        // (brief §4/§5) — residentToken_ is already empty here for every load
        // that moved the head onto a real file, so the test is a guard, not a
        // new state machine.
        if (carryArmed_ && *currentHbin_ == carryTarget_ && residentToken_.empty()) {
          carryArmed_ = false;  // one carry per arm, even if this open throws
          datastore_ = std::make_unique<Datastore>(*currentHbin_, std::move(carried_),
                                                   carryDelta_, &lastCarryOutcome_, carrySrc_);
          carried_.clear();
        } else {
          try {
            datastore_ = std::make_unique<Datastore>(*currentHbin_);
          } catch (...) {
            // BUG-384: name the state instead of letting a bare ENOENT out. The
            // honest-error guard above only fires while residentToken_ still
            // EQUALS the head; every site that clears the token while
            // currentHbin_ still names an unwritten virtual head lands here
            // instead, and "cannot open <path>" tells the next reader nothing
            // about why a file the session believes in was never written. This
            // is BUG-349's documented class ("it declares a DIRTY VIRTUAL head
            // clean, and the bytes that only ever existed in the worker are then
            // unreachable"), and the message now says so.
            //
            // Cost is on the FAILURE path only — the stat runs after an open has
            // already thrown, never on the hot success path.
            std::error_code sec;
            const bool missing =
                !std::filesystem::exists(*currentHbin_, sec);
            const bool ours = !workdir_.empty() &&
                              currentHbin_->rfind(workdir_, 0) == 0;
            if (missing && ours) {
              throw std::runtime_error(
                  "the working dataset names a scratch file that was never written (" +
                  *currentHbin_ +
                  "). This is a virtual resident head whose bytes existed only in the "
                  "compute worker and were never snapshotted to disk — see BUG-349/384. "
                  "Re-run from your last saved dataset.");
            }
            throw;
          }
        }
      }
      // ALLOC-C3A-ARC2A-PREVMAP: the deferral ends HERE, on every path — the
      // carried open has either proved its identity or fallen back, and either way
      // nobody may read the previous bytes again. Bounding the window to a single
      // open is what keeps the two-mapping RSS double-count to one carried open
      // instead of the whole verb (~CarryArm is only the backstop). Two placement
      // rules, both load-bearing: it runs OUTSIDE the carry branch (an open of
      // some OTHER file spends the arm too, so its mapping must not linger), and
      // OUTSIDE the _rllOpen scope just closed above — billing this munmap inside
      // rllOpen would make rllClose a subset of rllOpen instead of a sibling, and
      // would silently inflate the very phase ARC 2 is measured against
      // (counters.h RlArm: the sub-timers must PARTITION rl_load).
      releaseCarryPrev();
    }
    return datastore_.get();
  }

void SessionState::releaseResidentToken(const char* site) {
    if (scratchTraceOn())
      scratchTrace((residentDirty_ && currentHbin_.has_value() &&
                    residentToken_ == *currentHbin_)
                       ? "CLEAR(dirty-head)"
                       : "CLEAR",
                   site, residentToken_.empty() ? std::string("-") : residentToken_);
    residentToken_.clear();
    residentDirty_ = false;
  }

void SessionState::auditHeadBacked(const char* where) {
    if (!currentHbin_.has_value() || workdir_.empty()) return;
    const std::string& head = *currentHbin_;
    if (head.rfind(workdir_, 0) != 0) return;            // user files: not ours to audit
    if (residentDirty_ && residentToken_ == head) return;  // guarded virtual head
    if (head == lastHeadAudited_) return;                // already checked this head
    std::error_code aec;
    if (std::filesystem::exists(head, aec)) {
      lastHeadAudited_ = head;
      return;
    }
    ++tokenOrphanWarns_;
    std::fprintf(stderr,
                 "[piet_native] WARN head-unbacked at %s: currentHbin_=%s has no file and no "
                 "guarded token (token=%s dirty=%d virtual-save=%d) — the next reader of this "
                 "path gets ENOENT (BUG-384)\n",
                 where, head.c_str(), residentToken_.empty() ? "-" : residentToken_.c_str(),
                 residentDirty_ ? 1 : 0, virtualSaves_.count(head) ? 1 : 0);
    static const bool hardAbort = [] {
      const char* e = std::getenv("PIET_TOKEN_GUARD");
      return e != nullptr && std::string(e) == "abort";
    }();
    if (hardAbort) std::abort();
    lastHeadAudited_ = head;
  }

const std::vector<VarInfo>* SessionState::shadowVarlistIfCurrent() const {
    if (!shadowValid_ || !currentHbin_.has_value() || shadowCardHbin_ != *currentHbin_) {
      return nullptr;
    }
    return &shadowCard_;
  }

std::optional<int64_t> SessionState::residentRowsIfCurrent() const {
    if (residentReadDisabled()) return std::nullopt;
    if (!shadowValid_ || !currentHbin_.has_value() || shadowCardHbin_ != *currentHbin_) {
      return std::nullopt;
    }
    if (residentRows_ < 0) return std::nullopt;
    return residentRows_;
  }

const SessionState::ResidentComputedCol* SessionState::residentComputedCol(const std::string& name) const {
    if (!computedColName_.has_value() || *computedColName_ != name) return nullptr;
    std::optional<int64_t> rows = residentRowsIfCurrent();
    if (!rows.has_value() || *rows != computedCol_.nRows) return nullptr;
    return &computedCol_;
  }

void SessionState::residentComputedColInvalidate(const std::string& name) {
    if (computedColName_.has_value() && *computedColName_ == name) {
      computedColName_.reset();
      computedCol_ = ResidentComputedCol{};
    }
  }

void SessionState::clearData() {
    // BUG-381: `clear` abandons the head like every other direct assignment
    // here, and load() is not involved, so the reclaim has to be explicit.
    // Nothing can read the dropped head afterwards; a live preserve snapshot,
    // frame or `use` memo is spared by reclaimSupersededScratch's exclusions.
    if (currentHbin_.has_value()) reclaimSupersededScratch(*currentHbin_, /*abandoningHead=*/true);
    currentHbin_.reset();
    datastore_.reset();
    currentFile_.clear();  // P8 U1: honest cockpit — no stale source_file after `clear`
    datasetChanged = false;  // BETA A4 (BS-091): nothing loaded -> nothing changed
  }

}
