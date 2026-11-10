// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "piet/cclass.h"             // substituteCRefs: the dispatcher's own pre-pass for a definition line
#include "piet/datastore.h"
#include "piet/dispatcher.h"         // markout: a dispatched `quietly replace`
#include "piet/display_hardening.h"  // fmtStataDefaultG: what `display <number>` prints
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"        // hasMacroRefs, tryCaptureDefinition
#include "piet/eval/pyvalue.h"       // pythonFloatRepr: a round-trippable number for $S_3/$S_6/$S_7
#include "piet/eval/session.h"       // replaceRClass
#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/session_state.h"
#include "piet/stata_format.h"       // renderValue/padToField: the one %w.pf renderer
#include "piet/strutil.h"
#include "command_reply_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace command_reply_detail {

// ===========================================================================
// Shared helpers
// ===========================================================================

// An error whose Stata rc is known. `reason` "r(NNN)" is the exact-code channel
// eval::coarseRc honours; 198 also carries the syntax_error code so the coarse
// map agrees with it.
ojson envError(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array()) {
    for (const auto& b : result)
      if (isErrorBlock(b)) return &b;
  }
  return nullptr;
}

std::string joinWith(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  for (const auto& [k, v] : opts)
    if (k == key && !v.isBool) return v.strValue;
  return std::nullopt;
}

bool hasOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
            const std::string& key) {
  for (const auto& [k, v] : opts) {
    (void)v;
    if (k == key) return true;
  }
  return false;
}

std::optional<double> parseNumber(const std::string& sIn) {
  const std::string s = strutil::trim(sIn);
  if (s.empty()) return std::nullopt;
  try {
    size_t used = 0;
    const double v = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// A `%w.pf` / `%w.0fc` rendering through the one renderer, padded the way
// `display` pads (padToField). NaN prints Stata's `.`.
std::string fixedFmt(double v, int width, int prec, bool group) {
  fmt::StataFmtSpec sp;
  sp.hasWidth = true;
  sp.width = width;
  sp.prec = prec;
  sp.conv = 'f';
  sp.groupThousands = group;
  if (std::isnan(v)) return fmt::padToField(".", sp);
  const std::optional<std::string> r = fmt::renderValue(v, sp);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  return fmt::padToField(r.value_or(std::string(buf)), sp);
}

}  // namespace command_reply_detail

}  // namespace piet::handlers
