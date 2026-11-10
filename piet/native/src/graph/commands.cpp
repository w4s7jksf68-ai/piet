// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "piet/handlers.h"
#include "piet/eval/errors.h"
#include "piet/twoway_opts.h"
#include "support.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


// ===========================================================================
// scatter

ojson runScatter(Datastore& ds, const std::vector<std::string>& varlist,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.size() < 2) {
    return engineErrorBlockG("scatter requires two variables: `scatter yvar xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;

  // BS-050 (BETA E): Stata's `scatter y1 y2 ... x` puts the SHARED x
  // variable LAST -- every token before it is an independent y-series
  // plotted against that same x (previously only varlist[0]/varlist[1] were
  // ever read, so `scatter y1 y2 x` silently rendered y1-vs-y2 and dropped
  // the real x entirely). varlist.size()==2 is the single-series case and
  // keeps the EXACT pre-existing flat JSON shape (points/x_var/y_var)
  // untouched below -- only 3+ tokens engage the new "series" array, so no
  // existing consumer (buildSvgScatter's legacy branch, BS-010's selftest,
  // the not-yet-updated frontend) sees a shape change for the common case.
  std::string xvar = varlist.back();
  std::vector<std::string> yvars(varlist.begin(), varlist.end() - 1);
  for (const auto& yv : yvars) {
    if (!ds.has(yv)) return engineErrorBlockG("variable " + yv + " not found");
  }
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  auto x = numericColumn(ds, xvar);
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  int64_t nTotal = 0;

  ojson out;
  out["type"] = "graph";
  out["kind"] = "scatter";

  if (yvars.size() == 1) {
    auto y = numericColumn(ds, yvars[0]);
    ojson points = ojson::array();
    for (size_t i = 0; i < y.size(); ++i) {
      if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
      double xv = x[i], yv = y[i];
      points.push_back(ojson::array({xv, yv}));
      if (first) {
        xMin = xMax = xv;
        yMin = yMax = yv;
        first = false;
      } else {
        xMin = std::min(xMin, xv);
        xMax = std::max(xMax, xv);
        yMin = std::min(yMin, yv);
        yMax = std::max(yMax, yv);
      }
      ++nTotal;
    }
    out["x_var"] = xvar;
    out["y_var"] = yvars[0];
    out["points"] = points;
    out["n"] = nTotal;
  } else {
    ojson seriesArr = ojson::array();
    for (size_t si = 0; si < yvars.size(); ++si) {
      auto y = numericColumn(ds, yvars[si]);
      ojson points = ojson::array();
      int64_t nSeries = 0;
      for (size_t i = 0; i < y.size(); ++i) {
        if (!std::isfinite(y[i]) || !std::isfinite(x[i])) continue;
        double xv = x[i], yv = y[i];
        points.push_back(ojson::array({xv, yv}));
        if (first) {
          xMin = xMax = xv;
          yMin = yMax = yv;
          first = false;
        } else {
          xMin = std::min(xMin, xv);
          xMax = std::max(xMax, xv);
          yMin = std::min(yMin, yv);
          yMax = std::max(yMax, yv);
        }
        ++nSeries;
        ++nTotal;
      }
      ojson s;
      s["y_var"] = yvars[si];
      s["points"] = points;
      s["style"] = twoway::layerStyle(opts, static_cast<int>(si), "scatter");
      s["n"] = nSeries;
      seriesArr.push_back(s);
    }
    out["x_var"] = xvar;
    out["y_var"] = yvars[0];  // first series, for any single-y-shaped reader
    out["y_vars"] = yvars;    // full typed-order list
    out["series"] = seriesArr;
    // Backward-compat flat "points": the union of every series, so a
    // consumer that only reads the flat field (an as-yet-unupdated
    // renderer) still degrades gracefully to "every point plotted" instead
    // of an empty graph.
    ojson flatPoints = ojson::array();
    for (const auto& s : seriesArr) {
      for (const auto& p : s["points"]) flatPoints.push_back(p);
    }
    out["points"] = flatPoints;
    out["n"] = nTotal;
  }

  std::string joinedY;
  for (size_t i = 0; i < yvars.size(); ++i) {
    if (i) joinedY += " ";
    joinedY += yvars[i];
  }
  out["title"] = optTitleStr(opts, "title").value_or(joinedY + " vs " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvars[0]);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}


// ===========================================================================
// line (also serves bare `twoway line yvar xvar [if ...]`)

ojson runGraphLine(Datastore& ds, const std::vector<std::string>& varlist,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.size() < 2) {
    return engineErrorBlockG("line requires two variables: `line yvar xvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string yvar = varlist[0], xvar = varlist[1];
  if (!ds.has(yvar)) return engineErrorBlockG("variable " + yvar + " not found");
  if (!ds.has(xvar)) return engineErrorBlockG("variable " + xvar + " not found");

  auto y = numericColumn(ds, yvar);
  auto x = numericColumn(ds, xvar);

  std::vector<std::pair<double, double>> pairs;
  for (size_t i = 0; i < y.size(); ++i) {
    if (std::isfinite(y[i]) && std::isfinite(x[i])) pairs.emplace_back(x[i], y[i]);
  }
  // Points are connected in data order; only `sort` orders them by x ([G-3]
  // connect_options; BUG-404).
  if (hasOpt(opts, "sort")) {
    std::stable_sort(pairs.begin(), pairs.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
  }

  ojson points = ojson::array();
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  for (const auto& [xv, yv] : pairs) {
    points.push_back(ojson::array({xv, yv}));
    if (first) {
      xMin = xMax = xv;
      yMin = yMax = yv;
      first = false;
    } else {
      xMin = std::min(xMin, xv);
      xMax = std::max(xMax, xv);
      yMin = std::min(yMin, yv);
      yMax = std::max(yMax, yv);
    }
  }
  int64_t n = static_cast<int64_t>(pairs.size());

  ojson out;
  out["type"] = "graph";
  out["kind"] = "line";
  out["title"] = optTitleStr(opts, "title").value_or(yvar + " vs " + xvar);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xvar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvar);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["x_var"] = xvar;
  out["y_var"] = yvar;
  out["n"] = n;
  out["points"] = points;
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}


// ===========================================================================
// xtline

ojson runXtline(Datastore& ds, const std::vector<std::string>& varlist,
                const std::optional<std::string>& panelvar, const std::optional<std::string>& timevar,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("xtline requires a variable: `xtline yvar`.", "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  std::string yvar = varlist[0];
  if (!panelvar.has_value() || panelvar->empty()) {
    return engineErrorBlockG("xtline: panel variable not set. Run `xtset ivar [tvar]` first.",
                             "syntax_error");
  }
  if (!ds.has(yvar)) return engineErrorBlockG("variable " + yvar + " not found");
  if (!ds.has(*panelvar)) return engineErrorBlockG("panel variable " + *panelvar + " not found");
  bool haveTime = timevar.has_value() && !timevar->empty();
  if (haveTime && !ds.has(*timevar)) {
    return engineErrorBlockG("time variable " + *timevar + " not found");
  }

  auto yArr = numericColumn(ds, yvar);
  auto idArr = numericColumn(ds, *panelvar);
  std::vector<double> tArr;
  if (haveTime) {
    tArr = numericColumn(ds, *timevar);
  }
  // When no time variable is set, the x-axis is a per-panel-local row index
  // (0, 1, 2, ... within each panel), NOT a global running index across all
  // panels — matching the Python oracle (graph.py run_xtline: np.arange over
  // panel_mask.sum(), computed fresh per panel). The local counter is
  // assigned below, inside the per-uid loop, in dataset row order.

  std::set<double> uniqueIds;
  for (double v : idArr) {
    if (std::isfinite(v)) uniqueIds.insert(v);
  }

  ojson seriesArr = ojson::array();
  std::vector<double> allX, allY;
  int64_t nObsTotal = 0;

  for (double uid : uniqueIds) {
    std::vector<std::pair<double, double>> pts;
    double localIdx = 0.0;
    for (size_t i = 0; i < idArr.size(); ++i) {
      if (idArr[i] != uid) continue;
      double tv = haveTime ? tArr[i] : localIdx;
      localIdx += 1.0;
      if (!std::isfinite(yArr[i]) || !std::isfinite(tv)) continue;
      pts.emplace_back(tv, yArr[i]);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    ojson s;
    s["id"] = uid;
    std::string label;
    if (uid == std::floor(uid)) {
      label = std::to_string(static_cast<long long>(uid));
    } else {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", uid);
      label = buf;
    }
    s["label"] = label;
    ojson pointsJ = ojson::array();
    for (const auto& [tv, yv] : pts) {
      pointsJ.push_back(ojson::array({tv, yv}));
      allX.push_back(tv);
      allY.push_back(yv);
    }
    s["points"] = pointsJ;
    seriesArr.push_back(s);
    nObsTotal += static_cast<int64_t>(pts.size());
  }

  if (seriesArr.empty()) {
    return engineErrorBlockG("no non-missing observations for xtline " + yvar);
  }

  std::string xLabelDefault = haveTime ? *timevar : "obs";
  ojson out;
  out["type"] = "graph";
  out["kind"] = "xtline";
  out["title"] = optTitleStr(opts, "title").value_or(yvar + " by " + *panelvar + " over " +
                                                      (haveTime ? *timevar : "obs"));
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(xLabelDefault);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yvar);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["y_var"] = yvar;
  out["panel_var"] = *panelvar;
  out["time_var"] = haveTime ? *timevar : "";
  out["n_panels"] = static_cast<int64_t>(seriesArr.size());
  out["n_obs"] = nObsTotal;
  // BUG-179 (P2-PREP): `xtline y, overlay` — Stata's option for drawing every
  // panel on ONE set of axes instead of a grid of small multiples. It was
  // parsed by the shared option grammar and then read nowhere here, so the
  // request vanished silently (rc 0, ungrouped default layout).
  //
  // This is a RENDERING choice, and rendering is client-side in this
  // architecture — the kernel ships plot DATA (piet/frontend/js/graph.js's
  // own top-of-file note). The data side is what this file owns, so the flag
  // is published here; piet/frontend/js/graph.js has no consumer for it yet
  // and renderXtline still draws its single overlaid axis regardless.
  // Reported as such rather than silently half-done: after this change the
  // option is *reachable*, not yet *honored end-to-end*.
  //
  // Emitted ONLY when actually requested, not as an unconditional `false`.
  // An optional key is this contract's established idiom for exactly this
  // (internal note piet/CONTRACT.md already spells kdensity's own extras
  // `subkind?`/`normal_points?`), a missing key and `false` are the same
  // falsy value to the JS consumer, and the conditional form keeps every
  // xtline block that did NOT ask for overlay byte-identical — so the
  // frozen internal capture xtline.json capture does not move.
  if (optBool(opts, "overlay").value_or(false)) out["overlay"] = true;
  out["series"] = seriesArr;
  out["x_min"] = allX.empty() ? 0.0 : *std::min_element(allX.begin(), allX.end());
  out["x_max"] = allX.empty() ? 0.0 : *std::max_element(allX.begin(), allX.end());
  out["y_min"] = allY.empty() ? 0.0 : *std::min_element(allY.begin(), allY.end());
  out["y_max"] = allY.empty() ? 0.0 : *std::max_element(allY.begin(), allY.end());
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}


// ===========================================================================
// tsline

ojson runTsline(Datastore& ds, const std::vector<std::string>& varlist,
               const std::optional<std::string>& timevar,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  if (varlist.empty()) {
    return engineErrorBlockG("tsline requires at least one variable: `tsline yvar [yvar2 ...]`.",
                             "syntax_error");
  }
  if (auto err = rejectByFacet(opts)) return *err;
  if (!timevar.has_value() || timevar->empty()) {
    return engineErrorBlockG("tsline: time variable not set. Run `tsset tvar` or `xtset ivar tvar` first.",
                             "syntax_error");
  }
  if (!ds.has(*timevar)) return engineErrorBlockG("time variable " + *timevar + " not found");
  for (const auto& v : varlist) {
    if (!ds.has(v)) return engineErrorBlockG("variable " + v + " not found");
  }

  auto tArr = numericColumn(ds, *timevar);

  ojson seriesArr = ojson::array();
  std::vector<double> allX, allY;
  int64_t nObsTotal = 0;

  for (const auto& yvar : varlist) {
    auto yArr = numericColumn(ds, yvar);
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < tArr.size(); ++i) {
      if (!std::isfinite(tArr[i]) || !std::isfinite(yArr[i])) continue;
      pts.emplace_back(tArr[i], yArr[i]);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    ojson s;
    s["var"] = yvar;
    ojson pointsJ = ojson::array();
    for (const auto& [tv, yv] : pts) {
      pointsJ.push_back(ojson::array({tv, yv}));
      allX.push_back(tv);
      allY.push_back(yv);
    }
    s["points"] = pointsJ;
    seriesArr.push_back(s);
    nObsTotal += static_cast<int64_t>(pts.size());
  }

  std::string yLabelDefault = varlist.size() == 1 ? varlist[0] : "value";
  std::string titleDefault;
  if (varlist.size() == 1) {
    titleDefault = varlist[0] + " over " + *timevar;
  } else {
    std::string joined;
    for (size_t i = 0; i < varlist.size(); ++i) {
      if (i) joined += ", ";
      joined += varlist[i];
    }
    titleDefault = joined + " over " + *timevar;
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "tsline";
  out["title"] = optTitleStr(opts, "title").value_or(titleDefault);
  out["x_label"] = optTitleStr(opts, "xtitle").value_or(*timevar);
  out["y_label"] = optTitleStr(opts, "ytitle").value_or(yLabelDefault);
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["time_var"] = *timevar;
  ojson varsJ = ojson::array();
  for (const auto& v : varlist) varsJ.push_back(v);
  out["vars"] = varsJ;
  out["n_series"] = static_cast<int64_t>(seriesArr.size());
  out["n_obs"] = nObsTotal;
  out["series"] = seriesArr;
  out["x_min"] = allX.empty() ? 0.0 : *std::min_element(allX.begin(), allX.end());
  out["x_max"] = allX.empty() ? 0.0 : *std::max_element(allX.begin(), allX.end());
  out["y_min"] = allY.empty() ? 0.0 : *std::min_element(allY.begin(), allY.end());
  out["y_max"] = allY.empty() ? 0.0 : *std::max_element(allY.begin(), allY.end());
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-010)
  return out;
}


// ===========================================================================
// marginsplot

ojson runMarginsplot(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session) {
  const double kZ975 = 1.959963984540054;
  // BUG-1248: with no margins result to plot StataBE 19.5 answers "previous
  // command was not margins" r(301), with or without estimates or data in memory.
  if (!session.lastMargins.has_value()) {
    return ojson(eval::errorBlock("engine_error", "previous command was not margins",
                                  std::nullopt, std::nullopt, std::nullopt, "r(301)"));
  }
  const ojson& raw = *session.lastMargins;
  ojson coefs = raw.value("coefs", ojson::array());

  if (!session.lastMarginsDydx.empty()) {
    std::set<std::string> dydxSet(session.lastMarginsDydx.begin(), session.lastMarginsDydx.end());
    ojson filtered = ojson::array();
    for (const auto& c : coefs) {
      if (c.contains("name") && dydxSet.count(c["name"].get<std::string>())) filtered.push_back(c);
    }
    coefs = filtered;
  }
  if (coefs.empty()) {
    return engineErrorBlockG("marginsplot: margins result has no estimates to plot.");
  }

  std::string model = raw.value("model", std::string());
  int64_t nObs = raw.contains("n_obs") ? raw["n_obs"].get<int64_t>() : 0;

  ojson points = ojson::array();
  std::vector<double> allLo, allHi;
  for (const auto& c : coefs) {
    std::string name = c.value("name", std::string("?"));
    ojson bVal = c.contains("dydx") ? c["dydx"] : (c.contains("b") ? c["b"] : ojson(nullptr));
    ojson seVal = c.contains("se") ? c["se"] : ojson(nullptr);
    if (bVal.is_null() || seVal.is_null()) continue;
    double b, se;
    try {
      b = bVal.get<double>();
      se = seVal.get<double>();
    } catch (...) {
      continue;
    }
    double ciLo = b - kZ975 * se, ciHi = b + kZ975 * se;
    ojson p;
    p["name"] = name;
    p["b"] = b;
    p["se"] = se;
    p["ci_lo"] = ciLo;
    p["ci_hi"] = ciHi;
    points.push_back(p);
    allLo.push_back(ciLo);
    allHi.push_back(ciHi);
  }
  if (points.empty()) {
    return engineErrorBlockG("marginsplot: cannot build plot — no numeric AME estimates found.");
  }

  double yMin = *std::min_element(allLo.begin(), allLo.end());
  double yMax = *std::max_element(allHi.begin(), allHi.end());
  double span = (yMax != yMin) ? (yMax - yMin) : 0.1;
  yMin -= 0.1 * span;
  yMax += 0.1 * span;
  yMin = std::min(yMin, 0.0);
  yMax = std::max(yMax, 0.0);

  std::string modelLabel = model.empty() ? "" : (" (" + model + ")");
  std::string titleDefault = "Average Marginal Effects" + modelLabel;

  ojson out;
  out["type"] = "graph";
  out["kind"] = "marginsplot";
  out["title"] = optTitleStr(opts, "title").value_or(titleDefault);
  out["y_label"] = "Average marginal effect (dydx)";
  out["x_label"] = "";
  out["note"] = optTitleStr(opts, "note").value_or(std::string());
  out["n"] = nObs;
  out["model"] = model;
  out["points"] = points;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011, mirrors BS-010)
  return out;
}

}  // namespace piet::handlers
