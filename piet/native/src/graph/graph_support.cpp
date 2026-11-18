// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <cctype>
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
#include "piet/handlers/graph.h"
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
}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;

// mlabel(varlist): see graph.h. A word is checked only when it is a plain name or
// plainly not one (starts with a digit, `%`, ...); anything carrying a wildcard,
// range, time-series or factor operator is the plot's to expand.
std::optional<ojson> mlabelVarlistRefusal(
    SessionState& session, Datastore& ds,
    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  (void)session;
  for (const auto& [k, v] : opts) {
    if (k != "mlabel" || v.isBool) continue;
    for (const std::string& w : graph_detail::splitWsG(v.strValue)) {
      if (w.find_first_of("*?~-") != std::string::npos) continue;  // wildcard / range: the plot's
      const bool nameLike = std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_';
      if (!nameLike) {  // bank: `mlabel(45)` "45 invalid name", `mla(%5.0fc)` "invalid '%'": r(198)
        return graph_detail::stataRefusal(w + " invalid name", 198);
      }
      bool allWord = true;
      for (char c : w) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) allWord = false;
      }
      if (!allWord) continue;  // a ts/factor operator or subscript: the plot expands it
      if (w.size() > 32) return graph_detail::stataRefusal(w + " invalid name", 198);
      if (ds.has(w)) continue;
      int hits = 0;  // `set varabbrev on` (Stata's default): a unique prefix names the variable
      for (const auto& n : ds.names()) {
        if (n.compare(0, w.size(), w) == 0) ++hits;
      }
      if (hits == 1) continue;
      return graph_detail::stataRefusal(
          hits == 0 ? "variable " + w + " not found" : "variable " + w + " ambiguous abbreviation",
          111);
    }
  }
  return std::nullopt;
}

// unabbreviateGraphVar: see graph.h.
std::string unabbreviateGraphVar(SessionState& session, Datastore& ds, const std::string& word) {
  if (word.empty() || ds.has(word)) return word;
  if (const std::string* off = session.eval.globals.find(" __piet_set_varabbrev"))
    if (*off == "off") return word;
  for (char c : word)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return word;
  std::string hit;
  int hits = 0;
  for (const auto& n : ds.names()) {
    if (n.size() > word.size() && n.compare(0, word.size(), word) == 0) {
      hit = n;
      ++hits;
    }
  }
  return hits == 1 ? hit : word;
}

// graphStyleWordRefusal: see graph.h. The value of msymbol / connect / pstyle / recast is a
// list of style words; a top-level comma (suboptions of an option that takes none) or a
// `word(` group is not one.
std::optional<ojson> graphStyleWordRefusal(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  for (const auto& [k, v] : opts) {
    if (v.isBool) continue;
    // predopts() goes to predict, which has no level() (bank: `twoway lfit price weight,
    // predopts(level(90))` r(198) "option level() not allowed")
    if (k == "predopts" && v.strValue.find("level(") != std::string::npos)
      return graph_detail::stataRefusal("option level() not allowed", 198);
    if (k != "msymbol" && k != "ms" && k != "connect" && k != "pstyle" && k != "recast") continue;
    const std::string& t = v.strValue;
    if (t.find('(') != std::string::npos) {
      std::string sub;  // the first `word(...)` group, as Stata prints the refused sub-option
      size_t open = t.find('(');
      size_t b = open;
      while (b > 0 && (std::isalnum(static_cast<unsigned char>(t[b - 1])) || t[b - 1] == '_')) --b;
      int depth = 0;
      size_t e = open;
      for (; e < t.size(); ++e) {
        if (t[e] == '(') ++depth;
        if (t[e] == ')' && --depth == 0) break;
      }
      sub = t.substr(b, (e < t.size() ? e + 1 : t.size()) - b);
      return graph_detail::stataRefusal("option(s) " + sub + " not allowed", 198);
    }
    if (t.find(',') != std::string::npos) return graph_detail::stataRefusal("invalid 'editcopy'", 198);
  }
  return std::nullopt;
}

// The plot types that do NOT own each of the four options. Evidence, all StataBE 19.5
// (vs-stata bank 2026-11-10 unless noted): horizontal is refused on scatter, line,
// connected and the lfit/qfit family; barwidth on scatter, line, rcap, rspike (and the
// plot types that share their class: connected, the fit family, kdensity, function,
// rarea, rcapsym, rline, rscatter, rconnected); base on scatter, line, rarea,
// histogram (+ the same class siblings); range on scatter, line, histogram (+ the
// range-less range plots). spike/area/dropline/bar/dot own barwidth, base and
// horizontal; the fit family, function and kdensity own range; histogram owns
// horizontal and barwidth.
bool plotTypeRefusesOption(const std::string& plotType, const std::string& key) {
  std::string t;
  for (char c : plotType) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (t == "hist") t = "histogram";
  const bool plain = t == "scatter" || t == "connected" || t == "line";
  const bool fit = t == "lfit" || t == "qfit" || t == "lfitci" || t == "qfitci";
  const bool rangePlot = t == "rarea" || t == "rcap" || t == "rcapsym" || t == "rspike" ||
                         t == "rline" || t == "rscatter" || t == "rconnected";
  // level(), stdp, stdf, stdr and nofit belong to the confidence-band plots (lfitci, qfitci)
  // alone (bank: `twoway lfit price weight, level(90)` r(198) "option level() not allowed")
  if (key == "level" || key == "stdp" || key == "stdf" || key == "stdr" || key == "nofit")
    return t == "lfit" || t == "qfit";
  if (key == "horizontal") return plain || fit;
  if (key == "barwidth") return plain || fit || rangePlot || t == "kdensity" || t == "function";
  if (key == "base") return plain || fit || rangePlot || t == "kdensity" || t == "histogram";
  if (key == "range") return plain || rangePlot || t == "histogram";
  return false;
}

std::string plotTypeOwnSpelling(const std::string& plotType, const std::string& key) {
  std::string t;
  for (char c : plotType) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const bool fit = t == "lfit" || t == "qfit" || t == "lfitci" || t == "qfitci";
  if (fit && key == "r") return "range";
  if ((t == "lfitci" || t == "qfitci") && key == "l") return "level";
  return std::string();
}

}  // namespace piet::handlers

namespace piet::handlers::graph_detail {

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
