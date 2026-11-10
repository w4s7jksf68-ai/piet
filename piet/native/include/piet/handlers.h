// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// M2 vertical-slice command handlers — C++ ports of the corresponding
// native_handler branches in piet/backend/piet/dispatcher.py's _run_native()
// plus the cobra-estimator path (dispatch()'s "4b. Cobra estimator" section)
// for `regress`/`reg`/`feols`.
//
// Scope (ROADMAP M2): use, gen, replace, describe, summarize, regress. Each
// handler mirrors the oracle's behavior BUG-FOR-BUG (see use's BUG-023 note)
// and returns a single ResultBlock (nlohmann::ordered_json object) — the
// caller (dispatcher.cpp) appends it after the echo block, exactly like
// Python's `blocks.append(result_block)`.
//
// Handlers that hit a case outside the vertical slice (string-typed gen,
// bysort _n/_N gen, an if-filtered describe, …) return an explicit
// unsupported_* error block rather than attempting a partial port — M3 is
// where those land (see each handler's own comment for the exact carve-out).
#ifndef PIET_NATIVE_HANDLERS_H
#define PIET_NATIVE_HANDLERS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "piet/twoway_parse.h"
#include "vendor/nlohmann/json.hpp"

#include "piet/handlers/data_combine.h"
#include "piet/handlers/data_values.h"
#include "piet/handlers/descriptive.h"
#include "piet/handlers/estimates_tables.h"
#include "piet/handlers/estimation.h"
#include "piet/handlers/graph.h"
#include "piet/handlers/gtools_aliases.h"
#include "piet/handlers/io.h"
#include "piet/handlers/observations.h"
#include "piet/handlers/postestimation.h"
#include "piet/handlers/resampling.h"
#include "piet/handlers/results.h"
#include "piet/handlers/session_commands.h"
#include "piet/handlers/time_panel.h"
#include "piet/handlers/utility.h"
#include "piet/handlers/variables.h"
#include "piet/handlers/vce_types.h"
#include "piet/user_paths.h"

#endif  // PIET_NATIVE_HANDLERS_H
