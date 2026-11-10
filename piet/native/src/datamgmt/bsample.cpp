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
  if (idclusterOpt.has_value()) {
    ojson guard = newVarGuard(session, "bsample", *idclusterOpt);
    if (!guard.is_null()) return guard;
  }

  // The GROUPS being drawn: either every row on its own, or the rows of each
  // distinct cluster value. A factor cluster groups on the code (and every
  // missing cluster value is ONE group, as Stata treats it).
  std::vector<std::vector<int64_t>> groups;  // 1-based row indices
  if (!clusterOpt.has_value()) {
    groups.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) groups.push_back({i + 1});
  } else {
    const std::vector<std::string> cvars = splitWs(*clusterOpt);
    if (cvars.size() != 1)
      return rsError("bsample: this build clusters on ONE variable; cluster(" + *clusterOpt +
                         ") names " + std::to_string(cvars.size()) + ".",
                     "unimplemented");
    const std::string& cv = cvars[0];
    if (!ds->has(cv)) return rsError("variable " + cv + " not found");
    std::map<std::string, size_t> seen;
    const cecon::Column& c = ds->col(cv);
    for (int64_t i = 0; i < n; ++i) {
      std::string key;
      if (c.dtype == cecon::DType::Factor) {
        const int32_t code = c.codes()[i];
        key = code < 0 ? std::string(".") : ("c" + std::to_string(code));
      } else {
        const double v = c.f64()[i];
        key = std::isnan(v) ? std::string(".") : eval::pythonFloatRepr(v);
      }
      auto it = seen.find(key);
      if (it == seen.end()) {
        it = seen.emplace(key, groups.size()).first;
        groups.emplace_back();
      }
      groups[it->second].push_back(i + 1);
    }
  }

  const int64_t nGroups = static_cast<int64_t>(groups.size());
  int64_t draws = nGroups;  // default size = _N, or G when clustering
  if (!parsed.varlist.empty()) {
    long long want = 0;
    if (!parseStrictInt(parsed.varlist[0], &want) || want < 0)
      return rsSyntax("bsample: `" + parsed.varlist[0] + "' is not a non-negative whole number");
    draws = want;
    if (!clusterOpt.has_value() && draws > n)
      return rsError("bsample: cannot draw " + std::to_string(draws) +
                     " observation(s) without replacement from " + std::to_string(n) +
                     " — bsample draws WITH replacement, so # may not exceed _N in Stata either.");
    if (clusterOpt.has_value() && draws > nGroups)
      return rsError("bsample: cannot draw " + std::to_string(draws) + " cluster(s) from " +
                     std::to_string(nGroups));
  }

  std::vector<int64_t> idx;
  std::vector<int64_t> idcluster;
  int64_t nKept = -1;  // the fast path's originals: rows [0, nKept) of the result
  if (bsampleIsStataFastPath(parsed)) {
    // Stata's own draw on the session stream (core/rng_mt64.h), so `set seed s` +
    // bsample gives Stata's rows; shared with the bootstrap prefix (bsample_draws.h).
    for (int64_t row : bsdraw::srswrFast(session.seed.stream(), n, &nKept)) idx.push_back(row + 1);
  } else {
    const uint64_t seed = drawSeed(session, std::nullopt);
    for (int64_t d = 0; d < draws; ++d) {
      const uint32_t u = rowKey32(seed, d);
      const int64_t g = static_cast<int64_t>(u % static_cast<uint32_t>(nGroups));
      for (int64_t row : groups[static_cast<size_t>(g)]) {
        idx.push_back(row);
        idcluster.push_back(d + 1);
      }
    }
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
  if (nKept >= 0) {
    // e(sample) T2 (StataBE w2_esample_copies X2 `10|61|m0=20|m0_diff=10`): the
    // marker travels with each kept original row through the sort and keep, and
    // the rows `expand` creates are outside the sample (expand's rule,
    // runExpandIf). The merge carried the mask onto the copies; clear them.
    if (std::optional<std::vector<double>> cells = esampleHeadMask(session)) {
      bool any = false;
      for (size_t i = static_cast<size_t>(nKept); i < cells->size(); ++i) {
        any = any || (*cells)[i] != 0.0;
        (*cells)[i] = 0.0;
      }
      if (any && !esampleRewriteHeadMask(session, *cells))
        return rsError("bsample: the sample was drawn, but the estimation-sample marker could "
                       "not be cleared on the expanded copies; e(sample) is not reliable until "
                       "the next estimation.");
    }
  }
  return mutationResult(session, "bsample", idclusterOpt.has_value()
                                                ? std::vector<std::string>{*idclusterOpt}
                                                : std::vector<std::string>{});
}

}  // namespace piet::handlers
