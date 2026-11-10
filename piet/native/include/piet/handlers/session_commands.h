// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: session lifecycle, environment and filesystem shims, temp names,
// timer, macro verbs. Implemented under piet/native/src/environment/; temp names, confirm,
// macro verbs, dots and markout in src/language/; preserve/restore and the frames reset
// in src/datamgmt/.
#ifndef PIET_NATIVE_HANDLERS_SESSION_COMMANDS_H
#define PIET_NATIVE_HANDLERS_SESSION_COMMANDS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `preserve` / `restore` — preserve_restore.py's LIFO hbin-snapshot stack.
// `restore, preserve` (B5-D14, BUG-454): `rearm` restores AND re-snapshots,
// so a following `restore` can pop the same state again.
nlohmann::ordered_json runPreserve(SessionState& session);
nlohmann::ordered_json runRestore(SessionState& session, bool rearm = false);

// `tempfile NAME [NAME...]` / `tempname NAME [NAME...]` — S5/U4, silent on
// success (returns an EMPTY array — caller treats non-empty as error).
nlohmann::ordered_json runTempfile(const std::vector<std::string>& names, SessionState& session);
nlohmann::ordered_json runTempname(const std::vector<std::string>& names, SessionState& session);
// `tempvar NAME...` (BS-037) — sibling of tempfile/tempname above: mints
// unique, LEGAL, collision-free new variable names bound as local macros.
nlohmann::ordered_json runTempvar(const std::vector<std::string>& names, SessionState& session);

// `clear [subcmd]` — BS-044 (BETA campaign C4): `sub` is the lowered
// subcommand token (""/"all"/"*"/"results"/"matrix"/"programs"/"mata"/
// "ado"/"rngstream"/"rngstreams"/"frames"/"collect"/anything else); each
// gets its OWN scoped clear (bare drops only the dataset, `all` drops
// everything, `results`/`matrix` drop e()/r() state only, etc.) — see the
// .cpp definition for the full per-subcommand behavior.
nlohmann::ordered_json runClear(const std::string& sub, SessionState& session);

// `set obs N` — construct/extend the dataset to N observations.
nlohmann::ordered_json runSetObs(const std::string& nArg, SessionState& session);

// `cd [DIR]` / `pwd` — working-directory management.
nlohmann::ordered_json runCd(const std::string& dirArg, SessionState& session);
nlohmann::ordered_json runPwd(SessionState& session);

// `log using FILE [, replace|append] [text]` / `log close|off|on` (BUG-028,
// BS-006). Really opens FILE now and tees every subsequent outermost
// command's output to it as a plain-text transcript (SessionState::openLog/
// logTee; see this function's own doc comment in environment/logging_commands.cpp for the
// replace/append/text-vs-smcl rules). Returns an empty array (no block —
// Stata is silent on a successful log open/close/off/on), matching the
// oracle's `return None` handler convention; a single ojson object on
// failure (e.g. unknown option, file already exists, smcl requested).
nlohmann::ordered_json runLog(const std::string& raw, SessionState& session);

// U7/S9 offline shims (mirrors piet/backend/piet/shims.py) — package-
// presence / ssc-install / file-existence / directory / file-delete stubs
// so classroom do-files (e.g. analysis.do's `capture which drdid` /
// `capture confirm variable ...` guards) never halt on these no-data ops.
nlohmann::ordered_json runWhich(const std::string& raw, const std::vector<std::string>& varlist);
nlohmann::ordered_json runSsc(const std::vector<std::string>& varlist);
nlohmann::ordered_json runConfirm(const std::string& raw, const std::vector<std::string>& varlist,
                                   SessionState& session);
nlohmann::ordered_json runMkdir(const std::string& raw, const std::vector<std::string>& varlist,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session);
// BUG-180 (P2-PREP): `rmdir DIR` — remove one EMPTY directory. Same shape as
// runMkdir (it takes opts only so the shared reject net can refuse them —
// real Stata's rmdir has no options).
nlohmann::ordered_json runRmdir(const std::string& raw, const std::vector<std::string>& varlist,
                                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                 SessionState& session);
