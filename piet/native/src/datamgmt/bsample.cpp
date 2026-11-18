// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/observations.h"
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "estimation/bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

namespace {

// A one-column CSV of int64 values, for `cobra convert` to turn into an hbin.
//
// COPIED from writeIndexCsv in estimation/bootstrap_prefix.cpp, deliberately:
// that original lives in the ANONYMOUS namespace of its translation unit, so
// there is no declaration to reach, and the kRowIdx declared in
// estimation/resampling_prefix_internal.h is resample's own column name, not
// bsample's (see kRowIdx below). See dataset_shape_support.h's header comment
// for the hoist that is filed rather than done.
bool writeIndexCsv(const std::string& path, const std::string& colName,
                   const std::vector<int64_t>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f << colName << "\n";
  for (int64_t v : vals) f << v << "\n";
  f.flush();
  return static_cast<bool>(f);
}

// The row-index column bsample's index join keys on. Underscore-prefixed and
// long enough that it cannot collide with a user variable in practice; it never
// survives into the session's own dataset. (Copied alongside writeIndexCsv --
// same reason, and deliberately a DIFFERENT name from resample's `__piet_rs_row`
// so a leaked column can be attributed to the command that leaked it.)
constexpr const char* kRowIdx = "__piet_bs_row";

std::optional<ojson> refuseWeights(const eval::ParsedCommand& parsed, const std::string& verb) {
  if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
    return ojson(eval::unsupportedOption(
        *parsed.weightType + "=" + parsed.weightVar.value_or(""),
        "`" + verb + "` does not take weights in this build; a weighted percentile is a "
        "different estimator, not a reweighting of this one."));
  }
  return std::nullopt;
}

}  // namespace

// ===========================================================================
// bsample
// ===========================================================================

bool bsampleIsStataFastPath(const eval::ParsedCommand& parsed) {
  if (!parsed.varlist.empty()) return false;  // `bsample #` is the SRSWR path
  for (const auto& [k, v] : parsed.opts)
    if (k == "cluster" || k == "idcluster" || k == "strata" || k == "weight") return false;
  return true;
}

