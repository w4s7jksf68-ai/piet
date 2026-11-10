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
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "estimation/statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// `tsfilter hp NEW = Y [, smooth(#) trend(NEW2)]`
ojson runTsfilterHp(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("hpfilter")) return engineTooOld("tsfilter hp", "hpfilter");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  // varlist is `hp NEW = Y`; the filter word was already checked by the arm.
  std::string cycleName, yname;
  {
    std::string joined;
    for (std::size_t i = 1; i < parsed.varlist.size(); ++i) joined += parsed.varlist[i] + " ";
    const std::size_t eq = joined.find('=');
    if (eq == std::string::npos)
      return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");
    cycleName = strutil::trim(joined.substr(0, eq));
    yname = strutil::trim(joined.substr(eq + 1));
  }
  if (cycleName.empty() || yname.empty())
    return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");

  std::string trendName, smooth;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "trend") trendName = v.strValue;
    else if (k == "smooth") smooth = v.strValue;
  }
  // tsset/xtset is required: Stata's tsfilter is r(111) without one.
  if (!session.timevar.has_value())
    return aerErrorReason("tsfilter hp: you must tsset your data first", 111);

  std::vector<std::string> args = {"hpfilter", "--data", *session.currentHbin(), "--y", yname};
  if (!smooth.empty()) { args.push_back("--lambda"); args.push_back(smooth); }
  else {
    // The Ravn-Uhlig default is a function of the TIME-UNIT FORMAT, not a
    // constant 1600 -- %tw is 33,177,600 and %td is 110,930,628,906.25.
    // The display format is carried on the varLabels plane under a reserved
    // prefix, not on a named accessor.
    const std::string fmt = session.eval.varLabels.get(
        std::string(SessionState::kEvalVarFormatPrefix) + *session.timevar, "");
    if (!fmt.empty()) { args.push_back("--tsfmt"); args.push_back(fmt); }
  }
  if (session.panelvar.has_value()) { args.push_back("--panel"); args.push_back(*session.panelvar); }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("tsfilter hp: " + cobraErrorMessage(ce), ce.code);
  }

  ojson err;
  if (!estimWriteColumn(session, cycleName, jsonDoubles(raw["cycle"]), &err)) return err;
  if (!trendName.empty() && !estimWriteColumn(session, trendName, jsonDoubles(raw["trend"]), &err))
    return err;

  session.eval.rValues.set("smooth", raw.value("lambda", 1600.0));
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
