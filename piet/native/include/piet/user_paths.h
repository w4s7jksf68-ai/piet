// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// resolveUserPath and typedUserPath: the user-supplied-path resolver.
// Implemented under piet/native/src/fileio/ (user_paths.cpp).
#ifndef PIET_NATIVE_USER_PATHS_H
#define PIET_NATIVE_USER_PATHS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

// Canonical Stata-style user-supplied-path resolver (native-paths
// stabilization, item 2). The ONE place every handler that accepts a path a
// USER wrote in a do-file (use/save/append using/merge using/postfile
// using/graph save/graph export/erase/confirm file/mkdir) resolves it:
//   1. dequote (matching handlers::runUse's own historical dequote rule).
//   2. `~` expansion (HOME-relative only — same rule runCd used).
//   3. absolute paths pass through unchanged (POSIX leading '/'; on Windows
//      also a drive letter like "C:\" or "C:/", or a leading backslash).
//   4. otherwise join against session.cwd.
//   5. if session.cwd is unset (a headless driver that never seeds it),
//      fall back to getcwd().
// Deliberately declared in `piet` (not `piet::handlers`) — the graph code's
// own resolvePathG/dequoteG duplicate used to live outside the handlers TU
// for the same reason (a second `piet::handlers`-only symbol would still
// need re-declaring there); one shared symbol replaces both forks.
namespace piet {
std::string resolveUserPath(const SessionState& session, const std::string& rawPath);
// The same path as a message names it: step 1 above and nothing else. Stata's
// "file X not found" names the file as typed (`file sub/nofile.txt not found`),
// never the resolved absolute path, which would put this machine's working
// directory into every captured transcript (BUG-410).
std::string typedUserPath(const std::string& rawPath);
}  // namespace piet

#endif  // PIET_NATIVE_USER_PATHS_H
