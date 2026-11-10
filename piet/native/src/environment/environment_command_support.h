// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private helpers shared by the environment command files (shell, type/dir,
// sysdir/adopath/findfile/net/ado, sleep/error #). Definitions live in
// environment_command_support.cpp; consumers import them with
// `using namespace environment_detail;` and must not also import command_detail.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <string>
#include <vector>

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace environment_detail {

std::string trim(const std::string& s);
std::string lower(std::string s);
ojson displayBlock(const std::string& text);
ojson errBlock(const std::string& message, const std::string& code = "engine_error");
ojson errBlockRc(const std::string& message, const std::string& rc);
std::string dequote(const std::string& sIn);
std::string sessionCwd(const SessionState& session);
std::string argAfterCommand(const std::string& raw);

// Whitespace tokenizer that keeps a double-quoted run together. `quoted`
// records that the token carried quotes, which is what stops `"-rf"` (a
// deliberately quoted FILENAME) from being read as a flag two functions down.
struct ShellTok {
  std::string text;
  bool quoted = false;
};

std::vector<ShellTok> tokenizeShellArgs(const std::string& s);
bool globMatch(const std::string& pat, const std::string& name);
bool parseLongLong(const std::string& s, long long& out);

}  // namespace environment_detail
}  // namespace piet::handlers
