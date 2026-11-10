// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private to the resampling-prefix family (bootstrap_prefix.cpp,
// jackknife_permute_prefix.cpp, rolling_prefix.cpp, statsby_prefix.cpp): the helpers two
// or more of them share, defined in resampling_prefix_support.cpp.
// Shared design note of the family: the head of jackknife_permute_prefix.cpp.
#pragma once

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/session_state.h"

namespace piet {
namespace handlers {

// `bootstrap:` (bootstrap_prefix.cpp); runResample forwards method "bootstrap" here.
nlohmann::ordered_json runBootstrap(const std::string& expListRaw, const std::string& optsRaw,
                                    const std::string& innerCmd, SessionState& session);

namespace resampling_detail {

nlohmann::ordered_json rsError(const std::string& message, const std::string& code = "engine_error");
std::string nextPath(const std::string& workdir, const std::string& prefix);
CobraError errorFromRaw(const CobraRunResult& r);
std::string cobraErrorStr(const CobraError& ce);

// The row-index column every replicate transform joins on. Underscore-prefixed
// and long enough that it cannot collide with a user variable in practice; it
// never survives into the session's own dataset.
constexpr const char* kRowIdx = "__piet_rs_row";

struct ExpItem {
  std::string name;  // the label Stata prints in the leftmost column
  std::string expr;  // RAW, evaluated fresh after every replicate
};

std::vector<ExpItem> parseExpList(const std::string& raw, std::string* err);
std::optional<double> evalStat(const std::string& expr, SessionState& session);
std::string commaGroup(int64_t v);
std::string g9cell(double v);
std::string padLeft(const std::string& s, size_t w);
std::string padRight(const std::string& s, size_t w);
std::string fixed2(double v);
std::string fixed3(double v);
std::string fixed4(double v);
void setEMatrix(SessionState& session, const std::string& name, eval::EvalMatrix m);
std::string bootstrapTitle(const std::string& innerTitle, const std::string& cmdname);

}  // namespace resampling_detail
}  // namespace handlers
}  // namespace piet
