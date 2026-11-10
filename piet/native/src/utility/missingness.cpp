// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
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
// missings
// ===========================================================================
//
// FOUR SUB-VERBS, FOUR EXISTING PRIMITIVES. `report` reads the card; `dropvars`
// is runDrop; `dropobs` is runDropIf; `tag` is runEgen's rowmiss. The
// sub-verb is the first positional token, exactly as in missings.ado.
//
// WHY `report`'s OBSERVATION COUNT DOES NOT GO THROUGH rowmiss, which is what
// the plan specifies. `Frame::row_egen` calls `require_f64` on every column
// (src/data/frame.cpp), so rowmiss over a varlist containing a string variable
// THROWS -- and `missings report` with no varlist covers every variable, which
// on any real dataset includes strings. The two ways to use rowmiss here are
// (a) let the report fail on any dataset with a string variable, or (b) run it
// over the numeric subset and print a count that silently ignores missing
// string cells. Neither is acceptable, so the per-OBSERVATION predicate is
// computed here, over the same columns the per-VARIABLE table reports, while
// the per-variable COUNTS still come from the card and are never re-derived.
// `tag` still routes to runEgen: the user asked for that variable by name, and
// an honest engine error on a string varlist beats a second rowmiss.
ojson runMissings(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMissings.known,
                                            kVocabMissings.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return utilSyntax(
        "missings: syntax is `missings report|dropvars|dropobs|tag [varlist] [, generate(newvar) "
        "force]`");

  const std::string sub = lowerOf(parsed.varlist[0]);
  const std::vector<std::string> asked(parsed.varlist.begin() + 1, parsed.varlist.end());
  if (sub != "report" && sub != "dropvars" && sub != "dropobs" && sub != "tag") {
    return ojson(eval::unsupportedCommand("missings " + parsed.varlist[0], std::nullopt,
                                          std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  // `report` and `dropvars` both answer WHOLE-COLUMN questions off the card's
  // counts, which describe every observation; a qualifier would have to change
  // the counts themselves, so it is refused by name rather than ignored.
  // `dropobs` and `tag` forward the qualifier into the primitive they route to,
  // which already implements it.
  if (sub == "report" || sub == "dropvars") {
    ojson q = refuseQualifiers(
        parsed, "`missings " + sub +
            "` answers from the dataset's per-variable missing counts, which describe every "
            "observation. Restrict first (`preserve` + `keep if ...`), then run it.");
    if (!q.is_null()) return q;
  }

  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    return utilError(std::string("missings: ") + e.what(), "data_io");
  }
  if (dsp == nullptr) return ojson(eval::noDataLoaded());
  Datastore& ds = *dsp;

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(ds, "missings " + sub, asked, &vars)) return *err;
  const int64_t n = ds.nRows();
  const bool force = hasFlag(parsed.opts, "force");

  if (sub == "report") {
    std::ostringstream oss;
    oss << "Checking missings in " << (asked.empty() ? "all variables" : joinWith(vars, " "))
        << ":\n";
    size_t nameW = 0;
    for (const auto& nm : vars) nameW = std::max(nameW, nm.size());
    nameW = std::max<size_t>(nameW, 8);
    int64_t reported = 0;
    for (const auto& nm : vars) {
      const int64_t miss = missingCountOf(session, ds, nm);
      if (miss <= 0) continue;
      ++reported;
      oss << rightAlign(nm, nameW) << rightAlign(intText(miss), 12) << "\n";
    }
    if (reported == 0) oss << "  (no variables with missing values)\n";

    // The per-observation predicate: a row counts when ANY variable in scope is
    // missing there. See this function's header for why it is not rowmiss.
    std::vector<ColView> views;
    views.reserve(vars.size());
    for (const auto& nm : vars) views.push_back(viewOf(ds, nm));
    int64_t rowsWithMissing = 0;
    for (int64_t i = 0; i < n; ++i) {
      for (const auto& v : views) {
        if (cellMissing(v, i)) {
          ++rowsWithMissing;
          break;
        }
      }
    }
    oss << rowsWithMissing << " observations with missing values\n";
    return displayBlock(oss.str());
  }

  // missings: dropvars/dropobs on a dataset changed since it was
  // last saved need `force` -- `force option required with changed dataset`,
  // exit 4 (w2d_uncovered.log missings_dropvars_ctl / missings_dropobs_ctl
  // `4|4|6|`). That is the ado's only use of the option.
  if ((sub == "dropvars" || sub == "dropobs") && !force && session.datasetChanged)
    return utilErrorRc("force option required with changed dataset", 4);

  if (sub == "dropvars") {
    // missings: a variable qualifies when it is missing in every
    // observation used (`r(N) == `N''); :440-447 drop them and return
    // r(varlist) = the dropped names (missings_dropvars_force `0|2|6|b s|`).
    if (n == 0) return utilErrorRc("no observations", 2000);
    std::vector<std::string> doomed;
    for (const auto& nm : vars) {
      if (missingCountOf(session, ds, nm) == n) doomed.push_back(nm);
    }
    ojson out = silent();
    if (!doomed.empty()) {
      out = runDrop(doomed, session);
      if (firstError(out) != nullptr) return out;
    }
    session.eval.rValues.set("N", static_cast<double>(n));
    session.eval.rStrings.set("varlist", joinWith(doomed, " "));
    return out;
  }

  if (sub == "dropobs") {
    // missings: drop the observations missing on EVERY variable in
    // scope (`nmissing' == `nvars'`), strings included (missing() of "" is 1),
    // within the qualifier; r(n_dropped) is their count (:453). The row test is
    // the report's own per-cell predicate, written as a hidden column so the
    // drop itself goes through the ordinary `drop if` path with the user's
    // if/in ANDed in.
    std::vector<ColView> views;
    views.reserve(vars.size());
    for (const auto& nm : vars) views.push_back(viewOf(ds, nm));
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      bool all = !views.empty();
      for (const auto& v : views) {
        if (!cellMissing(v, i)) {
          all = false;
          break;
        }
      }
      cells.push_back(numCell(all ? 1.0 : 0.0));
    }
    const std::string tag = "__missings_dropobs_tag";
    std::string tagOut;
    if (auto err = writeColumn(session, *session.currentHbin(), tag, cells, "f64", true, &tagOut))
      return *err;
    session.load(tagOut);
    std::string cond = tag + " == 1";
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty())
      cond = "(" + *parsed.ifExpr + ") & (" + cond + ")";
    Datastore* before = session.datastore();
    const int64_t n0 = before != nullptr ? before->nRows() : n;
    ojson out = runDropIf(cond, session, parsed.inRange);
    if (firstError(out) != nullptr) {
      runDrop({tag}, session);
      return out;
    }
    Datastore* after = session.datastore();
    const int64_t n1 = after != nullptr ? after->nRows() : n0;
    ojson dropTag = runDrop({tag}, session);
    if (firstError(dropTag) != nullptr) return dropTag;
    session.eval.rValues.set("N", static_cast<double>(n));
    session.eval.rValues.set("n_dropped", static_cast<double>(n0 - n1));
    return out;
  }

  // tag
  const std::optional<std::string> gen = optStrAny(parsed.opts, {"generate", "gen"});
  if (!gen.has_value())
    return utilSyntax("missings tag: requires generate(newvar) -- the tag has nowhere to go "
                      "otherwise.");
  ojson guard = newVarGuard(session, "missings tag", *gen);
  if (!guard.is_null()) return guard;

  // rowmiss ONLY when every variable in scope is numeric. `Frame::row_egen`
  // calls `require_f64`, so rowmiss over a varlist containing a string variable
  // THROWS -- and `missings tag` with no varlist covers every variable, which on
  // any real dataset includes strings. Measured: the registry pangram's
  // `missings tag, generate(...)` on kitchensink_panel (which has the factor
  // column `grp`) died with "column 'grp' is a factor, not f64".
  //
  // The fallback is NOT a second definition of missingness: it is the SAME
  // per-row predicate `report` computes a dozen lines above, over the same
  // ColViews, so the count `report` prints and the column `tag` writes can
  // never disagree. Restricting rowmiss to the numeric subset instead would
  // have tagged rows on a narrower test than the user asked for -- a silently
  // wrong column, which is the one outcome worth more code to avoid.
  bool allNumeric = true;
  for (const auto& nm : vars) {
    if (ds.dtype(nm) == "factor") {
      allNumeric = false;
      break;
    }
  }
  if (allNumeric) {
    return runEgen(*gen + " = rowmiss(" + joinWith(vars, " ") + ")", std::string(), parsed.ifExpr,
                   {}, session, parsed.inRange);
  }

  std::vector<ColView> tagViews;
  tagViews.reserve(vars.size());
  for (const auto& nm : vars) tagViews.push_back(viewOf(ds, nm));
  std::vector<std::string> tagCells;
  tagCells.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    double c = 0.0;
    for (const auto& v : tagViews) {
      if (cellMissing(v, i)) c += 1.0;
    }
    tagCells.push_back(numCell(c));
  }
  std::string tagOut;
  if (auto err = writeColumn(session, *session.currentHbin(), *gen, tagCells, "f64", true, &tagOut))
    return *err;
  session.load(tagOut);
  return mutationResult(session, "gen", {*gen});
}

