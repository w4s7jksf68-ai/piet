// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private interfaces used across related command families.
#pragma once

#include "piet/handlers.h"
#include <set>

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

std::string trim(const std::string& s);
std::string lower(std::string s);
ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error");
ojson engineErrorBlockRc(const std::string& message, int rc);
std::string cobraErrorStr(const CobraError& ce);
std::string nextPath(const std::string& workdir, const std::string& prefix);
bool cloneOrCopyFile(const std::string& src, const std::string& dst);
CobraError errorFromRaw(const CobraRunResult& r);
bool cardHasName(const std::vector<VarInfo>& card, const std::string& name);
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
long findUnquotedComma(const std::string& s);
}  // namespace command_detail
ojson runSort(const std::vector<std::string>& varlist, SessionState& session);
std::string matrixNameAsListed(const std::string& name);
bool isValidDisplayFormat(const std::string& fmt);
}  // namespace piet::handlers
