// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
#include "piet/handlers/graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <memory>
#include <cstdio>
#include <fstream>
#include <regex>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"       // the shared Unicode name rule
#include "piet/datastore.h"   // kEsampleColumn
#include "piet/dispatcher.h"  // `estimates esample:` marks its rows through `generate`
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // eScalarKey(): the ONE e()-name table
#include "estimation/estimation_internal.h"  // e(sample) T2's mask writer (esampleRewriteHeadMask)
#include "io/win_compat.h"     // cecon::native_exists / to_native_path
#include "results/stored_estimates_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace stored_estimates_detail;

// ===========================================================================
// coefplot

namespace {

const double kZ975Cp = 1.959963984540054;

// _z_for_level: Abramowitz & Stegun 26.2.17 normal-inverse-CDF approximation.
double zForLevel(int level) {
  double p = (100 - level) / 200.0;
  if (p <= 0) return 3.5;
  double t = std::sqrt(-2.0 * std::log(p));
  const double c0 = 2.515517, c1 = 0.802853, c2 = 0.010328;
  const double d1 = 1.432788, d2 = 0.189269, d3 = 0.001308;
  double num = c0 + c1 * t + c2 * t * t;
  double den = 1 + d1 * t + d2 * t * t + d3 * t * t * t;
  return t - num / den;
}

ojson coeftableFromBlock(const ojson& estBlock) {
  if (estBlock.contains("coeftable")) return estBlock["coeftable"];
  if (estBlock.contains("outcomes") && estBlock["outcomes"].is_array() &&
      !estBlock["outcomes"].empty()) {
    return estBlock["outcomes"][0].value("coeftable", ojson::array());
  }
  return ojson::array();
}

// _read_store: session.estimates (primary) -> session.lastModel() "last"
// fallback (single-model pseudo-store built from the cached raw result).
std::vector<std::pair<std::string, ojson>> readStore(SessionState& session) {
  std::vector<std::pair<std::string, ojson>> out;
  if (!session.estimates.empty()) {
    for (const auto& [k, v] : session.estimates) out.emplace_back(k, v);
    return out;
  }
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return out;
  if (lmOpt->result.has_value() && lmOpt->result->contains("coeftable")) {
    ojson snap;
    snap["coeftable"] = (*lmOpt->result)["coeftable"];
    snap["depvar"] = lmOpt->depvar;
    snap["estimator"] = lmOpt->cobraCmd;
    snap["n_obs"] = lmOpt->result->value("n_obs", ojson(0));
    out.emplace_back("last", snap);
    return out;
  }
  ojson snap;
  snap["coeftable"] = ojson::array();
  snap["depvar"] = lmOpt->depvar;
  snap["estimator"] = lmOpt->cobraCmd;
  snap["n_obs"] = 0;
  out.emplace_back("last", snap);
  return out;
}

}  // namespace

