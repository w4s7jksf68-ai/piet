// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `sleep #` and `error #`.
#include "piet/handlers/session_commands.h"

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
using namespace environment_detail;

namespace {

// `error #`'s message table. Deliberately a fixed table and not a lookup into
// some shared registry: these are Stata's OWN canned strings for the codes a
// do-file raises by hand, and the empty entries (498/499, and anything absent)
// are Stata's real behaviour -- `error 499` prints nothing and sets _rc, which
// is why the rc travels in `reason` rather than being inferred from the text.
std::string errorCodeMessage(long long code) {
  switch (code) {
    case 1: return "You pressed Break";
    case 4: return "no; dataset in memory has changed since last saved";
    case 100: return "varlist required";
    case 102: return "too few variables specified";
    case 109: return "type mismatch";
    case 111: return "not found";
    case 119: return "statement out of context";
    case 190: return "request may not be combined with by";
    case 197: return "invalid syntax";
    case 198: return "invalid syntax";
    case 199: return "unrecognized command";
    case 301: return "last estimates not found";
    case 416: return "missing values encountered";
    case 430: return "convergence not achieved";
    case 459: return "something that should be true of your data is not";
    case 601: return "file not found";
    case 602: return "file already exists";
    case 603: return "file could not be opened";
    case 693: return "could not remove/create directory";
    case 2000: return "no observations";
    case 2001: return "insufficient observations";
    default: return "";  // 498/499 included: Stata prints nothing, rc still set
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// sleep #
//
// No upper bound on purpose: a corpus do-file that sleeps is waiting on
// something the author knew about, and a cap would turn a long wait into a
// silently different program rather than a slow one.
nlohmann::ordered_json runSleep(const std::vector<std::string>& varlist) {
  long long ms = 0;
  if (varlist.empty() || !parseLongLong(varlist[0], ms) || ms < 0) {
    return errBlock("sleep: milliseconds must be a non-negative integer", "syntax_error");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return ojson::array();  // Stata: silent
}

// ---------------------------------------------------------------------------
// error #
//
// The rc travels in `reason` rather than being inferred from the message,
// because half this table's entries share wording with each other (197 and 198
// are both "invalid syntax") and two of them have no wording at all -- so the
// text cannot carry the number and `capture`'s _rc must not be a guess.
nlohmann::ordered_json runErrorCode(const std::vector<std::string>& varlist) {
  long long code = 0;
  if (varlist.empty() || !parseLongLong(varlist[0], code)) {
    return errBlock("error: return code must be a number", "syntax_error");
  }
  if (code == 0) return ojson::array();  // Stata: `error 0' does nothing at all
  ojson b = errBlock(errorCodeMessage(code));
  b["reason"] = "r(" + std::to_string(code) + ")";
  return b;
}

}  // namespace piet::handlers
