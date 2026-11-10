// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Thin forwarders shared by the command handlers (dataset_use, data_generate, summarize, assert_command,
// estimation_sample_install/_carry, linear_estimation, mean_proportion_estimation).
// Promoted VERBATIM from the anonymous namespace those handlers used to share, modulo inline linkage.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace engine_call_detail {

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `lower(...)` call sites
// didn't need touching. Pure dedup, no behavior change.
inline std::string lower(std::string s) { return strutil::lower(std::move(s)); }

inline std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

inline ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// cobra_runner.py's `CobraError.__str__`: f"[{code}] {message}" — the
// dispatcher's universal `except CobraError as ce: ...(str(ce), code=ce.code)`
// pattern relies on this bracket-prefixed message (errors.py's error_block
// then coerces `code` to "engine_error" when it's not in the frozen
// ERROR_CODES allow-list, but the [code] prefix survives in the message
// either way — see e.g. the `[ts_no_tsset]` carve-out cells). Every
// CobraError catch site in this file must format its message through this
// helper, not ce.what() alone, to stay byte-identical with the oracle.
//
// BUG-038(a): the "[code] message" formatting itself now lives at ONE
// shared mapping point (piet::cobraErrorMessage, cobra_runner.h) that every
// such formatter across the codebase forwards to — this function stays a
// thin wrapper so its call sites in this file didn't need touching. The
// only BEHAVIOR change is cobra's `internal` code (its generic C++-
// exception catch-all, e.g. an unimplemented expression builtin): that one
// code no longer surfaces its raw "[internal] ..." shape verbatim (see
// cobra_runner.h's header comment for the two honest rewrites); every other
// code's bracket-prefixed format, and thus oracle byte-parity, is
// unchanged.
inline std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

}  // namespace engine_call_detail

}  // namespace piet::handlers
