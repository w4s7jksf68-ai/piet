// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "types.h"

namespace piet::dispatch_detail {

std::string versionText(double v);

std::optional<ojson> parseVersionNumber(const std::string& tok, double* out);

bool versionSetsUserVersion(SessionState& session);

std::string canonicalSetName(const std::string& word);

}  // namespace piet::dispatch_detail
