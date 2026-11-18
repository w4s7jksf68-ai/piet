// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "piet/handlers/graph.h"
#include "piet/eval/errors.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "graph_support.h"
#include "core/distributions.h"  // t_critical / z_critical (shared with cobra)

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
    // bank: `xtline beertax` on data that is not xtset is r(459) "must specify panelvar; use xtset"
    return stataRefusal("must specify panelvar; use xtset", 459);
  }
  // bank: `xtline calories cal2, overlay` is r(198) (overlay draws one variable)
  if (varlist.size() > 1 && optBool(opts, "overlay").value_or(false)) {
    return stataRefusal("option overlay may not be specified with multiple variables", 198);
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
    // bank: `tsline le` on data that is not tsset is r(111) "time variable not set, use tsset varname ..."
    return stataRefusal("time variable not set, use tsset varname ...", 111);
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
  // BUG-1248: with no margins result to plot StataBE 19.5 answers "previous
  // command was not margins" r(301), with or without estimates or data in memory.
  if (!session.lastMargins.has_value()) {
    return ojson(eval::errorBlock("engine_error", "previous command was not margins",
                                  std::nullopt, std::nullopt, std::nullopt, "r(301)"));
  }
  // The results plotted are the live margins results -- r() after `margins`,
  // e() after `margins, post` -- and with neither, marginsplot is r(301) "previous
  // command was not margins" (marginsplot's own check of r(cmd) and e(cmd)): a
  // margins that was refused, or a command that replaced r(), leaves nothing to
  // plot, and the cached reply of an earlier margins is not it. After mean,
  // proportion, ratio and total Stata plots their e(); not served here.
  {
    const std::string* rc = session.eval.rStrings.find("cmd");
    const std::string* ec = session.eval.eStrings.find("cmd");
    const bool live = (rc != nullptr && *rc == "margins") || (ec != nullptr && *ec == "margins");
    if (!live) {
      static const std::set<std::string> kSummaries = {"mean", "proportion", "ratio", "total"};
      if (ec != nullptr && kSummaries.count(*ec))
        return engineErrorBlockG("marginsplot after " + *ec +
                                     " is a valid Stata command that this version of PIET does not implement.",
                                 "unsupported_command");
      return ojson(eval::errorBlock("engine_error", "previous command was not margins", std::nullopt,
                                    std::nullopt, std::nullopt, "r(301)"));
    }
  }
  // x(dimlist) names dimensions of the margins result: its r(marg_dims) words,
  // an `at(v)` dimension also spelled `v`. Anything else is r(322) "<name> not
  // a dimension in margins results" (vs-stata bank 5bdc7495 `marginsplot,
  // x(nosuchvar)` after `margins foreign, at(mpg=(15 20 25 30))`).
  for (const auto& [k, v] : opts) {
    const std::string key = strutil::lower(k);
    if ((key != "x" && key != "xdimension") || v.isBool) continue;
    const std::string* dims = session.eval.rStrings.find("marg_dims");
    if (dims == nullptr) break;
    std::set<std::string> known;
    std::istringstream ds(*dims);
    for (std::string w; ds >> w;) {
      known.insert(w);
      if (w.size() > 4 && w.compare(0, 3, "at(") == 0 && w.back() == ')') known.insert(w.substr(3, w.size() - 4));
    }
    std::istringstream xs(v.strValue);
    for (std::string w; xs >> w;)
      if (!known.count(w))
        return ojson(eval::errorBlock("engine_error", w + " not a dimension in margins results",
                                      std::nullopt, std::nullopt, std::nullopt, "r(322)"));
  }
  const ojson& raw = *session.lastMargins;
  ojson coefs = raw.value("coefs", ojson::array());
  // The predictive at() and per-level paths cache the engine's `margins` array
  // (each point's `margin` and `se`) rather than `coefs` (bank 68a00bf7
  // `margins, at(mpg=(15 20 25 30))` after logit, 0add3b48 `margins rep78`
  // after regress: Stata plots them).
  if ((!coefs.is_array() || coefs.empty()) && raw.contains("margins") && raw["margins"].is_array())
    coefs = raw["margins"];

  if (!session.lastMarginsDydx.empty()) {
    std::set<std::string> dydxSet(session.lastMarginsDydx.begin(), session.lastMarginsDydx.end());
    ojson filtered = ojson::array();
    for (const auto& c : coefs) {
      if (!c.contains("name") || !c["name"].is_string()) continue;
      // A dydx() row is `x`, a factor effect `1.f`, either at a point `x:2._at`.
      std::string nm = c["name"].get<std::string>();
      const size_t colon = nm.find(':');
      if (colon != std::string::npos) nm = nm.substr(0, colon);
      const size_t dot = nm.find('.');
      if (dydxSet.count(nm) || (dot != std::string::npos && dydxSet.count(nm.substr(dot + 1))))
        filtered.push_back(c);
    }
    coefs = filtered;
  }
  if (coefs.empty()) {
    return engineErrorBlockG("marginsplot: margins result has no estimates to plot.");
  }

  std::string model = raw.value("model", std::string());
  int64_t nObs = raw.contains("n_obs") ? raw["n_obs"].get<int64_t>() : 0;

  // The interval drawn is margins' r(table) ll/ul (marginsplot saves r(table)
  // and plots its ll and ul rows): b -/+ crit se, crit at margins' level from
  // t(e(df_r)) after a least-squares fit, the normal otherwise (`margins rep78`
  // after `regress price i.rep78 mpg`: r(table) ll 801.02 = b - 1.9983 se, the
  // t(63) point, where 1.96 se drew 871.73), from the live margins results.
  const auto& ev = session.eval;
  const std::string* rCmd = ev.rStrings.find("cmd");
  const std::string* eCmd = ev.eStrings.find("cmd");
  const bool rMargins = rCmd != nullptr && *rCmd == "margins";
  const bool eMargins = !rMargins && eCmd != nullptr && *eCmd == "margins";
  double level = static_cast<double>(session.level);
  double dfR = std::numeric_limits<double>::quiet_NaN();
  if (rMargins) {
    if (const double* l = ev.rValues.find("level")) level = *l;
    if (const double* d = ev.rValues.find("df_r")) dfR = *d;
  } else if (eMargins) {
    if (const double* d = ev.eScalars.find("df_r")) dfR = *d;
  }
  const double alpha = level == 95.0 ? 0.05 : 1.0 - level / 100.0;
  const double crit = dfR > 0 ? cecon::t_critical(alpha, dfR) : cecon::z_critical(alpha);
  auto marginsMacro = [&](const char* k) -> std::string {
    const std::string* v = rMargins ? ev.rStrings.find(k) : eMargins ? ev.eStrings.find(k) : nullptr;
    return v != nullptr ? *v : std::string();
  };

  ojson points = ojson::array();
  std::vector<double> allLo, allHi;
  for (const auto& c : coefs) {
    // the engine's `margins` array names a point by its level (`1.rep78`)
    std::string name = c.contains("name") && c["name"].is_string()
                           ? c["name"].get<std::string>()
                           : (c.contains("at") && c["at"].is_string() ? c["at"].get<std::string>() : "?");
    // runMarginsGrid's predictive margins carry `margin` (the grid's coefs).
    ojson bVal = c.contains("dydx") ? c["dydx"]
                 : c.contains("b")  ? c["b"]
                 : (c.contains("margin") ? c["margin"] : ojson(nullptr));
    ojson seVal = c.contains("se") ? c["se"] : ojson(nullptr);
    if (bVal.is_null() || seVal.is_null()) continue;
    double b, se;
    try {
      b = bVal.get<double>();
      se = seVal.get<double>();
    } catch (...) {
      continue;
    }
    double ciLo = b - crit * se, ciHi = b + crit * se;
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
  std::string yLabel = "Average marginal effect (dydx)";
  // Stata's titles, when margins' results name them: margins' title with its
  // first letter raised, `of <term>` for a single marginlist term, `with #%
  // CIs`; the y axis is the prediction's label (else the expression), `Effects
  // on ...` for derivatives ("Predictive margins of rep78 with 95% CIs" /
  // "Linear prediction" after `margins rep78`).
  auto upperFirst = [](std::string t) {
    if (!t.empty()) t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
  };
  if (const std::string mt = marginsMacro("title"); !mt.empty()) {
    std::string t = upperFirst(mt);
    std::string term = marginsMacro("mlist1");  // its variables, blank-separated
    std::replace(term.begin(), term.end(), '#', ' ');
    double kMlist = std::numeric_limits<double>::quiet_NaN();
    if (const double* k = rMargins ? ev.rValues.find("k_mlist") : ev.eScalars.find("k_mlist")) kMlist = *k;
    if (kMlist == 1.0 && !term.empty() && term != "_cons") t += " of " + term;
    char pct[32];
    std::snprintf(pct, sizeof pct, "%g", level);
    titleDefault = t + " with " + pct + "% CIs";
    std::string pl = marginsMacro("predict1_label");
    if (pl.empty()) pl = marginsMacro("expression");
    if (!pl.empty()) {
      const bool derivs = !marginsMacro("derivatives").empty();
      if (derivs) {
        pl[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(pl[0])));
        pl = "Effects on " + pl;
      } else {
        pl = upperFirst(pl);
      }
      for (size_t at = pl.find("pr("); at != std::string::npos; at = pl.find("pr(", at + 3)) pl[at] = 'P';
      yLabel = pl;
    }
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "marginsplot";
  out["title"] = optTitleStr(opts, "title").value_or(titleDefault);
  out["y_label"] = yLabel;
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
