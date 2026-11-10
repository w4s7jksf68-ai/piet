// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

ojson graphMetaFor(const std::string& raw, const std::string& family,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts);

void registerGraphOutput(ojson& result, SessionState& session,
                          const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                          const std::string& raw = std::string());

std::string graphNameOption(const std::vector<std::pair<std::string, eval::OptValue>>& opts);

std::string expandDoublePipeLayers(const std::string& rawArgs);

void hoistTwowayLevelOptions(twoway::ParseResult& pr);

void graphAcceptedNotApplied(nlohmann::ordered_json& block, const std::string& cmdName,
                             const std::vector<std::pair<std::string, piet::eval::OptValue>>& opts,
                             const std::unordered_set<std::string>& applied);

}  // namespace piet::dispatch_detail