ojson runCoefplot(const std::vector<std::string>& names,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  auto store = readStore(session);
  if (store.empty()) {
    return engineErrorBlockEt(
        "coefplot: no stored estimates. "
        "Run a regression first, or use `eststo name` to store results.");
  }

  std::vector<std::pair<std::string, ojson>> selected;
  // W2 esttabnames (BUG-552): coefplot's model list is a namelist like every
  // other stored-estimates consumer -- `coefplot A_*` / `.` expand through
  // the one shared expansion, in store order. The loop below then looks up
  // exact names (a "." is the active results, built from e()).
  std::vector<std::string> expandedNames;
  if (!names.empty() && !session.estimates.empty()) {
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(names);
    if (ex.rc != 0) return estNamelistError(ex);
    for (const auto& n : ex.names) {
      if (n != ".") {
        expandedNames.push_back(n);
        continue;
      }
      if (!session.lastEstBlock.has_value()) {
        return engineErrorBlockEt(
            "coefplot: the active estimation results (.) carry no coefficient table this "
            "version of PIET can plot; store them first (`estimates store NAME`)");
      }
      store.emplace_back(".", makeSnapshot(*session.lastEstBlock));
      expandedNames.push_back(".");
    }
  } else {
    expandedNames = names;
  }
  if (!names.empty()) {
    for (const auto& nm : expandedNames) {
      bool found = false;
      for (const auto& [k, v] : store) {
        if (k == nm) {
          selected.emplace_back(k, v);
          found = true;
          break;
        }
      }
      if (!found) {
        std::string avail;
        for (size_t i = 0; i < store.size(); ++i) {
          if (i) avail += ", ";
          avail += store[i].first;
        }
        return engineErrorBlockEt("coefplot: model `" + nm +
                                  "` not found in estimates store. Available: " + avail);
      }
    }
  } else {
    selected = store;
  }
  if (selected.empty()) return engineErrorBlockEt("coefplot: no models to plot.");

  int level = 95;
  for (const auto& [k, v] : opts) {
    if (k == "level" && !v.isBool) {
      try {
        int lv = std::stoi(v.strValue);
        if (lv < 1 || lv > 99) {
          return engineErrorBlockEt("coefplot: level(#) must be between 1 and 99.", "syntax_error");
        }
        level = lv;
      } catch (...) {
        return engineErrorBlockEt("coefplot: level(#) must be an integer (e.g. level(90)).",
                                  "syntax_error");
      }
    }
  }
  bool needsRecompute = (level != 95);
  double zAlpha = needsRecompute ? zForLevel(level) : kZ975Cp;

  std::set<std::string> dropSet, keepSet;
  for (const auto& [k, v] : opts) {
    if (k == "drop" && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string tok;
      while (iss >> tok) dropSet.insert(tok);
    }
    if (k == "keep" && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string tok;
      while (iss >> tok) keepSet.insert(tok);
    }
  }

  std::optional<double> xline;
  for (const auto& [k, v] : opts) {
    if (k == "xline") {
      if (!v.isBool) {
        try {
          xline = std::stod(v.strValue);
        } catch (...) {
          return engineErrorBlockEt("coefplot: xline(#) must be numeric.", "syntax_error");
        }
      } else if (v.boolValue) {
        xline = 0.0;
      }
    }
  }

  ojson modelsOut = ojson::array();
  std::vector<double> allValues;
  bool anyCoefs = false;

  for (const auto& [modelName, estBlock] : selected) {
    ojson ct = coeftableFromBlock(estBlock);
    if (ct.empty()) {
      ojson m;
      m["name"] = modelName;
      m["coefs"] = ojson::array();
      modelsOut.push_back(m);
      continue;
    }
    ojson coefsOut = ojson::array();
    for (const auto& row : ct) {
      std::string name = row.value("name", std::string());
      if (dropSet.count(name)) continue;
      if (!keepSet.empty() && !keepSet.count(name)) continue;
      if (!row.contains("b") || row["b"].is_null()) continue;
      double bF;
      try {
        bF = row["b"].get<double>();
      } catch (...) {
        continue;
      }

      double ciLo = std::nan(""), ciHi = std::nan("");
      if (needsRecompute) {
        if (row.contains("se") && !row["se"].is_null()) {
          try {
            double seF = row["se"].get<double>();
            ciLo = bF - zAlpha * seF;
            ciHi = bF + zAlpha * seF;
          } catch (...) {
          }
        }
      } else {
        if (row.contains("ci_lo") && !row["ci_lo"].is_null()) {
          try {
            ciLo = row["ci_lo"].get<double>();
          } catch (...) {
          }
        }
        if (row.contains("ci_hi") && !row["ci_hi"].is_null()) {
          try {
            ciHi = row["ci_hi"].get<double>();
          } catch (...) {
          }
        }
      }

      ojson c;
      c["name"] = name;
      c["b"] = bF;
      c["ci_lo"] = std::isnan(ciLo) ? ojson(nullptr) : ojson(ciLo);
      c["ci_hi"] = std::isnan(ciHi) ? ojson(nullptr) : ojson(ciHi);
      coefsOut.push_back(c);
      anyCoefs = true;

      if (std::isfinite(bF)) allValues.push_back(bF);
      if (std::isfinite(ciLo)) allValues.push_back(ciLo);
      if (std::isfinite(ciHi)) allValues.push_back(ciHi);
    }
    ojson m;
    m["name"] = modelName;
    m["coefs"] = coefsOut;
    modelsOut.push_back(m);
  }

  if (!anyCoefs) {
    return engineErrorBlockEt("coefplot: no coefficients to plot after applying drop/keep filters.");
  }

  if (xline.has_value() && std::isfinite(*xline)) allValues.push_back(*xline);

  double xMin, xMax;
  if (!allValues.empty()) {
    xMin = *std::min_element(allValues.begin(), allValues.end());
    xMax = *std::max_element(allValues.begin(), allValues.end());
    double span = xMax - xMin;
    double pad = span > 0 ? span * 0.05 : 0.5;
    xMin -= pad;
    xMax += pad;
  } else {
    xMin = -1.0;
    xMax = 1.0;
  }

  std::vector<std::string> depvars;
  std::set<std::string> depvarSet;
  for (const auto& [k, v] : selected) {
    std::string dv = v.value("depvar", std::string());
    if (!dv.empty() && depvarSet.insert(dv).second) depvars.push_back(dv);
  }
  std::sort(depvars.begin(), depvars.end());
  std::string yLabelDefault;
  for (size_t i = 0; i < depvars.size(); ++i) {
    if (i) yLabelDefault += ", ";
    yLabelDefault += depvars[i];
  }
  if (yLabelDefault.empty()) yLabelDefault = "Coefficient";

  std::string titleDefault = depvars.empty() ? "Coefficient plot" : "Coefficient plot: " + yLabelDefault;

  std::string title = titleDefault, xLabel = "Coefficient estimate", yLabel = yLabelDefault;
  for (const auto& [k, v] : opts) {
    if (k == "title" && !v.isBool) title = v.strValue;
    if (k == "xtitle" && !v.isBool) xLabel = v.strValue;
    if (k == "ytitle" && !v.isBool) yLabel = v.strValue;
  }

  ojson block;
  block["type"] = "graph";
  block["kind"] = "coefplot";
  block["title"] = title;
  block["x_label"] = xLabel;
  block["y_label"] = yLabel;
  block["models"] = modelsOut;
  block["level"] = level;
  block["n_models"] = static_cast<int64_t>(modelsOut.size());
  block["x_min"] = xMin;
  block["x_max"] = xMax;
  if (xline.has_value()) block["xline"] = *xline;
  block["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011, mirrors BS-010)
  return block;
}

}  // namespace piet::handlers
