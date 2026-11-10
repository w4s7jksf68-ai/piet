// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/variables.h"
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
// recast
// ===========================================================================
//
// Exact recasts change only metadata; force narrows the actual values through
// the same store boundary as generate/replace (w2e_float_default:1565–1637).
ojson runRecast(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRecast.known,
                                            kVocabRecast.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "recast")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() < 2)
    return rsSyntax("recast: syntax is `recast type varlist [, force]`");

  const std::string type = lowerOf(parsed.varlist[0]);
  static const std::set<std::string> kNumericTypes = {"byte", "int", "long", "float", "double"};
  bool isNumeric = kNumericTypes.count(type) > 0;
  bool isString = false;
  {
    static const std::regex reStr(R"(^str([0-9]+)$)");
    std::smatch m;
    if (std::regex_match(type, m, reStr)) {
      isString = true;
      const long long w = std::stoll(m.str(1));
      if (w < 1 || w > 2045)
        return rsSyntax("recast: `" + type + "' — a str# width must be between 1 and 2045");
    } else if (type == "strl") {
      return rsError("recast: strL is not implemented in this build (the .dta writer has no strL "
                     "write path).",
                     "unimplemented");
    }
  }
  if (!isNumeric && !isString)
    return rsSyntax("recast: `" + parsed.varlist[0] +
                    "' is not a storage type — expected byte, int, long, float, double or str#");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("recast: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  // Validate EVERY name before writing ANY metadata: a half-applied recast is
  // metadata that disagrees with the command the user typed.
  for (size_t i = 1; i < parsed.varlist.size(); ++i) {
    const std::string& v = parsed.varlist[i];
    if (!ds->has(v)) return rsError("variable " + v + " not found");
    const bool colIsString = ds->dtype(v) == "factor";
    if (colIsString && isNumeric)
      return rsError("recast: `" + v +
                     "' is a string variable and " + type +
                     " is numeric; recast never converts between the two (use `destring`).");
    if (!colIsString && isString)
      return rsError("recast: `" + v + "' is a numeric variable and " + type +
                     " is a string type; recast never converts between the two (use `tostring`).");
  }
  const bool force = hasFlag(parsed.opts, "force");
  const std::map<std::string, cecon::StoreType> storeTypes = {
      {"byte", cecon::StoreType::Byte}, {"int", cecon::StoreType::Int},
      {"long", cecon::StoreType::Long}, {"float", cecon::StoreType::Float},
      {"double", cecon::StoreType::Double}};
  // Decide each variable before mutation. Non-force loss is a successful no-op
  // for that variable, not a declaration of a type its values cannot hold.
  std::vector<std::string> accepted, changed;
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  std::set<std::string> seen;
  for (size_t i = 1; i < parsed.varlist.size(); ++i) {
    const std::string& name = parsed.varlist[i];
    if (!seen.insert(name).second) continue;
    bool changes = false;
    if (isNumeric) {
      const auto& col = ds->col(name);
      for (int64_t row = 0; row < ds->nRows(); ++row) {
        const double old = col.f64()[row];
        const double value = cecon::frame_detail::round_to_store(old, storeTypes.at(type));
        if (!std::isnan(old) && (std::isnan(value) || value != old)) { changes = true; break; }
      }
    } else {
      const auto& col = ds->col(name);
      const size_t width = static_cast<size_t>(std::stoll(type.substr(3)));
      for (const auto& value : col.levels)
        if (value.size() > width) { changes = true; break; }
      // A string force truncation needs a byte-preserving string store. Do not
      // mislabel an unchanged payload as a narrower string in its absence.
      if (changes && force)
        return rsError("recast: forced string truncation is not implemented", "unimplemented");
    }
    if (changes && !force) continue;
    accepted.push_back(name);
    if (changes) {
      changed.push_back(name);
      args.insert(args.end(), {"--gen", name + " = " + name});
    }
  }
  if (!changed.empty()) {
    storage::appendStoreTypeArg(args, type);
    const std::string out = nextPath(session.workdir(), "recast");
    args.insert(args.end(), {"--out", out});
    if (auto err = runVerb(args)) return *err;
    session.load(out);
  }
  for (const auto& name : accepted) {
    session.varMeta[name].stataType = type;
    session.implicitFloatVars.erase(name);
  }
  if (!changed.empty()) return mutationResult(session, "gen", {}, {}, changed);
  return silent();
}

}  // namespace piet::handlers
