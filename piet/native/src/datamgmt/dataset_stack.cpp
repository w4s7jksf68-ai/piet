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

// ===========================================================================
// stack
// ===========================================================================

ojson runStack(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabStack.known,
                                            kVocabStack.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "stack")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("stack: syntax is `stack varlist, into(newvars) [clear wide group(#)]`");
  const bool wide = hasFlag(parsed.opts, "wide");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("stack: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  for (const auto& v : parsed.varlist)
    if (!ds->has(v)) return rsError("variable " + v + " not found");

  const size_t nv = parsed.varlist.size();
  // [D] stack: into(newvars) names one block's variables, so without group()
  // the varlist is cut into blocks of as many variables as into() names
  // (`stack x1 y x2 y, into(a b) clear` stacks (x1,y) over (x2,y);
  // w2_esample_copies.do:281, StataBE `0|0|122|`, .log:702). Only with
  // neither option is every variable its own block.
  std::optional<std::string> intoOpt = optStr(parsed.opts, "into");
  long long groups = static_cast<long long>(nv);
  if (auto g = optStr(parsed.opts, "group")) {
    if (!parseStrictInt(*g, &groups) || groups < 1)
      return rsSyntax("stack: group() requires a positive whole number, got `" + *g + "'");
  } else if (intoOpt.has_value()) {
    const size_t k = splitWs(*intoOpt).size();
    if (k == 0 || nv % k != 0)
      return rsSyntax("stack: " + std::to_string(nv) + " variable(s) do not divide into blocks of " +
                      std::to_string(k) + ", the number of into() names");
    groups = static_cast<long long>(nv / k);
  }
  if (static_cast<size_t>(groups) > nv || nv % static_cast<size_t>(groups) != 0)
    return rsSyntax("stack: " + std::to_string(nv) + " variable(s) do not divide into " +
                    std::to_string(groups) + " equal group(s)");
  const size_t per = nv / static_cast<size_t>(groups);

  std::vector<std::string> intoNames;
  if (intoOpt.has_value()) {
    intoNames = splitWs(*intoOpt);
  } else if (wide) {
    return rsSyntax("stack: into() is required with `wide` — without it the stacked columns would "
                    "take the names of the variables `wide` is keeping.");
  } else {
    // Stata's own default: the new variables take the first group's names. Safe
    // because the stacked data replace the originals.
    intoNames.assign(parsed.varlist.begin(), parsed.varlist.begin() + static_cast<long>(per));
  }
  if (intoNames.size() != per)
    return rsSyntax("stack: into() names " + std::to_string(intoNames.size()) +
                    " variable(s) but each group has " + std::to_string(per));
  for (const auto& nm : intoNames)
    if (!strutil::isValidStataIdentifier(nm))
      return rsSyntax("stack: invalid variable name `" + nm + "' — " +
                      strutil::stataIdentifierRuleText());

  const int64_t n = ds->nRows();
  const std::vector<std::string> before = ds->names();

  // Column texts, read once. A stacked column is a factor if ANY of the source
  // columns feeding that position is one: mixing a string into a numeric column
  // has to widen, never truncate.
  std::map<std::string, std::vector<std::string>> textOf;
  for (const auto& nm : before) textOf[nm] = columnAsStrings(*ds, nm);

  CsvTable t;
  t.names.push_back("_stack");
  t.isFactor.push_back(0);
  for (size_t c = 0; c < per; ++c) {
    bool anyFactor = false;
    for (long long g = 0; g < groups; ++g)
      if (ds->dtype(parsed.varlist[static_cast<size_t>(g) * per + c]) == "factor") anyFactor = true;
    t.names.push_back(intoNames[c]);
    t.isFactor.push_back(anyFactor ? 1 : 0);
  }
  if (wide) {
    for (const auto& nm : before) {
      if (std::find(t.names.begin(), t.names.end(), nm) != t.names.end())
        return rsSyntax("stack: `wide` keeps the original variables, and into() reuses the name `" +
                        nm + "' — choose different into() names.");
      t.names.push_back(nm);
      t.isFactor.push_back(ds->dtype(nm) == "factor" ? 1 : 0);
    }
  }
  t.rows.reserve(static_cast<size_t>(n) * static_cast<size_t>(groups));
  for (long long g = 0; g < groups; ++g) {
    for (int64_t i = 0; i < n; ++i) {
      std::vector<std::string> row;
      row.reserve(t.names.size());
      row.push_back(std::to_string(g + 1));
      for (size_t c = 0; c < per; ++c)
        row.push_back(textOf[parsed.varlist[static_cast<size_t>(g) * per + c]][static_cast<size_t>(i)]);
      if (wide)
        for (const auto& nm : before) row.push_back(textOf[nm][static_cast<size_t>(i)]);
      t.rows.push_back(std::move(row));
    }
  }

  std::string outPath;
  if (auto err = writeAndConvert(session, t, "stack", &outPath)) return *err;
  session.load(outPath);
  if (auto bad = assertRowCount(session, "stack", n * groups)) return *bad;
  // BUG-1147: `clear` only silences stack's warning; without it Stata warns, waits
  // for a key (a no-op in a do-file), and stacks all the same, whatever the data in
  // memory: `stack x1 x2 y z1, into(a b)` is rc 0, _N 4 (StataBE 19.5, stack.ado
  // never reads c(changed)). The warning comes after the varlist checks, so a
  // refused stack does not print it, and `quietly` drops it.
  if (hasFlag(parsed.opts, "clear")) return mutationResult(session, "stack", t.names, before);
  ojson out = ojson::array();
  out.push_back(displayBlock("Warning: data in memory will be lost.\n"
                             "         Press any key to continue, Ctrl-Break to abort."));
  out.push_back(mutationResult(session, "stack", t.names, before));
  return out;
}

}  // namespace piet::handlers
