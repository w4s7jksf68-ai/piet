// strmutate.h — row-wise PIET evaluator for STRING-valued / STRING-gated
// `gen`/`replace`/`count`/`keep if`/`drop if`. C++ port of
// piet/backend/piet/strmutate.py (the frozen 1.16.0 oracle).
//
// cobra is an all-f64 engine: it cannot evaluate string functions (lower,
// substr, strtrim, regexr, ...) nor hold/compare string columns (strings live
// in hbin as *factors* = int32 codes + a levels list). So any `gen`/`replace`
// that (a) declares a string storage type, (b) produces a string (rhs uses a
// string function / literal), (c) is gated on a string condition, or (d)
// targets/reads an existing factor column MUST be evaluated row-by-row in
// this layer instead of shelling out to `cobra eval --gen`. The result is
// written back as a factor (string) column, which cobra then consumes.
//
// The pure-numeric, no-string path is untouched (still cobra) —
// `needsStringEval` only returns true when a string is genuinely involved, so
// the all-f64 corpus stays byte-identical.
//
// Reuses the M1 evaluator infrastructure (pyeval/exprfns/pyvalue) for the
// per-row expression evaluation — the same machinery loops.cpp's evalBoolExpr
// uses — rather than a fresh eval engine.
#ifndef PIET_NATIVE_STRMUTATE_H
#define PIET_NATIVE_STRMUTATE_H

#include <optional>
#include <string>
#include <vector>

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::strmutate {

// needs_string_eval(session, varname, rhs, if_expr, str_typed, is_replace):
// decide whether a gen/replace must be evaluated row-wise (string path).
bool needsStringEval(SessionState& session, const std::string& varname, const std::string& rhs,
                     const std::optional<std::string>& ifExpr, bool strTyped, bool isReplace);

// needs_string_count(session, if_expr): decide whether a `count if` (or
// keep/drop if) condition must be evaluated row-wise (references a factor
// column / string fn / string literal).
bool needsStringCount(SessionState& session, const std::optional<std::string>& ifExpr);

// run_string_gen / run_string_replace: row-wise gen/replace. Returns the
// data_mutation block (or an error block on a bad expression).
nlohmann::ordered_json runStringGen(SessionState& session, const std::string& expr,
                                    const std::optional<std::string>& ifExpr, bool strTyped);
nlohmann::ordered_json runStringReplace(SessionState& session, const std::string& expr,
                                        const std::optional<std::string>& ifExpr, bool strTyped);

// count_rows_if: number of rows where a (possibly string-referencing)
// condition is true. Sets `ok=false` on an un-evaluatable condition (message
// carried in `errMsg`). `inRange` (BS-090, BETA Phase G): an optional `in
// a/b` row-range, ANDed into the SAME per-row mask this already builds
// (count/filter use this for string/factor conditions; calendar creation also
// reuses its full-length mask for numeric conditions). Callers must never silently
// drop a co-occurring `in` just because the `if` half took the row-wise
// detour — malformed/out-of-range forms set `ok=false` the same way an
// un-evaluatable `if` expression does, `errMsg` carries the same wording
// family as piet::handlers::inRangeRejectError.
struct CountResult {
  bool ok = true;
  bool inRangeError = false;  // true iff a failing `ok=false` is the `in` bound, not `if`
  long long n = 0;
  std::string errMsg;
  // Full-length sample mask, in original row order, when ok is true.
  std::vector<bool> selected;
};
CountResult countRowsIf(SessionState& session, const std::string& ifExpr,
                        const std::optional<std::string>& inRange = std::nullopt);

// filter_rows_if: row-wise `keep if` (keep=true) / `drop if` (keep=false) for
// conditions cobra can't evaluate. Returns the data_mutation block (or
// error). `inRange` (BS-090) ANDs the same row-range restriction into the
// mask before the keep/drop split is applied.
nlohmann::ordered_json filterRowsIf(SessionState& session, const std::string& ifExpr, bool keep,
                                    const std::optional<std::string>& inRange = std::nullopt);

// --- Selftest instrumentation (FLOOR campaign: slot-compiled evalRows) ------
//
// evalRowsMapWriteCount() counts per-row namespace-map WRITES on the row-eval
// path (evalRows). The slot-compiled fast path performs ZERO of them; the
// legacy per-row rebind performs (referenced-columns + row-specials) writes
// per row. Process-wide, env-independent — same idiom as
// Datastore::constructCount() / SessionState::cardFoldCount().
//
// The mechanism gate reads 0 here after a slot-compiled gen/count on a
// multi-row fixture. A nonzero count is the ONLY signal that slot-compilation
// silently stopped applying (e.g. compilePreboundExpr began rejecting an
// expression that used to compile): every value pin stays green either way,
// because the two paths compute identical answers.
int64_t evalRowsMapWriteCount();
void resetEvalRowsMapWriteCount();

// Force evalRows onto the legacy per-row namespace-bind path, bypassing
// slot-compilation. Selftest hook ONLY (default false; no production caller):
// lets a gate obtain the legacy answer for the SAME expression the slot path
// computed and assert the fallback still evaluates correctly with map
// writes > 0.
void setForceLegacyEvalRowsForTest(bool on);

}  // namespace piet::strmutate

#endif  // PIET_NATIVE_STRMUTATE_H
