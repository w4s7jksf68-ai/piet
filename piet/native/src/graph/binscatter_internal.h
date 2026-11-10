// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <optional>
#include <string>
#include <vector>

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace engine_report_detail {

// Shared by binscatter.cpp (the command), binscatter_syntax.cpp (option tail and
// varlist resolution) and binscatter_ereturn.cpp (touse, e(sample), replayed e()).

// One option as typed after the comma: its name, its argument (the text inside
// the outer parentheses, or nullopt for a bare word) and the whole token.
struct RawOpt {
  std::string name;
  std::optional<std::string> arg;
  std::string text;
};

// What binscatter's sample and its regressions are built from (ado).
struct BsSpec {
  std::vector<std::string> ys;
  std::string x, by, controls, absorb, wtype, wvar;
  bool fit = false, qfit = false, residualized = false, noaddmean = false;
  std::string xReg, x2Reg;
  std::vector<std::string> yReg;
};

std::vector<RawOpt> bsRawOptions(const std::string& raw);
std::string bsMatch(const std::string& typed);
ojson bsRcError(const std::string& message, int rc, const std::string& code = "engine_error");
std::optional<ojson> bsResolveNames(SessionState& session, std::string& text, bool single);
bool bsPostEsample(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp, double N);
void bsReplayRegressions(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp);

}  // namespace engine_report_detail

}  // namespace handlers
}  // namespace piet
