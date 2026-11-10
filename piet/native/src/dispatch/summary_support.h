// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

std::optional<std::string> parseTabstatStats(const std::optional<std::string>& statisticsVal,
                                             std::vector<std::string>& statKeys,
                                             std::vector<std::string>& statRows);

eval::EvalMatrix tabstatStatMatrix(const ojson& statsObj, const std::vector<std::string>& statKeys,
                                   const std::vector<std::string>& statRows,
                                   const std::vector<std::string>& colVars);

std::string tsPadLeft(const std::string& text, size_t width);

std::string tsPadRight(const std::string& text, size_t width);

std::string tsClip(const std::string& text, size_t width);

std::string tsShortName(const std::string& name, size_t room);

bool tsIsLeftJustified(const std::string& fmt);

bool tsIsTimeFormat(const std::string& fmt);

size_t tsFormatWidth(const std::string& fmt);

std::string tsFormatted(double value, const std::string& fmt, size_t width);

std::optional<VarInfo> tsVarCard(SessionState& session, const std::string& name);

std::string tsDeclaredFormat(SessionState& session, const std::string& name);

size_t tsClampedWidth(const eval::OptValue& given, const char* option, size_t fallback,
                      std::string& notes);

bool tsReadLookOptions(const eval::ParsedCommand& parsed, TabstatLookOptions& look,
                       std::optional<ojson>& refusal);

TabstatCellStyle tsCellStyleFor(SessionState& session, const TabstatLookOptions& look,
                                const std::string& var);

TabstatByColumn tsLayOutByColumn(SessionState& session, const std::string& byName,
                                 const ojson& groups, size_t labelWidth, bool totalRow);

std::string renderTabstatTable(SessionState& session, const eval::ParsedCommand& parsed,
                               const ojson& tsBlock, const std::optional<std::string>& byVal,
                               const std::optional<std::string>& columnsVal, bool noTotal,
                               const std::vector<std::string>& statKeys,
                               const std::vector<std::string>& statRows,
                               std::optional<ojson>& refusal);

nlohmann::ordered_json runTabulateGenerate(const std::string& stub,
                                           const std::vector<std::string>& varlist,
                                           const std::vector<std::string>& rawRows,
                                           SessionState& session,
                                           const std::optional<std::string>& ifExpr,
                                           const std::optional<std::string>& inRange,
                                           bool includeMissing);

DescribeShape describeDatasetShape(SessionState& session);

void captureSummarizeR(const ojson& summBlock, bool meanonly, bool detail,
                              SessionState& session);

}  // namespace piet::dispatch_detail
