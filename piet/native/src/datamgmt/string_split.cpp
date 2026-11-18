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

  // split.ado (StataBE 19.5): parse() holds parse STRINGS, the words of its
  // text (quotes keep a blank: parse(" " "-") is a blank and a hyphen;
  // parse(zzzzz) is one five-character string, parse(a e) two). With none the
  // string is a single blank and each remainder is trimmed. Each round cuts
  // every observation at the leftmost occurrence of any parse string; the round
  // repeats while some observation has text left, at least once, at most
  // limit() times. The source is trimmed first.
  const std::optional<std::string> parseOpt = optStrAny(parsed.opts, {"parse", "p"});
  std::vector<std::string> seps;
  bool trimParts = false;
  {
    const std::string raw = parseOpt.has_value() ? strutil::trim(*parseOpt) : std::string();
    size_t i = 0;
    while (i < raw.size()) {
      if (raw[i] == ' ' || raw[i] == '\t') {
        ++i;
        continue;
      }
      std::string tok;
      if (raw.compare(i, 2, "`\"") == 0) {
        const size_t e = raw.find("\"'", i + 2);
        tok = raw.substr(i + 2, (e == std::string::npos ? raw.size() : e) - (i + 2));
        i = e == std::string::npos ? raw.size() : e + 2;
      } else if (raw[i] == '"') {
        const size_t e = raw.find('"', i + 1);
        tok = raw.substr(i + 1, (e == std::string::npos ? raw.size() : e) - (i + 1));
        i = e == std::string::npos ? raw.size() : e + 1;
      } else {
        size_t e = i;
        while (e < raw.size() && raw[e] != ' ' && raw[e] != '\t') ++e;
        tok = raw.substr(i, e - i);
        i = e;
      }
      if (!tok.empty()) seps.push_back(tok);
    }
    if (seps.empty()) {
      seps.push_back(" ");
      trimParts = true;
    }
  }

  long long limit = -1;
  if (auto lv = optStr(parsed.opts, "limit")) {
    if (!parseStrictInt(*lv, &limit) || limit < 1)
      return rsSyntax("split: limit() requires a positive whole number, got `" + *lv + "'");
  }
  const bool destring = hasFlag(parsed.opts, "destring");
  std::string stub = optStrAny(parsed.opts, {"gen", "generate"}).value_or(var);
  if (!strutil::isValidStataIdentifier(stub + "1"))
    return rsSyntax("split: `" + stub + "' is not a usable variable-name stub — " +
                    strutil::stataIdentifierRuleText());

  const std::vector<std::string> values = columnAsStrings(*ds, var);
  auto trimBlank = [](const std::string& t) {
    size_t a = 0, b = t.size();
    while (a < b && t[a] == ' ') ++a;
    while (b > a && t[b - 1] == ' ') --b;
    return t.substr(a, b - a);
  };
  std::vector<std::string> rest;
  rest.reserve(values.size());
  bool anyObs = false;
  for (const auto& v : values) {
    anyObs = anyObs || !v.empty();
    rest.push_back(trimBlank(v));
  }
  if (!anyObs) return rsErrorRc("no observations", 2000);

  std::vector<std::vector<std::string>> allParts(values.size());
  size_t k = 0;
  bool go = true;
  while (go && (limit < 0 || static_cast<long long>(k) < limit)) {
    ++k;
    go = false;
    for (size_t r = 0; r < rest.size(); ++r) {
      std::string& vw = rest[r];
      size_t mp = std::string::npos, pl = 0;
      for (const auto& sp : seps) {
        const size_t at = vw.find(sp);
        if (at == std::string::npos) continue;
        if (mp == std::string::npos || at < mp) {
          mp = at;
          pl = sp.size();
        } else if (at == mp) {
          pl = sp.size();
        }
      }
      if (mp != std::string::npos) {
        allParts[r].push_back(vw.substr(0, mp));
        const std::string after = vw.substr(mp + pl);
        vw = trimParts ? trimBlank(after) : after;
      } else {
        allParts[r].push_back(vw);
        vw.clear();
      }
      go = go || !vw.empty();
    }
  }

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
  // split.ado's returns: r(varlist) (the new names, each followed by a blank),
  // r(nvars) (a macro) and r(k_new) (vs-stata bank: `split make` ->
  // "make1 make2 make3 ", "3", 3).
  eval::replaceRClass(session.eval);
  {
    std::string vl;
    for (const auto& nm : newVars) vl += nm + " ";
    session.eval.rStrings.set("varlist", vl);
  }
  session.eval.rStrings.set("nvars", std::to_string(k));
  session.eval.rValues.set("k_new", static_cast<double>(k));
  eval::declareRPosted(session.eval, "split", {"varlist", "nvars", "k_new"});
  if (k == 0) {
    // Zero parts (an all-empty column, or limit(0)) is not an error: it is a
    // dataset with nothing to add, and r(nvars)=0 says so.
    return silent();
  }
  return mutationResult(session, "split", newVars);
}

}  // namespace piet::handlers
