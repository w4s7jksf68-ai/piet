// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// Helpers shared by variable_bulk_rename.cpp (`tolower`, `renvars`) and
// variable_move.cpp (`move`).
namespace variable_rename_move_detail {

inline ojson k1Error(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules cannot
// recognise. `reason` is the only channel carrying an exact rc (BUG-187) and it
// outranks the coarse mapping. Every rc below is a measured one.
inline ojson k1ErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

inline ojson silent() { return ojson::array(); }

// The columns, from the session, without forcing a Datastore open when the
// resident shadow card can answer. Both commands here only ever need NAMES.
inline bool columnNames(SessionState& session, std::vector<std::string>* out, ojson* err) {
  Datastore* dsp = nullptr;
  try {
    dsp = session.datastore();
  } catch (const std::exception& e) {
    *err = k1Error(std::string("K1: ") + e.what(), "data_io");
    return false;
  }
  if (dsp == nullptr) {
    *err = ojson(eval::noDataLoaded());
    return false;
  }
  *out = dsp->names();
  return true;
}

}  // namespace variable_rename_move_detail

}  // namespace piet::handlers
