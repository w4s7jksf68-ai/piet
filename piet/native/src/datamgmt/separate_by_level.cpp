// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
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

// A number the way Stata names a variable after it: an integral value loses its
// decimal point, and the characters an identifier cannot carry become `_`.
std::string levelSuffix(double v) {
  std::string s;
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    s = std::to_string(static_cast<long long>(v));
  } else {
    s = eval::pythonFloatRepr(v);
  }
  std::string out;
  for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
  return out;
}

// A level's DISPLAY text, for a label: the plain number, not the sanitized name.
std::string levelText(double v) {
  if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
  return eval::pythonFloatRepr(v);
}

}  // namespace

// ===========================================================================
// separate
// ===========================================================================

ojson runSeparate(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSeparate.known,
                                            kVocabSeparate.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "separate")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return rsSyntax("separate: syntax is `separate varname, by(groupvar) [generate(stub) "
                    "shortlabel]`");
  const std::string var = parsed.varlist[0];
  const std::optional<std::string> byOpt = optStr(parsed.opts, "by");
  if (!byOpt.has_value() || byOpt->empty())
    return rsSyntax("separate: by() is required");
  const std::vector<std::string> byVars = splitWs(*byOpt);
  if (byVars.size() != 1)
    return rsError("separate: this build separates by ONE variable; by(" + *byOpt + ") names " +
                       std::to_string(byVars.size()) + ".",
                   "unimplemented");
  const std::string gvar = byVars[0];
  const bool shortLabel = hasFlag(parsed.opts, "shortlabel");
  const std::string stub = optStrAny(parsed.opts, {"generate", "gen"}).value_or(var);

  ReadFrame rf(session, {var, gvar});
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return rsError("separate: could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  if (!ds->has(var)) return rsError("variable " + var + " not found");
  if (!ds->has(gvar)) return rsError("variable " + gvar + " not found");
  if (ds->dtype(var) == "factor")
    return rsError("separate: `" + var +
                   "' is a string variable; separate builds numeric columns.");

  // The distinct levels of the by() variable, and for each one the expression
  // that selects it. A factor by() compares against the level TEXT, which the
  // engine's expression parser resolves directly (`factorvar == "literal"`).
  const bool gIsFactor = ds->dtype(gvar) == "factor";
  std::vector<std::string> suffixes;   // name suffix per level
  std::vector<std::string> labelText;  // level as the label shows it
  std::vector<std::string> predicates;
  if (gIsFactor) {
    const cecon::Column& c = ds->col(gvar);
    std::set<std::string> seen;
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < ds->nRows(); ++i) {
      const int32_t code = codes[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) continue;
      seen.insert(c.levels[static_cast<size_t>(code)]);
    }
    for (const auto& lv : seen) {
      if (lv.find('"') != std::string::npos)
        return rsError("separate: the level `" + lv + "' of " + gvar +
                       " contains a double quote, which this build cannot express in the "
                       "selection expression.");
      std::string sfx;
      for (char ch : lv) sfx += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
      suffixes.push_back(sfx);
      labelText.push_back(lv);
      predicates.push_back(gvar + "==\"" + lv + "\"");
    }
  } else {
    std::vector<double> g = columnAsDoubles(*ds, gvar);
    std::set<double> seen;
    for (double v : g)
      if (!std::isnan(v)) seen.insert(v);
    for (double lv : seen) {
      suffixes.push_back(levelSuffix(lv));
      labelText.push_back(levelText(lv));
      predicates.push_back(gvar + "==" + eval::pythonFloatRepr(lv));
    }
  }
  if (suffixes.empty())
    return rsError("separate: " + gvar + " has no non-missing values to separate by");
  if (suffixes.size() > 100)
    return rsError("separate: " + gvar + " has " + std::to_string(suffixes.size()) +
                   " distinct values; this build refuses to create more than 100 columns from one "
                   "`separate` (raise the by() variable's granularity instead).");

  std::vector<std::string> newVars;
  for (const auto& sfx : suffixes) {
    const std::string nm = stub + sfx;
    ojson guard = newVarGuard(session, "separate", nm);
    if (!guard.is_null()) return guard;
    if (std::find(newVars.begin(), newVars.end(), nm) != newVars.end())
      return rsError("separate: two levels of " + gvar + " both produce the variable name `" + nm +
                     "'; use generate() to pick a different stub.");
    newVars.push_back(nm);
  }

  // One `--gen` per engine call (cobra's eval takes one assignment at a time),
  // each step adopting its own output as the head before the next reads it —
  // the same shape a sequence of `gen` commands already takes (data_generate.cpp's
  // runGen), which is what makes the resident-frame opt-in safe: no
  // intermediate is ever read as a file behind the session's back.
  SessionState::ResidentEligible resident(session);
  for (size_t j = 0; j < newVars.size(); ++j) {
    const std::string out = nextPath(session.workdir(), "separate");
    const std::string expr = newVars[j] + "=cond((" + predicates[j] + "),(" + var + "),.)";
    if (auto err = runVerb({"eval", "--data", *session.currentHbin(), "--gen", expr, "--out", out}))
      return *err;
    session.load(out);
  }
  for (size_t j = 0; j < newVars.size(); ++j) {
    session.varMeta[newVars[j]].label =
        shortLabel ? labelText[j] : (var + ", " + gvar + " == " + labelText[j]);
  }
  return mutationResult(session, "separate", newVars);
}

}  // namespace piet::handlers
