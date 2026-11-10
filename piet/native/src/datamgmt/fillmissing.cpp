// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// fillmissing: fill missing values within by() groups (copy fills and statistic fills).
// Shared helpers: column_write_support.h. The family note for this command:
//
//   fillmissing  implements fillmissing per its help file (Shah). The copy fills
//                (any/previous/next/first/last) are row copies; the statistic fills
//                (mean/min/max/median) are a DISPATCHED `egen` (so the statistic and its
//                float storage are egen's) followed by a dispatched `replace` (so type
//                promotion, `byte` -> `float`, is replace's). `any` is the group's SMALLEST
//                non-missing value -- the package sorts the group descending and carries
//                the last value forward (StataBE probe p1 any: ., 5, ., 3 -> 3, not 5).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers/data_values.h"
#include "piet/loops.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "column_write_support.h"

// integ's spline must round every product and sum separately, as StataBE's expression
// evaluator does; a fused multiply-add changes the last bit (integ_basic is pinned to it).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch. The aliases are the minimum
// abbreviations the commands' own syntax lines declare (Generate -> g, Trapezoid -> t,
// Initial -> i, Label -> l, Nquantiles -> n), which the 2-character prefix floor cannot reach.
namespace {

// fillmissing: with() and by(); `by` is also how the dispatcher hands over the by-prefix.
const piet::eval::OptVocab& kVocabFillmissing =
    piet::eval::registerOptVocab({"fillmissing"}, {{"with", "by"}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace column_write_detail;

namespace {

// ===========================================================================
// fillmissing
// ===========================================================================

// A dense group id per row over the key columns (first-seen numbering).
std::vector<int64_t> groupIds(const std::vector<KeyCol>& keys, size_t n, int64_t* nGroups) {
  std::vector<int64_t> gid(n, 0);
  std::unordered_map<std::string, int64_t> seen;
  for (size_t i = 0; i < n; ++i) {
    std::string key;
    for (const auto& k : keys) {
      if (k.isString) {
        key += 'S';
        key += k.str[i];
        key += '\x1f';
      } else {
        double v = k.num[i];
        uint64_t bits = 0;
        if (std::isnan(v)) {
          bits = 0xFFFFFFFFFFFFFFFFull;
        } else {
          std::memcpy(&bits, &v, sizeof(bits));
        }
        key += 'N';
        key.append(reinterpret_cast<const char*>(&bits), sizeof(bits));
      }
    }
    auto it = seen.find(key);
    if (it == seen.end()) it = seen.emplace(key, static_cast<int64_t>(seen.size())).first;
    gid[i] = it->second;
  }
  *nGroups = static_cast<int64_t>(seen.size());
  return gid;
}

enum class FillMode { Any, Previous, Next, First, Last, Mean, Min, Max, Median, Inert };

bool fillModeOf(const std::string& w, FillMode* m) {
  static const std::map<std::string, FillMode> kModes = {
      {"any", FillMode::Any},       {"previous", FillMode::Previous}, {"next", FillMode::Next},
      {"first", FillMode::First},   {"last", FillMode::Last},         {"mean", FillMode::Mean},
      {"min", FillMode::Min},       {"max", FillMode::Max},           {"median", FillMode::Median},
      {"rand", FillMode::Inert},    {"pattern", FillMode::Inert}};
  auto it = kModes.find(w);
  if (it == kModes.end()) return false;
  *m = it->second;
  return true;
}

bool isStatMode(FillMode m) {
  return m == FillMode::Mean || m == FillMode::Min || m == FillMode::Max || m == FillMode::Median;
}

bool isOrderMode(FillMode m) {
  return m == FillMode::Previous || m == FillMode::Next || m == FillMode::First ||
         m == FillMode::Last;
}

// The copy fills, over one column's cells ("" = missing), within groups of rows that share a
// group id, in the current row order. Only rows in the sample are filled; a group is either
// wholly in or wholly out of the sample (the sample marker is part of the group key).
// `firstPass` receives how many cells the package's first `replace` of with(next) fills (a
// missing row followed by a non-missing one); the rest are its second, reversed pass.
std::vector<std::string> copyFill(const std::vector<std::string>& cells,
                                  const std::vector<double>* num, FillMode mode,
                                  const std::vector<int64_t>& gid, int64_t nGroups,
                                  const std::vector<char>& mask, int64_t* firstPass) {
  *firstPass = 0;
  const size_t n = cells.size();
  std::vector<std::vector<size_t>> members(static_cast<size_t>(nGroups));
  for (size_t i = 0; i < n; ++i) members[static_cast<size_t>(gid[i])].push_back(i);
  std::vector<std::string> out = cells;
  for (const auto& rows : members) {
    if (rows.empty() || !mask[rows.front()]) continue;
    switch (mode) {
      case FillMode::Any: {
        // The smallest non-missing value: the package sorts the group descending and carries
        // the last non-missing value into the missing rows below it.
        const std::string* best = nullptr;
        size_t bestRow = 0;
        for (size_t r : rows) {
          if (cells[r].empty()) continue;
          const bool better =
              best == nullptr || (num != nullptr ? (*num)[r] < (*num)[bestRow] : cells[r] < *best);
          if (better) {
            best = &cells[r];
            bestRow = r;
          }
        }
        if (best != nullptr)
          for (size_t r : rows)
            if (out[r].empty()) out[r] = *best;
        break;
      }
      case FillMode::Previous:
        for (size_t k = 1; k < rows.size(); ++k)
          if (out[rows[k]].empty()) out[rows[k]] = out[rows[k - 1]];
        break;
      case FillMode::Next:
        for (size_t k = 0; k + 1 < rows.size(); ++k)
          if (cells[rows[k]].empty() && !cells[rows[k + 1]].empty()) ++*firstPass;
        for (size_t k = rows.size(); k-- > 1;)
          if (out[rows[k - 1]].empty()) out[rows[k - 1]] = out[rows[k]];
        break;
      case FillMode::First: {
        const std::string v = cells[rows.front()];
        for (size_t r : rows)
          if (out[r].empty()) out[r] = v;
        break;
      }
      case FillMode::Last: {
        const std::string v = cells[rows.back()];
        for (size_t r : rows)
          if (out[r].empty()) out[r] = v;
        break;
      }
      default:
        break;
    }
  }
  return out;
}

// replace's and egen's notes: "(N real change(s) made)", "(N missing value(s) generated)".
std::string changesNote(int64_t n) {
  return "(" + std::to_string(n) + " real change" + (n == 1 ? "" : "s") + " made)\n";
}
std::string missingNote(int64_t n) {
  return "(" + std::to_string(n) + " missing value" + (n == 1 ? "" : "s") + " generated)\n";
}

}  // namespace

// `fillmissing varlist [if] [in] [, with(any|previous|next|first|last|mean|min|max|median)
// by(varlist)]`; `by g:` arrives as by(). Implements fillmissing per its help file (Shah):
// within each by() group of the sample, a missing value takes the group's smallest value
// (any, the default; probe p1 any), the previous / next non-missing value, the value at the
// group's first / last row even when that is missing (fill_first_by: all four stay .), or the
// group's mean / min / max / median as egen stores it (float: probe p2 meand 5/3 ->
// +1.aaaaaa0000000X+000; a byte variable widens to float, probe p3 meanb). A string variable
// takes any/previous/next/first/last; with a statistic it is reported and skipped at rc 0
// (fill_str_mean). The data keep their row order.
ojson runFillmissing(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFillmissing.known,
                                            kVocabFillmissing.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->nRows() == 0) return displayBlock("No observation in the dataset\n");
  }
  std::string with = "any";
  if (auto w = optStr(parsed.opts, "with")) {
    if (!w->empty()) with = *w;
  }
  FillMode mode = FillMode::Any;
  if (with == "linear")
    return ojson(eval::unsupportedOption(
        "with(linear)",
        "fillmissing: with(linear) recasts the variable to float and averages the neighbouring "
        "values through the package's own Mata routine; this version of PIET does not serve it. "
        "ipolate does linear interpolation."));
  if (!fillModeOf(with, &mode))  // the package prints its usage and exits at rc 0
    return displayBlock(
        "Option with() incorrectly specified\nwith accepts only these : previous, next, first, "
        "last, any\nmean, min, max, median, rand, pattern\n");
  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) {
    if (auto e = resolveVars(session, splitWs(*b), &byVars)) return *e;
  }

