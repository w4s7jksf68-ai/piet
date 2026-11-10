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


// ===========================================================================
// split
// ===========================================================================
//
// PIET-side decomposition, then k `setcol --new` calls. Deliberately NOT
// `gen stubj = word(v, j)`: `word()` is whitespace-only, and `parse("-")` is
// the corpus's most common form by a wide margin. Deliberately not a new cobra
// `split` verb either -- that would make this a two-binary change.
ojson runSplit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplit.known,
                                            kVocabSplit.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "split")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("split: syntax is `split varname [, parse(chars) gen(stub) limit(#) destring]`");
  if (parsed.varlist.size() > 1)
    return rsError("split: this build splits ONE variable per command; `" +
                       joinWith(parsed.varlist, " ") +
                       "` names " + std::to_string(parsed.varlist.size()) +
                       ". Run split once per variable.",
                   "unimplemented");

  const std::string var = parsed.varlist[0];
  ReadFrame rf(session, {var});
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return rsError("split: could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  if (!ds->has(var)) return rsError("variable " + var + " not found");
  if (ds->dtype(var) != "factor")
    return rsError("split: `" + var +
                   "' is a numeric variable; split works on string variables (Stata's `force` "
                   "option, which splits the numeric display text, is not implemented here).");

  // parse(): every CHARACTER given is a separator, and EVERY occurrence
  // separates, so `a--b` under parse("-") yields three parts and the middle one
  // is empty. With no parse(), the separator is whitespace: consecutive runs
  // collapse and leading/trailing whitespace is ignored (Stata's default).
  const std::optional<std::string> parseOpt = optStrAny(parsed.opts, {"parse", "p"});
  const std::string sepChars = parseOpt.value_or(std::string());
  const bool wsMode = !parseOpt.has_value();

  long long limit = -1;
  if (auto lv = optStr(parsed.opts, "limit")) {
    if (!parseStrictInt(*lv, &limit) || limit < 0)
      return rsSyntax("split: limit() requires a non-negative whole number, got `" + *lv + "'");
  }
  const bool destring = hasFlag(parsed.opts, "destring");
  std::string stub = optStrAny(parsed.opts, {"gen", "generate"}).value_or(var);
  if (!strutil::isValidStataIdentifier(stub + "1"))
    return rsSyntax("split: `" + stub + "' is not a usable variable-name stub — " +
                    strutil::stataIdentifierRuleText());

  const std::vector<std::string> values = columnAsStrings(*ds, var);

  auto splitOne = [&](const std::string& s) {
    std::vector<std::string> parts;
    if (wsMode) {
      std::string cur;
      for (char c : s) {
        if (c == ' ' || c == '\t') {
          if (!cur.empty()) {
            parts.push_back(cur);
            cur.clear();
          }
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) parts.push_back(cur);
    } else {
      std::string cur;
      for (char c : s) {
        if (sepChars.find(c) != std::string::npos) {
          parts.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      parts.push_back(cur);
      // A wholly empty source cell yields no parts at all, not one empty part.
      if (parts.size() == 1 && parts[0].empty()) parts.clear();
    }
    return parts;
  };

  std::vector<std::vector<std::string>> allParts;
  allParts.reserve(values.size());
  size_t k = 0;
  for (const auto& v : values) {
    allParts.push_back(splitOne(v));
    k = std::max(k, allParts.back().size());
  }
  if (limit >= 0) k = std::min(k, static_cast<size_t>(limit));

  // Every new name has to be legal and free BEFORE the first write, so a
  // collision on part 3 cannot leave parts 1 and 2 behind.
  std::vector<std::string> newVars;
  for (size_t j = 1; j <= k; ++j) {
    const std::string name = stub + std::to_string(j);
    ojson guard = newVarGuard(session, "split", name);
    if (!guard.is_null()) return guard;
    newVars.push_back(name);
  }

  std::string cur = *session.currentHbin();
  std::vector<std::string> partTypes(k);
  for (size_t j = 0; j < k; ++j) {
    std::vector<std::string> cells;
    cells.reserve(values.size());
    bool allNumeric = true;
    bool anyValue = false;
    for (const auto& parts : allParts) {
      if (j < parts.size()) {
        cells.push_back(parts[j]);
        if (!parts[j].empty()) {
          anyValue = true;
          double tmp = 0.0;
          if (!parseStrictDouble(parts[j], &tmp)) allNumeric = false;
        }
      } else {
        cells.emplace_back();
      }
    }
    // `destring` promotes part j to f64 only when EVERY non-empty value in that
    // part is a number -- per part, not per command, which is what makes
    // `split "1-a"` keep part 2 a string instead of failing the whole call.
    const std::string dtype = (destring && anyValue && allNumeric) ? "f64" : "factor";
    if (dtype == "f64") {
      // read_csv_column parses the f64 payload itself; blanks stay missing.
      // destring's storage rule: the smallest type that holds every value
      // (aer_w2 w2b_dataio split_destring: K2 byte).
      bool allInt = true, allFloat = true;
      double lo = 0.0, hi = 0.0;
      bool seen = false;
      for (auto& c : cells) {
        if (c.empty()) continue;
        double tmp = 0.0;
        parseStrictDouble(c, &tmp);
        if (!seen) { lo = hi = tmp; seen = true; }
        lo = std::min(lo, tmp);
        hi = std::max(hi, tmp);
        if (tmp != std::floor(tmp)) allInt = false;
        if (static_cast<double>(static_cast<float>(tmp)) != tmp) allFloat = false;
        c = numCell(tmp);
      }
      std::string t = "double";
      if (allInt && lo >= -127 && hi <= 100) t = "byte";
      else if (allInt && lo >= -32767 && hi <= 32740) t = "int";
      else if (allInt && lo >= -2147483647.0 && hi <= 2147483620.0) t = "long";
      else if (allFloat) t = "float";
      partTypes[j] = t;
    }
    std::string out;
    if (auto err = appendColumn(session, cur, newVars[j], cells, dtype, &out)) return *err;
    cur = out;
  }

  if (k > 0) session.load(cur);
  for (size_t j = 0; j < k; ++j) {
    if (!partTypes[j].empty()) {
      session.implicitFloatVars.erase(newVars[j]);
      session.varMeta[newVars[j]].stataType = partTypes[j];
    }
  }
  // r(nvars)/r(varlist), the two things a do-file reads back from split. Filled
  // HERE because the arm does not call captureRFromBlock for this command.
  session.eval.rValues.set("nvars", static_cast<double>(k));
  session.eval.rStrings.set("varlist", joinWith(newVars, " "));
  // split's whole r() set (r(nvars) and r(varlist)), so an r() it does not post reads missing (w2b append_ge(src):
  // r(sum) after split reads `.`).
  eval::declareRPosted(session.eval, "split", {"nvars", "varlist"});
  if (k == 0) {
    // Zero parts (an all-empty column, or limit(0)) is not an error: it is a
    // dataset with nothing to add, and r(nvars)=0 says so.
    return silent();
  }
  return mutationResult(session, "split", newVars);
}

}  // namespace piet::handlers
