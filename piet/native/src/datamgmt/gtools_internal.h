// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace gtools_detail {

// ---------------------------------------------------------------------------
// Small shared shapes.
// ---------------------------------------------------------------------------

inline ojson gtErr(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

struct ModeWord {
  std::string mode;      // "" when absent
  bool fromVarlist = false;
  size_t varlistIndex = 0;
  bool fromOpts = false;
  size_t optsIndex = 0;
  bool conflict = false;  // two DIFFERENT mode words given
};

bool parseDoubleStrict(const std::string& s, double* out);
std::vector<std::string> splitWords(const std::string& s);
ModeWord readModeWord(const eval::ParsedCommand& parsed);

}  // namespace gtools_detail

}  // namespace piet::handlers
