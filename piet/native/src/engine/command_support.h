// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Shared helpers of the engine-backed commands, defined in engine/command_support.cpp.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include <set>

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {
std::string trim(const std::string& s);
std::string lower(std::string s);
ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error");
ojson engineErrorBlockRc(const std::string& message, int rc);
std::string cobraErrorStr(const CobraError& ce);
std::string nextPath(const std::string& workdir, const std::string& prefix);
CobraError errorFromRaw(const CobraRunResult& r);
bool cardHasName(const std::vector<VarInfo>& card, const std::string& name);
std::string firstArgAfter(const std::string& raw, const std::string& word);
}  // namespace command_detail
}  // namespace piet::handlers
