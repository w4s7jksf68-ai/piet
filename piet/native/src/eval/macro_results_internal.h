// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/macros.h"
#include <functional>
#include <regex>

namespace piet::eval::macro_detail {
std::optional<double> jsonToFloat(const nlohmann::ordered_json& v);
std::string resolveEScalar(const std::string& name, const EvalSession& session);
std::string resolveERef(const std::string& name, const EvalSession& session);
}
