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
#include "graph_extension_internal.h"

namespace {

// cdfplot. `normal`/`exponential` overlay a fitted reference CDF and `xlog` is
// a log x scale; all three are real plot content this build does not draw, and
// drawing the plot without them is answering a different question, so they are
// refused rather than ignored.
// cdfplot.ado 1.04 hands every option it does not own to its `twoway ...,
// `goptions'`, so the graph-level twoway options PIET's twoway takes are known
// here too (StataBE 19.5, BUG-1521: `cdfplot x, nodraw`, xlabel(), scheme(),
// ysize(), xscale(), note(), graphregion(), subtitle(), caption(), xline(),
// plotregion(), aspectratio() are rc 0; a plot option such as lcolor() or
// msymbol() is twoway's r(198) "option lcolor() not allowed"). runCdfplot names
// the ones it does not draw. xti/yti are twoway's spellings of xtitle/ytitle,
// which xtick/ytick would otherwise make ambiguous.
const piet::eval::OptVocab& kVocabCdfplot = piet::eval::registerOptVocab(
    {"cdfplot"}, {{"by", "title", "xtitle", "ytitle", "ylabel", "legend", "name", "saving",
                   "nodraw", "subtitle", "note", "caption", "ttitle", "xlabel", "tlabel", "xmlabel",
                   "ymlabel", "tmlabel", "xtick", "ytick", "ttick", "xmtick", "ymtick", "xscale",
                   "yscale", "tscale", "xline", "yline", "tline", "scheme", "graphregion",
                   "plotregion", "xsize", "ysize", "fxsize", "fysize", "aspectratio", "scale", "text",
                   "ttext"},
                  {"normal", "exponential", "xlog"},
                  {{"xti", "xtitle"}, {"yti", "ytitle"}, {"xlabels", "xlabel"}, {"ylabels", "ylabel"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace graph_extension_detail;

// Defined in graph/twoway.cpp: the legend block twoway builds from legend() text
// (quote- and paren-aware; `off` only as a standalone top-level token);
// keyText(i) is the text of plot i's key (nullopt: no such plot).
ojson legendBlock(const std::string& legendText,
                  const std::function<std::optional<std::string>(int64_t)>& keyText, bool* hasOrder);

namespace {

// A number the way a legend labels it: an integral value loses its decimal.
std::string levelText(double v) {
  if (std::isnan(v)) return ".";
  if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
  return eval::pythonFloatRepr(v);
}

// ===========================================================================
// THE ECDF RANK RULE -- TRANSCRIBED FROM BUNDLE F's `cumul`
// ===========================================================================
//
// This is `runCumul`'s ranking (datamgmt/cumul.cpp), copied rather than
// called: `runCumul` is a whole command -- it validates its own varlist, writes
// a column through `setcol --new` and returns a `mutationResult` -- so there is
// no function to call for the ranks alone, and hoisting one out of another
// bundle's TU is outside this write manifest. The rule is therefore duplicated
// DELIBERATELY and identically: rank ascending with a stable sort so a tie
// keeps the rows' ORIGINAL order, then y = rank/N. `cumul`'s `equal` variant
// (every member of a tied block takes the block's highest rank) is NOT used
// here, because cdfplot has no `equal` option to select it.
//
// N/N == 1.0 exactly in IEEE double for every N, so the LAST point of every
// series this produces is exactly 1.0 -- not (n-1)/n and not n/(n+1), the two
// off-by-ones that look perfectly plausible on a chart.
//
// If `runCumul`'s rule ever changes, this must change with it.
std::vector<std::pair<double, double>> ecdfPoints(const std::vector<double>& xs) {
  std::vector<size_t> order;
  order.reserve(xs.size());
  for (size_t i = 0; i < xs.size(); ++i)
    if (!std::isnan(xs[i])) order.push_back(i);
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return xs[a] < xs[b]; });
  const double nn = static_cast<double>(order.size());
  std::vector<std::pair<double, double>> pts;
  pts.reserve(order.size());
  for (size_t r = 0; r < order.size(); ++r) {
    pts.emplace_back(xs[order[r]], static_cast<double>(r + 1) / nn);
  }
  return pts;
}

// The empty option list every layer built in this file styles itself against:
// `twoway::layerStyle` picks the palette colour for the layer index when no
// colour option is present, which is exactly what a cdfplot series wants.
const std::vector<std::pair<std::string, eval::OptValue>> kNoLayerOpts;

}  // namespace

// ===========================================================================
// cdfplot
// ===========================================================================
//
// The empirical CDF as a `kind:"twoway"` block whose layers are ordinary
// `kind:"line"` layers -- the shape layerLineOrConnected already produces, so
// nothing downstream needs to learn a new plot type. `by(g)` gives one layer
// per group.
//
// STATA'S cdfplot DRAWS A STEP FUNCTION AND THIS DRAWS A LINE. The renderer has
// no step plot type (graph/twoway.cpp's six are scatter/line/connected/lfit/
// qfit/rarea) and adding one is a frontend change, outside this manifest. The
// block therefore says so in its own note rather than letting the difference
// pass unremarked.
ojson runCdfplot(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "cdfplot";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCdfplot.known,
                                            kVocabCdfplot.unimplemented))
    return ojson(*err);
  if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
    return ojson(eval::unsupportedOption(
        *parsed.weightType + "=" + parsed.weightVar.value_or(""),
        "`cdfplot` plots an unweighted empirical CDF in this build; a weighted ECDF is a "
        "different estimator, not a reweighting of this one."));
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return gxSyntax(kCmd + ": syntax is `cdfplot varname [if] [in] [, by(groupvar)]` — one "
                           "variable.");
  const std::string var = trimOf(dequote(trimOf(parsed.varlist[0])));
  const std::optional<std::string> byVar = optStr(parsed.opts, "by");

  // `if`/`in`: the ECDF is computed over the SELECTED sample, which is exactly
  // what the shared filter returns -- no row-alignment problem here, so this
  // uses `applyIfAndInFilter` as designed rather than geodist's mask. The base
  // path comes from `session.datastore()` rather than `currentHbin()` (the
  // dispatcher's own summarize/tabulate filter sites do the same): the filter
  // hands the path to `cobra eval`, so it has to be a file that EXISTS, and
  // going through the Datastore is what guarantees a virtual head has been
  // materialized first. This runs ONLY when a qualifier is present, so the
  // unfiltered path keeps its §38 projection.
  std::string dataPath;
  bool filtered = false;
  const bool hasQualifier = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                            (parsed.inRange.has_value() && !parsed.inRange->empty());
  if (hasQualifier) {
    Datastore* base = nullptr;
    try {
      base = session.datastore();
    } catch (const std::exception& e) {
      return gxError(kCmd + ": could not read the data — " + e.what(), "data_io");
    }
    if (base == nullptr) return ojson(eval::noDataLoaded());
    dataPath = base->path();
    try {
      InRangeFilterResult fr = applyIfAndInFilter(session, dataPath, parsed.ifExpr, parsed.inRange);
      if (!fr.ok) return ojson(inRangeRejectError(fr, parsed.inRange.value_or(""), kCmd));
      filtered = (fr.dataPath != dataPath);
      dataPath = fr.dataPath;
    } catch (const CobraError& ce) {
      return gxError(kCmd + ": " + cobraErrorMessage(ce), ce.code);
    }
  }

  std::vector<std::string> wanted = {var};
  if (byVar.has_value() && !byVar->empty()) wanted.push_back(*byVar);
  // The filtered frame is a real file with the selected rows already in it, so
  // the §38 projection is only worth asking for on the unfiltered path.
  std::unique_ptr<Datastore> owned;
  std::optional<ReadFrame> rf;
  Datastore* ds = nullptr;
  if (filtered) {
    try {
      owned = std::make_unique<Datastore>(dataPath);
      ds = owned.get();
    } catch (const std::exception& e) {
      return gxError(kCmd + ": could not read the filtered data — " + e.what(), "data_io");
    }
  } else {
    rf.emplace(session, wanted);
    ds = rf->get();
    if (ds == nullptr)
      return gxError(kCmd + ": could not read the data" +
                         (rf->why().empty() ? "" : " — " + rf->why()),
                     "data_io");
  }

  if (!ds->has(var)) return gxError("variable " + var + " not found");
  if (ds->dtype(var) == "factor")
    return gxError(kCmd + ": `" + var + "' is a string variable; an ECDF needs a numeric "
                                        "variable.");
  const std::vector<double> x = columnAsDoubles(*ds, var);

  // Group the rows. Without by(), one group holding everything.
  std::vector<std::string> groupLabels;
  std::vector<std::vector<double>> groupValues;
  if (byVar.has_value() && !byVar->empty()) {
    if (!ds->has(*byVar)) return gxError("variable " + *byVar + " not found");
    const bool byFactor = ds->dtype(*byVar) == "factor";
    const std::vector<std::string> gText = columnAsStrings(*ds, *byVar);
    const std::vector<double> gNum = byFactor ? std::vector<double>() : columnAsDoubles(*ds, *byVar);
    // Ascending group order: by value for a numeric group variable, by level
    // text for a string one -- deterministic either way.
    std::vector<size_t> keys;
    keys.reserve(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
      if (gText[i].empty()) continue;  // missing group: excluded, like Stata's by()
      keys.push_back(i);
    }
    std::stable_sort(keys.begin(), keys.end(), [&](size_t a, size_t b) {
      if (byFactor) return gText[a] < gText[b];
      return gNum[a] < gNum[b];
    });
    for (size_t pos = 0; pos < keys.size();) {
      size_t end = pos;
      while (end + 1 < keys.size() &&
             (byFactor ? (gText[keys[end + 1]] == gText[keys[pos]])
                       : (gNum[keys[end + 1]] == gNum[keys[pos]])))
        ++end;
      std::vector<double> vals;
      vals.reserve(end - pos + 1);
      for (size_t q = pos; q <= end; ++q) vals.push_back(x[keys[q]]);
      groupLabels.push_back(*byVar + "=" +
                            (byFactor ? gText[keys[pos]] : levelText(gNum[keys[pos]])));
      groupValues.push_back(std::move(vals));
      pos = end + 1;
    }
  } else {
    groupValues.push_back(x);
    groupLabels.push_back(var);
  }

  ojson layers = ojson::array();
  ojson legendOrder = ojson::array();
  std::vector<std::string> keyLabels;  // each drawn layer's key text
  double xMin = std::numeric_limits<double>::infinity();
  double xMax = -std::numeric_limits<double>::infinity();
  int64_t nObs = 0;
  for (size_t g = 0; g < groupValues.size(); ++g) {
    const std::vector<std::pair<double, double>> pts = ecdfPoints(groupValues[g]);
    if (pts.empty()) continue;  // a group with nothing non-missing draws nothing
    ojson pointsJ = ojson::array();
    for (const auto& [px, py] : pts) {
      pointsJ.push_back(ojson::array({px, py}));
      xMin = std::min(xMin, px);
      xMax = std::max(xMax, px);
    }
    ojson lyr;
    lyr["kind"] = "line";
    lyr["points"] = pointsJ;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "line");
    lyr["n"] = static_cast<int64_t>(pts.size());
    if (byVar.has_value() && !byVar->empty()) {
      legendOrder.push_back(ojson::array({static_cast<int64_t>(layers.size()), groupLabels[g]}));
    }
    keyLabels.push_back(groupLabels[g]);
    layers.push_back(lyr);
    nObs += static_cast<int64_t>(pts.size());
  }
  if (!std::isfinite(xMin) || !std::isfinite(xMax)) {
    xMin = 0.0;
    xMax = 1.0;
  }

