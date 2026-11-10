// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/macros.h"
#include <functional>
#include <regex>

namespace piet::eval::macro_detail {
std::string absentClassMatrixRc(const std::string& s, char cls, const std::string& name);
std::optional<std::string> unpostedRRefusal(const EvalSession& session, const std::string& name);
bool takeCodedExpandError(const ExpandResult& exp, MacroFnError* err);
}
