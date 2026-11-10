// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: bootstrap/jackknife/permute, rolling, statsby prefixes.
// Implemented under piet/native/src/estimation/.
#ifndef PIET_NATIVE_HANDLERS_RESAMPLING_H
#define PIET_NATIVE_HANDLERS_RESAMPLING_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// -- resampling prefixes (P6 wave, estimation/bootstrap_prefix.cpp, estimation/jackknife_permute_prefix.cpp) -------------------
// `bootstrap:` / `jackknife:` / `permute:` — the loop runs PIET-side and the
// inner command goes through ordinary dispatch, so the prefixes are general
// (any command PIET supports, any exp_list the expression layer can evaluate).
// `expListRaw` MUST arrive verbatim: its r()/e()/_b[] references are evaluated
// once per replicate, not at parse time (see the .cpp header for why dispatch
// intercepts these before macro expansion). `permuteVar` is empty except for
// permute.
nlohmann::ordered_json runResample(const std::string& method, const std::string& permuteVar,
                                   const std::string& expListRaw, const std::string& optsRaw,
                                   const std::string& innerCmd, SessionState& session);

// `rolling explist, window(N) saving("FILE"[, replace]): command` — the same
// PIET-side skeleton as the resampling prefixes, with a WINDOW SLICE of the
// tsset time variable in place of a resample, so the inner command stays
// general. Requires a prior tsset/xtset.
nlohmann::ordered_json runRolling(const std::string& expListRaw, const std::string& optsRaw,
                                  const std::string& innerCmd, SessionState& session);

// `statsby _b _se, clear [double]: command` — the ungrouped form only (no by());
// see estimation/statsby_prefix.cpp for the naming and storage rules it follows.
nlohmann::ordered_json runStatsby(const std::string& expListRaw, const std::string& optsRaw,
                                  const std::string& innerCmd, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_RESAMPLING_H
