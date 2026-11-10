// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private to piet/native/src/fileio/: the four pieces of the fixed-column
// reader (fixed_column_import.cpp) that `infile` (infile.cpp) also uses --
// its type tokens, the `using ("data")` option repair, the empty-dataset
// rule, and the dictionary form `infile using dict`.
#pragma once

#include "vendor/nlohmann/json.hpp"

#include <optional>
#include <string>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace file_command_detail {

bool isTypeToken(const std::string& tok, bool* isString);
eval::ParsedCommand joinSpacedOptionValues(const eval::ParsedCommand& parsed);
std::optional<ojson> requireEmptyOrClear(const eval::ParsedCommand& parsed, SessionState& session);
ojson runDictionaryRead(const std::string& verb, const std::string& dictArg,
                        const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace file_command_detail

}  // namespace piet::handlers
