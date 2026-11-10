// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private interfaces used across related command families.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "estimation_internal.h"
#include <set>

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace est_detail {
struct IvParenResult {
  std::string y;
  std::vector<std::string> exog;
  std::vector<std::string> endog;
  std::vector<std::string> instruments;
  bool errored = false;
  std::string errorMsg;
};

std::string joinSpaceM4(const std::vector<std::string>& v);
IvParenResult parseIvParenM4(const std::vector<std::string>& tokens, bool exogAfterParen);
}  // namespace est_detail
namespace extended_fit_detail {
enum class OptShapeM4b { kAbsent, kBare, kEmpty, kOne, kMany };

struct VceShapesM4b {
  OptShapeM4b cluster = OptShapeM4b::kAbsent;
  std::string clusterRaw;  // trimmed cluster(...) text
  std::vector<std::string> clusterVars;
  OptShapeM4b robust = OptShapeM4b::kAbsent;  // kBare = the flag; kOne/kMany = robust(x)
  std::string robustRaw;
  bool sawVce = false;                    // vce()/vcov(), first one wins (as every loop here)
  std::string vceRaw;                     // trimmed
  std::string vceType;                    // canonicalVceType(); "" when unrecognised
  std::vector<std::string> vceRest;       // the tokens after the type
};

VceShapesM4b readVceShapesM4b(const eval::ParsedCommand& parsed);
ojson stataRefusalM4b(const std::string& message, int rc);
double numberAtM4b(const ojson& obj, const char* key, const std::string& name);
ojson drdidFitCoreM4(SessionState& session, const std::string& y,
                     const std::vector<std::string>& xs, const std::string& ivarName,
                     const std::string& timeVarName, const std::string& treatExpr,
                     const std::string& treatName, const std::optional<std::string>& ifExpr);
}  // namespace extended_fit_detail
}  // namespace piet::handlers
