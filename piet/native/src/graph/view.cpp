// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <cstddef>
#include <string>
#include <utility>

#include "vendor/nlohmann/json.hpp"

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;


// ===========================================================================
// L2-FIX-B (BUG-066 / COUNCIL-20260710 C3-F5): LIVE-VIEW point decimation.
//
// The graph pipeline shipped and rendered one JSON+SVG node per data point,
// unbounded -- a 108K/1M-point scatter is 108K/1M DOM nodes plus a
// multi-megabyte block copied across the native->frontend bridge, which the
// webview cannot absorb (BUG-066). C3-F5's ruling: density-preserving
// decimation for the LIVE VIEW ONLY with an honest "(showing N of M points)"
// caption; `graph export` keeps ALL points (the golden-pinned SVG builder =
// the REPIN seam) and so does everything else that reads session.lastGraph /
// namedGraphs (graph export/save/combine).
//
// The caller (dispatcher's graph blocks.push_back seams) invokes this AFTER
// the FULL block has already been deep-copied into session.lastGraph (and
// namedGraphs), so thinning the console-bound copy here shrinks ONLY the live
// view -- the export path is untouched, byte-for-byte, no re-pin.
//
// Method: an order-preserving proportional stride (keep every ceil(n/target)-th
// point, always anchoring the last one). Proportional subsampling preserves the
// cloud's relative DENSITY by construction (a region with k times more points
// keeps k times more samples), and order-preservation keeps connected-line /
// tsline shapes intact -- so one primitive serves scatter and line kinds alike.
// The axis ranges (x_min..y_max) are computed by the handler from the FULL data
// and left untouched, so the frame still spans the true extent even when a
// corner point is sampled out.

// Thin a [[x,y],...] array to <= `target` entries in place (order-preserving,
// last point always retained). Returns the resulting (shown) count.
static size_t strideThinPoints(ojson& arr, size_t target) {
  if (!arr.is_array()) return 0;
  const size_t n = arr.size();
  if (target == 0 || n <= target) return n;
  const size_t step = (n + target - 1) / target;  // ceil(n/target), always >= 2
  ojson kept = ojson::array();
  for (size_t i = 0; i < n; i += step) kept.push_back(arr[i]);
  if ((n - 1) % step != 0) kept.push_back(arr[n - 1]);  // anchor the last point
  arr = std::move(kept);
  return arr.size();
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


void decimateGraphBlockForView(ojson& block) {
  if (!block.is_object() || block.value("type", std::string()) != "graph") return;

  static constexpr size_t kMaxViewPoints = 5000;  // DOM/bridge budget per graph
  size_t total = 0, shown = 0;
  auto thinCounted = [&](ojson& pts) {
    if (!pts.is_array()) return;
    total += pts.size();
    shown += strideThinPoints(pts, kMaxViewPoints);
  };

  if (block.contains("layers") && block["layers"].is_array()) {
    // twoway: real data lives in each rendered layer's own points array.
    for (auto& lyr : block["layers"])
      if (lyr.is_object() && lyr.contains("points")) thinCounted(lyr["points"]);
  } else if (block.contains("series") && block["series"].is_array()) {
    // multi-y scatter / xtline / tsline: data lives in series[].points.
    for (auto& s : block["series"])
      if (s.is_object() && s.contains("points")) thinCounted(s["points"]);
    // The flat top-level "points" is native's backward-compat UNION of the
    // series; thin it too so the bridge copy shrinks, but do NOT count it --
    // that would double-count the series points.
    if (block.contains("points") && block["points"].is_array())
      strideThinPoints(block["points"], kMaxViewPoints);
  } else if (block.contains("points") && block["points"].is_array()) {
    // single scatter / line / connected.
    thinCounted(block["points"]);
  }

  if (shown < total) {
    block["n_shown_points"] = shown;
    block["n_total_points"] = total;
  }
}

}  // namespace piet::handlers
