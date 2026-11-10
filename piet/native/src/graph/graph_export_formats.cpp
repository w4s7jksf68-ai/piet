// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph_export_formats.cpp -- graph-family AI-13: `graph export` to .png, .pdf
// and .eps written in the requested format at the requested path (w2ai
// gf_exp_069 `as(eps)`, gf_exp_004 `width(1600) height(1080)`, gf_exp_019 .pdf,
// gf_exp_asonly `as(png)` on a suffixless name, log:1922-2011). The source is
// the graph's own structural SVG (buildGraphSvg): its drawing elements are
// read into a display list and replayed as a PostScript program (EPS), a PDF
// content stream (Helvetica, no embedding) and a raster (PNG, stored-deflate
// zlib, no external library). Text is drawn in the vector formats; the raster
// draws the shapes only. JPEG (.jpg) is the same raster, baseline-encoded.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "piet/handlers/graph.h"
#include "graph_export_scene.h"

namespace piet::handlers {

using namespace graph_export_detail;

bool graphSvgToFormat(const std::string& svg, const std::string& fmt, int widthPx, int heightPx,
                      std::string* out) {
  const Scene sc = parseSvg(svg);
  if (fmt == "pdf") {
    *out = toPdf(sc);
    return true;
  }
  if (fmt == "eps" || fmt == "ps") {
    *out = toEps(sc);
    return true;
  }
  if (fmt == "png") {
    *out = toPng(sc, widthPx, heightPx);
    return true;
  }
  if (fmt == "jpg" || fmt == "jpeg") {
    *out = toJpeg(sc, widthPx, heightPx, 90);
    return true;
  }
  return false;
}

}  // namespace piet::handlers