  // The x label defaults to the variable's own label when it has one; the
  // .hbin header carries none, so this reads the session's label projection and
  // falls back to the variable NAME rather than printing an empty axis.
  std::string xDefault = session.eval.varLabels.get(var, std::string());
  if (xDefault.empty()) xDefault = var;

  ojson block;
  block["type"] = "graph";
  block["kind"] = "twoway";
  block["title"] = optStr(parsed.opts, "title").value_or("Empirical CDF of " + var);
  block["subtitle"] = optStr(parsed.opts, "subtitle").value_or(std::string());  // as twoway's
  block["x_label"] = optStr(parsed.opts, "xtitle").value_or(xDefault);
  block["y_label"] = optStr(parsed.opts, "ytitle").value_or(std::string("Cumulative probability"));
  block["note"] = optStr(parsed.opts, "note").value_or(std::string());  // as twoway's
  block["x_min"] = xMin;
  block["x_max"] = xMax;
  block["y_min"] = 0.0;
  block["y_max"] = 1.0;
  block["layers"] = layers;
  {
    ojson axes;
    axes["xline"] = ojson::array();
    axes["yline"] = ojson::array();
    axes["xscale_noline"] = false;
    axes["yscale_noline"] = false;
    axes["xlabel_none"] = false;
    // `ylabel(none)` is the one ylabel spelling this block shape can express.
    std::string yl = lowerOf(optStr(parsed.opts, "ylabel").value_or(std::string()));
    axes["ylabel_none"] = (trimOf(yl) == "none");
    block["axes"] = axes;
  }
  {
    // cdfplot.ado hands legend() to its twoway (`goptions'), so it reads as
    // twoway's does -- order() over the group lines included (StataBE 19.5,
    // BUG-1309: `cdfplot x, by(g) legend(order(2 "two" 1 "one"))` rc 0); `off`
    // is a top-level token, not a substring (legend(order(1 "Payoff")) keeps
    // its legend). Without order() the keys are the groups in order.
    bool hasOrder = false;
    ojson legend = legendBlock(optStr(parsed.opts, "legend").value_or(std::string()),
                               [&](int64_t idx) -> std::optional<std::string> {
                                 if (idx < 0 || static_cast<size_t>(idx) >= keyLabels.size()) return std::nullopt;
                                 return keyLabels[static_cast<size_t>(idx)];
                               },
                               &hasOrder);
    if (!hasOrder) legend["order"] = legendOrder;
    block["legend"] = legend;
  }
  block["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011)
  block["n_obs"] = nObs;
  block["suppressed"] = session.graphicsOff;

  appendBlockNote(block, "(cdfplot: drawn as a line, not a step function)");
  {
    // The twoway options this block does not draw (BUG-1521), named as the twoway
    // arm names its own (dispatcher.cpp graphAcceptedNotApplied).
    static const std::unordered_set<std::string> kApplied = {
        "by", "title", "subtitle", "xtitle", "ytitle", "note", "ylabel", "legend", "name", "saving"};
    std::vector<std::string> noted;
    for (const auto& kv : parsed.opts) {
      if (kApplied.count(kv.first) == 0 && std::find(noted.begin(), noted.end(), kv.first) == noted.end())
        noted.push_back(kv.first);
    }
    if (!noted.empty()) {
      appendBlockNote(block, "(" + kCmd + ": " + joinWith(noted, ", ") +
                                 " accepted and not applied — this build draws the plot without " +
                                 (noted.size() == 1 ? "it" : "them") + ".)");
    }
  }
  if (layers.empty()) {
    // Plan edge case: zero observations is an EMPTY plot plus a note, never an
    // error -- a do-file that plots an empty subgroup keeps running.
    appendBlockNote(block, "(cdfplot: no non-missing observations — nothing to plot)");
  }
  if (auto err = honourSaving(session, kCmd, parsed.opts, block)) return *err;
  return block;
}

}  // namespace piet::handlers
