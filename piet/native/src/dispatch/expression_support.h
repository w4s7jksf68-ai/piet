// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

bool tryTempDeclIntercept(const std::string& line, SessionState& session,
                          const nlohmann::ordered_json& echo, nlohmann::ordered_json& out);

bool tryExtDisplayCapture(const std::string& lineIn, SessionState& session,
                          nlohmann::ordered_json& out);

std::string substituteInlineDisplay(const std::string& lineIn, SessionState& session,
                                    std::optional<nlohmann::ordered_json>& err);

std::optional<std::string> displayExpandablePrefix(const std::string& args, SessionState& session);

std::optional<nlohmann::ordered_json> displayPartialBlock(const std::string& args,
                                                          const nlohmann::ordered_json& err,
                                                          SessionState& session, bool evaluated);

bool isDisplayLine(const std::string& raw);

bool tryCLocal(const std::string& rawLine, SessionState& session, nlohmann::ordered_json& out);

bool rowConstNumberToken(const std::string& t);

bool rowOperandBoundaryLeft(char c);

bool rowOperandBoundaryRight(char c);

RowConstOperand rowLeftOperand(const std::string& e, size_t opPos);

RowConstOperand rowRightOperand(const std::string& e, size_t opEnd);

bool foldRowConstStringCompares(std::string& e, std::string* mismatch);

std::optional<std::string> firstBareName(const std::string& expr);

std::string substituteStoredScalars(const std::string& in, SessionState& session);

std::string substituteRcValue(const std::string& in, SessionState& session);

bool ifReadsCallerRc(const std::string& canonicalCmd);

std::string substituteStringCells(const std::string& in, SessionState& session);

std::string expandRowVarAbbrevs(const std::string& in, SessionState& session);

std::string rewriteEsampleToken(const std::string& in, SessionState& session,
                                       std::optional<ojson>* errOut);

std::string substituteRowExprRefs(const std::string& in, SessionState& session,
                                  std::optional<ojson>* errOut = nullptr,
                                  bool readsCallerRc = true);

std::vector<std::string> expandFactorGroupTokens(const std::vector<std::string>& in);

}  // namespace piet::dispatch_detail
