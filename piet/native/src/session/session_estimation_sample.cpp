// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: e(sample) reading and its named refusals.
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

namespace piet {

// e(sample) T2 -- see the declaration (session_state.h). The reading keys on
// lastModel_, whose only producers of "no model" are a fresh session and
// lastModel_reset() (`ereturn clear`, `clear all`/`clear results`/`clear
// matrix`, and the clearEResults that opens `ereturn post`) -- the same events
// that empty the e() stores. No model is 0 (w2_esample.log:95-129 A1-A5, :1634
// G11), and so is a POSTED e(), which leaves eval.lastModelResult set with no
// model behind it: a posted e() has no estimation sample (:1487 G5 |0|0|.|).
// Why not the macro layer's absent-e() predicate (resolveEScalar,
// eval/macros.cpp: `!session.lastModelResult`): the two agree on every event
// that installs or clears e(), but NOT on a model installed without a result
// JSON -- `pca` (estimation/principal_components.cpp runPca: `LastModel lm; ...
// setLastModel(lm)` with lm.result unset). There resolveEScalar reads "no e()"
// while a model is live, and e(sample) = 0 would be a silent answer for a fit
// whose sample PIET does not have; keyed on lastModel_, it is the named r(198)
// of kUnavailable. The macro layer reads THIS function through
// EvalSession::esampleObs1 and uses its own predicate only when no session is
// attached.
SessionState::EsampleReading SessionState::esampleReading() {
  EsampleReading r;
  if (!lastModel_.has_value()) {
    // kZero: no model, or a posted e() without esample(). A posting WITH
    // esample() (results/ereturn_commands.cpp runPost) reads its own mask column, under
    // the same cell checks a fit's mask gets below.
    if (!ePostedLive || ePostedEsampleSerial == 0 || !hasData()) return r;
    Datastore* ds = nullptr;
    try {
      ds = datastore();
    } catch (const std::exception& e) {
      r.kind = EsampleReading::Kind::kRefused;
      r.refusal = std::string("e(sample) cannot be read: ") + e.what();
      return r;
    }
    if (ds == nullptr || !ds->has(kEsampleColumn)) return r;  // the marker left with the data
    const cecon::Column& c = ds->col(kEsampleColumn);
    bool foreign = c.dtype != cecon::DType::F64;
    if (!foreign) {
      const double want = static_cast<double>(ePostedEsampleSerial);
      const double* v = c.f64();
      const int64_t n = ds->nRows();
      for (int64_t i = 0; i < n && !foreign; ++i)
        if (!std::isnan(v[i]) && v[i] != 0.0 && v[i] != want) foreign = true;
    }
    if (foreign) {
      r.kind = EsampleReading::Kind::kRefused;
      r.refusal = "e(sample) is not available in this version of PIET here: the "
                  "estimation-sample marker on the data belongs to other results than the "
                  "posted ones (`ereturn post ..., esample()`).";
      return r;
    }
    r.kind = EsampleReading::Kind::kColumn;
    r.serial = ePostedEsampleSerial;
    return r;
  }
  const LastModel& lm = *lastModel_;
  const std::string cmd = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
  switch (lm.esample) {
    case LastModel::EsampleState::kNone:
      return r;
    case LastModel::EsampleState::kColumn: {
      // The column follows the rows through every row verb because it IS a
      // column; a verb that replaces the data (use, clear, collapse, reshape,
      // drop _all) takes it away, and Stata's marker goes with the data too.
      if (!hasData()) return r;
      Datastore* ds = nullptr;
      try {
        ds = datastore();
      } catch (const std::exception& e) {
        // A head that cannot be opened (BUG-124/BUG-384) is named, never read
        // as "no column" -> 0, and never thrown past `capture`.
        r.kind = EsampleReading::Kind::kRefused;
        r.refusal = std::string("e(sample) cannot be read: ") + e.what();
        return r;
      }
      if (ds == nullptr || !ds->has(kEsampleColumn)) return r;  // the marker left with the data
      // The column on the rows may be another fit's (see LastModel::
      // esampleSerial): a nonzero cell that is not this model's serial means
      // the working frame came back from somewhere this model's rows never
      // were. Named, never counted. One pass over an f64 column.
      const cecon::Column& c = ds->col(kEsampleColumn);
      const double want = static_cast<double>(lm.esampleSerial);
      bool foreign = c.dtype != cecon::DType::F64 || lm.esampleSerial == 0;
      double lost = 0.0;  // < 0: a row verb PIET does not follow (piet/datastore.h EsampleLostBy)
      if (!foreign) {
        const double* v = c.f64();
        const int64_t n = ds->nRows();
        for (int64_t i = 0; i < n; ++i) {
          if (!std::isnan(v[i]) && v[i] < 0.0) {
            lost = v[i];
            break;
          }
          if (!std::isnan(v[i]) && v[i] != 0.0 && v[i] != want) foreign = true;
        }
      }
      if (lost < 0.0) {
        r.kind = EsampleReading::Kind::kRefused;
        r.refusal = std::string("e(sample) is not available after `") + esampleLostByVerb(lost) +
                    "` in this version of PIET: that command rebuilt the observations in a way "
                    "whose estimation-sample marker PIET cannot reproduce (Stata keeps it on one "
                    "row per group or per original observation, chosen by its own sort order).";
        return r;
      }
      if (foreign) {
        r.kind = EsampleReading::Kind::kRefused;
        r.refusal = "e(sample) is not available after " + cmd +
                    " in this version of PIET here: the estimation-sample marker on the data "
                    "belongs to another estimation (the data in memory is not the data these "
                    "results were fitted on, e.g. after a bootstrap/jackknife/permute prefix).";
        return r;
      }
      r.kind = EsampleReading::Kind::kColumn;
      r.serial = lm.esampleSerial;
      return r;
    }
    case LastModel::EsampleState::kStale:
      // T3: `estimates restore` reads e(sample) back from `_est_NAME`
      // (results/stored_estimates_commands.cpp runEstimatesRestore); the variable is gone (dropped,
      // or the data were replaced since the store).
      r.kind = EsampleReading::Kind::kRefused;
      r.refusal = "e(sample) is not available after `estimates restore` in this version of "
                  "PIET: the variable " +
                  (activeEstimatesName.empty() ? std::string("_est_<name>")
                                               : "_est_" + activeEstimatesName) +
                  " that holds these results' estimation sample is no longer in the data.";
      return r;
    case LastModel::EsampleState::kUnavailable:
      break;
  }
  r.kind = EsampleReading::Kind::kRefused;
  r.refusal = "e(sample) is not available after " + cmd +
              " in this version of PIET: the estimation sample of these results was not "
              "recorded (the engine does not report it for this command, or the results were "
              "stored from such a model and brought back by `estimates restore`).";
  return r;
}

std::optional<int> SessionState::esampleObs1(std::string* why) {
  const EsampleReading r = esampleReading();
  if (r.kind == EsampleReading::Kind::kRefused) {
    if (why != nullptr) *why = r.refusal;
    return std::nullopt;
  }
  if (r.kind == EsampleReading::Kind::kZero) return 0;
  // esampleReading just opened the head and found the column on it.
  Datastore* ds = datastore();
  if (ds == nullptr || ds->nRows() < 1) return 0;  // B29-style read of an empty frame
  const cecon::Column& c = ds->col(kEsampleColumn);
  if (c.dtype != cecon::DType::F64) return 0;
  // An admitted row holds the install's serial; 0 and missing (a row added
  // after the fit) read 0.
  return c.f64()[0] == static_cast<double>(r.serial) ? 1 : 0;
}

}  // namespace piet
