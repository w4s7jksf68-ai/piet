// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
// Private declarations shared by the loops family (language/loops.cpp,
// numlist_expansion.cpp, varlist_expansion.cpp, if_while_condition.cpp,
// legacy_for_command.cpp, parallel/parallel_block.cpp).

#include "piet/loops.h"

#include <optional>

#include "piet/eval/errors.h"
#include "piet/strutil.h"

namespace piet::loops::loops_detail {

inline ojson errBlock(const std::string& msg) { return eval::engineError(msg); }

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many lower(...)/trim(...) call
// sites didn't need touching. Pure dedup, no behavior change.
inline std::string lower(std::string s) { return strutil::lower(std::move(s)); }

inline std::string trim(const std::string& s) { return strutil::trim(s); }

struct NumlistResult {
  bool isError = false;
  ojson error;
  std::vector<std::string> tokens;
};

NumlistResult parseNumlist(const std::string& spec);

struct BoolEvalResult {
  bool isError = false;
  ojson error;
  bool value = false;
};

BoolEvalResult evalBoolExpr(const std::string& expr, SessionState& session);

VarlistExpandResult expandVarlistImpl(const std::string& spec, SessionState& session);
std::optional<ojson> checkKnownVarTokens(std::vector<std::string>& tokens,
                                         SessionState& session);

struct BodyRunResult {
  ojson blocks = ojson::array();
  bool hasError = false;
  // BUG-154: set iff the halt was a CLEAN `exit [N]` (mirrors
  // doexec::ExecuteResult::stoppedAtExitCode) — runCaptureBlock uses it to
  // set the exact Stata _rc (capture { exit 5 } → _rc 5, verified against
  // real Stata 19.5) instead of the coarse "1".
  std::optional<int> exitCode;
  std::optional<std::string> control;  // "continue" | "break"
};

BodyRunResult runBody(const std::vector<std::string>& bodyLines, SessionState& session,
                       const DispatchFn& dispatchFn);
void pushLoopBlock(ojson& allBlocks, bool& truncated, const ojson& b);

ojson runParallelBlock(const std::string& header, const std::vector<std::string>& bodyLines,
                       SessionState& session, const DispatchFn& dispatchFn);

}  // namespace piet::loops::loops_detail
