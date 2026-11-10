// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph_export_scene.h -- the display list `graph export` reads from the graph's
// structural SVG (graph_export_svg_reader.cpp) and hands to the vector
// (graph_export_vector.cpp) and raster (graph_export_raster.cpp) encoders.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace piet::handlers {

namespace graph_export_detail {

struct Rgb {
  double r = 0, g = 0, b = 0;
  bool none = false;
};

struct Prim {
  enum Kind { kPath, kCircle, kText } kind = kPath;
  std::vector<std::vector<std::pair<double, double>>> subpaths;  // kPath (closed flags below)
  std::vector<bool> closed;
  double cx = 0, cy = 0, r = 0;  // kCircle
  Rgb fill, stroke;
  double width = 1;
  std::string text;  // kText
  double size = 10;
  std::string anchor = "start";
};

struct Scene {
  double width = 520, height = 360;
  std::vector<Prim> prims;
};

Scene parseSvg(const std::string& svg);
std::string toPdf(const Scene& sc);
std::string toEps(const Scene& sc);
std::string toPng(const Scene& sc, int widthIn, int heightIn);
std::string toJpeg(const Scene& sc, int widthIn, int heightIn, int quality);

}  // namespace graph_export_detail

}  // namespace piet::handlers
