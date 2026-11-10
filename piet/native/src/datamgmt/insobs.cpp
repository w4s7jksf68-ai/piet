// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/observations.h"
#include "piet/handlers/session_commands.h"
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
// insobs
// ===========================================================================

ojson runInsobs(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInsobs.known,
                                            kVocabInsobs.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "insobs")) return *q;
  if (parsed.varlist.empty())
    return rsSyntax("insobs: syntax is `insobs # [, before(#) | after(#)]`");
  long long m = 0;
  if (!parseStrictInt(parsed.varlist[0], &m) || m <= 0)
    return rsSyntax("insobs: `" + parsed.varlist[0] +
                    "' is not a positive whole number of observations");

  const std::optional<std::string> beforeOpt = optStr(parsed.opts, "before");
  const std::optional<std::string> afterOpt = optStr(parsed.opts, "after");
  if (beforeOpt.has_value() && afterOpt.has_value())
    return rsSyntax("insobs: before() and after() cannot both be specified");

  if (!session.hasData()) {
    if (beforeOpt.has_value() || afterOpt.has_value())
      return rsError("insobs: before()/after() need data in memory to insert into");
    return runSetObs(std::to_string(m), session);
  }

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("insobs: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  // Where the blank rows go. All three forms take the SAME string-safe path
  // below; the plain form is simply "insert before observation n+1".
  //
  // The plan had the plain form delegate to `set obs N+m` -- one implementation
  // of "pad the frame with blank observations" rather than two
  // (runSetObs in environment/workspace_commands.cpp). MEASURED 2026-09-04, it cannot be reused: that path
  // is broken twice over on main, both pre-existing, both filed rather than
  // fixed here.
  //
  //   BUG-411 -- it calls `runCobraJson` on `append`, which prints a plain
  //   text line ("append: wrote ... = N rows"), so every `set obs N` that GROWS
  //   a loaded frame dies with "cobra produced non-JSON output". A two-line
  //   do-file reproduces it. `set obs` on an EMPTY session takes a different
  //   branch and works, which is why it survived this long.
  //
  //   BUG-412 -- underneath that, its pad CSV types every string column as
  //   numeric, so `cobra append` promotes the master's string column and
  //   replaces each value with its factor CODE: a frame whose `state` reads
  //   "al" comes back reading 0. BUG-411 is the only reason nobody has been
  //   bitten yet -- the visible failure masks the silent one, exactly as
  //   BUG-401's over-block masked BUG-404.
  //
  // The path below does not have BUG-412 because `writeAndConvert` carries a
  // per-column factor flag, so the pad frame is built with the master's own
  // dtypes and `append` has nothing to promote. Verified on `fatalities`:
  // state[1]=="al" before, state[3]=="al" after `insobs 2, before(1)`.
  long long pos = 0;
  if (!beforeOpt.has_value() && !afterOpt.has_value()) {
    pos = n + 1;
  } else if (beforeOpt.has_value()) {
    if (!parseStrictInt(*beforeOpt, &pos) || pos < 1 || pos > n + 1)
      return rsSyntax("insobs: before(" + *beforeOpt + ") must be an observation number between 1 and " +
                      std::to_string(n + 1));
  } else {
    if (!parseStrictInt(*afterOpt, &pos) || pos < 0 || pos > n)
      return rsSyntax("insobs: after(" + *afterOpt + ") must be an observation number between 0 and " +
                      std::to_string(n));
    pos += 1;  // `after(k)` is `before(k+1)`
  }

  // Positional insert, in four engine calls: stamp a sequence, append the blank
  // rows carrying a fractional sequence value that lands them exactly where the
  // user asked, sort, drop the stamp.
  const std::string seqVar = "__piet_insobs_seq";
  if (ds->has(seqVar))
    return rsError("insobs: the scratch column " + seqVar +
                   " already exists in this dataset; rename it and retry.");

  const std::string basePath = *session.currentHbin();
  const std::string seqPath = nextPath(session.workdir(), "insobs_seq");
  if (auto err = runVerb({"eval", "--data", basePath, "--egen",
                          std::string(seqVar) + " = seq()", "--out", seqPath}))
    return *err;
  // The blank-row frame and the sort below each write scratch, which pushes the
  // reclaim budget over and makes these two -- the oldest at that moment -- the
  // first eviction candidates while they are still the thing being read from.
  SessionState::ScratchPin pin(session, {basePath, seqPath});

  CsvTable t;
  for (const auto& nm : ds->names()) {
    t.names.push_back(nm);
    t.isFactor.push_back(ds->dtype(nm) == "factor" ? 1 : 0);
  }
  t.names.push_back(seqVar);
  t.isFactor.push_back(0);
  // Every new cell is MISSING -- `.`, never 0 -- and the sequence stamp is the
  // half-integer just below the target position, so an ascending sort drops the
  // new rows immediately before observation `pos` without disturbing any other
  // row's order.
  const std::string stamp = eval::pythonFloatRepr(static_cast<double>(pos) - 0.5);
  for (long long r = 0; r < m; ++r) {
    std::vector<std::string> row(t.names.size());
    row.back() = stamp;
    t.rows.push_back(std::move(row));
  }
  std::string padPath;
  if (auto err = writeAndConvert(session, t, "insobs_pad", &padPath)) return *err;

  const std::string appended = nextPath(session.workdir(), "insobs");
  if (auto err = runVerb({"append", "--data", seqPath, "--using", padPath, "--out", appended}))
    return *err;
  session.load(appended);

  ojson sorted = runSort({seqVar}, session);
  if (isErrorBlock(sorted)) return sorted;

  const std::string finalPath = nextPath(session.workdir(), "insobs_done");
  if (auto err = runVerb({"eval", "--data", *session.currentHbin(), "--drop", seqVar, "--out",
                          finalPath}))
    return *err;
  session.load(finalPath);

  if (auto bad = assertRowCount(session, "insobs", n + m)) return *bad;
  return mutationResult(session, "insobs");
}

}  // namespace piet::handlers