ojson runBsample(const eval::ParsedCommand& parsed, SessionState& session) {
  // strata() names variables that must exist whether or not this build draws within strata
  // (vs-stata bank: `bsample, strata(nosuchvar)` is r(111) "(error in option strata())").
  if (session.hasData())
    if (const auto strataText = optStr(parsed.opts, "strata")) {
      Datastore* sds = nullptr;
      try {
        sds = session.datastore();
      } catch (const std::exception&) {
      }
      if (sds != nullptr)
        for (std::istringstream words(*strataText); words;) {
          std::string nm;
          if (!(words >> nm)) break;
          if (!sds->has(nm))
            return rsErrorRc("variable " + nm + " not found\n(error in option strata())", 111);
        }
    }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabBsample.known,
                                            kVocabBsample.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "bsample")) return *q;
  if (auto w = refuseWeights(parsed, "bsample")) return *w;
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("bsample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  if (n == 0) return rsErrorRc("bsample: no observations", 2000);

  const std::optional<std::string> clusterOpt = optStr(parsed.opts, "cluster");
  const std::optional<std::string> idclusterOpt = optStr(parsed.opts, "idcluster");
  if (idclusterOpt.has_value() && !clusterOpt.has_value())
    return rsSyntax("bsample: idcluster() requires cluster()");

  if (ds->has(kRowIdx))
    return rsError("bsample: the scratch column " + std::string(kRowIdx) +
                   " already exists in this dataset; rename it and retry.");
  // idcluster(): an existing variable of that name is replaced (bsample.ado:
  // `capture confirm new variable` / `confirm variable` / `drop`); vs-stata bank
  // `bsample, cluster(rep78) idcluster(mpg)` rc 0, k 12.
  bool dropOldId = false;
  if (idclusterOpt.has_value()) {
    if (ds->has(*idclusterOpt)) {
      dropOldId = true;
    } else {
      ojson guard = newVarGuard(session, "bsample", *idclusterOpt);
      if (!guard.is_null()) return guard;
    }
  }

  // bsample.ado (StataBE 19.5). Without cluster() and without `#` the fast path
  // (no r() is touched); otherwise `count if touse` runs first, so r(N) is the
  // number of observations drawn from.
  //  - `#`: SRSWR -- the fast path's draw, then one more uniform per resampled row
  //    in that order, the rows sorted by it and the first # kept. # must be a
  //    whole number in 1.._N (r(198) / r(498)).
  //  - cluster(var): the data sorted by the cluster (stable; every missing value
  //    of one kind is one cluster, sorted last), the clusters numbered 1..G
  //    (G = 1 is r(460)), and ClustSRSWR's draw (bsdraw::clustSrswr, shared with
  //    the bootstrap prefix). idcluster(): NewClustID -- the data ordered by
  //    (cluster, copy, row) and each (cluster, copy) numbered 1, 2, ...
  //    `#` below G with cluster() draws again (NewClustID's random order): not
  //    implemented, refused by name.
  std::vector<int64_t> idx;        // 1-based input rows, in the result's row order
  std::vector<char> isCopy;        // rows `expand` created: outside e(sample)
  std::vector<int64_t> idcluster;  // idcluster(): the new cluster number of each row
  long long nsamp = -1;
  if (!parsed.varlist.empty()) {
    double want = 0;
    if (!parseStrictDouble(parsed.varlist[0], &want))
      return rsErrorRc("expression: " + parsed.varlist[0] + ": is not an integer", 198);
    if (std::fabs(want - std::trunc(want)) >= 1e-7)
      return rsErrorRc("expression: " + parsed.varlist[0] + ": is not an integer", 198);
    if (want <= 0) return rsErrorRc("resample size must be greater than zero", 498);
    nsamp = static_cast<long long>(std::llround(want));
  }
  const bool fast = bsampleIsStataFastPath(parsed);
  if (!clusterOpt.has_value()) {
    if (nsamp > n)
      return rsErrorRc("resample size must not be greater than number of observations", 498);
    int64_t nKept = 0;
    std::vector<int64_t> rows = bsdraw::srswrFast(session.seed.stream(), n, &nKept);
    std::vector<char> copy(rows.size(), 0);
    for (size_t i = static_cast<size_t>(nKept); i < rows.size(); ++i) copy[i] = 1;
    if (nsamp >= 0 && nsamp != n) {
      std::vector<double> w(rows.size());
      for (auto& x : w) x = session.seed.stream().runiform();
      std::vector<size_t> ord(rows.size());
      std::iota(ord.begin(), ord.end(), size_t{0});
      std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return w[a] < w[b]; });
      ord.resize(static_cast<size_t>(nsamp));
      std::vector<int64_t> r2;
      std::vector<char> c2;
      for (size_t k : ord) {
        r2.push_back(rows[k]);
        c2.push_back(copy[k]);
      }
      rows.swap(r2);
      copy.swap(c2);
    }
    for (size_t i = 0; i < rows.size(); ++i) {
      idx.push_back(rows[i] + 1);
      isCopy.push_back(copy[i]);
    }
  } else {
    const std::vector<std::string> cvars = splitWs(*clusterOpt);
    if (cvars.size() != 1)
      return rsError("bsample: this build clusters on ONE variable; cluster(" + *clusterOpt +
                         ") names " + std::to_string(cvars.size()) + ".",
                     "unimplemented");
    const std::string& cv = cvars[0];
    if (!ds->has(cv)) return rsErrorRc("variable " + cv + " not found", 111);
    const cecon::Column& c = ds->col(cv);
    const bool isStr = c.dtype == cecon::DType::Factor;
    auto strOf = [&](int64_t i) -> std::string {
      const int32_t code = c.codes()[i];
      return (code >= 0 && static_cast<size_t>(code) < c.levels.size())
                 ? c.levels[static_cast<size_t>(code)]
                 : std::string();
    };
    auto cmp = [&](int64_t a, int64_t b) -> int {
      if (isStr) {
        const std::string sa = strOf(a), sb = strOf(b);
        return sa < sb ? -1 : (sb < sa ? 1 : 0);
      }
      return cecon::stata_cmp(c.f64()[a], c.f64()[b]);
    };
    std::vector<int64_t> sorted(static_cast<size_t>(n));
    std::iota(sorted.begin(), sorted.end(), int64_t{0});
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&](int64_t a, int64_t b) { return cmp(a, b) < 0; });
    std::vector<int64_t> clid(static_cast<size_t>(n), 0);
    int64_t G = 0;
    for (size_t q = 0; q < sorted.size(); ++q) {
      if (q == 0 || cmp(sorted[q], sorted[q - 1]) != 0) ++G;
      clid[q] = G;
    }
    if (G == 1) return rsErrorRc("singleton cluster detected", 460);
    if (nsamp > G) return rsErrorRc("resample size must not be greater than number of clusters", 498);
    if (nsamp >= 0 && nsamp != G)
      return ojson(eval::unsupportedOption(
          "bsample #, cluster()",
          "a resample of fewer clusters than the data holds orders the drawn clusters with a "
          "further draw (bsample.ado's NewClustID) that this build does not reproduce."));
    const std::vector<int64_t> pos = bsdraw::clustSrswr(session.seed.stream(), clid, G);
    std::vector<int64_t> mult(static_cast<size_t>(G) + 1, 0);
    {
      std::vector<int64_t> seen(static_cast<size_t>(n), 0);
      for (int64_t p : pos) ++seen[static_cast<size_t>(p)];
      for (int64_t q = 0; q < n; ++q)
        mult[static_cast<size_t>(clid[static_cast<size_t>(q)])] = std::max(
            mult[static_cast<size_t>(clid[static_cast<size_t>(q)])], seen[static_cast<size_t>(q)]);
    }
    if (!idclusterOpt.has_value()) {
      std::vector<int64_t> firstSeen(static_cast<size_t>(n), 0);
      for (int64_t p : pos) {
        idx.push_back(sorted[static_cast<size_t>(p)] + 1);
        isCopy.push_back(firstSeen[static_cast<size_t>(p)]++ > 0 ? 1 : 0);
      }
    } else {
      int64_t id = 0;
      size_t q0 = 0;
      while (q0 < sorted.size()) {
        size_t q1 = q0;
        while (q1 < sorted.size() && clid[q1] == clid[q0]) ++q1;
        const int64_t m = mult[static_cast<size_t>(clid[q0])];
        for (int64_t k = 0; k < m; ++k) {
          ++id;
          for (size_t q = q0; q < q1; ++q) {
            idx.push_back(sorted[q] + 1);
            isCopy.push_back(k > 0 ? 1 : 0);
            idcluster.push_back(id);
          }
        }
        q0 = q1;
      }
    }
  }
  if (!fast) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(n));
    eval::declareRPosted(session.eval, "bsample", {"N"});
  }

  if (dropOldId) {
    ojson dropped = runDrop({*idclusterOpt}, session);
    if (dropped.is_object() && dropped.value("type", std::string()) == "error") return dropped;
  }

  // Three steps, exactly the shape the bootstrap replicate in
  // estimation/bootstrap_prefix.cpp uses: stamp the base with a row index, convert the drawn indices to a
  // frame, m:1-join them back. `merge`'s default keeps using-only rows, which
  // would silently add every UNDRAWN observation to the resample, so the
  // using-only side is dropped explicitly.
  const std::string basePath = *session.currentHbin();
  const std::string baseIdx = nextPath(session.workdir(), "bsample_baseidx");
  if (auto err = runVerb({"eval", "--data", basePath, "--egen",
                          std::string(kRowIdx) + " = seq()", "--out", baseIdx}))
    return *err;
  // The steps below each mint scratch, which pushes the reclaim budget over and
  // makes these two -- the oldest at that moment -- the first eviction
  // candidates while they are still the thing being drawn from.
  SessionState::ScratchPin pin(session, {basePath, baseIdx});

  const std::string csv = nextPath(session.workdir(), "bsample_idx") + ".csv";
  registerScratchMint(csv);  // BUG-389
  if (!writeIndexCsv(csv, kRowIdx, idx)) {
    cecon::native_unlink(csv);
    return rsError("bsample: could not write the resample index (disk full or I/O error)",
                   "data_io");
  }
  const std::string idxHbin = nextPath(session.workdir(), "bsample_idxh");
  if (auto err = runVerb({"convert", "--data", csv, "--out", idxHbin, "--delim", ","})) return *err;

  std::string drawFrame = idxHbin;
  if (idclusterOpt.has_value()) {
    // The draw ORDINAL, which is what `idcluster()` means: two draws of the same
    // cluster must be distinguishable downstream.
    std::vector<std::string> cells;
    cells.reserve(idcluster.size());
    for (int64_t v : idcluster) cells.push_back(std::to_string(v));
    std::string withId;
    if (auto err = appendColumn(session, idxHbin, *idclusterOpt, cells, "f64", &withId))
      return *err;
    drawFrame = withId;
  }

  const std::string merged = nextPath(session.workdir(), "bsample_merged");
  if (auto err = runVerb({"merge", "--data", drawFrame, "--using", baseIdx, "--type", "m:1",
                          "--key", kRowIdx, "--out", merged}))
    return *err;
  const std::string out = nextPath(session.workdir(), "bsample");
  if (auto err = runVerb({"eval", "--data", merged, "--keepif", "_merge!=2", "--drop", "_merge",
                          "--drop", kRowIdx, "--out", out}))
    return *err;
  session.load(out);
  if (auto bad = assertRowCount(session, "bsample", static_cast<int64_t>(idx.size()))) return *bad;
  {
    // e(sample) T2 (StataBE w2_esample_copies X2 `10|61|m0=20|m0_diff=10`): the
    // marker travels with each kept original row through the sort and keep, and
    // the rows `expand` creates are outside the sample (expand's rule,
    // runExpandIf). The merge carried the mask onto the copies; clear them.
    if (std::optional<std::vector<double>> cells = esampleHeadMask(session)) {
      bool any = false;
      for (size_t i = 0; i < cells->size() && i < isCopy.size(); ++i) {
        if (!isCopy[i]) continue;
        any = any || (*cells)[i] != 0.0;
        (*cells)[i] = 0.0;
      }
      if (any && !esampleRewriteHeadMask(session, *cells))
        return rsError("bsample: the sample was drawn, but the estimation-sample marker could "
                       "not be cleared on the expanded copies; e(sample) is not reliable until "
                       "the next estimation.");
    }
  }
  if (idclusterOpt.has_value()) {
    // NewClustID: `gen byte` 0/1, then the running sum -- byte while the ids fit.
    int64_t top = 0;
    for (int64_t v : idcluster) top = std::max(top, v);
    session.varMeta[*idclusterOpt].stataType =
        top <= 100 ? std::string("byte") : (top <= 32740 ? std::string("int") : std::string("long"));
    session.implicitFloatVars.erase(*idclusterOpt);
  }
  return mutationResult(session, "bsample", idclusterOpt.has_value()
                                                ? std::vector<std::string>{*idclusterOpt}
                                                : std::vector<std::string>{});
}

}  // namespace piet::handlers
