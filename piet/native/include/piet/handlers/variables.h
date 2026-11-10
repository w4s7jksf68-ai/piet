// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: names, order, labels, formats, notes, storage types, signatures,
// varlist resolution. Implemented under piet/native/src/datamgmt/; lookfor and findname in
// src/descriptive/, fvunab in src/language/, resolveByKeyNames in src/dispatch/.
#ifndef PIET_NATIVE_HANDLERS_VARIABLES_H
#define PIET_NATIVE_HANDLERS_VARIABLES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `label variable VAR "text"` / `label values VAR [LBLNAME]` — session
// var-editor metadata (label.py's run_label / label_define.py's
// apply_label_values). lblname=nullopt (or empty) detaches the label.
nlohmann::ordered_json runLabelVariable(const std::string& varname, const std::string& text,
                                         SessionState& session);
nlohmann::ordered_json runLabelValues(const std::string& varname,
                                       const std::optional<std::string>& lblname,
                                       SessionState& session, bool nofix = false);

// `format VARLIST %fmt` — BS-005/D6 (BETA B-data): sets a display-layer
// session attribute (varMeta[name].format) honored by browse/list; NOT
// persisted into hbin (post-beta P1/hbin-v2). Unknown/malformed %fmt is an
// honest reject. See datamgmt/label_metadata.cpp's runFormat for the recognized format
// grammar (numeric/string/date families — not a full typed-format engine).
nlohmann::ordered_json runFormat(const std::vector<std::string>& varlist, const std::string& fmt,
                                  SessionState& session);
// The %fmt grammar `format` accepts (anything else is r(120) "invalid
// %format"), and whether a valid one is a string format (%#s, %-#s, %~#s).
// `file write` reads the same two rules.
bool isValidDisplayFormat(const std::string& fmt);
bool isStringDisplayFormat(const std::string& fmt);

// `label define LBLNAME # "text" ... [, add|modify]` / `label list
// [LBLNAME...]` / `label dir` / `label drop LBLNAME...` — M3b, C++ port of
// label_define.py.
nlohmann::ordered_json runLabelDefine(const std::string& rawLine, SessionState& session);
nlohmann::ordered_json runLabelList(const std::vector<std::string>& lblnames, SessionState& session);
nlohmann::ordered_json runLabelDir(SessionState& session);
nlohmann::ordered_json runLabelDrop(const std::vector<std::string>& lblnames, SessionState& session);
// `label language [NAME] [, new copy delete rename]` (LBL-LANGUAGE).
nlohmann::ordered_json runLabelLanguage(const std::vector<std::string>& args,
                                         const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                         SessionState& session);

// `labelbook [LBLNAME...]` — P1-W2-0: direct-read report over
// session.labelDefs/valueLabels/varLblname (no engine change — the data
// already arrives via `use`'s --out-meta sidecar, session_state.h:1054-1067).
// Scope = label set + definition + attached variables, NOT Stata's full
// diagnostic block (Range/Gaps/Unique/Missing — those need a data scan this
// direct-read report does not perform).
nlohmann::ordered_json runLabelbook(const std::vector<std::string>& lblnames, SessionState& session);

// `datasignature` — the engine's content fingerprint, forwarded verbatim.
// Stata-SHAPED (`<rows>:<cols>(<meta>):<a>:<b>`) but deliberately NOT
// Stata-comparable: Stata publishes no algorithm for its two checksums and its
// parenthetical term is not self-consistent across fixtures, so the engine
// pins the CONTRACT instead (deterministic · reorder/value/narrowing-sensitive ·
// format-insensitive) — see src/data/datasignature.h's Q9 ruling. The reported
// string is the engine's own; do not present it as a Stata cross-check.
nlohmann::ordered_json runDatasignature(SessionState& session);
// BUG-1461: `datasignature set [, reset]`, `confirm [, strict]` and `clear`, the
// signature kept in the dataset's _dta[datasignature_*] characteristics.
nlohmann::ordered_json runDatasignatureSub(
    const std::string& sub, const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    SessionState& session);

