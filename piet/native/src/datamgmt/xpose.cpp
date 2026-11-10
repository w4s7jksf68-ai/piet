// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_combine.h"
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

ojson echoBlock(const std::string& text) {
  ojson b;
  b["type"] = "echo";
  b["text"] = text;
  return b;
}

}  // namespace

// ===========================================================================
// xpose
// ===========================================================================

ojson runXpose(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabXpose.known,
                                            kVocabXpose.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "xpose")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (!hasFlag(parsed.opts, "clear")) {
    // Stata's own rc for "this would lose the data in memory".
    return rsErrorRc("xpose: you must specify the `clear` option — transposing replaces every "
                     "variable and observation in memory.",
                     4);
  }
  const bool wantVarname = hasFlag(parsed.opts, "varname");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("xpose: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  if (n == 0) return rsErrorRc("xpose: no observations", 2000);

  const std::vector<std::string> before = ds->names();
  std::vector<std::string> numeric;
  int dropped = 0;
  for (const auto& nm : before) {
    if (ds->dtype(nm) == "f64")
      numeric.push_back(nm);
    else
      ++dropped;
  }
  if (numeric.empty())
    return rsError("xpose: every variable is a string variable; there is nothing numeric to "
                   "transpose (this build drops string variables, as Stata does).");

  // The transpose: one row per surviving ORIGINAL column, one column per
  // original observation, named v1..vn. `varname` puts the original names in a
  // leading string column `_varname`.
  CsvTable t;
  if (wantVarname) {
    t.names.push_back("_varname");
    t.isFactor.push_back(1);
  }
  for (int64_t i = 0; i < n; ++i) {
    t.names.push_back("v" + std::to_string(i + 1));
    t.isFactor.push_back(0);
  }
  t.rows.reserve(numeric.size());
  for (const auto& nm : numeric) {
    std::vector<std::string> row;
    row.reserve(t.names.size());
    if (wantVarname) row.push_back(nm);
    const double* d = ds->col(nm).f64();
    for (int64_t i = 0; i < n; ++i) row.push_back(numCell(d[i]));
    t.rows.push_back(std::move(row));
  }

  std::string outPath;
  if (auto err = writeAndConvert(session, t, "xpose", &outPath)) return *err;
  session.load(outPath);
  if (auto bad = assertRowCount(session, "xpose", static_cast<int64_t>(numeric.size())))
    return *bad;

  ojson out = ojson::array();
  if (dropped > 0) {
    out.push_back(echoBlock("(xpose: " + std::to_string(dropped) + " string variable(s) dropped)"));
  }
  if (hasFlag(parsed.opts, "promote")) {
    // READ, not ignored: this build stores every column as a double, so there
    // is no narrower type to promote to. Saying so beats accepting a storage
    // decision that never happened.
    out.push_back(echoBlock(
        "(xpose: promote has no effect in this build — every transposed variable is stored as "
        "double)"));
  }
  out.push_back(mutationResult(session, "xpose", t.names, before));
  return out;
}

}  // namespace piet::handlers