nlohmann::ordered_json runErase(const std::string& raw, const std::vector<std::string>& varlist,
                                 SessionState& session);

// ---- AER-SCOPE1 bundle D (ENV, 2026-09-03) ---------------------------------
// The environment verbs the ninety-paper corpus opens with. Three shapes:
//
//   accept-note  net / ado / adopath / sysdir / pause / gr_edit -- PIET has no
//                ado directory and no network, so the honest answer is a
//                visible one-line note and rc 0. A silent no-op would be the
//                same behaviour with the evidence removed, and a refusal would
//                stop the do-file on its first line (these sit in the preamble
//                of 25, 15, 18 and 9 packages respectively).
//   translate    shell -- an allowlist of file-manipulation verbs mapped onto
//                PIET's own erase/mkdir/rmdir/copy, so the effect the do-file
//                depended on actually happens. NO OS process is ever spawned;
//                anything outside the allowlist is refused by name.
//   implement    type / dir / findfile / sleep / error / frames reset -- small
//                real behaviours with no engine involvement.
nlohmann::ordered_json runShell(const std::string& args, SessionState& session);
nlohmann::ordered_json runType(const std::string& raw, const std::vector<std::string>& varlist,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                SessionState& session);
nlohmann::ordered_json runDir(const std::string& raw, const std::vector<std::string>& varlist,
                               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                               SessionState& session);
nlohmann::ordered_json runNet(const std::string& raw, const std::vector<std::string>& varlist,
                               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                               SessionState& session);
nlohmann::ordered_json runAdo(const std::string& raw, const std::vector<std::string>& varlist);
nlohmann::ordered_json runAdopath(const std::string& raw, const std::vector<std::string>& varlist,
                                   SessionState& session);
nlohmann::ordered_json runSysdir(const std::string& raw, const std::vector<std::string>& varlist,
                                  SessionState& session);
nlohmann::ordered_json runFindfile(const std::string& raw, const std::vector<std::string>& varlist,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    SessionState& session);
// The first FNAME on the session adopath, searched as findfile searches it
// (each entry, then its letter subdirectory); nullopt when none. The
// dispatcher's ado autoload reads it.
std::optional<std::string> findOnAdopath(SessionState& session, const std::string& fname);
nlohmann::ordered_json runSleep(const std::vector<std::string>& varlist);
nlohmann::ordered_json runErrorCode(const std::vector<std::string>& varlist);
nlohmann::ordered_json runFramesReset(SessionState& session);

// `timer on|off|clear|list [N]` (BS-124, BETA campaign C4) — `sub` is the
// lowered sub-command ("on"/"off"/"clear"/"list"); `slotArg` is the parsed
// slot number (1..100) when given, unset for the bare `timer clear`/
// `timer list` forms. See the .cpp definition for the full state machine.
nlohmann::ordered_json runTimer(const std::string& sub, const std::optional<int>& slotArg,
                                 SessionState& session);

//
// language/macro_command.cpp -- the `macro` verb (tryMacroVerbIntercept); the declarations below it
// for require, _dots and markout are defined in environment/package_requirements.cpp,
// language/progress_dots.cpp and language/markout.cpp.
// `macro drop|define|shift|list|dir ...`, consumed as a raw line before the
// tokenizer (whose unsupported_macro net names `macro`). Called twice, like
// tryTempDeclIntercept: on the raw line (`expanded` false, which defers any
// line still holding `$` or a backtick) and on the macro-expanded line.
// Returns true when it consumed the line, with the reply in `out`.
bool tryMacroVerbIntercept(const std::string& line, SessionState& session,
                           const nlohmann::ordered_json& echo, nlohmann::ordered_json& out,
                           bool expanded);
// `require pkg[op version] ... [, install strict]` (Correia/Seay) -- accept-note.
nlohmann::ordered_json runRequire(const eval::ParsedCommand& parsed, SessionState& session);
// `_dots 0, title() reps()` header and `_dots # rc` progress marks.
nlohmann::ordered_json runDots(const eval::ParsedCommand& parsed, SessionState& session);

// `markout markvar [varlist] [, strok]`.
nlohmann::ordered_json runMarkout(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_SESSION_COMMANDS_H
