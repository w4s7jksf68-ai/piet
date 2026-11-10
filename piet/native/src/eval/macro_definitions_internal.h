// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/macros.h"
#include <functional>
#include <regex>

namespace piet::eval::macro_detail {
bool classifyFailedEqExpr(const std::string& exprIn, const std::string& raised,
                          MacroFnError* err, const EvalSession* session = nullptr);
std::optional<std::string> tryExtendedMacroFn(const std::string& rhsIn, EvalSession& session,
                                              MacroFnError* err);
std::string extMacroFnName(const std::string& tok);
}
