// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

char asciiLower(char c);

std::optional<std::string> stripPrefix(const std::string& line, const PrefixTable& table);

BysortPrefix parseBysortPrefix(const std::string& cmdLower, const std::vector<std::string>& varlist,
                               const std::string& raw);

std::pair<std::string, std::optional<std::string>> splitTopLevelComma(const std::string& s);

std::string joinSpace(const std::vector<std::string>& v);

bool keysAlreadyGrouped(Datastore& ds, const std::vector<std::string>& keyNames,
                        bool stataStringOrder = false);

std::vector<std::string> engineSortClaim(const Datastore& ds);

bool keysLeadClaim(const std::vector<std::string>& keys, const std::vector<std::string>& claim);

}  // namespace piet::dispatch_detail
