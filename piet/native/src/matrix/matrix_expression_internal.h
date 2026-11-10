// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// The matrix-expression evaluator's interface to the matrix commands, defined in
// matrix/matrix_expression.cpp.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include <functional>

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {
struct MxValue {
  bool scalar = false;
  bool stored = false;  // a stored matrix itself (`P`, `e(b)`), not a computed one
  bool absent = false;  // an absent nullmat(): absentArg is its argument
  std::string absentArg;
  double s = 0.0;
  eval::EvalMatrix m;  // every row/column has a name slot; "" is named by position
};
void applyDefaultStripe(eval::EvalMatrix& mat);
bool splitTopLevel(const std::string& e, std::vector<std::string>& parts,
                   std::vector<char>& ops);
bool evalMatrixScalarExpr(const std::string& exprIn, SessionState& session, double& outVal,
                          std::string& err, bool lhsSubscript = false);
ojson matrixExprErrorBlock(const std::string& err);
std::optional<eval::EvalMatrix> resolveClassMatrixRef(char cls, const std::string& which,
                                                       SessionState& session, std::string& err,
                                                       int* rcOut = nullptr);
eval::EvalMatrix mxAsMatrix(const MxValue& v);
std::optional<MxValue> evalMx(const std::string& text, SessionState& session, std::string& err,
                              bool joinOperand = false);
std::optional<eval::EvalMatrix> evalMatrixRhs(const std::string& rhs, SessionState& session,
                                              std::string& err);
void storeMatrixResult(SessionState& session, const std::string& name, eval::EvalMatrix mat);
std::string formatMatrixCell(double v);
std::string renderMatrixList(const std::string& name, const eval::EvalMatrix& m,
                             const std::function<std::string(double)>& cellText = nullptr);
ojson runMatrixStripe(bool isRow, const std::string& name, const std::string& namesRhs,
                       SessionState& session);
ojson runMatrixAccum(const std::string& name, const std::string& rhs, SessionState& session);
}  // namespace command_detail
}  // namespace piet::handlers
