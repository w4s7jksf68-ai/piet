// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/macros.h"
#include <functional>
#include <regex>

namespace piet::eval {
std::optional<double> ancillaryEbCoef(const EvalSession& session, const std::string& name,
                                      const std::string& kind);
std::string matrixElementText(double v);
}
namespace piet::eval::macro_detail {
std::string unresolvedMatrixOperandError(const std::string& expr,
                                                const EvalSession& session);
}
