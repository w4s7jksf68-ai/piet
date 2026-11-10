// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private interfaces used across related command families.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "estimation/estimation_internal.h"
#include <set>

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace postest_detail {
ojson noPreviousEstimation();
ojson noPreviousEstimationFor(const SessionState& session, bool invalidAfterPosting);
std::vector<std::string> fitConventionFlagsM4(const LastModel& lm);
void pushFitAsisM4(std::vector<std::string>& args, const LastModel& lm);
std::vector<std::string> lastModelBaseArgv(const std::string& cobraCmd, const LastModel& lm);
ojson runPostEstimationM4(const std::string& verb, const std::vector<std::string>& combo,
                          SessionState& session, const LastModel& lm);
const std::set<std::string>& postestOlsCmdsM4();
const std::set<std::string>& postestGlmCmdsM4();
bool postestFitLacksConstantM4(const LastModel& lm);
ojson postestNoConstantRejectM4(const std::string& verb, const LastModel& lm);
bool postestNoConstantUnrefittableM4(const LastModel& lm);
bool postestRefitMatchesFitM4(const std::string& verb, const LastModel& lm, ojson& reject);
bool postestEstimatorOkM4(const std::string& verb, const LastModel& lm, ojson& reject);
bool coefBelongsToVarM4(const std::string& name, const std::string& var);
std::string coefSourceVarM4(const std::string& name);
void postEstimationBlockM4(SessionState& session, const ojson& block, const std::string& cmdName,
                           const std::string& depvar, const std::string& dataPath);
bool buildLinearAmeRawM4(const LastModel& lm, const std::vector<std::string>& dydxVars,
                         ojson& out, ojson& reject);
std::string nextPathEst(const std::string& workdir, const std::string& prefix);
}  // namespace postest_detail
}  // namespace piet::handlers
