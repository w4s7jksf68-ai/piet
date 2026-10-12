// R2 (giant-file split, 2026-09-01): cross-TU declarations for the shared
// helpers of the former 7,859-line estimation_m4.cpp. Definitions stay in
// estimation_m4.cpp (namespace est_detail); the two continuation TUs include
// this and `using namespace est_detail;`. Default arguments live HERE (the
// declaration) and are stripped from the definitions, per the one-default rule.
#pragma once
#include <string>
#include <vector>
#include "nlohmann/json.hpp"
#include "piet/session_state.h"
#include "piet/eval/errors.h"  // eval::ParsedCommand / OptValue

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace est_detail {
struct EstimationArgvResult {
  std::vector<std::string> argv;
  bool errored = false;
  ojson errorBlock;
};

void captureFitFilterM4(LastModel& lm, const std::vector<std::string>& argv);
std::string cobraErrorStrM4(const CobraError& ce);
ojson cobraToEstimationM4(const ojson& raw, const std::string& cobraCmd,
                           const SessionState* session = nullptr);
bool computeYMinMaxM4(SessionState& session, const std::string& yName,
                       const std::optional<std::string>& ifExpr, double& outMin, double& outMax,
                       ojson& errBlockOut);
ojson engineErrorBlockM4(const std::string& message, const std::string& code = "engine_error");
std::string joinCommaM4(const std::vector<std::string>& v);
std::string lowerM4(std::string s);
std::string nextPathM4(const std::string& workdir, const std::string& prefix);
double normalPvalue(double z);
std::string trimM4(const std::string& s);
EstimationArgvResult buildEstimationArgvM4(const std::string& cobraCmd,
                                            const eval::ParsedCommand& parsed, SessionState& session,
                                            const std::optional<std::string>& extraFe = std::nullopt);
nlohmann::json runCobraJsonM4(std::vector<std::string> args);
void applyFactorValueLabelsM4(ojson& coeftable, const SessionState& session);
struct VcovResult {
  std::vector<std::string> argv;
  bool errored = false;
  std::string errorMsg;
};
inline constexpr double kZ975 = 1.959963984540054;  // qnorm(0.975)
VcovResult vcovFromOpts(const eval::ParsedCommand& parsed, bool* sawRobustFlag = nullptr);
void pushLevelIfGivenM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv);
ojson runP7Mle(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
               SessionState& session, ojson* rawOut = nullptr);
std::vector<std::string> splitTokensM4(const std::string& s);
std::vector<std::string> parseAtNumlistM4(const std::string& at);
void fillCoeftableStatsM4(ojson& block);
std::optional<std::string> applyIfFilterM4(SessionState& session, const std::string& hbinPath,
                                           const std::optional<std::string>& ifExpr,
                                           ojson& errBlockOut,
                                           const std::optional<std::string>& inRange = std::nullopt);
}  // namespace est_detail
// Cross-TU (defined in estimation_m4b.cpp, consumed by estimation_m4c.cpp):
ojson csdidToEstimationM4(const ojson& raw, const std::string& subcmd);
}  // namespace piet::handlers