// `recast type varlist [, force]` -- METADATA only; the frame does not move, so
// this deliberately does not call `mutationResult`.
nlohmann::ordered_json runRecast(const eval::ParsedCommand& parsed, SessionState& session);

// `notes[:] ...` / `note` -- reads and writes `session.characteristics`, which
// the .dta writer already round-trips verbatim, so persistence is free.
nlohmann::ordered_json runNotes(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER scope 2, bundle K1 (datamgmt/variable_bulk_rename.cpp, datamgmt/variable_move.cpp) ------------------------
// `tolower [varlist]` -- lowercase variable NAMES. NOT atomic: it renames until
// one fails, which is what its five-line ado does (measured rc 110 on a name
// that is already lowercase).
nlohmann::ordered_json runTolower(const eval::ParsedCommand& parsed, SessionState& session);
// `move v1 v2` -- put v1 in v2's POSITION. Direction-dependent: before() when
// v1 is later, after() when v1 is earlier. `before()` alone is wrong one way.
nlohmann::ordered_json runMove(const eval::ParsedCommand& parsed, SessionState& session);
// `renvars [varlist] \ newlist` or `renvars [varlist], <one transformation>`.
// presub()/postsub() substitute at the START/END only -- not everywhere.
nlohmann::ordered_json runRenvars(const eval::ParsedCommand& parsed, SessionState& session);

// Package-007 unblock (T7). `lookfor word [word ...]` -- variables whose name or
// label contains a word (case-insensitive); describes them and sets r(varlist).
nlohmann::ordered_json runLookfor(const eval::ParsedCommand& parsed, SessionState& session);

// `rename OLDVAR NEWVAR` — two-arg form only.
nlohmann::ordered_json runRename(const std::string& oldvar, const std::string& newvar,
                                  SessionState& session);

// `clonevar NEWVAR = OLDVAR [if COND] [in RANGE]` — value copy + metadata clone
// (BUG-003; `if` per BS-031(c), `in` per BUG-1096 — see the .cpp comment).
nlohmann::ordered_json runClonevar(const std::string& newvar, const std::string& oldvar,
                                    SessionState& session,
                                    const std::optional<std::string>& ifExpr = std::nullopt,
                                    const std::optional<std::string>& inRange = std::nullopt);

// `order VARLIST [, first|last|before(VAR)|after(VAR)]` — column reorder.
nlohmann::ordered_json runOrder(const std::vector<std::string>& varlist,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session);

// THE varlist-name resolution of the by/bysort keys (dispatcher.cpp, BUG-532):
// wildcards and ranges expand through loops::expandVarlist, an exact name
// stays, a unique prefix stands for its variable, several prefix matches are
// r(111) "<tok> ambiguous abbreviation"; a token that matches nothing is left
// as typed. `names` is rewritten in place; the return is an error block or
// nullopt. binscatter's by()/controls()/absorb() use it too (FU-1088).
std::optional<nlohmann::ordered_json> resolveByKeyNames(SessionState& session,
                                                        std::vector<std::string>& names);

//
// descriptive/variable_search.cpp, language/factor_varlist_unab.cpp, datamgmt/value_label_numbering.cpp, datamgmt/bulk_variable_labels.cpp -- verbs that read or rename, never compute.
// `findname [varlist] [, varlabeltext()]` (Cox) -- sets r(varlist).
nlohmann::ordered_json runFindname(const eval::ParsedCommand& parsed, SessionState& session);

// `fvunab lmacname : varlist` -- operator prefixes kept, canonical case.
nlohmann::ordered_json runFvunab(const eval::ParsedCommand& parsed, SessionState& session);
// `numlabel [lblnames], add|remove [mask() force]`.
nlohmann::ordered_json runNumlabel(const eval::ParsedCommand& parsed, SessionState& session);
// `labvars varlist "label" ... [, remove]` (Klein).
nlohmann::ordered_json runLabvars(const eval::ParsedCommand& parsed, SessionState& session);
// `renpfix old [new]` -- renames in order, stopping at the first clash.
nlohmann::ordered_json runRenpfix(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_VARIABLES_H
