// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Table-command family routing after common parsing/option resolution.
#ifndef PIET_NATIVE_TABLE_COMMANDS_H
#define PIET_NATIVE_TABLE_COMMANDS_H

#include <optional>
#include <string>

#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
namespace eval {
struct ParsedCommand;
}
namespace commands {

// cmd is the canonical command name. nullopt means this family did not handle
// it; a handled null JSON or array is still a result. The caller alone flattens
// arrays/drops null blocks and applies the common dispatch lifecycle.
std::optional<nlohmann::ordered_json> runTableCommand(
    const std::string& cmd, const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace commands
}  // namespace piet

#endif  // PIET_NATIVE_TABLE_COMMANDS_H
