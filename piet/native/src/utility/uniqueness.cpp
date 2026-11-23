// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/utility.h"
#include "piet/dispatcher.h"  // unique, detail: the summary of the record counts

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

#include "engine/scratch_columns.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG -- Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/handlers/observations.h"  // runDrop: unique.ado's `cap drop _Unique`

#include "datamgmt/variable_metadata_support.h"
#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// unique / gunique
// ===========================================================================
//
// ONE arm, two spellings. `unique varlist` prints the two lines every do-file
// greps for and publishes r(unique)/r(N)/r(sum); `unique varlist, by(g)
// gen(new)` writes the per-group distinct count into EVERY observation of the
// group and prints nothing.
//
// MISSING IS A VALUE HERE, deliberately: `unique id` on a column with three
// missing ids counts one more distinct value, matching unique.ado (this is the
// exact point where `unique` and `distinct` disagree, and it is why they are
// two commands rather than one with a flag).
ojson runUnique(const eval::ParsedCommand& parsed, SessionState& session) {
  // `_all` is every variable, in dataset order (vs-stata bank c052e647680a9488 `unique _all`).
  if (std::find(parsed.varlist.begin(), parsed.varlist.end(), "_all") != parsed.varlist.end() &&
      session.hasData()) {
    std::vector<std::string> names;
    if (const auto* card = session.shadowVarlistIfCurrent()) {
      for (const auto& info : *card) names.push_back(info.name);
    } else if (Datastore* rds = session.datastore()) {
      names = rds->names();
    }
    eval::ParsedCommand expanded = parsed;
    expanded.varlist.clear();
    for (const auto& t : parsed.varlist) {
      if (t != "_all") { expanded.varlist.push_back(t); continue; }
      for (const auto& nm : names)
        if (std::find(expanded.varlist.begin(), expanded.varlist.end(), nm) == expanded.varlist.end())
          expanded.varlist.push_back(nm);
    }
    return runUnique(expanded, session);
  }
  const std::string typed = lowerOf(parsed.cmd);
  const bool gtools = !typed.empty() && typed[0] == 'g';
  const std::string verb = gtools ? "gunique" : "unique";

  // The two options that are gunique's alone, named with the spelling the user
  // typed. The reject net below would catch both anyway (they are in the shared
  // `unimplemented` list), but the net cannot say WHICH command's option it is
  // refusing, and "gunique, replace" replacing the data in memory is a large
  // enough difference to be worth naming.
  // gunique's replace only lets gen() (or _Unique) overwrite an existing
  // variable (gunique.ado); unique has no such option of that meaning.
  const bool replaceGen = gtools && hasFlag(parsed.opts, "replace");
  if (!gtools && hasFlag(parsed.opts, "replace")) {
    return ojson(eval::unsupportedOption(
        "replace",
        "`" + verb +
            ", replace` REPLACES the data in memory with one row per distinct combination. This "
            "build's " + verb +
            " only counts; use `duplicates drop` or `contract` to collapse the data."));
  }
  // gunique, detail (no by()): the group sizes summarized in detail after
  // the count line (gunique.ado runs gstats sum on its counts); r() is the
  // count's. With by() it also lists the top levels -- not drawn here.
  const bool detail = hasFlag(parsed.opts, "detail");
  if (gtools && detail && optStr(parsed.opts, "by").has_value()) {
    return ojson(eval::unsupportedOption(
        "detail",
        "`" + verb +
            ", detail` prints a per-variable breakdown this build does not produce. Run `distinct "
            "varlist` for the per-variable table."));
  }
  // `missing` is gunique's alone; unique.ado's syntax line has no such option.
  if (!gtools && hasFlag(parsed.opts, "missing"))
    return ojson(eval::errorBlock("syntax_error", "option missing not allowed", std::nullopt,
                                  std::nullopt, std::nullopt, "r(198)"));
  {
    std::vector<std::pair<std::string, eval::OptValue>> rest;
    for (const auto& kv : parsed.opts)
      if (!(detail && kv.first == "detail") && !(replaceGen && kv.first == "replace"))
        rest.push_back(kv);
    if (auto err = eval::rejectUnknownOptions(rest, kVocabUnique.known,
                                              kVocabUnique.unimplemented))
      return ojson(*err);
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return utilSyntax(verb + ": syntax is `" + verb + " varlist [if] [in] [, by(varlist) gen(newvar)]`");
  const bool withMissing = gtools && hasFlag(parsed.opts, "missing");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);
  // gen() is read only with by(): both ados ignore it otherwise. by() without
  // gen() writes _Unique (unique.ado drops an existing one first; gunique.ado
  // refuses an existing one, r(110)).
  std::optional<std::string> gen = optStrAny(parsed.opts, {"gen", "generate"});
  if (byVars.empty()) gen.reset();
  bool dropOldUnique = false;
  if (!byVars.empty() && !gen.has_value()) {
    gen = std::string("_Unique");
    Datastore* hd = session.datastore();
    if (hd != nullptr && hd->has("_Unique")) {
      if (gtools && !replaceGen)
        return ojson(eval::errorBlock("syntax_error",
                                      "Variable _Unique already exists.\nUse the gen() option to "
                                      "specify a new variable.",
                                      std::nullopt, std::nullopt, std::nullopt, "r(110)"));
      dropOldUnique = true;
    }
  }
  if (gtools) {
    Datastore* hd = session.datastore();
    if (hd != nullptr && hd->nRows() < 1)
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, "r(2000)"));
  }

  // The gen() form WRITES a column, so it must be as long as the frame and
  // aligned to it. `applyIfAndInFilter` returns a filtered FRAME, not a mask,
  // so honouring a qualifier here would need row identity this layer does not
  // have -- and the failure mode would be a plausible-looking, misaligned
  // column. Refused by name instead. The report form has no such problem and
  // honours both qualifiers.
  // gunique by(): the variable is missing outside the if/in sample (its
  // group id is missing there).
  std::vector<double> inSample;
  const bool haveQual = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                        (parsed.inRange.has_value() && !parsed.inRange->empty());
  if (gen.has_value() && gtools && haveQual) {
    std::string qualifier;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) qualifier += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value() && !parsed.inRange->empty())
      qualifier += " in " + *parsed.inRange;
    ScratchColumns scratch(session);
    std::string col;
    if (auto failed = scratch.add("1", qualifier, &col)) return *failed;
    inSample = scratch.values(col);
  } else if (gen.has_value()) {
    ojson q = refuseQualifiers(
        parsed, "`" + verb +
            ", gen()` writes a column as long as the dataset, and this build cannot map a "
            "filtered sample back onto the original rows. Restrict first (`preserve` + `keep "
            "if ...`), then run " + verb + ".");
    if (!q.is_null()) return q;
  }

  std::vector<std::string> readCols = parsed.varlist;
  for (const auto& b : byVars) readCols.push_back(b);
  ReadScope scope(session, parsed, readCols, verb, !gen.has_value());
  if (scope.failed())
    return scope.error().is_null() ? utilError(verb + ": could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }

  const int64_t n = ds.nRows();
  std::vector<ColView> views;
  views.reserve(parsed.varlist.size());
  for (const auto& nm : parsed.varlist) views.push_back(viewOf(ds, nm));

  auto rowKey = [&](int64_t i) {
    std::vector<uint64_t> key;
    key.reserve(views.size());
    for (const auto& v : views) key.push_back(cellKey(v, i));
    return key;
  };
  // unique.ado counts a missing value as a value (marksample ..., novarlist);
  // gunique drops a row with any missing value unless `missing`.
  auto rowIn = [&](int64_t i) {
    if (!inSample.empty() && std::isnan(inSample[static_cast<size_t>(i)])) return false;
    if (!gtools || withMissing) return true;
    for (const auto& v : views)
      if (cellMissing(v, i)) return false;
    return true;
  };

  // The whole-sample counts both ados publish before any by() work.
  std::map<std::vector<uint64_t>, int64_t> sizes;
  int64_t nIn = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (!rowIn(i)) continue;
    ++nIn;
    ++sizes[rowKey(i)];
  }
  const int64_t nUnique = static_cast<int64_t>(sizes.size());
  eval::replaceRClass(session.eval);
  std::ostringstream oss;
  if (gtools) {
    int64_t minJ = 0, maxJ = 0;
    if (!sizes.empty()) {
      minJ = std::numeric_limits<int64_t>::max();
      for (const auto& kv : sizes) {
        minJ = std::min(minJ, kv.second);
        maxJ = std::max(maxJ, kv.second);
      }
    }
    session.eval.rValues.set("N", static_cast<double>(nIn));
    session.eval.rValues.set("J", static_cast<double>(nUnique));
    session.eval.rValues.set("unique", static_cast<double>(nUnique));
    session.eval.rValues.set("minJ", static_cast<double>(minJ));
    session.eval.rValues.set("maxJ", static_cast<double>(maxJ));
    eval::declareRPosted(session.eval, "gunique", {"N", "J", "unique", "minJ", "maxJ"});
    if (nIn == 0) {
      oss << "(no observations)\n";
    } else {
      oss << "N = " << nIn << "; " << nUnique << (minJ == maxJ ? " balanced" : " unbalanced")
          << " groups of sizes " << minJ << " to " << maxJ << "\n";
    }
  } else {
    // unique.ado: r(unique) and r(sum) are both the count of distinct values in
    // the sample, r(N) the sample size; the by() form adds nothing to r().
    session.eval.rValues.set("unique", static_cast<double>(nUnique));
    session.eval.rValues.set("sum", static_cast<double>(nUnique));
    session.eval.rValues.set("N", static_cast<double>(nIn));
    eval::declareRPosted(session.eval, "unique", {"unique", "sum", "N"});
    oss << "Number of unique values of " << joinWith(parsed.varlist, " ") << " is  " << nUnique
        << "\n";
    oss << "Number of records is  " << nIn << "\n";
  }
  // detail: the number of records of each distinct combination, summarized with
  // `summarize, detail` (by group with by()) under the title "Records per <varlist>";
  // unique's own r() is what stays (vs-stata bank 9e9f3bcce8ee0286, b1c9f63b1865f435).
  std::vector<std::pair<std::string, double>> rSaved;
  for (const char* k : {"N", "J", "unique", "minJ", "maxJ"})
    if (const double* v = session.eval.rValues.find(k)) rSaved.emplace_back(k, *v);
  auto runDetail = [&]() -> ojson {
    ojson outBlocks = ojson::array();
    std::string q;
    if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) q += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) q += " in " + *parsed.inRange;
    const std::string vl = joinWith(parsed.varlist, " ");
    const std::string bl = joinWith(byVars, " ");
    // gunique counts its groups over rows with no missing key (unless
    // missing) and shows the counts under a temporary name.
    const std::string cv = gtools ? "__000000" : "__unique_records";
    std::vector<std::string> cmds;
    if (!q.empty()) cmds.push_back("quietly keep" + q);
    if (gtools && !withMissing)
      for (const auto& v : parsed.varlist) cmds.push_back("quietly drop if missing(" + v + ")");
    cmds.push_back("quietly bysort " + (bl.empty() ? "" : bl + " ") + vl + ": generate long " + cv +
                   " = _N if _n == 1");
    if (!gtools) cmds.push_back("label variable " + cv + " \"Records per " + vl + "\"");
    cmds.push_back((bl.empty() ? std::string() : "by " + bl + ": ") + "summarize " + cv +
                   ", detail");
    // unique.ado is a program, and its preserve is that program's own: it does
    // not touch a copy the caller holds, and the caller's later restore still
    // finds it. The working copy therefore lives one preserve level deeper than
    // the code that called unique (a second preserve at the caller's own level
    // would be r(621), and the restore below would then use up the caller's copy).
    struct DeeperPreserveLevel {
      SessionState& s;
      explicit DeeperPreserveLevel(SessionState& st) : s(st) { ++s.doFileDepth; }
      ~DeeperPreserveLevel() { --s.doFileDepth; }
      DeeperPreserveLevel(const DeeperPreserveLevel&) = delete;
      DeeperPreserveLevel& operator=(const DeeperPreserveLevel&) = delete;
    } level(session);
    {
      ojson kept = dispatch("preserve", session);
      for (const auto& b : kept)
        if (b.is_object() && b.value("type", std::string()) == "error") {
          outBlocks.push_back(b);
          return outBlocks;
        }
    }
    for (size_t c = 0; c < cmds.size(); ++c) {
      ojson r = dispatch(cmds[c], session);
      bool bad = false;
      for (const auto& b : r)
        if (b.is_object() && b.value("type", std::string()) == "error") bad = true;
      if (bad || c + 1 == cmds.size())
        for (const auto& b : r)
          if (b.is_object() && b.value("type", std::string()) != "echo") outBlocks.push_back(b);
      if (bad) break;
    }
    {
      ojson back = dispatch("restore", session);
      for (const auto& b : back)
        if (b.is_object() && b.value("type", std::string()) == "error") outBlocks.push_back(b);
    }
    if (gtools) {
      // gunique's r() was set before the summary ran; it is what remains.
      eval::replaceRClass(session.eval);
      for (const auto& [k, v] : rSaved) session.eval.rValues.set(k, v);
      eval::declareRPosted(session.eval, "gunique", {"N", "J", "unique", "minJ", "maxJ"});
      return outBlocks;
    }
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("unique", static_cast<double>(nUnique));
    session.eval.rValues.set("sum", static_cast<double>(nUnique));
    session.eval.rValues.set("N", static_cast<double>(nIn));
    eval::declareRPosted(session.eval, "unique", {"unique", "sum", "N"});
    return outBlocks;
  };
  if (byVars.empty() || (gtools && nIn == 0)) {
    if (!detail) return displayBlock(oss.str());
    ojson all = ojson::array();
    all.push_back(displayBlock(oss.str()));
    for (auto& b : runDetail()) all.push_back(b);
    return all;
  }

  // by(): the per-group count of distinct varlist values.
  //  unique.ado: on the by-group's FIRST observation, missing elsewhere
  //    (`by by: gen g = uniq[_N] if _n==1`; BUG-422, StataBE 19.5 pin).
  //  gunique.ado: on EVERY observation of the group whose varlist is not
  //    missing (`gegen g = tag(by id) ...` then `gegen g = sum(g) if !mi(id),
  //    by(by) replace`).
  int64_t nGroups = 0;
  std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);
  std::vector<std::set<std::vector<uint64_t>>> perGroup(static_cast<size_t>(nGroups));
  for (int64_t i = 0; i < n; ++i) {
    if (!rowIn(i)) continue;
    perGroup[static_cast<size_t>(gid[static_cast<size_t>(i)])].insert(rowKey(i));
  }
  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  std::vector<bool> groupSeen(static_cast<size_t>(nGroups), false);
  for (int64_t i = 0; i < n; ++i) {
    const size_t g = static_cast<size_t>(gid[static_cast<size_t>(i)]);
    if (gtools) {
      cells.push_back(rowIn(i) ? numCell(static_cast<double>(perGroup[g].size())) : std::string());
      continue;
    }
    if (groupSeen[g]) {
      cells.emplace_back();  // Stata missing on every row but the group's first
      continue;
    }
    groupSeen[g] = true;
    cells.push_back(numCell(static_cast<double>(perGroup[g].size())));
  }
  const std::string newvar = *gen;
  if (replaceGen && !dropOldUnique) {
    Datastore* hd = session.datastore();
    dropOldUnique = hd != nullptr && hd->has(newvar);
  }
  if (dropOldUnique) {
    ojson dropped = runDrop({newvar}, session);
    if (dropped.is_object() && dropped.value("type", std::string()) == "error") return dropped;
  } else {
    ojson guard = newVarGuard(session, verb, newvar);
    if (!guard.is_null()) return guard;
  }

  ojson blocks = ojson::array();
  blocks.push_back(displayBlock(oss.str()));
  if (detail) {
    ojson det = runDetail();
    for (auto& b : det) {
      if (b.is_object() && b.value("type", std::string()) == "error") return det;
      blocks.push_back(b);
    }
  }
  std::string out;
  if (auto err = writeColumn(session, *session.currentHbin(), newvar, cells, "f64", true, &out))
    return *err;
  session.load(out);
  blocks.push_back(mutationResult(session, "gen", {newvar}));
  return blocks;
}

