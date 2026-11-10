// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private declarations shared by the e(b) column names, the r(table) / r-class
// lifecycle and the default e() macros of a fit.
#pragma once

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

#include <string>
#include <vector>

#include "piet/session_state.h"

namespace piet::handlers::rtable_detail {

using ojson = nlohmann::ordered_json;

double rtNan();
double rowNum(const ojson& row, const char* key);

struct RtCol {
  std::string name;
  const ojson* row = nullptr;  // null: a base-level column (b 0, the rest missing)
};

std::string omittedColumnName(const std::string& name);
bool stataColumns(SessionState& session, const LastModel& lm, const ojson& ct,
                  std::vector<RtCol>& cols);
void annotateEbStripe(SessionState& session);

std::string commandWord(const std::string& rawCmd);
void postDefaultCmdline(SessionState& session);
void postDefaultEMacros(SessionState& session);

}  // namespace piet::handlers::rtable_detail
