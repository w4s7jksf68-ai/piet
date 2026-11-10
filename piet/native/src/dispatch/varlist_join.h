// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private declarations for existing dispatcher support shared by command TUs.
#pragma once

#include <string>
#include <vector>

namespace piet {
// One blank between varlist tokens. Implementation remains in dispatcher.cpp.
std::string joinVarlist(const std::vector<std::string>& varlist);
}  // namespace piet
