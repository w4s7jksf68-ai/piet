// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// session memory. Method bodies retain their original ordering.
#include "piet/session_state.h"

namespace piet {

int64_t SessionState::liveNamedBytes(bool peakSampleMode ) {
    int64_t total = 0;

    // 1. resident frame — Σ column buffers: f64 8B·rows, factor 4B·rows codes
    //    PLUS its level dictionary once (§2.2). datastore() may materialise a
    //    virtual resident head here; harmless at the emit site (the run is
    //    complete, nothing observable runs after) and unreachable env-off.
    //    Guarded: a diagnostic must never crash a completed run.
    try {
      if (peakSampleMode && residentDirty_) {
        // peak sample over a dirty head — see the method doc: skip, don't
        // materialise mid-run.
      } else if (Datastore* ds = datastore()) {
        const int64_t rows = ds->nRows();
        // METADATA-ONLY column walk (FIX-PIET-COUNTERTAX-PEAKSAMPLE). This used
        // to iterate `ds->varlist()`, i.e. it built the VarInfo CARD for every
        // column — and buildVarInfo() computes `n_missing`, an O(nrows) NaN/
        // code scan that faults the column's ENTIRE mmap payload back in
        // (datastore.cpp Datastore::nMissing). n_missing is not in the §2.2
        // enumeration at all: this walk needs only dtype, nRows and the level
        // dictionary, all of which are metadata already resident in the
        // Dataset's column table. The card was pure tax, and under the peak
        // sample it was paid once PER TOP-LEVEL ITEM: measured on a2_mini
        // (instruments/alloc_churn_scoping.md §1, interleaved on/off A/B ×2)
        // PIET_EMIT_COUNTERS=1 cost +11.6% wall / +45% PIET wall / +98k page
        // faults for only +0.79s CPU — fault-wait, not compute.
        // The VALUE is unchanged: names() is built from the same
        // ds_.columns() varlist() walks, in the same order; the dtype test and
        // the level strings summed are the same ones buildVarInfo() copies a
        // view of.
        for (const std::string& cname : ds->names()) {
          const cecon::Column& c = ds->col(cname);
          if (c.dtype == cecon::DType::Factor) {
            total += 4 * rows;
            // The column's OWN level vector — the same strings a VarInfo's
            // LevelsRef views, read without the owning copy that bumps
            // levelStringCopies() (datastore.h), so this read is still free of
            // the selftest tripwire.
            for (const std::string& lvl : c.levels) {
              total += static_cast<int64_t>(lvl.size());
            }
          } else {
            total += 8 * rows;  // f64
          }
        }
      }
    } catch (...) {
      // resident-frame bytes unavailable (materialise failed) — report the
      // rest rather than aborting the whole run's counters block.
    }

    // 2. eststo / esttab store — Σ serialized bytes of each stored model
    //    snapshot (§2.2, session_state.h estimates vector-of-pairs).
    for (const auto& kv : estimates) {
      total += static_cast<int64_t>(kv.first.size() + kv.second.dump().size());
    }

    // 3. graph gallery — Σ serialized bytes of each named graph spec (§2.2,
    //    namedGraphs map).
    for (const auto& kv : namedGraphs) {
      total += static_cast<int64_t>(kv.first.size() + kv.second.dump().size());
    }
    total += sersets.bytes();  // serset-twoway SS: the graphs' sersets

    // 4. open postfiles — Σ open post-buffer bytes accumulating until postclose
    //    (§2.2): the parallel name/type arrays + every staged (kind,text) cell.
    for (const auto& kv : postfiles) {
      const PostHandle& ph = kv.second;
      total += static_cast<int64_t>(kv.first.size());
      for (const auto& s : ph.names) total += static_cast<int64_t>(s.size());
      for (const auto& s : ph.types) total += static_cast<int64_t>(s.size());
      for (const auto& row : ph.rows) {
        for (const auto& cell : row) {
          total += static_cast<int64_t>(cell.first.size() + cell.second.size());
        }
      }
    }

    // 5. active model — the one live estimation result, distinct from the named
    //    `estimates` store (§2.2 "active model").
    if (lastModel_.has_value() && lastModel_->result.has_value()) {
      total += static_cast<int64_t>(lastModel_->result->dump().size());
    }

    // 6. scalars / matrices / macros — small, but named + retained ⇒ counted
    //    (§2.2, session.eval.*). r()/e() transient stores are excluded (not
    //    user-named persistent entitlement).
    auto dictBytes = [](const auto& d) -> int64_t {
      int64_t b = 0;
      for (const auto& kv : d.items()) {
        b += static_cast<int64_t>(kv.first.size() + kv.second.size());
      }
      return b;
    };
    total += dictBytes(eval.locals);   // _locals macros
    total += dictBytes(eval.globals);  // _globals macros
    total += dictBytes(eval.scalars);  // named scalars
    for (const auto& kv : eval.matrices) {
      total += static_cast<int64_t>(kv.first.size());
      for (const auto& r : kv.second.rownames) total += static_cast<int64_t>(r.size());
      for (const auto& c : kv.second.colnames) total += static_cast<int64_t>(c.size());
      for (const auto& mrow : kv.second.data) {
        total += static_cast<int64_t>(mrow.size()) * 8;  // f64 cells
      }
    }

    return total;
  }

int64_t SessionState::liveNamedBytesNeutral(bool peakSampleMode) {
    const int64_t savedConstructs = Datastore::constructCount();
    const int64_t savedSnap = counters::g_snapshotCount;
    const int64_t savedTouched = counters::g_touchedBytes;
    const int64_t savedSetcol = counters::g_setcolBytes;
    const int64_t savedSnapBytes = counters::g_snapshotBytes;
    const int64_t savedUseDisk = counters::g_useDiskLoads;
    const int64_t savedUseVirtual = counters::g_useVirtualHits;
    const int64_t savedDtaConverts = counters::g_dtaConverts;
    const bool wasNull = (datastore_ == nullptr);

    c4TraceSuppress_ = true;  // a neutralised closure must not emit a trace line
    const int64_t v = liveNamedBytes(peakSampleMode);
    c4TraceSuppress_ = false;

    if (wasNull) datastore_.reset();  // restore exact cache state (was empty)
    Datastore::setConstructCount(savedConstructs);
    counters::g_snapshotCount = savedSnap;
    counters::g_touchedBytes = savedTouched;
    counters::g_setcolBytes = savedSetcol;
    counters::g_snapshotBytes = savedSnapBytes;
    counters::g_useDiskLoads = savedUseDisk;
    counters::g_useVirtualHits = savedUseVirtual;
    counters::g_dtaConverts = savedDtaConverts;
    return v;
  }

}
