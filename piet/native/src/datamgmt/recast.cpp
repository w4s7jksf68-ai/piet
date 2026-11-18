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
#include "piet/loops.h"  // expandVarlist
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
ojson runRecast(const eval::ParsedCommand& parsedIn, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsedIn.opts, kVocabRecast.known,
                                            kVocabRecast.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsedIn, "recast")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  // StataBE 19.5 (vs-stata bank 2026-11-10): a type and no varlist, or a lone
  // varlist, is r(100) "varlist required" (`recast double`, `recast price`); a
  // word that is no storage type, a str# outside 1..2045, and a numeric type
  // for a string variable (or the reverse) are r(109) "VAR:  TYPE invalid",
  // per variable after the names are found (`recast zznotatype price`, `recast
  // str0 make`, `recast str3000 make`, `recast double make`).
  if (parsedIn.varlist.size() < 2)
    return rsErrorRc("varlist required", 100);
  // The names after the type are a varlist: ranges (`price-weight`), wildcards
  // and abbreviations expand first.
  eval::ParsedCommand parsed = parsedIn;
  {
    std::string spec;
    for (size_t i = 1; i < parsedIn.varlist.size(); ++i)
      spec += (spec.empty() ? "" : " ") + parsedIn.varlist[i];
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    parsed.varlist.resize(1);
    for (const auto& t : ex.tokens) parsed.varlist.push_back(t);
  }
  const std::string type = lowerOf(parsed.varlist[0]);
  static const std::set<std::string> kNumericTypes = {"byte", "int", "long", "float", "double"};
  bool isNumeric = kNumericTypes.count(type) > 0;
  bool isString = false;
  {
    static const std::regex reStr(R"(^str([0-9]+)$)");
    std::smatch m;
    if (std::regex_match(type, m, reStr)) {
      isString = true;
      const long long w = m.str(1).size() > 6 ? 0 : std::stoll(m.str(1));
      if (w < 1 || w > 2045) isString = false;  // r(109) per variable below
    } else if (type == "strl") {
      return rsError("recast: strL is not implemented in this build (the .dta writer has no strL "
                     "write path).",
                     "unimplemented");
    }
  }

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
  }
  for (size_t i = 1; i < parsed.varlist.size(); ++i) {
    const std::string& v = parsed.varlist[i];
    const bool colIsString = ds->dtype(v) == "factor";
    if ((!isNumeric && !isString) || (colIsString && isNumeric) || (!colIsString && isString))
      return rsErrorRc(v + ":  " + parsed.varlist[0] + " invalid", 109);
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
  std::optional<int64_t> lastCount;
  for (size_t i = 1; i < parsed.varlist.size(); ++i) {
    const std::string& name = parsed.varlist[i];
    if (!seen.insert(name).second) continue;
    bool changes = false;
    // recast tests every variable by counting the values a copy in the new type
    // would change (`count if x ~= copy`), and that count is the last r(N) it
    // leaves behind -- the variable already held in the requested type is
    // skipped before any count (vs-stata bank: `recast double price` r(N) 0,
    // `recast byte price` 74, `recast int f, force` 37).
    const auto heldIt = session.varMeta.find(name);
    const bool alreadyThatType = heldIt != session.varMeta.end() &&
                                 heldIt->second.stataType.has_value() &&
                                 *heldIt->second.stataType == type;
    int64_t nDiff = 0;
    if (isNumeric) {
      const auto& col = ds->col(name);
      for (int64_t row = 0; row < ds->nRows(); ++row) {
        const double old = col.f64()[row];
        const double value = cecon::frame_detail::round_to_store(old, storeTypes.at(type));
        if (!std::isnan(old) && (std::isnan(value) || value != old)) { changes = true; ++nDiff; }
      }
    } else {
      const auto& col = ds->col(name);
      const size_t width = static_cast<size_t>(std::stoll(type.substr(3)));
      for (int64_t row = 0; row < ds->nRows(); ++row) {
        const int32_t code = col.codes()[row];
        if (code >= 0 && static_cast<size_t>(code) < col.levels.size() &&
            col.levels[static_cast<size_t>(code)].size() > width) {
          changes = true;
          ++nDiff;
        }
      }
      // A string force truncation needs a byte-preserving string store. Do not
      // mislabel an unchanged payload as a narrower string in its absence.
      if (changes && force)
        return rsError("recast: forced string truncation is not implemented", "unimplemented");
    }
    if (!alreadyThatType) lastCount = nDiff;
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
    // c(changed): a storage type that moves marks the data changed; a recast
    // refused for loss ("74 values would be changed; not changed") or to the
    // type already held does not (vs-stata bank: `recast str5 make` -> 0,
    // `recast byte foreign rep78` -> 1).
    auto mit = session.varMeta.find(name);
    const bool hadType = mit != session.varMeta.end() && mit->second.stataType.has_value();
    if (!hadType || *mit->second.stataType != type) session.datasetChanged = true;
    session.varMeta[name].stataType = type;
    session.implicitFloatVars.erase(name);
  }
  if (lastCount.has_value()) {
    // The `count` that tested the last variable is an r-class command: it
    // replaces r() with r(N).
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(*lastCount));
    eval::declareRPosted(session.eval, "recast", {"N"});
  }
  if (!changed.empty()) return mutationResult(session, "gen", {}, {}, changed);
  return silent();
}

}  // namespace piet::handlers
