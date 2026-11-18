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
struct P7MleArgv {
  bool errored = false;
  ojson errorBlock;
  std::vector<std::string> argv;
};

std::optional<std::string> vceOptionShapeErrorM4(const eval::ParsedCommand& parsed);
P7MleArgv buildP7MleArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                         const std::vector<std::string>& vceAllowed);
ojson runP7MleAsM4(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
                          SessionState& session, ojson* rawOut, const std::string& stataCmd);
}  // namespace est_detail
namespace fit_detail {
void putEScalarNL(SessionState& s, const std::string& n, double v);
void putEStringNL(SessionState& s, const std::string& n, const std::string& v);
void cacheLastModelM4(SessionState& session, const std::string& cobraCmd,
                       const std::vector<std::string>& argv, const ojson& block, const ojson& raw,
                       const std::string& esampleValues = std::string(),
                       const std::string& stataCmd = std::string());
// BUG-1482 (panel_estimation.cpp): the rank of a one-way clustered e_V, Stata's
// r(rankc); nullopt when e_V is absent or not square.
std::optional<double> clusteredVRankM4(const ojson& raw);
// One-way cluster with G - 1 < k: Stata's e(rank) (panel_estimation.cpp).
std::optional<double> fewClustersRankM4(const ojson& raw, double G, double k);
}  // namespace fit_detail
void applyBuiltinMultiwayReplyM4(ojson& result);
void postBuiltinMultiwayEM4(SessionState& session, const ojson& raw);
void postENameListsXtregFeM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed);
}  // namespace piet::handlers
