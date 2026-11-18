// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

StorageTypeInfo stripStorageType(const std::string& raw);

StorageTypeInfo withSessionDefaultType(const SessionState& session, StorageTypeInfo info);

void refreshCardFormat(const SessionState& session, ojson& blk, const std::string& varname);

void recordDeclaredStataType(SessionState& session, const std::string& varname,
                             const StorageTypeInfo& info, ojson& blk);

void appendGenMissingNote(ojson& blocks, const std::string& varname);

std::vector<ojson> runCompressMeta(const eval::ParsedCommand& parsed, SessionState& session);

int64_t rowsBeforeForRN(SessionState& session);

std::optional<bool> targetVarExists(SessionState& session, const std::string& varname);

std::optional<ojson> genTargetNameError(SessionState& session, const std::string& varname,
                                        const std::string& typeWord = std::string());

std::optional<ojson> replaceTargetError(SessionState& session, StorageTypeInfo& info);

std::optional<ojson> replaceNoPromote(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool& noPromote);

std::string genTargetName(const std::string& exprAfterType);

// generate ..., before(v) | after(v): reads the placement from the options (before() with after()
// is r(198); a variable that is not in the data is r(111)). On success `word` / `var` hold the
// placement ("" when none was asked for) and the result is empty.
std::optional<ojson> generatePlacementRequest(
    SessionState& session, const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::string& word, std::string& var);

// Moves the variable a generate just made next to `var` (what `order name, word(var)` does) and
// appends order's own non-echo blocks to `blocks`.
void placeGeneratedVariable(SessionState& session, const std::string& name,
                            const std::string& word, const std::string& var, ojson& blocks);

std::string strippedTypeWord(const std::string& raw, const StorageTypeInfo& info);

std::optional<ojson> conditionAsNameError(const std::optional<std::string>& ifExpr);

StringInRangeFold foldStringInRange(const std::optional<std::string>& ifExpr,
                                    const std::optional<std::string>& inRange);

ojson stringInRangeError(const StringInRangeFold& fold, const std::string& inRangeRaw);

std::string variableLabelText(const std::string& text);

std::optional<ojson> preResolveGenExpr(std::string& expr, std::string& rhs,
                                              SessionState& session);

}  // namespace piet::dispatch_detail
