// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private interfaces used across related command families.
#pragma once

#include "piet/handlers.h"
#include "estimation_internal.h"
#include <set>

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace est_detail {
struct P7MleArgv {
  bool errored = false;
  ojson errorBlock;
  std::vector<std::string> argv;
};

std::string lowerM4(std::string s);
std::string trimM4(const std::string& s);
ojson engineErrorBlockM4(const std::string& message, const std::string& code);
std::optional<std::string> vceOptionShapeErrorM4(const eval::ParsedCommand& parsed);
std::string cobraErrorStrM4(const CobraError& ce);
std::string joinCommaM4(const std::vector<std::string>& v);
ojson cobraToEstimationM4(const ojson& raw, const std::string& cobraCmd,
                           const SessionState* session);
void captureFitFilterM4(LastModel& lm, const std::vector<std::string>& argv);
EstimationArgvResult buildEstimationArgvM4(const std::string& cobraCmd,
                                            const eval::ParsedCommand& parsed, SessionState& session,
                                            const std::optional<std::string>& extraFe);
nlohmann::json runCobraJsonM4(std::vector<std::string> args);
std::string nextPathM4(const std::string& workdir, const std::string& prefix);
std::optional<std::string> applyIfFilterM4(SessionState& session, const std::string& hbinPath,
                                           const std::optional<std::string>& ifExpr,
                                           ojson& errBlockOut,
                                           const std::optional<std::string>& inRange);
std::optional<std::vector<int64_t>> esampleRowsM4(SessionState& session, const LastModel& lm);
const double* f64ColumnM4(SessionState& session, const std::string& name);
void pushLevelIfGivenM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv);
P7MleArgv buildP7MleArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                         const std::vector<std::string>& vceAllowed);
ojson runP7MleAsM4(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
                          SessionState& session, ojson* rawOut, const std::string& stataCmd);
ojson runP7Mle(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
               SessionState& session, ojson* rawOut);
bool depvarEqColsM4(const std::string& depvar, const ojson& coefs, const char* bKey,
                    std::vector<AncillaryEbColM4>& out);
bool postAncillaryEbM4(SessionState& session, const ojson& raw,
                       const std::vector<AncillaryEbColM4>& cols);
}  // namespace est_detail
namespace fit_detail {
void putEScalarNL(SessionState& s, const std::string& n, double v);
void putEStringNL(SessionState& s, const std::string& n, const std::string& v);
void cacheLastModelM4(SessionState& session, const std::string& cobraCmd,
                       const std::vector<std::string>& argv, const ojson& block, const ojson& raw,
                       const std::string& esampleValues = std::string(),
                       const std::string& stataCmd = std::string());
}  // namespace fit_detail
void applyBuiltinMultiwayReplyM4(ojson& result);
void postBuiltinMultiwayEM4(SessionState& session, const ojson& raw);
void postENameListsXtregFeM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed);
}  // namespace piet::handlers
