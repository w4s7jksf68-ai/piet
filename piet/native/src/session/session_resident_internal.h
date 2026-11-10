// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private to the SessionState resident-frame files (session_engine_routing.cpp,
// session_resident_head.cpp, session_resident_card.cpp). Each function caches one
// environment read in a function-local static, so each has exactly one definition,
// in session_engine_routing.cpp.
#pragma once

#include "piet/session_state.h"

namespace piet::session_detail {
bool serveDisabled();         // PIET_NO_SERVE, read once per process
const char* routelog_path();  // PIET_ROUTELOG, read once per process; nullptr when unset
}  // namespace piet::session_detail