// ===========================================================================
// mdesc
// ===========================================================================
//
// The same card counts `missings report` reads, in mdesc's own three-column
// layout. Nothing is scanned: every number below is either the card's or
// arithmetic on it.
ojson runMdesc(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMdesc.known,
                                            kVocabMdesc.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    return utilError(std::string("mdesc: ") + e.what(), "data_io");
  }
  if (dsp == nullptr) return ojson(eval::noDataLoaded());
  Datastore& ds = *dsp;

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(ds, "mdesc", parsed.varlist, &vars)) return *err;
  const int64_t n = ds.nRows();

  size_t nameW = 0;
  for (const auto& nm : vars) nameW = std::max(nameW, nm.size());
  nameW = std::max<size_t>(nameW, 8);

  std::ostringstream oss;
  oss << rightAlign("Variable", nameW) << " |" << rightAlign("Missing", 12)
      << rightAlign("Total", 12) << rightAlign("Percent Missing", 18) << "\n";
  oss << std::string(nameW + 1, '-') << "+" << std::string(42, '-') << "\n";
  int64_t lastMiss = 0;
  double lastPct = 0.0;
  for (const auto& nm : vars) {
    const int64_t miss = missingCountOf(session, ds, nm);
    const double pct = (n > 0) ? (100.0 * static_cast<double>(miss) / static_cast<double>(n)) : 0.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", pct);
    oss << rightAlign(nm, nameW) << " |" << rightAlign(intText(miss), 12)
        << rightAlign(intText(n), 12) << rightAlign(std::string(buf), 18) << "\n";
    lastMiss = miss;
    lastPct = pct;
  }
  // mdesc.ado leaves r() describing the LAST variable it processed, the same
  // rule the plan states for distinct's r(ndistinct)/r(N). This is the one
  // number in this bundle that the P5 pin would flip if the oracle keeps a
  // whole-table total instead.
  session.eval.rValues.set("miss", static_cast<double>(lastMiss));
  session.eval.rValues.set("total", static_cast<double>(n));
  session.eval.rValues.set("percent", lastPct);
  return displayBlock(oss.str());
}


}  // namespace piet::handlers
