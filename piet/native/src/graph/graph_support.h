// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <limits>

namespace piet { class Datastore; class SessionState; }

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

ojson engineErrorBlockG(const std::string& message, const std::string& code = "engine_error");

std::vector<double> numericColumn(Datastore& ds, const std::string& name);

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const std::string& key);

std::optional<bool> optBool(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                             const std::string& key);

std::optional<ojson> rejectByFacet(const std::vector<std::pair<std::string, eval::OptValue>>& opts);

std::optional<std::string> optTitleStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        const std::string& key);

std::string firstNameToken(const std::string& raw);

ojson stataRefusal(const std::string& message, int rc);

std::optional<std::string> optLast(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const std::string& key);

bool hasOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& key);

std::string trimG(const std::string& s);

std::vector<std::string> splitWsG(const std::string& s);

std::optional<double> parseNumberG(const std::string& raw);

double storeAs(double v, bool asFloat);

double reldifG(double x, double y);

std::string varTitleG(SessionState& session, const std::string& v);

std::string displayFormatOf(SessionState& session, Datastore& ds, const std::string& var);
}  // namespace piet::handlers::graph_detail
