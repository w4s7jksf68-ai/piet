// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from coefplot 1.8.8 (MIT).
// See THIRD_PARTY_NOTICES.
//
// coefplot_syntax.h -- private to graph/coefplot_syntax.cpp and graph/coefplot.cpp: the
// coefplot grammar's shared types and entry points.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace coefplot_detail {

inline ojson refuseCp(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

// A refusal carried out of the pipeline.
struct CpError {
  ojson block;
};

inline std::string trimC(const std::string& s) { return strutil::trim(s); }

// Tokens: a parenthesised group is one token (with `paren` set, the parens
// removed); otherwise blank-separated words (a word may hold parens).
struct Tok {
  std::string text;
  bool paren = false;
};

struct Opt {
  std::string name;  // as typed, lower case
  bool hasArg = false;
  std::string arg;   // the text inside the parentheses
};

// One level's options by canonical name (the last spelling wins).
struct OptMap {
  std::map<std::string, Opt> m;
  std::vector<Opt> twoway;
  bool has(const std::string& k) const { return m.count(k) != 0; }
  const Opt* get(const std::string& k) const {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
  }
};

struct ModelSpec {
  std::string name;    // estimates name, "." or the matrix() model name
  std::string matrix;  // matrix(...) argument, "" = e() mode
  OptMap opts;         // the model's own options (not the last model's)
};
struct PlotSpec2 {
  std::vector<ModelSpec> models;
  OptMap opts;  // plot-level options (the last model's comma part)
  bool skip = false;
};
struct SubgrSpec {
  std::vector<PlotSpec2> plots;
  OptMap opts;
};

// Defined in coefplot_syntax.cpp.
size_t skipQuote(const std::string& s, size_t i);
bool quoteStart(const std::string& s, size_t i);
std::pair<std::string, std::string> parseComma(const std::string& s);
std::vector<Tok> tokensParen(const std::string& s);
std::vector<Opt> parseOptList(const std::string& s);
OptMap optMapOf(const std::string& text);
std::optional<CpError> parsePlots(SessionState& session, const std::string& text, SubgrSpec* sg);
void parseSubgraphs(const std::string& args, std::vector<std::string>* subgr, std::string* globalOpts);

}  // namespace coefplot_detail

}  // namespace piet::handlers
