// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "vendor/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "gtools_internal.h"

namespace piet::handlers {

namespace gtools_detail {

// Whole-string numeric parse. A trailing-garbage value such as cuts(5x 95) must
// be REJECTED, not silently truncated to 5 (std::strtod's own behaviour) --
// a truncated cut is a different winsorization with no diagnostic. Same
// discipline the winsor2 arm already applies to its own cuts().
bool parseDoubleStrict(const std::string& s, double* out) {
  if (s.empty()) return false;
  const char* begin = s.c_str();
  char* end = nullptr;
  const double v = std::strtod(begin, &end);
  if (end == begin) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  if (!std::isfinite(v)) return false;
  *out = v;
  return true;
}

std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

// ---------------------------------------------------------------------------
// gquantiles mode word.
// ---------------------------------------------------------------------------

namespace {

bool isModeWord(const std::string& lowered) {
  return lowered == "xtile" || lowered == "_pctile" || lowered == "pctile";
}

}  // namespace

// The mode word can arrive in either half of the line, and both spellings are
// real gtools usage, so both are scanned. In `gquantiles x, _pctile nq(4)` the
// tokenizer splits at the comma and the mode lands in parsed.opts as a bare
// flag; a caller who writes it before the comma leaves it in parsed.varlist.
// Reading only one of the two would leave the other silently unrecognised, and
// "unrecognised mode" here means the wrong one of three different commands.
ModeWord readModeWord(const eval::ParsedCommand& parsed) {
  ModeWord out;
  for (size_t i = 0; i < parsed.varlist.size(); ++i) {
    const std::string low = strutil::lower(parsed.varlist[i]);
    if (!isModeWord(low)) continue;
    out.mode = low;
    out.fromVarlist = true;
    out.varlistIndex = i;
    break;
  }
  for (size_t i = 0; i < parsed.opts.size(); ++i) {
    const std::string low = strutil::lower(parsed.opts[i].first);
    if (!parsed.opts[i].second.isBool || !isModeWord(low)) continue;
    if (!out.mode.empty() && out.mode != low) out.conflict = true;
    if (out.mode.empty()) out.mode = low;
    out.fromOpts = true;
    out.optsIndex = i;
    break;
  }
  return out;
}

}  // namespace gtools_detail

}  // namespace piet::handlers
