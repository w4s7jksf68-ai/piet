// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "io/win_compat.h"  // to_native_path / native_exists: binscatter's savedata files
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch -- binscatter reruns the ado's marksample/regressions
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"  // findUnquotedComma — binscatter reads its options as typed
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/stata_format.h"  // fmt::general — %w.0g text of file write / regress
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle — Arc G's graph half reuses it verbatim

#include "graph_point_layers.h"

namespace piet {
namespace handlers {

namespace engine_report_detail {

// --- Arc G: the graph half of rdplot / binscatter ---------------------------
//
// COMPOSE, DO NOT INVENT. rdplot and binscatter are both, structurally,
// "some binned/pre-averaged points plus one overlay curve" — exactly the
// shape `twoway (scatter y x) (line y x)` already draws. So this section
// builds the SAME block shape runTwowayLayers (twoway.cpp) emits —
// `{"type":"graph","kind":"twoway","layers":[{"kind":"scatter",...},
// {"kind":"line",...}]}` — and hands it to buildSvgTwoway UNCHANGED; no new
// SVG-drawing code exists anywhere in this file or in twoway.cpp. The one
// shared piece pulled in is twoway::layerStyle (piet/twoway_opts.h), the
// exact per-layer color/marker resolver every twoway layer already uses.

// One rendered twoway layer, built exactly the way layerScatter/
// layerLineOrConnected (twoway.cpp) build theirs — `style` comes from
// twoway::layerStyle with an EMPTY opts list (these two commands have no
// per-layer color/symbol options of their own), so a scatter layer gets a
// filled circle and a line layer gets a plain stroked path, same as any
// other twoway plot's default styling.
ojson qeGraphLayer(const std::string& kind, ojson points, int layerIdx) {
  ojson lyr;
  lyr["kind"] = kind;
  const int64_t n = static_cast<int64_t>(points.size());
  lyr["points"] = std::move(points);
  lyr["style"] = twoway::layerStyle({}, layerIdx, kind);
  lyr["n"] = n;
  return lyr;
}

// Assembles the full `{"type":"graph","kind":"twoway",...}` block, field for
// field the same as runTwowayLayers' own tail (twoway.cpp) — x_min/x_max/
// y_min/y_max computed from the layers actually drawn, an empty-data
// fallback to a unit range rather than an error (mirroring runTwowayLayers'
// own "Stata still draws the framed plot ... Never error" rule), and the
// same axes/legend/name/n_obs/suppressed fields every other graph command's
// block carries so `graph export`/`graph save`/`graph combine` (which read
// those fields generically) see nothing unusual about a QE graph.
ojson qeGraphBlock(const std::vector<ojson>& layers, const std::string& title,
                   const std::string& xlabel, const std::string& ylabel,
                   SessionState& session, std::optional<double> xlineAt) {
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  int64_t nObs = 0;
  ojson layersJ = ojson::array();
  for (const auto& lyr : layers) {
    for (const auto& p : lyr.value("points", ojson::array())) {
      double x = p[0].get<double>(), y = p[1].get<double>();
      if (first) {
        xMin = xMax = x;
        yMin = yMax = y;
        first = false;
      } else {
        xMin = std::min(xMin, x);
        xMax = std::max(xMax, x);
        yMin = std::min(yMin, y);
        yMax = std::max(yMax, y);
      }
    }
    nObs += lyr.value("n", 0);
    layersJ.push_back(lyr);
  }
  if (first) {  // no plottable point in any layer
    xMin = 0.0; xMax = 1.0; yMin = 0.0; yMax = 1.0;
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "twoway";
  out["title"] = title;
  out["subtitle"] = std::string();
  out["x_label"] = xlabel;
  out["y_label"] = ylabel;
  out["note"] = std::string();
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["layers"] = layersJ;

  ojson axes;
  axes["xline"] = ojson::array();
  if (xlineAt.has_value()) axes["xline"].push_back(*xlineAt);
  axes["yline"] = ojson::array();
  axes["xscale_noline"] = false;
  axes["yscale_noline"] = false;
  axes["xlabel_none"] = false;
  axes["ylabel_none"] = false;
  out["axes"] = axes;

  ojson legend;
  legend["order"] = ojson::array();
  legend["cols"] = nullptr;
  legend["size"] = nullptr;
  legend["off"] = true;  // neither command has a legend() option to honour
  out["legend"] = legend;

  out["name"] = nullptr;  // rdplot/binscatter accept no name() option (yet)
  out["n_obs"] = nObs;
  out["suppressed"] = session.graphicsOff;
  return out;
}

// The dual-flow cache seam (view.cpp's L2-FIX-B / BUG-066): the FULL
// block goes into session.lastGraph verbatim (so `graph export`/`graph save`
// see every point, byte-identical to what was computed); a SEPARATE copy is
// thinned by decimateGraphBlockForView for the live console/bridge view. The
// two must never alias the same ojson, or the mutation done for the view
// would corrupt the one graph export reads back later.
ojson registerQeGraph(ojson block, SessionState& session) {
  session.lastGraph = block;      // full copy, this is what export/save read
  ojson view = block;             // independent deep copy for the live view
  decimateGraphBlockForView(view);
  return view;
}

}  // namespace engine_report_detail

}  // namespace handlers
}  // namespace piet