  // The package decides "string" once and never resets it, so every variable after a string
  // one is handled as a string (probe p2 strnum). With a statistic that only adds its notice;
  // with a copy fill a numeric variable meets `== ""` and StataBE stops at r(109) part way
  // through, leaving the data re-sorted. That is refused before anything is written.
  std::vector<char> asString(vars.size(), 0);
  {
    Datastore* ds = session.datastore();
    bool seenString = false;
    for (size_t j = 0; j < vars.size(); ++j) {
      const bool isStr = ds->col(vars[j]).dtype == cecon::DType::Factor;
      if (!isStr && seenString && mode != FillMode::Inert && !isStatMode(mode))
        return rcError("type mismatch -- fillmissing handles every variable after the string "
                       "variable in the varlist as a string; run it once per variable",
                       109);
      seenString = seenString || isStr;
      asString[j] = seenString ? 1 : 0;
    }
  }

  ScratchCols scratch(session);
  const std::string touseCol = "__piet_t1g_touse";
  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask, isStatMode(mode) ? touseCol : "")) return *e;
  if (isStatMode(mode) && hasQualifier(parsed)) scratch.add(touseCol);

  // Groups: the by() variables and the sample marker, as the package forms them.
  std::vector<KeyCol> keys;
  {
    Datastore* ds = session.datastore();
    for (const auto& b : byVars) keys.push_back(readKeyCol(*ds, b));
    KeyCol t;
    t.num.reserve(mask.size());
    for (char m : mask) t.num.push_back(m ? 1.0 : 0.0);
    keys.push_back(std::move(t));
  }
  const size_t n = mask.size();
  // The order fills read the group in its sorted order. When the rows are not already in
  // by()/sample order StataBE sorts them first, and its sort leaves tied rows in an order of
  // its own (probe p1 in: `in 1/2, with(last)` read row 1 as the group's last); that order
  // cannot be reproduced, so those fills are refused instead of answered from another order.
  if (isOrderMode(mode)) {
    for (size_t i = 1; i < n; ++i) {
      if (keysCompare(keys, i - 1, i) > 0)
        return rcError("fillmissing, with(" + with + "): the data are not in by()/sample order, "
                       "and Stata would read each group in the order its own sort leaves tied "
                       "rows in, which this version of PIET cannot reproduce. Sort by the by() "
                       "variables first (`sort g t`) and fill without if/in, or restrict with "
                       "`keep if` first.",
                       199);
    }
  }
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(keys, n, &nGroups);

  std::string text;
  std::vector<std::string> rewritten;
  for (size_t j = 0; j < vars.size(); ++j) {
    const std::string& v = vars[j];
    if (asString[j] && isStatMode(mode)) {
      text += "Variable " + v + " is a string variable, option with(" + with +
              ") cannot be used with string variables.\n";
      continue;
    }
    if (mode == FillMode::Inert) continue;  // rand/pattern: accepted by the package, no branch
    if (isStatMode(mode)) {
      const char* fn = mode == FillMode::Mean ? "mean"
                       : mode == FillMode::Min ? "min"
                       : mode == FillMode::Max ? "max"
                                               : "median";
      const std::string stat = "__piet_t1g_stat";
      dropColumns(session, {stat});
      scratch.add(stat);
      std::string line = "quietly egen " + stat + " = " + fn + "(" + v + ")";
      if (hasQualifier(parsed)) line += " if " + touseCol + " == 1";
      if (!byVars.empty()) line += ", by(" + joinSpace(byVars) + ")";
      if (auto e = firstError(dispatch(line, session))) return *e;
      int64_t fills = 0, statMissing = 0;
      {
        Datastore* ds = session.datastore();
        const std::vector<double> cur = columnDoubles(*ds, v);
        const std::vector<double> st = columnDoubles(*ds, stat);
        for (size_t i = 0; i < cur.size(); ++i) {
          if (std::isnan(st[i])) ++statMissing;
          if (mask[i] && std::isnan(cur[i]) && !std::isnan(st[i])) ++fills;
        }
      }
      // The package's egen and replace are not quiet (community log fill_mean_by:
      // "(2 missing values generated)" then "(4 real changes made)").
      if (statMissing > 0) text += missingNote(statMissing);
      if (fills > 0) {
        const std::string before = storage::resolveReplaceType(session, v);
        std::string rl = "quietly replace " + v + " = " + stat + " if missing(" + v +
                         ") & !missing(" + stat + ")";
        if (auto e = firstError(dispatch(rl, session))) return *e;
        const std::string after = storage::resolveReplaceType(session, v);
        if (after != before)  // probe p3 meanb: "variable b was byte now float"
          text += "variable " + v + " was " + before + " now " + after + "\n";
        rewritten.push_back(v);
      }
      text += changesNote(fills);
      dropColumns(session, {stat});
      continue;
    }
    Datastore* ds = session.datastore();
    const bool isStr = ds->col(v).dtype == cecon::DType::Factor;
    const std::vector<std::string> cells = columnCells(*ds, v);
    std::vector<double> num;
    if (!isStr) num = columnDoubles(*ds, v);
    int64_t firstPass = 0;
    const std::vector<std::string> filled =
        copyFill(cells, isStr ? nullptr : &num, mode, gid, nGroups, mask, &firstPass);
    int64_t changed = 0;
    for (size_t i = 0; i < cells.size(); ++i) changed += filled[i] != cells[i] ? 1 : 0;
    // The package's one noisy `replace` per variable; with(next) runs two (probe p1 next:
    // "(3 real changes made)" then "(0 real changes made)").
    if (mode == FillMode::Next) {
      text += changesNote(firstPass);
      text += changesNote(changed - firstPass);
    } else {
      text += changesNote(changed);
    }
    if (changed == 0) continue;
    if (auto e = setColumn(session, v, filled, isStr, false, "")) return *e;
    rewritten.push_back(v);
  }
  scratch.clear();
  ojson out = ojson::array();
  if (!text.empty()) out.push_back(displayBlock(text));
  if (!rewritten.empty()) out.push_back(mutationResult(session, "gen", {}, {}, rewritten));
  return out;
}

}  // namespace piet::handlers
