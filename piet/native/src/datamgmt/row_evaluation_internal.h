// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private declarations shared by the row-evaluator family:
// datamgmt/row_evaluation.cpp (the evaluator; defines every helper declared here
// except the inline errBlock), datamgmt/string_generate_replace.cpp and
// datamgmt/row_conditions.cpp.
#pragma once

#include "piet/strmutate.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"

namespace piet::strmutate {

using ojson = nlohmann::ordered_json;
using eval::PyValue;

namespace row_eval_detail {

std::string trim(const std::string& s);
std::string nextPath(const std::string& workdir, const std::string& prefix);

inline ojson errBlock(const std::string& msg, const std::string& code = "engine_error") {
  return eval::engineError(msg, code);
}

CobraError errorFromRaw(const CobraRunResult& r);
std::string rewriteExpr(const std::string& expr);

// --- T7b phase timer (report-only; see counters.h StrmutPhases) ------------
// Scoped stopwatch billing one PIET-side phase of the string gen/replace path.
// Deliberately times the SUB-INTERVALS only (the row loop, not evalRows'
// once-per-call column decode + expression compile), so that
//   verb wall - engine_ms - (evalrows+outbuild+csvwrite+reload)
// is a genuine residual rather than a definition — that residual is the check
// that this decomposition is complete, and it is what would expose the
// per-call setup if it were ever non-negligible.
struct PhaseTimer {
  int64_t* sink;
  std::chrono::steady_clock::time_point t0;
  explicit PhaseTimer(int64_t* s) : sink(s), t0(std::chrono::steady_clock::now()) {}
  ~PhaseTimer() {
    *sink += std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - t0)
                 .count();
  }
};

// Per-row evaluation result: one produced value per row + the condition mask.
struct EvalRowsResult {
  bool ok = true;
  std::string errMsg;
  std::string errReason;          // "r(NNN)" when the failing call named Stata's code
  std::string errText;            // with errReason: the call's own words ("type mismatch")

  std::vector<PyValue> values;    // meaningful only where mask is true
  std::vector<bool> valueIsSet;   // false where rhs was null / cond false
  std::vector<bool> mask;         // condition truth per row (all true if no cond)
};

ojson rowErrBlock(const std::string& msg, const EvalRowsResult& er);
EvalRowsResult evalRows(SessionState& session, Datastore& ds,
                        const std::optional<std::string>& rhsPy,
                        const std::optional<std::string>& condPy);
std::string coerceStr(const PyValue& v);
double coerceNum(const PyValue& v);
std::string csvEscape(const std::string& text);
std::string csvNum(double v);

}  // namespace row_eval_detail

// BUG-370: see the comment above extractEngineOnlyTokens in datamgmt/row_evaluation.cpp.
struct TsIfPrep {
  std::string rewrittenIf;  // == the input when there is no operator to resolve
  std::string evalPath;     // "" when there is nothing to do; else a scratch frame
  std::string errMsg;       // non-empty iff `failed`
  std::string errCode = "engine_error";
  bool failed = false;
};

TsIfPrep materializeTsIf(SessionState& session, const std::string& ifExpr,
                         const std::optional<std::string>& inRange);

}  // namespace piet::strmutate
