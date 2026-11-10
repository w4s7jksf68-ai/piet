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

// ===========================================================================
// sample
// ===========================================================================

ojson runSample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSample.known,
                                            kVocabSample.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "sample")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("sample: syntax is `sample # [, count]` (# is a percent unless `count` is "
                    "given)");
  double amount = 0.0;
  if (!parseStrictDouble(parsed.varlist[0], &amount))
    return rsSyntax("sample: `" + parsed.varlist[0] + "' is not a number");
  const bool byCount = hasFlag(parsed.opts, "count");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("sample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  int64_t k = 0;
  if (byCount) {
    if (amount != std::floor(amount) || amount < 0)
      return rsSyntax("sample: with `count`, # must be a non-negative whole number");
    k = static_cast<int64_t>(amount);
    if (k > n)
      return rsError("sample: cannot draw " + std::to_string(k) + " observation(s) from " +
                     std::to_string(n));
  } else {
    if (!(amount >= 0.0 && amount <= 100.0))
      return rsSyntax("sample: a percent must be between 0 and 100");
    // Round-half-up, as the plan's Stata-semantics ruling states.
    k = static_cast<int64_t>(std::floor(static_cast<double>(n) * amount / 100.0 + 0.5));
    if (k > n) k = n;
  }

  const std::string keepVar = "__piet_sample_keep";
  if (ds->has(keepVar))
    return rsError("sample: the scratch column " + keepVar +
                   " already exists in this dataset; rename it and retry.");

  // A key per ROW, then keep the k smallest. Equivalent to drawing k rows
  // without replacement, and reproducible without any ordering assumption: the
  // row index breaks a key tie so the answer never depends on sort stability.
  const uint64_t seed = drawSeed(session, std::nullopt);
  std::vector<std::pair<uint32_t, int64_t>> keys;
  keys.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) keys.emplace_back(rowKey32(seed, j), j);
  std::sort(keys.begin(), keys.end());
  std::vector<char> keep(static_cast<size_t>(n), 0);
  for (int64_t j = 0; j < k; ++j) keep[static_cast<size_t>(keys[static_cast<size_t>(j)].second)] = 1;

  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) cells.push_back(keep[static_cast<size_t>(j)] ? "1" : "0");

  std::string flagged;
  if (auto err = appendColumn(session, *session.currentHbin(), keepVar, cells, "f64", &flagged))
    return *err;
  const std::string out = nextPath(session.workdir(), "sample");
  if (auto err = runVerb({"eval", "--data", flagged, "--keepif", std::string(keepVar) + "==1",
                          "--drop", keepVar, "--out", out}))
    return *err;
  session.load(out);
  if (auto bad = assertRowCount(session, "sample", k)) return *bad;
  return mutationResult(session, "sample");
}

// ===========================================================================
// splitsample
// ===========================================================================

ojson runSplitsample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplitsample.known,
                                            kVocabSplitsample.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "splitsample")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string gen = optStrAny(parsed.opts, {"generate", "gen"}).value_or("_sample");
  long long nsplit = 2;
  if (auto v = optStr(parsed.opts, "nsplit")) {
    if (!parseStrictInt(*v, &nsplit) || nsplit < 2)
      return rsSyntax("splitsample: nsplit() requires a whole number of at least 2, got `" + *v +
                      "'");
  }
  // BUG-1120 review: generate() is read as a newvarlist, so a storage type is
  // "byte invalid varname" r(198) and if/in/using/with open a qualifier the option
  // does not take, "if not allowed" r(101) (StataBE 19.5).
  if (auto r = strutil::newVarOptionRefusal(gen)) return rsErrorRc(r->message, r->rc);
  ojson guard = newVarGuard(session, "splitsample", gen);
  if (!guard.is_null()) return guard;

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("splitsample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  if (n == 0) return rsErrorRc("splitsample: no observations", 2000);

  const uint64_t seed = drawSeed(session, optStr(parsed.opts, "rseed"));
  std::vector<std::pair<uint32_t, int64_t>> keys;
  keys.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) keys.emplace_back(rowKey32(seed, j), j);
  std::sort(keys.begin(), keys.end());
  // Rank r (0-based) -> group floor(r*nsplit/n)+1. Equal sizes when nsplit
  // divides n; otherwise the remainder lands on the LOW groups first, which is
  // the plan's stated convention.
  std::vector<int64_t> grp(static_cast<size_t>(n), 0);
  for (int64_t r = 0; r < n; ++r) {
    const int64_t g = (r * nsplit) / n + 1;
    grp[static_cast<size_t>(keys[static_cast<size_t>(r)].second)] = g;
  }
  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) cells.push_back(std::to_string(grp[static_cast<size_t>(j)]));

  std::string out;
  if (auto err = appendColumn(session, *session.currentHbin(), gen, cells, "f64", &out)) return *err;
  session.load(out);

  ojson blocks = ojson::array();
  if (hasFlag(parsed.opts, "show")) {
    std::vector<int64_t> counts(static_cast<size_t>(nsplit) + 1, 0);
    for (int64_t j = 0; j < n; ++j) counts[static_cast<size_t>(grp[static_cast<size_t>(j)])] += 1;
    std::ostringstream oss;
    oss << gen << "  |      Freq.\n";
    oss << "------------+------------\n";
    for (long long g = 1; g <= nsplit; ++g) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%11lld | %10lld\n", g,
                    static_cast<long long>(counts[static_cast<size_t>(g)]));
      oss << buf;
    }
    blocks.push_back(displayBlock(oss.str()));
  }
  blocks.push_back(mutationResult(session, "splitsample", {gen}));
  return blocks;
}

}  // namespace piet::handlers