// ===========================================================================
// distinct
// ===========================================================================
//
// The table `distinct` prints, and the ONE place this bundle's two counting
// commands differ: distinct EXCLUDES missing unless `missing` is given, where
// `unique` always counts it as a value.
ojson runDistinct(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDistinct.known,
                                            kVocabDistinct.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const bool joint = hasFlag(parsed.opts, "joint");
  const bool withMissing = hasFlag(parsed.opts, "missing");

  ReadScope scope(session, parsed, parsed.varlist, "distinct", true);
  if (scope.failed())
    return scope.error().is_null() ? utilError("distinct: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();

  std::vector<std::string> vars;
  if (auto err = resolveVarlist(ds, "distinct", parsed.varlist, &vars)) return *err;

  const int64_t n = ds.nRows();
  std::vector<ColView> views;
  views.reserve(vars.size());
  for (const auto& nm : vars) views.push_back(viewOf(ds, nm));

  struct Row {
    std::string name;
    int64_t total = 0;
    int64_t distinct = 0;
  };
  std::vector<Row> rows;

  if (joint) {
    // One row: the distinct COMBINATIONS. Without `missing`, an observation
    // missing on ANY of the variables is out of the count entirely (casewise),
    // which is what makes "total" here smaller than any single variable's.
    std::set<std::vector<uint64_t>> keys;
    int64_t total = 0;
    for (int64_t i = 0; i < n; ++i) {
      bool anyMissing = false;
      for (const auto& v : views) {
        if (cellMissing(v, i)) {
          anyMissing = true;
          break;
        }
      }
      if (anyMissing && !withMissing) continue;
      std::vector<uint64_t> key;
      key.reserve(views.size());
      for (const auto& v : views) key.push_back(cellKey(v, i));
      keys.insert(std::move(key));
      ++total;
    }
    rows.push_back(Row{"joint", total, static_cast<int64_t>(keys.size())});
  } else {
    for (size_t c = 0; c < vars.size(); ++c) {
      std::set<uint64_t> keys;
      int64_t total = 0;
      for (int64_t i = 0; i < n; ++i) {
        if (cellMissing(views[c], i) && !withMissing) continue;
        keys.insert(cellKey(views[c], i));
        ++total;
      }
      rows.push_back(Row{vars[c], total, static_cast<int64_t>(keys.size())});
    }
  }

  // distinct.ado (1.2.1, the version the bank ran): abbrev(#) sets the name
  // column (default the longest name), never below 5, names shortened as
  // abbrev() does; min()/max() show only the rows whose distinct count lies
  // in [min, max] (swapped, with a note, when min > max). r() is the last
  // row's whether shown or not.
  long long abbrevW = -1, minD = 0;
  double maxD = std::numeric_limits<double>::infinity();
  for (const auto& [k, v] : parsed.opts) {
    if (v.isBool) continue;
    long long x = 0;
    try {
      x = std::stoll(strutil::trim(v.strValue));
    } catch (...) {
      return ojson(eval::errorBlock("syntax_error", "option " + k + "() incorrectly specified",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
    if (k == "abbrev") abbrevW = x;
    if (k == "min" || k == "minimum") minD = x;
    if (k == "max" || k == "maximum") maxD = static_cast<double>(x);
  }
  std::string swapNote;
  if (static_cast<double>(minD) > maxD) {
    const long long lo = static_cast<long long>(maxD), hi = minD;
    swapNote = "min(" + std::to_string(hi) + ") max(" + std::to_string(lo) +
               ") interpreted as min(" + std::to_string(lo) + ") max(" + std::to_string(hi) + ")\n";
    minD = lo;
    maxD = static_cast<double>(hi);
  }
  auto shown = [&](const Row& r) {
    return static_cast<double>(r.distinct) >= static_cast<double>(minD) &&
           static_cast<double>(r.distinct) <= maxD;
  };
  auto g9 = [](int64_t v) { return rightAlign(piet::fmtStataDefaultG(static_cast<double>(v)), 9); };
  std::ostringstream oss;
  oss << swapNote;
  if (joint) {
    oss << "\n        Observations\n      total   distinct\n";
    for (const auto& r : rows)
      if (shown(r))
        oss << rightAlign(piet::fmtStataDefaultG(static_cast<double>(r.total)), 11) << "  "
            << g9(r.distinct) << "\n";
  } else {
    size_t w = 0;
    if (abbrevW < 0) {
      for (const auto& r : rows) w = std::max(w, r.name.size());
    } else {
      w = static_cast<size_t>(abbrevW);
    }
    w = std::max<size_t>(w, 5);
    oss << "\n" << std::string(w + 2, ' ') << "|        Observations\n";
    oss << std::string(w + 2, ' ') << "|      total   distinct\n";
    oss << std::string(w + 2, '-') << "+" << std::string(22, '-') << "\n";
    for (const auto& r : rows)
      if (shown(r))
        oss << " " << rightAlign(variable_metadata_detail::stataAbbrev(r.name, static_cast<int>(w)), w) << " |  "
            << g9(r.total) << "  " << g9(r.distinct) << "\n";
  }

  // distinct.ado returns the LAST row's numbers (the joint row when joint was
  // asked for), not a total across rows.
  if (!rows.empty()) {
    session.eval.rValues.set("ndistinct", static_cast<double>(rows.back().distinct));
    session.eval.rValues.set("N", static_cast<double>(rows.back().total));
  }
  return displayBlock(oss.str());
}


}  // namespace piet::handlers
