// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points implemented under piet/native/src/utility/ (one file per
// command family; see that directory's README).
#ifndef PIET_NATIVE_HANDLERS_UTILITY_H
#define PIET_NATIVE_HANDLERS_UTILITY_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// ---- AER-SCOPE1 bundle I (UTIL, 2026-09-04) --------------------------------
// Data-tidying utilities, built on primitives that already exist: the card's
// missing counts, runDrop/runDropIf, runEgen's rowmiss, runLabelDefine/
// runLabelValues, runRename, runTempvar. Nothing here computes a statistic a
// second time.
//
// Three of the seventeen commands in the plan's scope table are ABSENT on
// purpose. `dropmiss` and `renvars` have no package under their own name and
// did not arrive with a sibling, so their authorship could not be confirmed and
// the campaign's attribution rule gives them no row (an unregistered name
// already answers r(199), so nothing is lost by waiting). `strdist`/`matchit`/
// `reclink` are record linkage, refused for the same reason plus scope.

// `unique varlist [if] [in] [, by() gen()]` (and gtools' `gunique`).
nlohmann::ordered_json runUnique(const eval::ParsedCommand& parsed, SessionState& session);
// `distinct [varlist] [, joint missing]`.
nlohmann::ordered_json runDistinct(const eval::ParsedCommand& parsed, SessionState& session);
// `missings [varlist], report|dropvars|dropobs|tag()` -- each sub-verb routes to
// an existing primitive rather than re-deriving missingness.
nlohmann::ordered_json runMissings(const eval::ParsedCommand& parsed, SessionState& session);
// `mdesc [varlist]` -- the missing-value table; publishes r(miss)/r(total)/r(percent).
nlohmann::ordered_json runMdesc(const eval::ParsedCommand& parsed, SessionState& session);
// `labmask numvar, values(var) [lblname()]` -- synthesise a value label from a
// second variable's text and attach it.
nlohmann::ordered_json runLabmask(const eval::ParsedCommand& parsed, SessionState& session);
// `renvarlab [varlist], prefix()|suffix()|upper|lower`.
nlohmann::ordered_json runRenvarlab(const eval::ParsedCommand& parsed, SessionState& session);

// `strdist {var|"lit"} {var|"lit"} [, gen(name) maxdist(#)]` -- byte-wise
// Levenshtein. Default new-variable name is the literal word `strdist`; the
// two-literal form with no gen() displays the number and sets r(d).
nlohmann::ordered_json runStrdist(const eval::ParsedCommand& parsed, SessionState& session);
// `splitvallabels var [, length(#)]` -- publishes r(relabel).
nlohmann::ordered_json runSplitvallabels(const eval::ParsedCommand& parsed, SessionState& session);
// `marksample name [, novarlist]` -- PROGRAM-INTERNAL only; refuses at top level
// because the local it reads (`varlist`) is set by `syntax`, which only runs
// inside a program.
nlohmann::ordered_json runMarksample(const eval::ParsedCommand& parsed, SessionState& session);
// `filelist [, directory() pattern() norecursive]` -- REPLACES the dataset with
// one row per file (dirname, filename, fsize).
nlohmann::ordered_json runFilelist(const eval::ParsedCommand& parsed, SessionState& session);
// `carryforward varlist, gen()|replace` -- last-non-missing forward fill, reset
// at each by-group boundary.
nlohmann::ordered_json runCarryforward(const eval::ParsedCommand& parsed, SessionState& session);
// `rangestat (stat) var, interval(key lo hi) [by()]` -- a closed window
// [key+lo, key+hi] including the row itself; results in ORIGINAL row order.
nlohmann::ordered_json runRangestat(const eval::ParsedCommand& parsed, SessionState& session);
// `tsspell [varname | , cond()] [, spell() seq() end()]` -- needs tsset.
nlohmann::ordered_json runTsspell(const eval::ParsedCommand& parsed, SessionState& session);

// AER scope-2 B2-3: `bcal create|describe|load|dir|check` (Stata business
// calendars). The reader/writer/index arithmetic are in piet/bcal.h; this is
// the command surface.
nlohmann::ordered_json runBcal(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_UTILITY_H
