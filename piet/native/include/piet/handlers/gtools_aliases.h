// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: gtools/ftools alias translation.
// Implemented under piet/native/src/datamgmt/.
#ifndef PIET_NATIVE_HANDLERS_GTOOLS_ALIASES_H
#define PIET_NATIVE_HANDLERS_GTOOLS_ALIASES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// ---- AER-SCOPE1 bundle C (GTOOLS, 2026-09-03) ------------------------------
// gtools/ftools are drop-in fast replacements for commands PIET already has, so
// the campaign spends ONE translation step rather than ten reimplementations.
// The shim drops options that only describe SPEED (`fast`, `verbose`), renames
// the ones that differ only in spelling, and REFUSES by name the ones that would
// change the answer (`gcollapse, merge` is not a collapse; `gquantiles, by()`
// wants per-group cutpoints). What it never does is forward an option the native
// command would silently ignore.
struct GtoolsTranslation {
  eval::ParsedCommand parsed;   // rewritten: canonical head, translated options
  std::string canonical;        // the native command to dispatch as
  nlohmann::ordered_json error; // non-empty -> refuse with this block
};
bool isGtoolsAlias(const std::string& canonical);
// Name-only fold, for the by-prefix path: `bysort g: gegen ...` has to reach the
// egen by()-injection branch, which compares the sub-command name before any
// translation has happened.
std::string gtoolsCanonicalName(const std::string& canonical);
GtoolsTranslation translateGtoolsAlias(const eval::ParsedCommand& parsed,
                                        const std::string& canonical, SessionState& session);
// `gquantiles x, _pctile nq(#)` / `pctile new = x, nq(#)` -- the one form with no
// native twin. The percentile rule is transcribed from the engine's own
// stata_pctile so the two cannot disagree.
// gquantiles' own options (cutpoints/cutoffs/quantiles/cutquantiles, by,
// altdef, binfreq, genp, minmax, replace, method, dedup, cutifin, strict,
// returnlimit): true when the line uses one, so the gquantiles arm computes it.
// gisid: the uniqueness check over an if/in sample, and the r() gtools leaves
// after a gisid that succeeds.
nlohmann::ordered_json runGisidSample(const eval::ParsedCommand& parsed, SessionState& session,
                                      bool missok);
void gisidPublishResults(const eval::ParsedCommand& parsed, SessionState& session);
bool gquantilesNeedsExtended(const eval::ParsedCommand& parsed);
nlohmann::ordered_json runGQuantilesExtended(const eval::ParsedCommand& parsed,
                                             SessionState& session);
nlohmann::ordered_json runGQuantilesPctile(const eval::ParsedCommand& parsed,
                                            SessionState& session);
// `gstats winsor|sum` -- a multiplex, not an alias: the two sub-commands forward
// to different native handlers and the rest are refused by name.
nlohmann::ordered_json runGstats(const eval::ParsedCommand& parsed, SessionState& session);
void gstatsPublishGroupResults(const eval::ParsedCommand& parsed, SessionState& session);
void gquantilesPublishXtile(const eval::ParsedCommand& parsed, SessionState& session,
                            const std::string& expText);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_GTOOLS_ALIASES_H
