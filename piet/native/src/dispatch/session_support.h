// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

std::string versionText(double v);

std::optional<ojson> parseVersionNumber(const std::string& tok, double* out);

bool versionSetsUserVersion(SessionState& session);

std::string canonicalSetName(const std::string& word);

// True when the lowercased word is a Stata setting's full name (`trace`, `dp`),
// not only the beginning of one.
bool isFullStataSettingWord(const std::string& word);

// Stata's own refusal of a `set NAME ...` line PIET would otherwise accept and
// ignore, as {message, "r(N)"}: a NAME that is no setting, or a value the
// setting does not take. nullopt = no refusal (the `set` arms decide).
std::optional<std::pair<std::string, std::string>> checkedStataSetWord(
    const eval::ParsedCommand& parsed, bool hasData);

}  // namespace piet::dispatch_detail
