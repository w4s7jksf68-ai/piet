// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string>
#include <optional>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/eval/errors.h"
#include "graph_support.h"

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;




ojson engineErrorBlockG(const std::string& message, const std::string& code ) {
  return eval::engineError(message, code);
}


// _numeric_column: f64 columns as-is (NaN = missing); factor columns as
// their integer codes cast to double, with code<0 (missing) -> NaN.
std::vector<double> numericColumn(Datastore& ds, const std::string& name) {
  const cecon::Column& c = ds.col(name);
  int64_t n = ds.nRows();
  std::vector<double> out(static_cast<size_t>(n));
  if (ds.dtype(name) == "f64") {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = d[i];
  } else {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      out[static_cast<size_t>(i)] = codes[i] < 0 ? std::nan("") : static_cast<double>(codes[i]);
    }
  }
  return out;
}


std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key && !v.isBool) return v.strValue;
  }
  return std::nullopt;
}


std::optional<bool> optBool(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                             const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) {
      if (v.isBool) return v.boolValue;
      return true;  // a valued option is still "truthy" for a boolean check
    }
  }
  return std::nullopt;
}


// BS-052 (BETA E): `by(groupvar)` real Stata faceting (multiple small-
// multiple panels split by a grouping variable) has no renderer anywhere in
// PIET -- previously every graph command simply never read the `by` key, so
// `scatter y x, by(foreign)` silently rendered ONE ungrouped panel and threw
// the grouping away with no signal at all. Honest-reject it by name instead
// (Doctrine (1): an explicit gap, never a silently different answer) --
// shared by every bare graph-command entry point AND runTwowayLayers' global
// options below, since `by()` is always a whole-graph option, never a
// per-layer one.
std::optional<ojson> rejectByFacet(const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  for (const auto& [k, v] : opts) {
    if (k == "by") {
      return ojson(eval::unsupportedOption(
          "by()",
          "Graph faceting (splitting into multiple panels by a grouping variable) is not yet "
          "supported in PIET -- run a separate graph command per group instead."));
    }
  }
  return std::nullopt;
}


// U1 (BS-008) + U2 (BS-009): title()/subtitle()/xtitle()/ytitle()/note()
// text extraction. Stata's grammar is `title("text"[, suboptions])` --
// tokenizer.cpp's parseOpts (the shared quote/paren-aware option grammar)
// captures the WHOLE parenthesised content verbatim, so e.g. opts["title"]
// is literally `"text", suboptions` today: the unstripped quote characters
// AND any glued sub-option both reach the graph block and thence the
// exported SVG (BS-008 evidence: "corrupts ~100% of idiomatic quoted
// titles"). This is the ONE shared consumer (BS-099: "single title/axis/
// legend/name/note consumer") every title/subtitle/xtitle/ytitle/note
// callsite below routes through instead of raw optStr().
static std::string dequoteTitleText(std::string s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t");
  s = s.substr(a, b - a + 1);

  if (!s.empty() && (s.front() == '"' || s.front() == '\'')) {
    char q = s.front();
    size_t close = s.find(q, 1);
    if (close != std::string::npos) return s.substr(1, close - 1);
    // Unterminated quote (parseOpts is paren-balanced, not quote-aware) --
    // fall through to the bare-token handling below.
  }
  // Bare/unquoted value (or unterminated-quote fallback): still strip a
  // glued `, suboption(...)` tail at the first top-level comma so e.g.
  // `title(Wages, size(large))` doesn't leak `size(large)` into the text.
  size_t comma = s.find(',');
  if (comma != std::string::npos) s = s.substr(0, comma);
  a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}


// title/subtitle/xtitle/ytitle/note all share the exact same dequote +
// glued-suboption-strip rule -- one accessor for all five keys.
std::optional<std::string> optTitleStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        const std::string& key) {
  auto raw = optStr(opts, key);
  if (!raw.has_value()) return std::nullopt;
  return dequoteTitleText(*raw);
}


// Stata's name(X, replace)/name(X): take the first comma/whitespace-
// delimited token as the registered name, discarding any glued suboption
// (e.g. `replace` -- BS-057). Mirrors dispatcher.cpp's identical twoway-
// path name() split (this codebase's per-TU convention: the same small
// parsing rule copied per call site rather than factored into a shared
// header -- see e.g. nextPathG's identical per-file duplication note above).
std::string firstNameToken(const std::string& raw) {
  static const std::regex reNameSplit(R"([,\s]+)");
  std::sregex_token_iterator it(raw.begin(), raw.end(), reNameSplit, -1), endIt;
  return (it != endIt) ? it->str() : raw;
}


// A refusal with the oracle's own rc (errors.h errorBlock, reason "r(#)": the
// shape coarseRc() honours, so `display _rc` is never 1).
ojson stataRefusal(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}


// The LAST occurrence of an option: Stata's syntax keeps the last of a
// repeated option (hk_h_dupw2: width(0.5) ... width(2) gives r(width) 2).
std::optional<std::string> optLast(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const std::string& key) {
  std::optional<std::string> out;
  for (const auto& [k, v] : opts) {
    if (k == key) out = v.isBool ? std::string() : v.strValue;
  }
  return out;
}


bool hasOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key && (!v.isBool || v.boolValue)) return true;
  }
  return false;
}


std::string trimG(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}


std::vector<std::string> splitWsG(const std::string& s) {
  std::istringstream is(s);
  std::vector<std::string> out;
  std::string t;
  while (is >> t) out.push_back(t);
  return out;
}


// A whole-text number (`1.5`, `-4`, `.75`), or nullopt.
std::optional<double> parseNumberG(const std::string& raw) {
  const std::string s = trimG(raw);
  if (s.empty()) return std::nullopt;
  char* end = nullptr;
  const double d = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || !std::isfinite(d)) return std::nullopt;
  return d;
}


double storeAs(double v, bool asFloat) {
  if (!asFloat || std::isnan(v)) return v;
  return static_cast<double>(static_cast<float>(v));
}


// Stata's reldif(x, y) = |x - y| / (|y| + 1).
double reldifG(double x, double y) { return std::fabs(x - y) / (std::fabs(y) + 1.0); }


// The variable label, else the name (kdensity's x-axis title).
std::string varTitleG(SessionState& session, const std::string& v) {
  session.syncEvalVarLabels();
  std::string l = session.eval.varLabels.get(v, std::string());
  return l.empty() ? v : l;
}


std::string displayFormatOf(SessionState& session, Datastore& ds, const std::string& var) {
  if (ds.has(var)) {
    const std::vector<VarInfo> card = ds.formatCard(std::vector<std::string>{var});
    if (!card.empty()) return session.displayFormat(card.front());
  }
  VarInfo bare;
  bare.name = var;
  return session.displayFormat(bare);
}

}  // namespace piet::handlers::graph_detail
