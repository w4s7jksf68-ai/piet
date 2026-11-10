// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private to language/program.cpp and language/program_syntax.cpp: the two
// string forwarders both files call, and the `syntax` binder (defined in
// program_syntax.cpp) that handleReturnStmt calls.
#pragma once

#include <string>
#include <utility>

#include "piet/program.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

namespace piet::program::program_detail {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many trim(...)/lower(...) call
// sites didn't need touching. Pure dedup, no behavior change.
inline std::string trim(const std::string& s) { return strutil::trim(s); }

inline std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// Bind `syntax SPEC` against the local `0' (defined in program_syntax.cpp).
bool bindSyntax(const std::string& specIn, SessionState& session, ojson* err);

}  // namespace piet::program::program_detail
