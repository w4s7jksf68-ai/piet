// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: merge, append, joinby, reshape, stack, xpose, frames.
// Implemented under piet/native/src/datamgmt/.
#ifndef PIET_NATIVE_HANDLERS_DATA_COMBINE_H
#define PIET_NATIVE_HANDLERS_DATA_COMBINE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `xpose, clear [varname]` -- whole-frame transpose through a CSV round trip.
nlohmann::ordered_json runXpose(const eval::ParsedCommand& parsed, SessionState& session);

// `stack varlist, into() clear [wide group()]`.
nlohmann::ordered_json runStack(const eval::ParsedCommand& parsed, SessionState& session);

// `reshape long|wide STUB... , i(IDVARS) j(JVAR)`.
nlohmann::ordered_json runReshape(const std::vector<std::string>& stubs,
                                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   SessionState& session);

// `append using FILE [, generate(NAME) keep(VARLIST)]` — union-concat rows from a
// second dataset. `generate` (B5-D10), when given, marks the appended rows: 0 for
// the original master rows, 1 for the nUsing rows appended from `using`. `keepSpec`
// (BUG-549) is keep()'s raw varlist, expanded against the USING file's names.
// A variable numeric on one side and string on the other is r(106) unless
// `force` (BUG-550), which takes the using cells as missing of the master's type.
// A .dta using's value-label definitions are copied unless `nolabel` (BUG-551).
// The using file's notes are appended after the master's unless `nonotes`.
// Each master variable the using data widen adds Stata's "(variable x was float,
// now double to accommodate using data's values)" line to `typeNotes` (BUG-1092).
nlohmann::ordered_json runAppend(const std::string& filepath, SessionState& session,
                                  const std::optional<std::string>& genVar = std::nullopt,
                                  const std::optional<std::string>& keepSpec = std::nullopt,
                                  bool force = false, bool nolabel = false, bool nonotes = false,
                                  std::vector<std::string>* typeNotes = nullptr);

// `append using FILE [FILE ...] [, ...]` — append.ado's loop: every file
// described first (r(601)), r(N) = the last file's _N, generate() codes 1..k,
// and an empty master takes the first file whole.
nlohmann::ordered_json runAppendFiles(const std::vector<std::string>& files, SessionState& session,
                                       const std::optional<std::string>& genVar = std::nullopt,
                                       const std::optional<std::string>& keepSpec = std::nullopt,
                                       bool force = false, bool nolabel = false,
                                       bool nonotes = false,
                                       std::vector<std::string>* typeNotes = nullptr);

// `merge 1:1|m:1|1:m|m:m KEYLIST using FILE [, opts]` (mergeType already mapped by
// map_mtype); `sequential` = `merge 1:1 _n` (keylist empty).
nlohmann::ordered_json runMerge(const std::string& mergeType, const std::vector<std::string>& keylist,
                                 const std::string& usingPath,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session, bool sequential = false,
                                 bool oldSyntax = false, bool oldSort = false);

// `joinby key1 [key2 ...] using FILE` (bare form only) — BS-004/D5 (BETA
// B-data): PIET-NATIVE inner key-join (the analysis.do:451 shape). For
// every master row, emits one output row per USING row sharing its key
// values (the full within-key-group cross-product — joinby's own
// documented, order-independent behavior for repeated keys on either side,
// DISTINCT from Stata's discouraged `merge, m:m` row-order-paired
// semantics, which this build does not implement); a master row with zero
// using matches is dropped (unmatched(none), the default and — per D5 —
// the ONLY mode this build implements). ANY joinby option
// (unmatched()/update/updatemiss/_merge()/nolabel/...) is an honest reject
// naming the option (post-beta WP-18) — the dispatcher arm enforces this
// via the zero-known-options net before calling this handler. The join
// itself is ONE engine call (`cobra joinby --data --using --keys --out`,
// L1b) — PIET keeps the using-file resolution (.dta/.csv -> .hbin
// pre-convert; BUG-125 virtual-save materialization) and all
// pre-validation (keys present on both sides, non-key column-name
// overlap) so the established PIET-side error messages still fire before
// the engine is reached.
// `unmatched` is joinby.ado's UnMatch value (none|master|using|both);
// `mergeVar` its _merge() name (the marker exists when either asks for it).
nlohmann::ordered_json runJoinby(const std::vector<std::string>& keylist,
                                  const std::string& usingPathArg, SessionState& session,
                                  const std::string& unmatched = "none",
                                  const std::optional<std::string>& mergeVar = std::nullopt);

// -- frames (P6 wave, frames.cpp) ----------------------------------
// Stata `frame`s: additional named datasets beside the current one. A PIET
// session concept only — each frame is one hbin in the session workdir, and
// every operation is expressed with engine verbs that already exist (eval,
// merge, rename), so no contract surface moves. See frames.cpp's
// header comment for the design and the deliberate scope limits (the current
// frame is always `default`; frame create/change/pwf are honest rejects).
nlohmann::ordered_json runFramePut(const std::vector<std::string>& varlist,
                                   const std::optional<std::string>& ifExpr,
                                   const std::string& into, SessionState& session);
nlohmann::ordered_json runFrameDrop(const std::string& name, SessionState& session);
// `frame NAME: CMD` — runs CMD with NAME as the current dataset, then restores.
// Mutations persist into the frame, as they do in Stata.
nlohmann::ordered_json runFramePrefix(const std::string& name, const std::string& innerCmd,
                                      SessionState& session);
// `frlink m:1|1:1 KEY..., frame(F) generate(G)` — G holds each row's matching
// observation number in F (missing when unmatched), computed as an m:1 merge
// against a helper carrying F's row index.
nlohmann::ordered_json runFrlink(const std::string& linkType,
                                 const std::vector<std::string>& keylist,
                                 const std::string& frameName, const std::string& genName,
                                 SessionState& session);
// `frget NEW=VAR [...], from(LINKVAR)` — copies variables out of the frame the
// link points into, matched through the link.
nlohmann::ordered_json runFrget(const std::vector<std::pair<std::string, std::string>>& renames,
                                const std::string& fromLink, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_DATA_COMBINE_H
