// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Estimation-block helpers shared by linear_estimation.cpp (regress/areg/
// reghdfe/feols) and mean_proportion_estimation.cpp; defined once in
// linear_estimation_support.cpp.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace linear_estimation_detail {

ojson stataRefusalBlock(const std::string& message, int rc);
std::string joinComma(const std::vector<std::string>& v);
ojson cobraToEstimation(const ojson& raw, const std::string& cobraCmd,
                         const SessionState* session = nullptr);

}  // namespace linear_estimation_detail

}  // namespace piet::handlers
