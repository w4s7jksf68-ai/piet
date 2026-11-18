// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/display_hardening.h"
#include "piet/handlers/graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle -- the ONE layer-style rule
#include "piet/twoway_parse.h"
#include "graph_extension_internal.h"

namespace {

// grc1leg2. `legendfrom` picks the surviving legend; rows/cols/title/imargin/
// graphregion/name go straight through to `runGraphCombine`, which reads every
// one of them; `xcommon`/`ycommon` are implemented here; `saving` is honoured
// through `runGraphSave`; the six placement options are read to build the
// accepted-and-not-applied note. `holes()` (a grid with gaps) and `altshrink`
// (a different panel-shrink rule) would both change the drawn layout, and this
// build has neither, so they are refused by name rather than dropped.
// AER-SCOPE2 K1: `grc1leg` (Vince Wiggins' original) now routes here too, and
// the 12-paper corpus measures ITS option usage as caption(17) graphregion(24)
// imargin(21) iscale(19) legendfrom(2) note(18) position(19) row(1) rows(1)
// ycommon(22). Four of those ten -- caption, iscale, note, row -- were absent
// from the set below, and an absent option is r(198) uncaptured, which STOPS the
// do-file and shows up as a `stopped_at` RED in run_probe. They are added to the
// KNOWN set because they are layout/annotation options the combine path already
// carries or can ignore without changing a number; `row` is Stata's singular
// spelling of `rows`.
const piet::eval::OptVocab& kVocabGrc1leg = piet::eval::registerOptVocab(
    {"grc1leg2", "grc1leg"},
                  {{"legendfrom", "rows", "row", "cols", "title", "xcommon", "ycommon", "imargin",
                    "graphregion", "name", "saving", "ring", "position", "span", "symxsize",
                    "symysize", "labsize", "caption", "note", "iscale"},
                   {"holes", "altshrink"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace graph_extension_detail;

// ===========================================================================
// grc1leg2
// ===========================================================================
//
// Design decision 1: build the ordinary `graph combine` block, then switch off
// every panel's legend except the one `legendfrom()` names (default: the first
// panel). `piet/frontend/js/graph.js:795` already checks `legend.off` before it
// draws a legend, so this needs no frontend change -- which is the entire
// reason the alternative (merging the panels' legends into one combined legend
// the way real grc1leg does) was rejected: that is a renderer change, and the
// renderer is outside this bundle.
//
// The visible difference from real grc1leg: the surviving legend stays INSIDE
// its own panel rather than moving to the combined graph's margin, so
// ring()/position()/span() and the symbol-size options cannot be honoured.
// They are read and named in a note rather than dropped.
// The legend keys of a twoway graph read from the statement that drew it (a layer of the block does not record how
// many y variables its plot had): one per y variable of a scatter / line / connected / area / bar / spike / dropline
// plot, one per fitted line or function. nullopt for any other plot type or a statement that is not a twoway.
static std::optional<size_t> twowayKeysFromStatement(const std::string& command) {
  std::string rest = strutil::trim(command);
  size_t e = 0;
  while (e < rest.size() && !std::isspace(static_cast<unsigned char>(rest[e])) && rest[e] != '(') ++e;
  const std::string w0 = strutil::lower(rest.substr(0, e));
  if (w0.size() < 2 || std::string("twoway").compare(0, w0.size(), w0) != 0) return std::nullopt;
  rest = strutil::trim(rest.substr(e));
  if (rest.empty()) return std::nullopt;
  if (!twoway::hasToplevelParens(rest)) rest = twoway::expandDoublePipes(rest);
  if (!twoway::hasToplevelParens(rest)) {
    // one bare plot: its options follow its comma, so wrap it as a single layer
    const long long comma = eval::findUnquotedComma(rest);
    rest = "(" + (comma < 0 ? rest : rest.substr(0, static_cast<size_t>(comma))) + ")";
  }
  const twoway::ParseResult pr = twoway::parseTwoway(rest);
  if (pr.layers.empty()) return std::nullopt;
  static const char* kMultiY[] = {"scatter", "line", "connected", "area", "bar", "spike", "dropline"};
  static const char* kOneKey[] = {"lfit", "qfit", "lowess", "function"};
  size_t keys = 0;
  for (const auto& l : pr.layers) {
    const std::string t = strutil::lower(l.plottype);
    bool multi = false, one = false;
    for (const char* k : kMultiY)
      if (t.size() >= 2 && std::string(k).compare(0, t.size(), t) == 0) multi = true;
    for (const char* k : kOneKey)
      if (t.size() >= 3 && std::string(k).compare(0, t.size(), t) == 0) one = true;
    if (multi) keys += l.varlist.size() > 2 ? l.varlist.size() - 1 : 1;
    else if (one) keys += 1;
    else return std::nullopt;
  }
  return keys;
}

// The number of legend keys a drawn graph has: the series it draws. nullopt when this block's series are not
// listed (a by-graph or a combine: each panel has its own legend). `command` is the statement that drew it.
static std::optional<size_t> legendKeyCount(const ojson& p, const std::string& command) {
  if (!p.is_object()) return std::nullopt;
  const std::string kind = p.value("kind", std::string());
  auto distinctSeries = [](const ojson& arr) {
    std::set<std::string> names;
    for (const auto& e : arr)
      if (e.is_object()) names.insert(e.value("series", std::string()));
    return names.size();
  };
  if (kind == "twoway") {
    if (!p.contains("layers") || !p["layers"].is_array()) return std::nullopt;
    // two plots draw at least two keys; one plot may hold several series, which only the statement tells
    if (p["layers"].size() >= 2) return p["layers"].size();
    return twowayKeysFromStatement(command);
  }
  if (kind == "scatter") {
    if (p.contains("series") && p["series"].is_array() && !p["series"].empty()) return p["series"].size();
    return size_t{1};  // one y variable: one series
  }
  if (kind == "tsline") {
    if (p.contains("series") && p["series"].is_array()) return p["series"].size();
    return std::nullopt;
  }
  if (kind == "bar") {
    if (p.contains("bars") && p["bars"].is_array()) return distinctSeries(p["bars"]);
    if (p.contains("boxes") && p["boxes"].is_array()) return distinctSeries(p["boxes"]);
  }
  return std::nullopt;
}

ojson runGrc1leg(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "grc1leg2";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGrc1leg.known,
                                            kVocabGrc1leg.unimplemented))
    return ojson(*err);
  if (parsed.varlist.empty())
    return gxSyntax(kCmd + ": syntax is `" + kCmd +
                    " name1 name2 [...] [, legendfrom(name) rows(#) cols(#) title(...)]`");

  ojson result = ojson(runGraphCombine(parsed.varlist, parsed.opts, session));
  if (isErrorBlock(result)) return renameDelegateError(result, kCmd, "graph combine");
  if (!result.is_object() || !result.contains("panels") || !result["panels"].is_array()) {
    // Defensive: runGraphCombine returns either an error or a combine block.
    return gxError(kCmd + ": internal — `graph combine` returned no panels.", "internal");
  }

  ojson& panels = result["panels"];
  const size_t nPanels = panels.size();

  // Which panel keeps its legend. Default = the first, per the plan.
  size_t keepIdx = 0;
  if (auto lf = optStr(parsed.opts, "legendfrom")) {
    const std::string want = trimOf(dequote(trimOf(*lf)));
    bool found = false;
    for (size_t i = 0; i < parsed.varlist.size() && i < nPanels; ++i) {
      const std::string tok = trimOf(dequote(trimOf(parsed.varlist[i])));
      if (tok == want) {
        keepIdx = i;
        found = true;
        break;
      }
    }
    if (!found) {
      // A panel may also be identified by the name stored INSIDE its block
      // (`graph save`'s registered name), which is not always the token typed.
      for (size_t i = 0; i < nPanels; ++i) {
        if (panels[i].is_object() && panels[i].value("name", std::string()) == want) {
          keepIdx = i;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      return gxError(kCmd + ": legendfrom() names `" + want +
                     "', which is not one of the combined graphs: " +
                     joinWith(parsed.varlist, " ") + ".");
    }
  }

  // The legend is taken from one panel's legend object; grc1leg2 sizes and draws it from the keys the graph's
  // legend holds, and a legend of a single key is too few (bank: `grc1leg2 ga gb` with ga = `scatter price mpg
  // if foreign==0, legend(on)`, one series, r(111)). The keys are the series the graph draws: one per plot of a
  // twoway graph, one per y variable of a scatter or a bar graph. A panel whose series cannot be counted here
  // (a by-graph, a combine) is not refused. (Wiggins' grc1leg reads the legend differently: the same two graphs
  // run there at rc 0.)
  if (parsed.cmd == "grc1leg2") {
    const std::string nm = keepIdx < parsed.varlist.size() ? trimOf(dequote(trimOf(parsed.varlist[keepIdx]))) : "";
    std::string command;
    if (const auto mt = session.graphMeta.find(nm); mt != session.graphMeta.end() && mt->second.is_object())
      command = mt->second.value("command", std::string());
    const std::optional<size_t> keys = legendKeyCount(panels[keepIdx], command);
    if (keys.has_value() && *keys < 2) {
      return ojson(eval::errorBlock("engine_error",
                                    "The legend in graph #" + std::to_string(keepIdx + 1) +
                                        (nm.empty() ? std::string() : ", -" + nm + "-,") + " has too few keys",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
  }

  // The one real edit: every OTHER panel loses its legend. The kept panel is
  // left exactly as it was -- if the user had already put `legend(off)` on it,
  // grc1leg2 does not turn it back on.
  for (size_t i = 0; i < nPanels; ++i) {
    if (i == keepIdx) continue;
    if (!panels[i].is_object()) continue;
    panels[i]["legend"]["off"] = true;
  }
  result["legend_from"] = static_cast<int64_t>(keepIdx);

  // xcommon/ycommon: one axis range shared by every panel that has one. This
  // is READ AND APPLIED rather than accepted-and-noted, because an axis scale
  // is a claim about the numbers, not a cosmetic.
  const bool xcommon = hasFlag(parsed.opts, "xcommon");
  const bool ycommon = hasFlag(parsed.opts, "ycommon");
  if (xcommon || ycommon) {
    auto unify = [&](const char* loKey, const char* hiKey) {
      double lo = std::numeric_limits<double>::infinity();
      double hi = -std::numeric_limits<double>::infinity();
      bool any = false;
      for (const auto& p : panels) {
        if (!p.is_object() || !p.contains(loKey) || !p.contains(hiKey)) continue;
        if (!p[loKey].is_number() || !p[hiKey].is_number()) continue;
        const double a = p[loKey].get<double>(), b = p[hiKey].get<double>();
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        lo = std::min(lo, a);
        hi = std::max(hi, b);
        any = true;
      }
      if (!any) return;
      for (auto& p : panels) {
        if (!p.is_object() || !p.contains(loKey) || !p.contains(hiKey)) continue;
        if (!p[loKey].is_number() || !p[hiKey].is_number()) continue;
        p[loKey] = lo;
        p[hiKey] = hi;
      }
    };
    if (xcommon) unify("x_min", "x_max");
    if (ycommon) unify("y_min", "y_max");
  }

  // Accepted, not applied -- named, never silent (defect class 3).
  const std::vector<std::string> cosmetic = presentOpts(
      parsed.opts, {"ring", "position", "span", "symxsize", "symysize", "labsize"});
  if (!cosmetic.empty()) {
    appendBlockNote(result, "(" + kCmd + ": " + joinWith(cosmetic, ", ") +
                                " accepted and not applied — the kept legend stays inside its own "
                                "panel in this build)");
  }

  if (auto err = honourSaving(session, kCmd, parsed.opts, result)) return *err;
  return result;
}

}  // namespace piet::handlers
