// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// tsfilter hp: Hodrick-Prescott cycle and trend columns computed by the engine.

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "estimation/statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

namespace command_detail {
// time/time_panel_state.cpp: the one display-format resolver tsset reads, and the
// r(unit) word tsset posts for a format.
std::string tsCurrentFormat(SessionState& session, Datastore* ds, const std::string& var);
std::string tsUnitWord(const std::string& period, const std::string& fmt);
}  // namespace command_detail

namespace {
// tsfilter.ado's `Trend(string) Smooth(string)`: the minimum spellings are one
// letter, below the prefix resolver's two-letter floor, so they are DECLARED
// aliases (unioned into the dispatcher's tsfilter vocabulary). `s(100)` was
// accepted and then missed by the raw-key read below, which ran the default
// smoothing (r(smooth) 1600 for Stata's 100).
const eval::OptVocab& kVocabTsfilterHp = eval::registerOptVocab(
    {"tsfilter"}, {{"trend", "smooth"}, {}, {{"t", "trend"}, {"s", "smooth"}}});
}  // namespace

// `tsfilter hp NEW = Y [, smooth(#) trend(NEW2)]`
ojson runTsfilterHp(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("hpfilter")) return engineTooOld("tsfilter hp", "hpfilter");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  // `[type] newvar`: tsfilter.ado's newvarlist (and trend()) takes a storage type
  // before the name -- `tsfilter hp double gc = gnp96` is a double gc, not a
  // variable literally named "double gc".
  auto splitTyped = [](const std::string& spec, std::string* type, std::string* name) {
    std::istringstream iss(spec);
    std::vector<std::string> words;
    for (std::string w; iss >> w;) words.push_back(w);
    static const std::set<std::string> kTypes = {"byte", "int", "long", "float", "double"};
    if (words.size() == 2 && kTypes.count(strutil::lower(words[0])) != 0) {
      *type = strutil::lower(words[0]);
      *name = words[1];
    } else {
      type->clear();
      *name = strutil::trim(spec);
    }
  };

  // varlist is `hp NEW = Y`; the filter word was already checked by the arm.
  std::string cycleName, cycleType, yname;
  {
    std::string joined;
    for (std::size_t i = 1; i < parsed.varlist.size(); ++i) joined += parsed.varlist[i] + " ";
    const std::size_t eq = joined.find('=');
    if (eq == std::string::npos)
      return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");
    splitTyped(joined.substr(0, eq), &cycleType, &cycleName);
    yname = strutil::trim(joined.substr(eq + 1));
  }
  if (cycleName.empty() || yname.empty())
    return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");

  std::string trendName, trendType, smooth;
  (void)kVocabTsfilterHp;  // registration only: the dispatcher canonicalizes the keys
  for (const auto& [k, v] : parsed.opts) {
    if (k == "trend") splitTyped(v.strValue, &trendType, &trendName);
    else if (k == "smooth") smooth = v.strValue;
  }
  // tsset/xtset is required: Stata's tsfilter is r(111) without one.
  if (!session.timevar.has_value())
    return aerErrorReason("tsfilter hp: you must tsset your data first", 111);

  // tsfilter.ado's syntax, in its order (vs-stata bank 2026-11-10): the
  // varlist holds variables, not expressions (`= ln(gnp96)` r(198) "ln(gnp96)
  // is an invalid varlist"); no name twice (`= gnp96 gnp96` r(198) "duplicate
  // names found in varlist"); smooth() one number (`smooth(100 1600)` r(198));
  // the new name is new (`tsfilter hp gnp96 = gnp96` r(110)); and the sample
  // has no gaps -- a time period missing, or the series missing inside it, is
  // r(498) "sample may not contain gaps" (61e1e384e344c91b, d8a27b177a1dafe8).
  {
    auto rc198 = [](const std::string& m) {
      return ojson(eval::errorBlock("syntax_error", m, std::nullopt, std::nullopt, std::nullopt,
                                    "r(198)"));
    };
    if (yname.find_first_of("()") != std::string::npos) return rc198(yname + " is an invalid varlist");
    std::istringstream yw(yname);
    std::vector<std::string> ys;
    for (std::string w; yw >> w;) ys.push_back(w);
    for (size_t i = 0; i < ys.size(); ++i)
      for (size_t j = i + 1; j < ys.size(); ++j)
        if (ys[i] == ys[j]) return rc198("duplicate names found in varlist");
    {
      std::istringstream sw(smooth);
      int nsm = 0;
      for (std::string w; sw >> w;) ++nsm;
      if (nsm > 1) return rc198("only one argument may be specified in smooth()");
    }
    Datastore* tds = session.datastore();
    if (tds != nullptr && cycleName.find_first_of(" \t") == std::string::npos &&
        tds->has(cycleName))
      return ojson(eval::errorBlock("engine_error", "variable " + cycleName + " already defined",
                                    std::nullopt, std::nullopt, std::nullopt, "r(110)"));
    // The gap rule, read where the sample is the plain rows of one series.
    if (tds != nullptr && ys.size() == 1 && !session.panelvar.has_value() &&
        !parsed.ifExpr.has_value() && !parsed.inRange.has_value() && tds->has(ys[0]) &&
        tds->has(*session.timevar) && tds->dtype(ys[0]) == "f64" &&
        tds->dtype(*session.timevar) == "f64") {
      const int64_t nr = tds->nRows();
      const double* yv = tds->col(ys[0]).f64();
      const double* tv = tds->col(*session.timevar).f64();
      std::vector<double> times;
      for (int64_t r = 0; r < nr; ++r)
        if (!std::isnan(yv[r]) && yv[r] < 8.98846567431158e307 && !std::isnan(tv[r]))
          times.push_back(tv[r]);
      std::sort(times.begin(), times.end());
      const double delta = session.delta.value_or(1.0);
      for (size_t i = 1; i < times.size(); ++i)
        if (times[i] - times[i - 1] > delta * (1 + 1e-9))
          return ojson(eval::errorBlock("engine_error", "sample may not contain gaps",
                                        std::nullopt, std::nullopt, std::nullopt, "r(498)"));
    }
  }

  // tsfilter.ado: `qui tsset` -> r(unit), the time unit of the tsset variable's
  // display format. The Ravn-Uhlig default smoothing is a function of it, not a
  // constant 1600 -- %tw is 33,177,600 and %td is 110,930,628,906.25 -- and it
  // is posted as r(unit). The format comes from the one resolver tsset reads
  // (SessionState::displayFormat): the varLabels plane read here before is a
  // projection synced only when an expression reads it, so right after `use` +
  // `xtset` it was empty and every daily series got 1600.
  const std::string tsFmt =
      command_detail::tsCurrentFormat(session, session.datastore(), *session.timevar);
  const std::string unitWord = command_detail::tsUnitWord(std::string(), tsFmt);
  std::vector<std::string> args = {"hpfilter", "--data", *session.currentHbin(), "--y", yname};
  if (!smooth.empty()) { args.push_back("--lambda"); args.push_back(smooth); }
  else if (!tsFmt.empty()) {
    args.push_back("--tsfmt");
    args.push_back(tsFmt);
  }
  if (session.panelvar.has_value()) { args.push_back("--panel"); args.push_back(*session.panelvar); }
  // if/in: tsfilter.ado marks the sample and filters only it; every row outside
  // it is missing in the new variables. Both go to the engine as a row mask over
  // the whole frame (`in` resolved here to literal rows), so the results stay row
  // for row. Without them the filter ran on all rows at rc 0 (gnp96 if
  // date>tq(1970q1): 1971q4 -77.7203 for Stata's -62.9800).
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args.push_back("--keepif");
    args.push_back(*parsed.ifExpr);
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    Datastore* ds = session.datastore();
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
    if (!rr.ok) {
      InRangeFilterResult bad;
      bad.ok = false;
      bad.negative = rr.negative;
      bad.refusal = rr.refusal;
      return inRangeRejectError(bad, *parsed.inRange, "tsfilter");
    }
    args.push_back("--inrange");
    args.push_back(std::to_string(rr.a) + "/" + std::to_string(rr.b));
  }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("tsfilter hp: " + cobraErrorMessage(ce), ce.code);
  }

  // The new variables are stored as tsfilter.ado's `newvarlist` types them: the
  // type typed before the name, else the default (c(type), float unless `set type
  // double`). A float cyclical component is what `summarize gc` reads (StataBE
  // 19.5: r(mean) 1.5698688e-07 for the float store, 1.04e-10 unrounded).
  const std::string cycleStore = storage::resolveCreateType(session, cycleType);
  ojson err;
  if (!estimWriteColumn(session, cycleName, jsonDoubles(raw["cycle"]), &err, cycleStore))
    return err;
  storage::recordCreateType(session, cycleName, cycleType, cycleStore);
  if (!trendName.empty()) {
    const std::string trendStore = storage::resolveCreateType(session, trendType);
    if (!estimWriteColumn(session, trendName, jsonDoubles(raw["trend"]), &err, trendStore))
      return err;
    storage::recordCreateType(session, trendName, trendType, trendStore);
  }

  session.eval.rValues.set("smooth", raw.value("lambda", 1600.0));
  if (!unitWord.empty()) session.eval.rStrings.set("unit", unitWord);
  session.eval.rStrings.set("method", "Hodrick-Prescott");
  session.eval.rStrings.set("varlist", yname);
  session.eval.rStrings.set("filterlist", cycleName);
  if (!trendName.empty()) session.eval.rStrings.set("trendlist", trendName);

  // tsfilter is otherwise SILENT; a short panel is the one exception, and it is
  // a note rather than a refusal (measured: rc 0, that panel left missing).
  std::string noteText;
  if (raw.contains("notes") && raw["notes"].is_array())
    for (const auto& n : raw["notes"]) noteText += "note: " + n.get<std::string>() + "\n";
  ojson mut = mutationResult(session, "tsfilter",
                              trendName.empty() ? std::vector<std::string>{cycleName}
                                                : std::vector<std::string>{cycleName, trendName},
                              {});
  if (noteText.empty()) return mut;
  ojson arr = ojson::array();
  arr.push_back(displayBlock(noteText));
  arr.push_back(mut);
  return arr;
}

}  // namespace piet::handlers
