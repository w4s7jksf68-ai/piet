// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/observations.h"
#include "piet/handlers/utility.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"

#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// carryforward
// ===========================================================================
//
// Last-non-missing forward fill, reset at every by-group boundary.
//
// ROW ORDER IS THE ANSWER, not an implementation detail. The dispatcher's
// by-prefix branch already sorted the frame; this walks the frame in its
// CURRENT physical order, carrying each group's last non-missing value forward,
// and writes every result back at the row index it came from. A group whose
// rows are not contiguous (an explicit `by()` on unsorted data) still fills
// from the previous row OF THE SAME GROUP, which is the same answer sorting
// would have given and a defensible one when it was not.
ojson runCarryforward(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCarryforward.known,
                                            kVocabCarryforward.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return utilSyntax(
        "carryforward: syntax is `[bysort g:] carryforward varlist, gen(newvarlist)|replace`");

  const bool replace = hasFlag(parsed.opts, "replace");
  const std::optional<std::string> gen = optStrAny(parsed.opts, {"gen", "generate"});
  if (replace == gen.has_value())
    return utilSyntax("carryforward: give exactly one of gen(newvarlist) and replace.");

  std::vector<std::string> newVars;
  if (gen.has_value()) {
    newVars = splitWs(*gen);
    if (newVars.size() != parsed.varlist.size())
      return utilSyntax("carryforward: gen() names " + intText(static_cast<int64_t>(newVars.size())) +
                        " variable(s) for " + intText(static_cast<int64_t>(parsed.varlist.size())) +
                        " input variable(s); the two lists must be the same length.");
  }

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);

  // A written column must be as long as the frame and aligned to it; a filtered
  // read cannot be mapped back. Refused by name (registry allowIf is false for
  // this row, so this is the belt to the tokenizer's suspenders).
  {
    ojson q = refuseQualifiers(
        parsed, "`carryforward` writes a column as long as the dataset, and this build cannot map a "
        "filtered sample back onto the original rows. Restrict first (`preserve` + `keep if "
        "...`), then run carryforward.");
    if (!q.is_null()) return q;
  }

  std::vector<std::string> readCols = parsed.varlist;
  for (const auto& b : byVars) readCols.push_back(b);
  ReadScope scope(session, parsed, readCols, "carryforward", false);
  if (scope.failed())
    return scope.error().is_null() ? utilError("carryforward: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }
  for (const auto& nm : newVars) {
    ojson guard = newVarGuard(session, "carryforward", nm);
    if (!guard.is_null()) return guard;
  }

  const int64_t n = ds.nRows();
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

  std::string cur = *session.currentHbin();
  std::vector<std::string> rewritten;
  for (size_t k = 0; k < parsed.varlist.size(); ++k) {
    const std::string& src = parsed.varlist[k];
    const std::string dtype = (ds.dtype(src) == "factor") ? "factor" : "f64";
    std::vector<std::string> cells = columnAsStrings(ds, src);
    // "" is missing in a setcol payload, which is exactly the state that gets
    // filled -- and a group's FIRST observation, having nothing before it,
    // keeps its missing rather than borrowing from another group.
    std::vector<std::string> last(static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
    std::vector<char> haveLast(static_cast<size_t>(std::max<int64_t>(nGroups, 1)), 0);
    for (int64_t i = 0; i < n; ++i) {
      const size_t g = static_cast<size_t>(gid[static_cast<size_t>(i)]);
      std::string& cell = cells[static_cast<size_t>(i)];
      if (!cell.empty()) {
        last[g] = cell;
        haveLast[g] = 1;
      } else if (haveLast[g]) {
        cell = last[g];
      }
    }
    const std::string target = gen.has_value() ? newVars[k] : src;
    std::string out;
    if (auto err = writeColumn(session, cur, target, cells, dtype, gen.has_value(), &out))
      return *err;
    cur = out;
    if (!gen.has_value()) rewritten.push_back(target);
  }

  session.load(cur);
  if (gen.has_value()) return mutationResult(session, "gen", newVars);
  return mutationResult(session, "gen", {}, {}, rewritten);
}

// ===========================================================================
// tsspell
// ===========================================================================
//
// Spells over a tsset series: `_spell` numbers them within panel, `_seq`
// numbers the observations within a spell, `_end` flags each spell's last
// observation. Two forms:
//
//   tsspell varname       a spell is a run of EQUAL values of varname
//   tsspell, cond(expr)   a spell is a run of observations where expr is true
//
// In either form a TIME GAP ends the spell: the previous observation must be
// exactly one `delta` earlier (tsset's own step, defaulting to 1), or the run
// restarts. The cond() form leaves non-spell observations at 0/0/0, which is
// tsspell.ado's own convention and is why `_spell` is 1-based.
//
// LIKE rangestat, THE ANSWERS GO BACK IN ORIGINAL ROW ORDER: the panel/time
// ordering below is an index permutation and never a rewrite of the frame.
ojson runTsspell(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTsspell.known,
                                            kVocabTsspell.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (!session.timevar.has_value() || session.timevar->empty())
    return utilError(
        "tsspell: run `tsset timevar` (or `xtset panelvar timevar`) first so PIET knows which "
        "variable orders the spells.",
        "unsupported_option");

  const std::optional<std::string> cond = optStr(parsed.opts, "cond");
  if (parsed.varlist.size() > 1)
    return utilSyntax("tsspell: syntax is `tsspell [varname] [, cond(expression) spell(name) "
                      "seq(name) end(name)]`");
  if (parsed.varlist.empty() && !cond.has_value())
    return utilSyntax("tsspell: give either a varname or cond(expression) -- without one there is "
                      "nothing to define a spell.");
  if (!parsed.varlist.empty() && cond.has_value())
    return utilSyntax("tsspell: give either a varname or cond(expression), not both.");

  {
    ojson q = refuseQualifiers(
        parsed, "`tsspell` writes columns as long as the dataset and walks the series in time order; a "
        "restricted sample would silently redefine where a spell breaks. Restrict first "
        "(`preserve` + `keep if ...`), then run tsspell.");
    if (!q.is_null()) return q;
  }

  const std::string spellVar = optStr(parsed.opts, "spell").value_or(std::string("_spell"));
  const std::string seqVar = optStr(parsed.opts, "seq").value_or(std::string("_seq"));
  const std::string endVar = optStr(parsed.opts, "end").value_or(std::string("_end"));
  for (const auto& nm : {spellVar, seqVar, endVar}) {
    ojson guard = newVarGuard(session, "tsspell", nm);
    if (!guard.is_null()) return guard;
  }
  if (spellVar == seqVar || spellVar == endVar || seqVar == endVar)
    return utilSyntax("tsspell: spell(), seq() and end() must name three different variables.");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);
  if (byVars.empty() && session.panelvar.has_value() && !session.panelvar->empty())
    byVars.push_back(*session.panelvar);

  // The cond() form needs a row-wise truth value of an arbitrary expression.
  // PIET has exactly one expression evaluator and it lives behind `generate`;
  // this materializes the indicator through it and drops it afterwards, so
  // there is no second evaluator to drift.
  std::string indicator;
  if (cond.has_value()) {
    if (auto err = materializeIndicator(session, "tsspell", *cond, &indicator)) return *err;
  }
  // The indicator column is scaffolding, never data. It has to be gone BEFORE
  // `mutationResult` reads the frame, or the varlist this command reports would
  // name a temporary the user never asked for and cannot see -- so the drop is
  // a callable that the success path fires at exactly that point and the
  // failure paths fire on the way out. Idempotent: whichever runs first wins.
  bool indicatorLive = !indicator.empty();
  ojson indicatorDropError;
  auto dropIndicator = [&]() {
    if (!indicatorLive) return;
    indicatorLive = false;
    ojson dropped = runDrop({indicator}, session);
    if (const ojson* e = firstError(dropped)) indicatorDropError = *e;
  };

  ojson result;
  {
    std::vector<std::string> readCols = {*session.timevar};
    if (!parsed.varlist.empty()) readCols.push_back(parsed.varlist[0]);
    if (!indicator.empty()) readCols.push_back(indicator);
    for (const auto& b : byVars) readCols.push_back(b);

    ReadScope scope(session, parsed, readCols, "tsspell", false);
    if (scope.failed()) {
      result = scope.error().is_null() ? utilError("tsspell: could not read the data", "data_io")
                                       : scope.error();
    } else {
      Datastore& ds = *scope.get();
      std::string missingName;
      for (const auto& nm : readCols) {
        if (!ds.has(nm)) missingName = nm;
      }
      if (!missingName.empty()) {
        result = utilError("variable " + missingName + " not found");
      } else if (ds.dtype(*session.timevar) == "factor") {
        result = utilError("tsspell: the time variable `" + *session.timevar +
                           "' is a string variable; tsspell needs a numeric time index.");
      } else {
        const int64_t n = ds.nRows();
        const double* tv = ds.col(*session.timevar).f64();
        const double delta = session.delta.value_or(1.0);
        int64_t nGroups = 0;
        const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

        const bool byValue = !parsed.varlist.empty();
        ColView valueView;
        const double* ind = nullptr;
        if (byValue)
          valueView = viewOf(ds, parsed.varlist[0]);
        else
          ind = ds.col(indicator).f64();

        std::vector<std::vector<int64_t>> order(
            static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
        for (int64_t i = 0; i < n; ++i)
          order[static_cast<size_t>(gid[static_cast<size_t>(i)])].push_back(i);
        for (auto& g : order) {
          std::stable_sort(g.begin(), g.end(),
                           [&](int64_t a, int64_t b) { return tv[a] < tv[b]; });
        }

        std::vector<double> spell(static_cast<size_t>(n), 0.0);
        std::vector<double> seq(static_cast<size_t>(n), 0.0);
        std::vector<double> endf(static_cast<size_t>(n), 0.0);

        for (const auto& g : order) {
          int64_t spellNo = 0;
          int64_t seqNo = 0;
          int64_t prevRow = -1;
          bool inSpell = false;
          for (size_t j = 0; j < g.size(); ++j) {
            const int64_t r = g[j];
            const bool active = byValue ? true : (!std::isnan(ind[r]) && ind[r] != 0.0);
            if (!active) {
              inSpell = false;
              prevRow = r;
              continue;
            }
            bool newSpell = !inSpell;
            if (!newSpell && prevRow >= 0) {
              // A gap in TIME breaks the run whatever the values do.
              const double dt = tv[r] - tv[prevRow];
              if (std::isnan(dt) || dt != delta) newSpell = true;
            }
            if (!newSpell && byValue && prevRow >= 0) {
              if (cellKey(valueView, r) != cellKey(valueView, prevRow)) newSpell = true;
            }
            if (newSpell) {
              ++spellNo;
              seqNo = 1;
            } else {
              ++seqNo;
            }
            spell[static_cast<size_t>(r)] = static_cast<double>(spellNo);
            seq[static_cast<size_t>(r)] = static_cast<double>(seqNo);
            inSpell = true;
            prevRow = r;
          }
          // `_end` marks the LAST observation of each spell, which is known
          // only once the following observation (or the group's end) is seen.
          for (size_t j = 0; j < g.size(); ++j) {
            const int64_t r = g[j];
            if (spell[static_cast<size_t>(r)] == 0.0) continue;
            bool last = true;
            if (j + 1 < g.size()) {
              const int64_t nx = g[j + 1];
              last = spell[static_cast<size_t>(nx)] != spell[static_cast<size_t>(r)];
            }
            endf[static_cast<size_t>(r)] = last ? 1.0 : 0.0;
          }
        }

        auto cellsOf = [&](const std::vector<double>& v) {
          std::vector<std::string> out;
          out.reserve(static_cast<size_t>(n));
          for (int64_t i = 0; i < n; ++i) out.push_back(numCell(v[static_cast<size_t>(i)]));
          return out;
        };

        std::string cur = *session.currentHbin();
        bool failed = false;
        for (const auto& [nm, vals] :
             std::vector<std::pair<std::string, const std::vector<double>*>>{
                 {spellVar, &spell}, {seqVar, &seq}, {endVar, &endf}}) {
          std::string out;
          if (auto err = writeColumn(session, cur, nm, cellsOf(*vals), "f64", true, &out)) {
            result = *err;
            failed = true;
            break;
          }
          cur = out;
        }
        if (!failed) {
          session.load(cur);
          dropIndicator();
          result = mutationResult(session, "gen", {spellVar, seqVar, endVar});
        }
      }
    }
  }

  // Every path that did not reach the success branch above still owes the drop.
  // A drop failure is reported only when nothing worse already has.
  dropIndicator();
  if (!indicatorDropError.is_null() && !isErrorBlock(result)) return indicatorDropError;
  return result;
}


}  // namespace piet::handlers
