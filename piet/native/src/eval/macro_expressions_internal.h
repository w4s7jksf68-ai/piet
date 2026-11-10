// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/macros.h"
#include <functional>
#include <regex>

namespace piet::eval {
std::string stataMacroRealText(double d);
}
namespace piet::eval::macro_detail {
bool stringFnNumericArgImpl(const std::string& e, std::string* call);
std::string stataMissingText(double d);
std::string substituteVarSubscripts(const std::string& in, EvalSession& session,
                                    std::optional<std::string>* whole,
                                    bool* stringLeft = nullptr,
                                    std::vector<std::string>* outerSpans = nullptr);
bool isStataCClassStringName(const std::string& name);
bool isStataFunctionName(const std::string& name);
std::string capNumericTextToStataLocalDigits(const std::string& text);
std::string substituteBareCRefs(const std::string& expr, EvalSession& session);
std::string macroRealText(double v);
}
