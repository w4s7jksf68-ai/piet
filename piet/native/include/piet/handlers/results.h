// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: r()/e() stores, scalars, matrices, r(table).
// Implemented under piet/native/src/results/ and src/matrix/; the r(table) machinery
// in src/postestimation/.
#ifndef PIET_NATIVE_HANDLERS_RESULTS_H
#define PIET_NATIVE_HANDLERS_RESULTS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `scalar NAME = EXPR` is captured by the macro layer (session.eval.scalars)
// — no separate handler needed; see dispatcher.cpp's try_capture_definition
// call. `scalar list [NAME]` / `scalar dir` ARE separate handlers:
nlohmann::ordered_json runScalarList(const std::vector<std::string>& names, SessionState& session);

// `ereturn list` (BS-039) — display-only dump of the e() values PIET
// actually stores after an estimation (session.lastModel()). The other
// sub-verbs are runEreturn's (below).
nlohmann::ordered_json runEreturnList(SessionState& session);

// W2D-T01 (ereturn_commands.cpp): every `ereturn` sub-verb except `list` --
// `clear` (empties every e() store), `post [b [V]] [, obs() depname() dof()
// properties()]` (a NEW e(); esample() is refused by name until PIET keeps an
// e(sample) mask), `local NAME ...`, and a named refusal for the rest. Returns
// one error block, or an empty array on silent success.
nlohmann::ordered_json runEreturn(const eval::ParsedCommand& parsed, SessionState& session);

// `matrix NAME = e(b)` / `matrix NAME = e(V)` — capture the last estimation's
// coefficient vector / diagonal se^2 matrix into session.eval.matrices. Silent
// on success; other matrix forms return an explicit unsupported error.
nlohmann::ordered_json runMatrixDef(const std::string& raw, SessionState& session);

// A row or column name as `matrix list` and `matlist` print it: a factor
// level's `bn` marker is not shown (StataBE lists `2bn.g` as `2.g`, while
// `: colnames` keeps `2bn.g`).
std::string matrixNameAsListed(const std::string& name);

// ---- AER-SCOPE1 bundle B (RESULTS, 2026-09-03) -----------------------------
// r() made visible, and the matrix<->dataset round trip.
//
// `return list` prints what `ereturn list` prints, from the other store, so it
// reuses that handler's formatter rather than growing a second one that can
// drift from it (results/scalar_results.cpp::runEreturnList).
nlohmann::ordered_json runReturnList(SessionState& session);
// `svmat [type] A [, names(col|eqcol|matcol|stub)]` -- matrix columns become
// variables. Extends _N when the matrix has more rows than the dataset, which
// is Stata's documented behaviour and the reason this cannot be a pure read.
nlohmann::ordered_json runSvmat(const eval::ParsedCommand& parsed, SessionState& session);
// `getmata getlist [, double force replace]` -- Mata vectors/matrices from the
// SEPARATE Mata store (mata_subset.h) become variables, with getmata.ado's rc
// rules (111 not found, 102/103 name count, 110 exists, 459 rows != _N) and
// its r(K_new)/r(K_existing). `update` and `id()` are refused by name. Always
// returns exactly one object.
nlohmann::ordered_json runGetmata(const eval::ParsedCommand& parsed, SessionState& session);
// `mkmat varlist [if] [in], matrix(M) [rownames(v)] [nomissing] [obs]` -- the
// other direction.
nlohmann::ordered_json runMkmat(const eval::ParsedCommand& parsed, SessionState& session);
// `matlist EXP [, format(%fmt) ...]` where EXP is a matrix name or e(b)/e(V).
nlohmann::ordered_json runMatlist(const eval::ParsedCommand& parsed, SessionState& session);

// r(table) (w2c_postest family 1) — postestimation/rtable_rclass_lifecycle.cpp. ONE builder for every
// estimation block: rows `b se t|z pvalue ll ul df crit eform`, columns in Stata's
// e(b) order including a factor's base-level column. `level` is the DECLARED
// confidence level (the command's level(#), else `set level`). Returns false (and
// builds nothing) when Stata's table cannot be reproduced exactly — including a
// block whose p-values/intervals disagree with that level and reference.
bool buildRTableFromBlock(SessionState& session, const nlohmann::ordered_json& block,
                          double level, eval::EvalMatrix& out);
// Build and post r(table) from `block`, recording it (with the live e() identity)
// as the estimation machinery's own. False when nothing could be built (nothing
// is posted).
bool postRTableFromBlock(SessionState& session, const nlohmann::ordered_json& block,
                         double level);
// Identity of the live e() block (0 = none). Changes whenever e() is replaced;
// O(1), so dispatch() can take it on every entry.
std::uintptr_t liveEstimationIdentity(const SessionState& session);
// The r-class replacement for every handler that MERGES its keys into r(): if r()
// was written since `mark` (a store generation moved, or an r() matrix was posted
// or erased), erase every r() scalar, user r() macro and r() matrix last written
// at or before it. Reserved \x01 keys (s(), test's accumulate set) stay. True if
// r() was written. See eval/session.h nextStoreGeneration.
bool replaceStaleRClass(eval::EvalSession& ev, const eval::RStoreMark& mark);
// dispatcher.cpp's dispatch() calls this after every command with the e() identity
// and the r() mark it took on entry, and whether the command was a LEAF (no
// dispatch() ran inside it): r() written by the command replaces all of r()
// (replaceStaleRClass; a container is judged from its last inner command on); a
// leaf that replaced e() replaces r() and posts the table of the block it returned;
// a container that replaced e() keeps only a table posted for the e() now live;
// `margins` retires it.
void rTableAfterCommand(SessionState& session, const nlohmann::ordered_json& result,
                        const std::string& rawCmd, std::uintptr_t eIdAtEntry, bool leaf,
                        const eval::RStoreMark& markAtEntry);

// Empty every e() store a later reader can see: `ereturn clear`, and permute and
// jackknife before what they post (ereturn_commands.cpp).
void clearEResults(SessionState& session);

//
// matrix_sort_select.cpp -- the matrix store.
// `matsort matname [col] [up|down]` (Millar).
nlohmann::ordered_json runMatsort(const eval::ParsedCommand& parsed, SessionState& session);
// `matselrc m1 m2 [, row() col() names]` (Cox, STB-56 dm79).
nlohmann::ordered_json runMatselrc(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_RESULTS_H
