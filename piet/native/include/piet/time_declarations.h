// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Parsed tsset/tset/xtset command execution, after common option resolution.
#ifndef PIET_NATIVE_TIME_DECLARATIONS_H
#define PIET_NATIVE_TIME_DECLARATIONS_H

#include <string>
#include <unordered_set>

#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
namespace eval {
struct ParsedCommand;
}
namespace commands {

// Caller accepts only canonical xtset/tsset/tset and supplies that command's
// existing registered known-options set. Appends to the caller's result array;
// common echo, prefix handling and outer result/state lifecycle stay there.
// parsed is mutable because varlist validation resolves its names in place.
void runTimeDeclaration(const std::string& cmd, eval::ParsedCommand& parsed,
                        SessionState& session,
                        const std::unordered_set<std::string>& selectedKnownOptions,
                        nlohmann::ordered_json& blocks);

}  // namespace commands
}  // namespace piet

#endif  // PIET_NATIVE_TIME_DECLARATIONS_H
