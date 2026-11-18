// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private declarations shared by the e(b) column names, the r(table) / r-class
// lifecycle and the default e() macros of a fit.
#pragma once

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

#include <optional>
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
// baseOnly: post only the `(base)` notes (margins' table, whose other notes come
// from its error codes).
void postCoefficientLabels(SessionState& session, const eval::EvalMatrix& tbl, bool baseOnly = false);
// The coefficient-table note of each column (`(base)`, `(omitted)`, `(empty)`,
// `(constrained)`, "" for none); nullopt where it cannot be decided.
std::vector<std::optional<std::string>> coefficientNotes(SessionState& session,
                                                        const std::vector<std::string>& names,
                                                        const std::vector<double>& b,
                                                        const std::vector<double>& se,
                                                        const std::vector<bool>& eform);

std::string commandWord(const std::string& rawCmd);
void postDefaultCmdline(SessionState& session);
void postDefaultEMacros(SessionState& session);

}  // namespace piet::handlers::rtable_detail
