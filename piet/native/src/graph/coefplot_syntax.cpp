// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from coefplot 1.8.8 (MIT).
// See THIRD_PARTY_NOTICES.
//
// coefplot_syntax.cpp -- coefplot 1.8.8's command grammar:
// paren / quote aware scanning, the option table and its abbreviations, and the
// subgraph / plot / model grammar. The data pipeline is graph/coefplot.cpp.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <tuple>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/serset.h"
#include "piet/strutil.h"
#include "io/win_compat.h"

#include "coefplot_syntax.h"

namespace piet::handlers {

using namespace coefplot_detail;

namespace {

std::string lowerC(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

namespace coefplot_detail {

// ---------------------------------------------------------------------------
// text scanning (paren / quote aware)

// Index just past the closing quote of a quote starting at i ("..." or
// `"..."', nested compound quotes counted).
size_t skipQuote(const std::string& s, size_t i) {
  if (s[i] == '"') {
    const size_t c = s.find('"', i + 1);
    return c == std::string::npos ? s.size() : c + 1;
  }
  // compound
  int depth = 0;
  size_t j = i;
  while (j < s.size()) {
    if (s.compare(j, 2, "`\"") == 0) {
      ++depth;
      j += 2;
      continue;
    }
    if (s.compare(j, 2, "\"'") == 0) {
      --depth;
      j += 2;
      if (depth == 0) return j;
      continue;
    }
    ++j;
  }
  return s.size();
}

bool quoteStart(const std::string& s, size_t i) {
  return s[i] == '"' || s.compare(i, 2, "`\"") == 0;
}

}  // namespace coefplot_detail

namespace {

// Splits on a top-level separator string (outside parens/brackets/quotes).
std::vector<std::string> splitTop(const std::string& s, const std::string& sep) {
  std::vector<std::string> out;
  int depth = 0;
  size_t start = 0, i = 0;
  while (i < s.size()) {
    if (quoteStart(s, i)) {
      i = skipQuote(s, i);
      continue;
    }
    const char c = s[i];
    if (c == '(' || c == '[') ++depth;
    else if ((c == ')' || c == ']') && depth > 0) --depth;
    if (depth == 0 && s.compare(i, sep.size(), sep) == 0) {
      out.push_back(s.substr(start, i - start));
      i += sep.size();
      start = i;
      continue;
    }
    ++i;
  }
  out.push_back(s.substr(start));
  return out;
}

}  // namespace

namespace coefplot_detail {

// `_parse comma lhs rhs`: the text before the first top-level comma, and the
// rest without the comma.
std::pair<std::string, std::string> parseComma(const std::string& s) {
  int depth = 0;
  size_t i = 0;
  while (i < s.size()) {
    if (quoteStart(s, i)) {
      i = skipQuote(s, i);
      continue;
    }
    const char c = s[i];
    if (c == '(' || c == '[') ++depth;
    else if ((c == ')' || c == ']') && depth > 0) --depth;
    else if (c == ',' && depth == 0) return {s.substr(0, i), s.substr(i + 1)};
    ++i;
  }
  return {s, std::string()};
}

std::vector<Tok> tokensParen(const std::string& s) {
  std::vector<Tok> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    if (s[i] == '(') {
      int depth = 0;
      size_t j = i;
      while (j < s.size()) {
        if (quoteStart(s, j)) {
          j = skipQuote(s, j);
          continue;
        }
        if (s[j] == '(') ++depth;
        else if (s[j] == ')' && --depth == 0) break;
        ++j;
      }
      out.push_back({s.substr(i + 1, (j < s.size() ? j : s.size()) - i - 1), true});
      i = j < s.size() ? j + 1 : s.size();
      continue;
    }
    size_t j = i;
    int depth = 0;
    while (j < s.size()) {
      if (quoteStart(s, j)) {
        j = skipQuote(s, j);
        continue;
      }
      if (s[j] == '(' || s[j] == '[') ++depth;
      else if ((s[j] == ')' || s[j] == ']') && depth > 0) --depth;
      else if (depth == 0 && std::isspace(static_cast<unsigned char>(s[j]))) break;
      ++j;
    }
    out.push_back({s.substr(i, j - i), false});
    i = j;
  }
  return out;
}

// ---------------------------------------------------------------------------
// options

// `name` or `name(arg)`, a blank allowed before the parenthesis (cf_keep_sp
// `keep (x1 x2 )`).
std::vector<Opt> parseOptList(const std::string& s) {
  std::vector<Opt> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
    if (j == i) {  // stray character
      if (quoteStart(s, i)) i = skipQuote(s, i);
      else ++i;
      continue;
    }
    Opt o;
    o.name = lowerC(s.substr(i, j - i));
    size_t k = j;
    while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
    if (k < s.size() && s[k] == '(') {
      int depth = 0;
      size_t m = k;
      while (m < s.size()) {
        if (quoteStart(s, m)) {
          m = skipQuote(s, m);
          continue;
        }
        if (s[m] == '(') ++depth;
        else if (s[m] == ')' && --depth == 0) break;
        ++m;
      }
      o.hasArg = true;
      o.arg = s.substr(k + 1, (m < s.size() ? m : s.size()) - k - 1);
      i = m < s.size() ? m + 1 : s.size();
    } else {
      i = j;
    }
    out.push_back(o);
  }
  return out;
}

}  // namespace coefplot_detail

namespace {

enum class OptKind { kGlobal, kModel, kPlot, kSubgr };
struct OptDef {
  const char* full;
  size_t minLen;
  OptKind kind;
  int arg;  // 0 either, 1 bare only, 2 argument only
};

const std::vector<OptDef>& optDefs() {
  static const std::vector<OptDef> d = {
      // global (parse_globalopts :725-765)
      {"horizontal", 3, OptKind::kGlobal, 1},   {"vertical", 4, OptKind::kGlobal, 1},
      {"sort", 4, OptKind::kGlobal, 0},         {"orderby", 7, OptKind::kGlobal, 2},
      {"order", 5, OptKind::kGlobal, 2},        {"bycoefs", 3, OptKind::kGlobal, 1},
      {"norecycle", 5, OptKind::kGlobal, 1},    {"grid", 4, OptKind::kGlobal, 2},
      {"nooffsets", 5, OptKind::kGlobal, 1},    {"format", 6, OptKind::kGlobal, 2},
      {"nolabels", 5, OptKind::kGlobal, 1},     {"coeflabels", 5, OptKind::kGlobal, 2},
      {"noeqlabels", 7, OptKind::kGlobal, 1},   {"eqlabels", 3, OptKind::kGlobal, 2},
      {"eqstrict", 8, OptKind::kGlobal, 1},     {"headings", 4, OptKind::kGlobal, 2},
      {"groups", 5, OptKind::kGlobal, 2},       {"plotlabels", 5, OptKind::kGlobal, 2},
      {"bylabels", 8, OptKind::kGlobal, 2},     {"generate", 3, OptKind::kGlobal, 0},
      {"relocate", 5, OptKind::kGlobal, 2},     {"replace", 7, OptKind::kGlobal, 1},
      {"addplot", 7, OptKind::kGlobal, 2},      {"nodrop", 6, OptKind::kGlobal, 1},
      {"legend", 3, OptKind::kGlobal, 2},       {"byopts", 4, OptKind::kGlobal, 2},
      {"pcycle", 4, OptKind::kGlobal, 2},
      // model (_merge_modelopts :1507-1531)
      {"omitted", 4, OptKind::kModel, 1},       {"baselevels", 4, OptKind::kModel, 1},
      {"bname", 1, OptKind::kModel, 2},         {"atname", 2, OptKind::kModel, 0},
      {"swapnames", 4, OptKind::kModel, 1},     {"keep", 4, OptKind::kModel, 2},
      {"drop", 4, OptKind::kModel, 2},          {"rename", 6, OptKind::kModel, 2},
      {"eqrename", 5, OptKind::kModel, 2},      {"asequation", 4, OptKind::kModel, 0},
      {"eform", 5, OptKind::kModel, 0},         {"rescale", 7, OptKind::kModel, 2},
      {"transform", 5, OptKind::kModel, 2},     {"noci", 4, OptKind::kModel, 1},
      {"levels", 1, OptKind::kModel, 2},        {"ciname", 2, OptKind::kModel, 2},
      {"vname", 1, OptKind::kModel, 2},         {"sename", 2, OptKind::kModel, 2},
      {"dfname", 2, OptKind::kModel, 2},        {"citype", 6, OptKind::kModel, 2},
      {"aux", 3, OptKind::kModel, 2},
      // plot (_merge_plotopts :1269-1286)
      {"label", 3, OptKind::kPlot, 2},          {"offset", 6, OptKind::kPlot, 2},
      {"pstyle", 4, OptKind::kPlot, 2},         {"axis", 2, OptKind::kPlot, 2},
      {"recast", 6, OptKind::kPlot, 2},         {"mlabels", 2, OptKind::kPlot, 1},
      {"mlabel", 2, OptKind::kPlot, 2},         {"mlabvposition", 6, OptKind::kPlot, 2},
      {"cionly", 6, OptKind::kPlot, 1},         {"citop", 5, OptKind::kPlot, 1},
      {"cismooths", 3, OptKind::kPlot, 1},      {"cismooth", 3, OptKind::kPlot, 2},
      {"ciopts", 4, OptKind::kPlot, 2},         {"cirecast", 4, OptKind::kPlot, 2},
      {"ifopt", 2, OptKind::kPlot, 2},          {"weightopt", 1, OptKind::kPlot, 2},
      {"nokey", 5, OptKind::kPlot, 1},          {"key", 3, OptKind::kPlot, 0},
      // subgraph
      {"bylabel", 5, OptKind::kSubgr, 2},
  };
  return d;
}

// The canonical option an option word names, or nullptr (a twoway option).
const OptDef* resolveOpt(const Opt& o) {
  for (const OptDef& d : optDefs()) {
    if (o.name == d.full && (d.arg == 0 || (d.arg == 1) != o.hasArg)) return &d;
  }
  for (const OptDef& d : optDefs()) {
    if (d.arg == 1 && o.hasArg) continue;
    if (d.arg == 2 && !o.hasArg) continue;
    const std::string f(d.full);
    if (o.name.size() >= d.minLen && o.name.size() <= f.size() && f.compare(0, o.name.size(), o.name) == 0)
      return &d;
  }
  return nullptr;
}

}  // namespace

namespace coefplot_detail {

OptMap optMapOf(const std::string& text) {
  OptMap om;
  for (const Opt& o : parseOptList(text)) {
    const OptDef* d = resolveOpt(o);
    if (d == nullptr) {
      om.twoway.push_back(o);
      continue;
    }
    om.m[d->full] = o;
  }
  return om;
}

}  // namespace coefplot_detail

namespace {

// ---------------------------------------------------------------------------
// the plot/model grammar

// `estimates dir pattern`: the stored names matching, in store order; 111
// when none.
std::optional<CpError> expandEstNames(SessionState& session, const std::string& pat,
                                      std::vector<std::string>* out) {
  bool any = false;
  for (const auto& kv : session.estimates) {
    if (strutil::globMatch(pat, kv.first)) {
      out->push_back(kv.first);
      any = true;
    }
  }
  if (!any) return CpError{refuseCp("estimation result " + pat + " not found", 111)};
  return std::nullopt;
}

// parse_models (:1404-1451) on the text inside one plot's parentheses (or a
// bare model word).
std::optional<CpError> parseModels(SessionState& session, const std::string& text, PlotSpec2* plot) {
  const std::vector<std::string> parts = splitTop(text, "\\");
  for (size_t p = 0; p < parts.size(); ++p) {
    auto [model, opts] = parseComma(parts[p]);
    model = trimC(model);
    const bool last = p + 1 == parts.size();
    if (last) plot->opts = optMapOf(opts);
    if (model.empty() && (last ? true : trimC(opts).empty())) {
      if (!last || plot->models.empty()) {
        ModelSpec ms;
        ms.name = ".";
        if (!last) ms.opts = optMapOf(opts);
        plot->models.push_back(ms);
      }
      continue;
    }
    if (model.find('(') != std::string::npos) {
      // matrix(name[...])
      ModelSpec ms;
      const size_t open = model.find('(');
      const std::string head = lowerC(trimC(model.substr(0, open)));
      const size_t close = model.rfind(')');
      const std::string inner =
          close == std::string::npos ? model.substr(open + 1) : model.substr(open + 1, close - open - 1);
      if (head.empty() || std::string("matrix").compare(0, head.size(), head) != 0) {
        return CpError{refuseCp(model + " not allowed", 198)};
      }
      ms.matrix = trimC(inner);
      size_t e = 0;
      while (e < ms.matrix.size() && ms.matrix[e] != '[' && !std::isspace(static_cast<unsigned char>(ms.matrix[e])))
        ++e;
      ms.name = ms.matrix.substr(0, e);
      if (!last) ms.opts = optMapOf(opts);
      plot->models.push_back(ms);
      continue;
    }
    std::istringstream is(model.empty() ? std::string(".") : model);
    std::string w;
    while (is >> w) {
      std::vector<std::string> names;
      if (w.find('*') != std::string::npos || w.find('?') != std::string::npos) {
        if (auto e = expandEstNames(session, w, &names)) return e;
      } else {
        names.push_back(w);
      }
      for (const auto& n : names) {
        ModelSpec ms;
        ms.name = n;
        if (!last) ms.opts = optMapOf(opts);
        plot->models.push_back(ms);
      }
    }
  }
  if (plot->models.empty()) {
    ModelSpec ms;
    ms.name = ".";
    plot->models.push_back(ms);
  }
  if (plot->models.size() == 1 && plot->models[0].name == "_skip" && plot->models[0].matrix.empty())
    plot->skip = true;
  return std::nullopt;
}

}  // namespace

namespace coefplot_detail {

// parse_plots (:1172-1205).
std::optional<CpError> parsePlots(SessionState& session, const std::string& text, SubgrSpec* sg) {
  auto [plotsText, opts] = parseComma(text);
  sg->opts = optMapOf(opts);
  for (const Tok& t : tokensParen(plotsText)) {
    if (t.paren) {
      PlotSpec2 p;
      if (auto e = parseModels(session, t.text, &p)) return e;
      sg->plots.push_back(std::move(p));
      continue;
    }
    std::vector<std::string> names;
    if (t.text.find('(') == std::string::npos &&
        (t.text.find('*') != std::string::npos || t.text.find('?') != std::string::npos)) {
      if (auto e = expandEstNames(session, t.text, &names)) return e;
    } else {
      names.push_back(t.text);
    }
    for (const auto& n : names) {
      PlotSpec2 p;
      if (auto e = parseModels(session, n, &p)) return e;
      sg->plots.push_back(std::move(p));
    }
  }
  if (sg->plots.empty()) {
    PlotSpec2 p;
    ModelSpec ms;
    ms.name = ".";
    p.models.push_back(ms);
    sg->plots.push_back(p);
  }
  return std::nullopt;
}

// parse_subgraphs (:693-723): subgraphs split at `||`; the global options are
// the comma part of the LAST piece.
void parseSubgraphs(const std::string& args, std::vector<std::string>* subgr, std::string* globalOpts) {
  std::vector<std::string> pieces = splitTop(args, "||");
  bool empty = true;
  for (size_t i = 0; i < pieces.size(); ++i) {
    std::string piece = pieces[i];
    const bool last = i + 1 == pieces.size();
    if (i > 0) {
      // a `||` precedes this piece
      if (empty) subgr->push_back(".");
      empty = true;
    }
    if (last) {
      auto [lhs, rhs] = parseComma(piece);
      *globalOpts = rhs;
      piece = lhs;
    }
    if (!trimC(piece).empty()) {
      empty = false;
      subgr->push_back(piece);
    }
  }
  // the `||` handling above adds "." for an empty piece BETWEEN separators; the
  // first piece's emptiness is decided at the first `||` (parse_subgraphs adds
  // "." there too) -- undo the extra one for a leading `||`-less empty start.
  if (subgr->empty()) subgr->push_back(".");
}

}  // namespace coefplot_detail

}  // namespace piet::handlers
