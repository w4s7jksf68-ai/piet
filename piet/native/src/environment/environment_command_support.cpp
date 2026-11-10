// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Helpers shared by the environment command files (shell, type/dir, sysdir/adopath/
// findfile/net/ado, sleep/error #): reply blocks, text, cwd, argument recovery,
// shell-style tokenizing, glob and integer parsing.
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "piet/bundle_paths.h"
#include "piet/eval/errors.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::to_native_path / env_utf8 (BUG-050/051)
#include "environment_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace environment_detail {

std::string trim(const std::string& s) { return strutil::trim(s); }
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

ojson errBlock(const std::string& message, const std::string& code) {
  return ojson(eval::engineError(message, code));
}

// The exact-rc shape eval::coarseRc honours (BUG-187): a call site that KNOWS
// the Stata code says so in `reason` as "r(NNN)", and `capture` hands that
// number back instead of the generic 1 the message-sniffing fallback yields.
ojson errBlockRc(const std::string& message, const std::string& rc) {
  ojson b = errBlock(message);
  b["reason"] = rc;
  return b;
}

std::string dequote(const std::string& sIn) {
  std::string s = trim(sIn);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

std::string sessionCwd(const SessionState& session) {
  namespace fs = std::filesystem;
  if (session.cwd.has_value() && !session.cwd->empty()) return *session.cwd;
  std::error_code ec;
  fs::path p = fs::current_path(ec);
  return ec ? std::string(".") : p.u8string();
}

// The single argument of a one-argument file command, taken off the RAW text
// when the tokenized varlist did not survive (quoted paths with spaces). The
// comma cut is deliberate and its own limit: an option list starts there, and a
// filename containing a comma is not a shape this corpus produces -- the
// varlist path, which is tried first by every caller, handles it if it ever is.
std::string argAfterCommand(const std::string& raw) {
  std::string s = trim(raw);
  const size_t sp = s.find_first_of(" \t");
  if (sp == std::string::npos) return "";
  s = trim(s.substr(sp + 1));
  const size_t comma = s.find(',');
  if (comma != std::string::npos) s = trim(s.substr(0, comma));
  return dequote(s);
}

std::vector<ShellTok> tokenizeShellArgs(const std::string& s) {
  std::vector<ShellTok> out;
  ShellTok cur;
  bool inTok = false;
  bool inQuote = false;
  for (const char c : s) {
    if (c == '"') {
      inQuote = !inQuote;
      cur.quoted = true;
      inTok = true;
      continue;
    }
    if (!inQuote && (c == ' ' || c == '\t')) {
      if (inTok) {
        out.push_back(cur);
        cur = ShellTok();
        inTok = false;
      }
      continue;
    }
    cur.text.push_back(c);
    inTok = true;
  }
  if (inTok) out.push_back(cur);
  return out;
}

// Iterative `*`/`?` matcher (no character classes, no `**`): the shared one,
// where `?` is one character (a UTF-8 sequence), not one byte.
bool globMatch(const std::string& pat, const std::string& name) {
  return strutil::globMatch(pat, name);
}

bool parseLongLong(const std::string& s, long long& out) {
  const std::string t = trim(s);
  if (t.empty()) return false;
  size_t i = 0;
  if (t[0] == '+' || t[0] == '-') i = 1;
  if (i >= t.size()) return false;
  for (size_t k = i; k < t.size(); ++k) {
    if (!std::isdigit(static_cast<unsigned char>(t[k]))) return false;
  }
  try {
    out = std::stoll(t);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

}  // namespace environment_detail

}  // namespace piet::handlers
