// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane ENV: macro and program-environment verbs --
// `macro drop|define|shift|list|dir`, `require`, `_dots`, `postutil`, `markout`
// and `lrtest`. Answers: tests/eval/golden/aer_w2/w2ax_t1_official.log and
// w2ax_t1_community.log (StataBE 19.5), plus the lane's own StataBE probes
// (envp1-3, recorded in the selftest's comments where an arm rests on one).
//
// EVERY VERB HERE IS WIRING OVER A STORE OR A PRIMITIVE THAT ALREADY EXISTED.
//
// The same defect classes apply here verbatim: every
// return is a block object with a "type" key or an array of them (SHAPE);
// every Stata rc that is not 198/199 travels in the `reason` channel as
// "r(NNN)" (RC); every option a command reads is registered below at namespace
// scope, real-but-unread options in `unimplemented` (VOCABULARY); lrtest
// replaces r() itself (R-CLASS).
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

// Shared reply-block, option-access and number-text helpers of the small
// commands split out of one source file: macro_command.cpp, progress_dots.cpp,
// markout.cpp (language/), package_requirements.cpp (environment/),
// postfile_handles.cpp (fileio/), likelihood_ratio_test.cpp (postestimation/).
// Definitions: command_reply_support.cpp.
namespace piet::handlers::command_reply_detail {

nlohmann::ordered_json envError(const std::string& message, int rc);
nlohmann::ordered_json displayBlock(const std::string& text);
bool isErrorBlock(const nlohmann::ordered_json& b);
const nlohmann::ordered_json* firstError(const nlohmann::ordered_json& result);
std::string joinWith(const std::vector<std::string>& v, const std::string& sep);
std::vector<std::string> splitWs(const std::string& s);
std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key);
bool hasOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
            const std::string& key);
std::optional<double> parseNumber(const std::string& sIn);
std::string fixedFmt(double v, int width, int prec, bool group);

}  // namespace piet::handlers::command_reply_detail
