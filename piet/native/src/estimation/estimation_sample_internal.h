// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The e(sample) mask API: the fit handlers install the estimation-sample marker,
// and the data verbs that build observations out of others carry it. Included
// by estimation/estimation_internal.h and by the data and dispatch headers that
// need only this part of it.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "vendor/nlohmann/json.hpp"
#include "piet/datastore.h"  // EsampleLostBy
#include "piet/session_state.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
// e(sample) T2 (defined in estimation_sample_install.cpp; runRegress in
// linear_estimation.cpp was the first wired fit): the fit handlers' mask
// install, shared by the iv (instrumental_variables.cpp), xtreg,fe (panel_estimation.cpp),
// newey and prais (time_series_estimation.cpp) routes.
std::string esampleFitArgs(SessionState& session, std::vector<std::string>& argv,
                           const std::string& indexVar, int64_t headRows);
bool esampleInstallOnHead(SessionState& session, const std::string& valuesPath, ojson& raw,
                          LastModel& lm);
// The column write alone, onto `basePath`'s frame (runRegress's residuals() frame,
// runPoisson's ppmlhdfe d frame); returns the new frame's path or "".
std::string esampleInstallColumn(SessionState& session, const std::string& valuesPath,
                                 ojson& raw, const std::string& basePath,
                                 bool residentEligible, unsigned long long* serialOut);
// e(sample) T2, the data verbs that build observations out of others (estimation_sample_carry.cpp,
// where the StataBE rules are cited): expand, cross,
// collapse, contract, joinby, bsample. Each is a no-op when the working frame
// carries no mask.
std::optional<std::vector<double>> esampleHeadMask(SessionState& session);
bool esampleRewriteHeadMask(SessionState& session, const std::vector<double>& cells);
bool esampleMarkHeadLost(SessionState& session, EsampleLostBy by);
// merge 1:m (blockLen null) and joinby (blockLen = esampleUsingRunLengths): the
// marker stays on the first copy of each master row (estimation_sample_carry.cpp cites the rule).
bool esampleKeepFirstCopy(SessionState& session, const std::vector<std::string>& keys,
                          const std::map<std::string, int64_t>* blockLen);
std::map<std::string, int64_t> esampleUsingRunLengths(const Datastore& usingDs,
                                                      const std::vector<std::string>& keys);
// collapse/contract: read the groups' marks BEFORE the verb (`restricted`: an
// if/in, weight or casewise rule may leave some of a group's rows out), write
// them onto the result AFTER it.
struct EsampleGroupMarks {
  bool active = false;  // the frame carried a mask and the verb has by-groups
  bool lost = false;    // Stata's answer depends on its tie order: EsampleLostBy
  std::map<std::string, double> byKey;  // by-values -> the group's first row's cell
};
EsampleGroupMarks esampleGroupMarksBefore(SessionState& session,
                                          const std::vector<std::string>& byVars,
                                          bool restricted);
bool esampleGroupMarksAfter(SessionState& session, const EsampleGroupMarks& g,
                            const std::vector<std::string>& byVars, EsampleLostBy by);
}  // namespace piet::handlers
