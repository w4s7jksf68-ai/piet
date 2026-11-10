// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "vendor/nlohmann/json.hpp"
#include "piet/twoway_opts.h"

namespace piet::handlers::graph_detail {
using ojson = nlohmann::ordered_json;


static std::string svgEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}


// SVG geometry constants (graph_export.py's _W/_H/_MT/_MR/_MB/_ML).
constexpr double kSvgW = 520.0, kSvgH = 360.0;

constexpr double kSvgMT = 34.0, kSvgMR = 18.0, kSvgMB = 48.0, kSvgML = 62.0;

constexpr double kSvgPlotW = kSvgW - kSvgML - kSvgMR;
   // 440
constexpr double kSvgPlotH = kSvgH - kSvgMT - kSvgMB;
   // 278

static std::vector<double> niceTicksG(double lo, double hi, int target = 5) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0};
  if (lo == hi) return {lo};
  double span = hi - lo;
  double raw = span / target;
  double mag = std::pow(10.0, std::floor(std::log10(raw)));
  double norm = raw / mag;
  double step;
  if (norm < 1.5) step = 1;
  else if (norm < 3) step = 2;
  else if (norm < 7) step = 5;
  else step = 10;
  step *= mag;
  double start = std::ceil(lo / step) * step;
  std::vector<double> ticks;
  for (double t = start; t <= hi + step * 1e-9; t += step) {
    ticks.push_back(std::abs(t) < step * 1e-9 ? 0.0 : t);
  }
  if (ticks.empty()) ticks = {lo, hi};
  return ticks;
}


static std::string trimTrailingZeros(std::string s) {
  if (s.find('.') == std::string::npos) return s;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}


static std::string fmtTickG(double v) {
  if (v == 0) return "0";
  double a = std::abs(v);
  char buf[64];
  if (a >= 1e6) {
    double f = v / 1e6;
    std::snprintf(buf, sizeof(buf), a >= 1e7 ? "%.0fM" : "%.1fM", f);
    return buf;
  }
  if (a >= 1e4) {
    std::snprintf(buf, sizeof(buf), "%.0fk", v / 1e3);
    return buf;
  }
  if (v == std::floor(v)) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
  }
  if (a >= 100) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
  }
  if (a >= 1) {
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return trimTrailingZeros(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.2g", v);
  return trimTrailingZeros(buf);
}


static std::pair<double, double> paddedRangeG(double lo, double hi, double padLo = 0.04, double padHi = 0.04) {
  if (!std::isfinite(lo) || !std::isfinite(hi)) return {0.0, 1.0};
  if (lo == hi) return {lo - 0.5, hi + 0.5};
  double span = hi - lo;
  return {lo - span * padLo, hi + span * padHi};
}


static double svgSx(double x, double x0, double x1, double plotW = kSvgPlotW) {
  return kSvgML + ((x - x0) / (x1 - x0)) * plotW;
}


// Conservative advance-width estimate (SVG has no font metrics here): em
// fractions per character class, one em per non-ASCII code point.
static double svgTextWidth(const std::string& s, double fontSize) {
  double em = 0.0;
  for (unsigned char c : s) {
    if (c >= 0x80) {
      if (c >= 0xC0) em += 1.0;
    } else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') {
      em += 0.9;
    } else if (c >= 'A' && c <= 'Z') {
      em += 0.72;
    } else if (c == ' ' || c == 'i' || c == 'j' || c == 'l' || c == 't' || c == 'f' || c == 'r' || c == 'I' ||
               c == '.' || c == ',' || c == ':' || c == ';' || c == '|' || c == '\'' || c == '!') {
      em += 0.4;
    } else {
      em += 0.6;
    }
  }
  return em * fontSize;
}

static double svgSy(double y, double y0, double y1) {
  return kSvgMT + kSvgPlotH - ((y - y0) / (y1 - y0)) * kSvgPlotH;
}


static char fbuf[64];

static std::string fmtNum(double v, const char* spec = "%.1f") {
  std::snprintf(fbuf, sizeof(fbuf), spec, v);
  return fbuf;
}


static void svgFrameAndAxes(std::vector<std::string>& lines, std::pair<double, double> xr,
                      std::pair<double, double> yr, const std::string& xLabel,
                      const std::string& yLabel, bool suppressXTickLabels = false,
                      bool suppressYTickLabels = false, double plotW = kSvgPlotW) {
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  lines.push_back("  <rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" + fmtNum(kSvgMT, "%.0f") +
                   "\" width=\"" + fmtNum(plotW, "%.0f") + "\" height=\"" + fmtNum(kSvgPlotH, "%.0f") +
                   "\" fill=\"none\" stroke=\"#aaa\" stroke-width=\"1\"/>");

  for (double t : niceTicksG(x0, x1)) {
    double sx = svgSx(t, x0, x1, plotW);
    if (sx < kSvgML - 0.5 || sx > kSvgML + plotW + 0.5) continue;
    double ty = kSvgMT + kSvgPlotH;
    lines.push_back("  <line x1=\"" + fmtNum(sx) + "\" y1=\"" + fmtNum(ty, "%.0f") + "\" x2=\"" +
                     fmtNum(sx) + "\" y2=\"" + fmtNum(ty + 5, "%.0f") +
                     "\" stroke=\"#555\" stroke-width=\"1\"/>");
    // marginsplot (BS-011) suppresses the numeric x tick text -- it places
    // its own categorical term-name labels instead (mirrors graph.js's
    // drawFrameAndAxes(..., {suppressXTickLabels:true}) for the identical
    // reason). The tick MARK still draws either way.
    if (!suppressXTickLabels) {
      lines.push_back("  <text x=\"" + fmtNum(sx) + "\" y=\"" + fmtNum(ty + 18, "%.0f") +
                       "\" text-anchor=\"middle\" font-size=\"10\" fill=\"#444\">" +
                       svgEscape(fmtTickG(t)) + "</text>");
    }
  }

  for (double t : niceTicksG(y0, y1)) {
    double sy = svgSy(t, y0, y1);
    if (sy < kSvgMT - 0.5 || sy > kSvgMT + kSvgPlotH + 0.5) continue;
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML - 5, "%.0f") + "\" y1=\"" + fmtNum(sy) +
                     "\" x2=\"" + fmtNum(kSvgML, "%.0f") + "\" y2=\"" + fmtNum(sy) +
                     "\" stroke=\"#555\" stroke-width=\"1\"/>");
    // coefplot (BS-011) suppresses the numeric y tick text -- it places its
    // own per-coefficient name labels instead (mirrors graph.js's
    // drawFrameAndAxes(..., {suppressYTickLabels:true})).
    if (!suppressYTickLabels) {
      lines.push_back("  <text x=\"" + fmtNum(kSvgML - 9, "%.0f") + "\" y=\"" + fmtNum(sy + 3.5) +
                       "\" text-anchor=\"end\" font-size=\"10\" fill=\"#444\">" + svgEscape(fmtTickG(t)) +
                       "</text>");
    }
  }

  double midX = kSvgML + plotW / 2;
  lines.push_back("  <text x=\"" + fmtNum(midX) + "\" y=\"" + fmtNum(kSvgH - 10, "%.0f") +
                   "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#333\">" + svgEscape(xLabel) +
                   "</text>");
  double midY = kSvgMT + kSvgPlotH / 2;
  lines.push_back("  <text x=\"14\" y=\"" + fmtNum(midY) +
                   "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#333\" transform=\"rotate(-90 14 " +
                   fmtNum(midY) + ")\">" + svgEscape(yLabel) + "</text>");
}


static void svgTitle(std::vector<std::string>& lines, const std::string& title, double width = kSvgW) {
  lines.push_back("  <text x=\"" + fmtNum(width / 2) + "\" y=\"20\" text-anchor=\"middle\" " +
                   "font-size=\"13\" font-weight=\"bold\" fill=\"#111\">" + svgEscape(title) + "</text>");
}


static double jnum(const ojson& v, double dflt = 0.0) {
  if (v.is_null()) return dflt;
  try {
    return v.get<double>();
  } catch (...) {
    return dflt;
  }
}


// jnum's "read a possibly-absent/possibly-null object KEY" counterpart --
// used by buildSvgMarginsplot/buildSvgCoefplot below (BS-011) where a
// coefplot row's ci_lo/ci_hi may be JSON null (no CI for that coefficient;
// see eststo_esttab.cpp's runCoefplot, which stores `ojson(nullptr)` for a
// NaN bound) -- `obj[key]` alone would still be a legal (null) ojson, but
// jnum() only guards a value ALREADY in hand, not a missing key.
static double jnumOr(const ojson& obj, const char* key, double dflt) {
  if (!obj.contains(key) || obj[key].is_null()) return dflt;
  return jnum(obj[key], dflt);
}


// BS-055 (BETA E): shared multi-series renderer -- xtline (`series:
// [{id,label,points}]`) and tsline (`series: [{var,points}]`) both carry
// their data in a top-level `series` array, NOT the flat `points` key every
// OTHER kind uses. buildSvgDispatch had no "xtline"/"tsline" branch at all,
// so both silently fell through to buildSvgScatter's DEFAULT arm, which
// reads a "points" key neither block shape has -- frame/axes/title render
// (svgFrameAndAxes doesn't care about kind), zero data (an empty `pts`
// array), exactly the bug's own description. Also reused by BS-050's
// multi-y `scatter y1 y2 x` -- see buildSvgScatter's delegation below.
// `asMarkers=true` draws small circles (scatter-style); false draws
// connected polylines (xtline/tsline-style) -- matching each kind's own
// natural rendering.
static std::string buildSvgSeriesPlot(const ojson& block, bool asMarkers) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  lines.push_back("  <clipPath id=\"sc\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(kSvgPlotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");
  lines.push_back("  <g clip-path=\"url(#sc)\">");
  ojson series = block.value("series", ojson::array());
  for (size_t si = 0; si < series.size(); ++si) {
    ojson pts = series[si].value("points", ojson::array());
    if (pts.empty()) continue;
    std::string color;
    if (series[si].contains("style") && series[si]["style"].contains("color") &&
        series[si]["style"]["color"].is_string()) {
      color = series[si]["style"]["color"].get<std::string>();
    } else {
      color = twoway::kSeriesColors[si % twoway::kSeriesColors.size()];
    }
    if (!asMarkers) {
      std::string d;
      for (size_t i = 0; i < pts.size(); ++i) {
        double sx = svgSx(jnum(pts[i][0]), xr.first, xr.second);
        double sy = svgSy(jnum(pts[i][1]), yr.first, yr.second);
        d += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
      }
      if (!d.empty()) d.pop_back();
      lines.push_back("    <path d=\"" + d + "\" fill=\"none\" stroke=\"" + color +
                       "\" stroke-width=\"1.5\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/>");
    } else {
      double r = pts.size() > 2000 ? 1.3 : (pts.size() > 500 ? 1.8 : 2.4);
      for (const auto& xy : pts) {
        double cx = svgSx(jnum(xy[0]), xr.first, xr.second);
        double cy = svgSy(jnum(xy[1]), yr.first, yr.second);
        lines.push_back("    <circle cx=\"" + fmtNum(cx) + "\" cy=\"" + fmtNum(cy) + "\" r=\"" +
                         fmtNum(r) + "\" fill=\"" + color + "\" opacity=\"0.8\"/>");
      }
    }
  }
  lines.push_back("  </g>");
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


static std::string buildSvgScatter(const ojson& block) {
  // BS-050: multi-y `scatter y1 y2 x` populates a "series" array (2+
  // entries) alongside the legacy flat "points" -- delegate to the shared
  // multi-series renderer (marker/dot style) so every y-series actually
  // gets its own distinguishable color instead of all points collapsing
  // into one flat #1f77b4 blob. The single-y case (no "series" key, the
  // overwhelmingly common path) is UNCHANGED below.
  if (block.contains("series") && block["series"].is_array() && !block["series"].empty()) {
    return buildSvgSeriesPlot(block, /*asMarkers=*/true);
  }
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  ojson pts = block.value("points", ojson::array());
  double r = pts.size() > 2000 ? 1.3 : (pts.size() > 500 ? 1.8 : 2.4);
  lines.push_back("  <clipPath id=\"gc\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(kSvgPlotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");
  lines.push_back("  <g clip-path=\"url(#gc)\">");
  for (const auto& xy : pts) {
    double cx = svgSx(jnum(xy[0]), xr.first, xr.second);
    double cy = svgSy(jnum(xy[1]), yr.first, yr.second);
    lines.push_back("    <circle cx=\"" + fmtNum(cx) + "\" cy=\"" + fmtNum(cy) + "\" r=\"" +
                     fmtNum(r) + "\" fill=\"#1f77b4\" opacity=\"0.8\"/>");
  }
  lines.push_back("  </g>");
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


static std::string buildSvgHistogram(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))), 0.02, 0.02);
  double yMaxRaw = jnum(block.value("y_max", ojson(0.0)));
  double yMax = yMaxRaw > 0 ? yMaxRaw : 1.0;
  std::pair<double, double> yr = {0.0, yMax * 1.08};
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  double baseY = svgSy(0, yr.first, yr.second);
  for (const auto& b : block.value("bins", ojson::array())) {
    double bx0 = svgSx(jnum(b["x0"]), xr.first, xr.second);
    double bx1 = svgSx(jnum(b["x1"]), xr.first, xr.second);
    double ytop = svgSy(jnum(b["y"]), yr.first, yr.second);
    double w = std::max(0.5, bx1 - bx0 - 0.6);
    double h = std::max(0.0, baseY - ytop);
    lines.push_back("  <rect x=\"" + fmtNum(bx0 + 0.3) + "\" y=\"" + fmtNum(ytop) + "\" width=\"" +
                     fmtNum(w) + "\" height=\"" + fmtNum(h) +
                     "\" fill=\"#1f77b4\" stroke=\"white\" stroke-width=\"0.5\" opacity=\"0.85\"/>");
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


static std::string buildSvgLine(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()));
  ojson pts = block.value("points", ojson::array());
  if (!pts.empty()) {
    std::string d;
    for (size_t i = 0; i < pts.size(); ++i) {
      double cx = svgSx(jnum(pts[i][0]), xr.first, xr.second);
      double cy = svgSy(jnum(pts[i][1]), yr.first, yr.second);
      d += (i == 0 ? "M" : "L") + fmtNum(cx, "%.2f") + " " + fmtNum(cy, "%.2f") + " ";
    }
    if (!d.empty()) d.pop_back();
    lines.push_back("  <path d=\"" + d + "\" fill=\"none\" stroke=\"#1f77b4\" stroke-width=\"1.5\"/>");
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


static std::string buildSvgTwoway(const ojson& block) {
  auto xr = paddedRangeG(jnum(block.value("x_min", ojson(0.0))), jnum(block.value("x_max", ojson(1.0))));
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))));
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;
  // A right-hand legend needs room for its widest rendered label: shrink the
  // plot rect so label text ends inside the viewBox; only if the plot would
  // fall below kMinPlotW widen the viewBox instead.
  ojson layers = block.value("layers", ojson::array());
  ojson legend = block.value("legend", ojson::object());
  constexpr double kLegendGap = 4, kLegendText = 13, kLegendPad = 4, kLegendFont = 9, kMinPlotW = 160;
  double legendLabelW = 0.0;
  bool hasLegend = false;
  // A key's text lines are joined by "\n"; index -1 is text with no key (BUG-1311).
  auto labelLines = [](const ojson& v) {
    std::vector<std::string> out;
    std::stringstream ss(v.is_string() ? v.get<std::string>() : v.dump());
    std::string line;
    while (std::getline(ss, line, '\n')) out.push_back(line);
    if (out.empty()) out.emplace_back();
    return out;
  };
  if (!legend.value("off", false)) {
    for (const auto& pair : legend.value("order", ojson::array())) {
      int layerIdx = pair[0].get<int>();
      if (layerIdx < -1 || layerIdx >= static_cast<int>(layers.size())) continue;
      for (const auto& line : labelLines(pair[1]))
        legendLabelW = std::max(legendLabelW, svgTextWidth(line, kLegendFont));
      hasLegend = true;
    }
  }
  double plotW = kSvgPlotW, viewW = kSvgW;
  if (hasLegend) {
    double legendW = kLegendGap + kLegendText + legendLabelW + kLegendPad;
    plotW = std::min(kSvgPlotW, kSvgW - kSvgML - legendW);
    if (plotW < kMinPlotW) {
      plotW = kMinPlotW;
      viewW = std::ceil(kSvgML + plotW + legendW);
    }
  }
  std::vector<std::string> lines = {"<svg viewBox=\"0 0 " + fmtNum(viewW, "%.0f") +
                                     " 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()), viewW);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()),
                  false, false, plotW);

  lines.push_back("  <clipPath id=\"tw-clip\"><rect x=\"" + fmtNum(kSvgML, "%.0f") + "\" y=\"" +
                   fmtNum(kSvgMT, "%.0f") + "\" width=\"" + fmtNum(plotW, "%.0f") + "\" height=\"" +
                   fmtNum(kSvgPlotH, "%.0f") + "\"/></clipPath>");

  ojson axes = block.value("axes", ojson::object());
  for (const auto& yv : axes.value("yline", ojson::array())) {
    double syRef = svgSy(jnum(yv), y0, y1);
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML, "%.0f") + "\" y1=\"" + fmtNum(syRef) + "\" x2=\"" +
                     fmtNum(kSvgML + plotW, "%.0f") + "\" y2=\"" + fmtNum(syRef) +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>");
  }
  for (const auto& xv : axes.value("xline", ojson::array())) {
    double sxRef = svgSx(jnum(xv), x0, x1, plotW);
    lines.push_back("  <line x1=\"" + fmtNum(sxRef) + "\" y1=\"" + fmtNum(kSvgMT, "%.0f") + "\" x2=\"" +
                     fmtNum(sxRef) + "\" y2=\"" + fmtNum(kSvgMT + kSvgPlotH, "%.0f") +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"4 3\"/>");
  }

  lines.push_back("  <g clip-path=\"url(#tw-clip)\">");
  for (const auto& layer : layers) {
    std::string lkind = layer.value("kind", std::string("line"));
    ojson style = layer.value("style", ojson::object());

    // serset-twoway FIT: lfitci/qfitci draw their band like rarea, then their
    // fitted line like lfit/qfit (the two branches below are no longer
    // exclusive).
    const bool fitCi = lkind == "lfitci" || lkind == "qfitci";
    if (lkind == "rarea" || fitCi) {
      ojson band = layer.value("band", ojson::array());
      if (band.size() >= 2) {
        std::string color = style.value("fcolor", std::string("#aec7e8"));
        double opacity = jnum(style.value("opacity", ojson(0.3)), 0.3);
        std::string lcolor = style.value("lcolor", std::string("none"));
        double lwidth = jnum(style.value("lwidth", ojson(0.0)), 0.0);
        std::string fwd, rev;
        for (size_t i = 0; i < band.size(); ++i) {
          double sx = svgSx(jnum(band[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(band[i][2]), y0, y1);
          fwd += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        for (size_t i = band.size(); i-- > 0;) {
          double sx = svgSx(jnum(band[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(band[i][1]), y0, y1);
          rev += "L" + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        std::string d = fwd + rev + "Z";
        std::string strokeAttr = (lwidth > 0 && lcolor != "none")
                                      ? ("stroke=\"" + lcolor + "\" stroke-width=\"" + fmtNum(lwidth) + "\"")
                                      : "stroke=\"none\"";
        lines.push_back("    <path d=\"" + d + "\" fill=\"" + color + "\" fill-opacity=\"" +
                         fmtNum(opacity, "%.3g") + "\" " + strokeAttr + "/>");
      }
    }
    if (lkind == "histogram") {
      // serset-twoway HKL: a histogram layer's bars.
      std::string color = style.value("fcolor", style.value("color", std::string("#1f77b4")));
      for (const auto& b : layer.value("bars", ojson::array())) {
        const double bx0 = svgSx(b.value("x0", 0.0), x0, x1, plotW);
        const double bx1 = svgSx(b.value("x1", 0.0), x0, x1, plotW);
        const double by = svgSy(b.value("y", 0.0), y0, y1);
        const double by0 = svgSy(0.0, y0, y1);
        lines.push_back("    <rect x=\"" + fmtNum(std::min(bx0, bx1), "%.2f") + "\" y=\"" +
                        fmtNum(std::min(by, by0), "%.2f") + "\" width=\"" +
                        fmtNum(std::fabs(bx1 - bx0), "%.2f") + "\" height=\"" +
                        fmtNum(std::fabs(by0 - by), "%.2f") + "\" fill=\"" + color +
                        "\" fill-opacity=\"0.6\" stroke=\"#ffffff\" stroke-width=\"0.5\"/>");
      }
    }
    if (layer.contains("pc")) {
      // serset-twoway HIST-5: pci/pcarrowi segments.
      std::string color = style.value("color", style.value("lcolor", std::string("#1f77b4")));
      for (const auto& sg : layer["pc"]) {
        lines.push_back("    <line x1=\"" + fmtNum(svgSx(jnum(sg[0]), x0, x1, plotW), "%.2f") + "\" y1=\"" +
                        fmtNum(svgSy(jnum(sg[1]), y0, y1), "%.2f") + "\" x2=\"" +
                        fmtNum(svgSx(jnum(sg[2]), x0, x1, plotW), "%.2f") + "\" y2=\"" +
                        fmtNum(svgSy(jnum(sg[3]), y0, y1), "%.2f") + "\" stroke=\"" + color +
                        "\" stroke-width=\"1.5\"/>");
      }
    }
    if (layer.contains("segments")) {
      // serset-twoway RNG: one vertical segment per observation (rcap & co).
      std::string color = style.value("color", style.value("lcolor", std::string("#1f77b4")));
      for (const auto& sg : layer["segments"]) {
        const double sx = svgSx(jnum(sg[0]), x0, x1, plotW);
        lines.push_back("    <line x1=\"" + fmtNum(sx, "%.2f") + "\" y1=\"" +
                        fmtNum(svgSy(jnum(sg[1]), y0, y1), "%.2f") + "\" x2=\"" + fmtNum(sx, "%.2f") +
                        "\" y2=\"" + fmtNum(svgSy(jnum(sg[2]), y0, y1), "%.2f") + "\" stroke=\"" +
                        color + "\" stroke-width=\"1\"/>");
      }
    }
    if (layer.contains("base") && layer.contains("points")) {
      // serset-twoway RNG: area/spike/dropline/bar to their base.
      std::string color = style.value("fcolor", style.value("color", std::string("#1f77b4")));
      const double by = svgSy(layer.value("base", 0.0), y0, y1);
      ojson pts = layer["points"];
      if (lkind == "area" && pts.size() >= 2) {
        std::string d = "M" + fmtNum(svgSx(jnum(pts[0][0]), x0, x1, plotW), "%.2f") + " " + fmtNum(by, "%.2f") + " ";
        for (const auto& p : pts)
          d += "L" + fmtNum(svgSx(jnum(p[0]), x0, x1, plotW), "%.2f") + " " +
               fmtNum(svgSy(jnum(p[1]), y0, y1), "%.2f") + " ";
        d += "L" + fmtNum(svgSx(jnum(pts[pts.size() - 1][0]), x0, x1, plotW), "%.2f") + " " +
             fmtNum(by, "%.2f") + " Z";
        lines.push_back("    <path d=\"" + d + "\" fill=\"" + color + "\" fill-opacity=\"0.5\" stroke=\"none\"/>");
      } else {
        for (const auto& p : pts) {
          const double sx = svgSx(jnum(p[0]), x0, x1, plotW);
          lines.push_back("    <line x1=\"" + fmtNum(sx, "%.2f") + "\" y1=\"" + fmtNum(by, "%.2f") +
                          "\" x2=\"" + fmtNum(sx, "%.2f") + "\" y2=\"" +
                          fmtNum(svgSy(jnum(p[1]), y0, y1), "%.2f") + "\" stroke=\"" + color +
                          "\" stroke-width=\"1.5\"/>");
        }
      }
    }
    const bool kdLine = lkind == "kdensity";
    if (lkind == "line" || lkind == "connected" || lkind == "scatter" || lkind == "lfit" ||
        lkind == "qfit" || fitCi || kdLine) {
      // BUG-099: lfit/qfit's `points` shape is identical to line's (a
      // 2-point line / a 100-point sampled curve, both plain [x,y] pairs),
      // so they render through this SAME polyline branch -- no marker dots
      // (the `lkind == "scatter" || lkind == "connected"` guard below
      // deliberately excludes them, matching Stata: a fit line never plots
      // marker symbols).
      ojson pts = layer.value("points", ojson::array());
      if (pts.empty()) continue;
      std::string color = style.value("color", std::string("#1f77b4"));
      double lwidth = jnum(style.value("lwidth", ojson(1.5)), 1.5);
      std::string lpattern = style.contains("lpattern") && !style["lpattern"].is_null()
                                  ? style["lpattern"].get<std::string>()
                                  : std::string();
      std::string msymbol = style.contains("msymbol") && !style["msymbol"].is_null()
                                 ? style["msymbol"].get<std::string>()
                                 : std::string();
      double msize = jnum(style.value("msize", ojson(2.4)), 2.4);
      bool hollow = style.value("hollow", false);

      if ((lkind == "line" || lkind == "connected" || lkind == "lfit" || lkind == "qfit" || fitCi ||
           kdLine) &&
          lwidth > 0) {
        std::string d;
        for (size_t i = 0; i < pts.size(); ++i) {
          double sx = svgSx(jnum(pts[i][0]), x0, x1, plotW);
          double sy = svgSy(jnum(pts[i][1]), y0, y1);
          d += (i == 0 ? "M" : "L") + fmtNum(sx, "%.2f") + " " + fmtNum(sy, "%.2f") + " ";
        }
        if (!d.empty()) d.pop_back();
        std::string dashAttr = lpattern.empty() ? "" : (" stroke-dasharray=\"" + lpattern + "\"");
        lines.push_back("    <path d=\"" + d + "\" fill=\"none\" stroke=\"" + color +
                         "\" stroke-width=\"" + fmtNum(lwidth) +
                         "\" stroke-linejoin=\"round\" stroke-linecap=\"round\"" + dashAttr + "/>");
      }
      if ((lkind == "scatter" || lkind == "connected") && !msymbol.empty() && msymbol != "none") {
        std::string fill = hollow ? "none" : color;
        double sw = hollow ? lwidth : 0.0;
        for (const auto& pt : pts) {
          double cx = svgSx(jnum(pt[0]), x0, x1, plotW);
          double cy = svgSy(jnum(pt[1]), y0, y1);
          lines.push_back("    <circle cx=\"" + fmtNum(cx, "%.2f") + "\" cy=\"" + fmtNum(cy, "%.2f") +
                           "\" r=\"" + fmtNum(msize) + "\" fill=\"" + fill + "\" stroke=\"" + color +
                           "\" stroke-width=\"" + fmtNum(sw) + "\"/>");
        }
      }
    }
  }
  lines.push_back("  </g>");

  if (!legend.value("off", false)) {
    ojson order = legend.value("order", ojson::array());
    if (!order.empty()) {
      double lx = kSvgML + plotW + kLegendGap;
      int li = 0;
      for (const auto& pair : order) {
        int layerIdx = pair[0].get<int>();
        // FU-113: an index with no matching plot is ignored WITHOUT consuming a
        // row (the counter used to advance before this check, leaving a gap).
        if (layerIdx < -1 || layerIdx >= static_cast<int>(layers.size())) continue;
        double ly = kSvgMT + 14 + li * 14;
        if (layerIdx >= 0) {
          ojson lst = layers[static_cast<size_t>(layerIdx)].value("style", ojson::object());
          std::string color = lst.value("color", lst.value("fcolor", std::string("#1f77b4")));
          lines.push_back("  <line x1=\"" + fmtNum(lx, "%.0f") + "\" y1=\"" + fmtNum(ly - 3) + "\" x2=\"" +
                           fmtNum(lx + 10, "%.0f") + "\" y2=\"" + fmtNum(ly - 3) + "\" stroke=\"" + color +
                           "\" stroke-width=\"1.5\"/>");
        }
        // one row per text line, the key on the first (BUG-1311)
        for (const auto& line : labelLines(pair[1])) {
          ly = kSvgMT + 14 + li * 14;
          ++li;
          lines.push_back("  <text x=\"" + fmtNum(lx + kLegendText, "%.0f") + "\" y=\"" + fmtNum(ly) +
                           "\" text-anchor=\"start\" font-size=\"9\" fill=\"#333\">" + svgEscape(line) +
                           "</text>");
        }
      }
    }
  }
  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


// BS-011 (BETA B-graph-B): coefplot/marginsplot SVG export builders. Neither
// kind had a native (headless `--do`) SVG renderer before -- unreachable
// until now because session.lastGraph was never populated for these two
// kinds (see runCoefplot's/runMarginsplot's dispatcher call sites), so
// `graph export`/`graph save`+`graph combine` after a coefplot/marginsplot
// always hit the "no graph in memory" error and buildSvgDispatch's kind
// table never needed an entry for them. Wiring lastGraph WITHOUT also
// adding these two branches would have swapped that honest error for a
// crash/garbage render (buildSvgScatter indexing `points[i][0]` on a block
// whose points/models are shaped completely differently) -- so the two go
// together. Geometry mirrors piet/frontend/js/graph.js's renderMarginsplot/
// renderCoefplot exactly (categorical axis + whisker-and-dot layout); the
// color palette matches THIS file's existing scatter/line/twoway builders
// (#1f77b4 points, #888 dashed reference line) rather than graph.js's CSS
// theme tokens -- the native SVG export has always been a fixed, theme-
// independent palette (see buildSvgScatter et al. above), never the live
// console's --graph-accent-* variables.

static std::string buildSvgMarginsplot(const ojson& block) {
  ojson pts = block.value("points", ojson::array());
  int n = static_cast<int>(pts.size());

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));

  // Categorical x axis: one evenly-spaced slot per AME term (1..n); numeric
  // x tick text suppressed -- the term-name labels below take its place
  // (graph.js's identical suppressXTickLabels rationale).
  auto xr = paddedRangeG(0.5, n + 0.5, 0.05, 0.05);
  auto yr = paddedRangeG(jnum(block.value("y_min", ojson(0.0))), jnum(block.value("y_max", ojson(1.0))), 0.05, 0.05);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string()), block.value("y_label", std::string()),
                  /*suppressXTickLabels=*/true, /*suppressYTickLabels=*/false);
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  double zy = svgSy(0.0, y0, y1);
  if (zy >= kSvgMT - 0.5 && zy <= kSvgMT + kSvgPlotH + 0.5) {
    lines.push_back("  <line x1=\"" + fmtNum(kSvgML, "%.0f") + "\" y1=\"" + fmtNum(zy) + "\" x2=\"" +
                     fmtNum(kSvgML + kSvgPlotW, "%.0f") + "\" y2=\"" + fmtNum(zy) +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"3 3\"/>");
  }

  double labelY = kSvgMT + kSvgPlotH + 18;
  bool rotate = n > 4;
  for (int i = 0; i < n; ++i) {
    double sx = svgSx(i + 1.0, x0, x1);
    std::string name = pts[static_cast<size_t>(i)].value("name", std::string("?"));
    std::string transformAttr =
        rotate ? (" transform=\"rotate(-35 " + fmtNum(sx) + " " + fmtNum(labelY, "%.0f") + ")\"") : "";
    lines.push_back("  <text x=\"" + fmtNum(sx) + "\" y=\"" + fmtNum(labelY, "%.0f") + "\" text-anchor=\"" +
                     (rotate ? "end" : "middle") + "\" font-size=\"10\" fill=\"#444\"" + transformAttr + ">" +
                     svgEscape(name) + "</text>");
  }

  for (int i = 0; i < n; ++i) {
    const ojson& d = pts[static_cast<size_t>(i)];
    double sx = svgSx(i + 1.0, x0, x1);
    double b = jnumOr(d, "b", 0.0);
    double ciLo = jnumOr(d, "ci_lo", b);
    double ciHi = jnumOr(d, "ci_hi", b);
    double syLo = svgSy(ciLo, y0, y1), syHi = svgSy(ciHi, y0, y1), syB = svgSy(b, y0, y1);
    double capLen = 4;
    lines.push_back("  <line x1=\"" + fmtNum(sx) + "\" y1=\"" + fmtNum(syLo) + "\" x2=\"" + fmtNum(sx) +
                     "\" y2=\"" + fmtNum(syHi) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <line x1=\"" + fmtNum(sx - capLen) + "\" y1=\"" + fmtNum(syLo) + "\" x2=\"" +
                     fmtNum(sx + capLen) + "\" y2=\"" + fmtNum(syLo) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <line x1=\"" + fmtNum(sx - capLen) + "\" y1=\"" + fmtNum(syHi) + "\" x2=\"" +
                     fmtNum(sx + capLen) + "\" y2=\"" + fmtNum(syHi) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    lines.push_back("  <circle cx=\"" + fmtNum(sx) + "\" cy=\"" + fmtNum(syB) + "\" r=\"3\" fill=\"#1f77b4\"/>");
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


static std::string buildSvgCoefplot(const ojson& block) {
  struct Row {
    std::string label;
    double b;
    bool hasCI;
    double ciLo, ciHi;
  };
  std::vector<Row> rows;
  ojson models = block.value("models", ojson::array());
  bool multi = models.size() > 1;
  for (const auto& m : models) {
    std::string mname = m.value("name", std::string());
    for (const auto& c : m.value("coefs", ojson::array())) {
      std::string cname = c.value("name", std::string());
      bool hasCI = c.contains("ci_lo") && !c["ci_lo"].is_null() && c.contains("ci_hi") && !c["ci_hi"].is_null();
      rows.push_back({multi ? (mname + ":" + cname) : cname, jnumOr(c, "b", std::nan("")), hasCI,
                      hasCI ? jnumOr(c, "ci_lo", 0.0) : 0.0, hasCI ? jnumOr(c, "ci_hi", 0.0) : 0.0});
    }
  }

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"};
  svgTitle(lines, block.value("title", std::string()));

  int n = static_cast<int>(rows.size());

  // X: coefficient value, always widened to include 0 so the zero reference
  // line is in frame (mirrors renderCoefplot's `Math.min(0, xlo)`); falls
  // back to deriving a range from the rows themselves only if the block's
  // own x_min/x_max are missing/non-finite (runCoefplot always sets both,
  // so this fallback is defensive, not the normal path).
  double xlo = jnum(block.value("x_min", ojson(std::nan(""))), std::nan(""));
  double xhi = jnum(block.value("x_max", ojson(std::nan(""))), std::nan(""));
  if (!std::isfinite(xlo) || !std::isfinite(xhi)) {
    bool any = false;
    xlo = xhi = 0.0;
    for (const auto& r : rows) {
      std::vector<double> vs = {r.b};
      if (r.hasCI) { vs.push_back(r.ciLo); vs.push_back(r.ciHi); }
      for (double v : vs) {
        if (!std::isfinite(v)) continue;
        if (!any) { xlo = xhi = v; any = true; } else { xlo = std::min(xlo, v); xhi = std::max(xhi, v); }
      }
    }
  }
  xlo = std::min(xlo, 0.0);
  xhi = std::max(xhi, 0.0);
  auto xr = paddedRangeG(xlo, xhi, 0.05, 0.05);
  auto yr = paddedRangeG(0.5, n + 0.5, 0.05, 0.05);
  svgFrameAndAxes(lines, xr, yr, block.value("x_label", std::string("Coefficient estimate")), std::string(),
                  /*suppressXTickLabels=*/false, /*suppressYTickLabels=*/true);
  auto [x0, x1] = xr;
  auto [y0, y1] = yr;

  double zx = svgSx(0.0, x0, x1);
  if (zx >= kSvgML - 0.5 && zx <= kSvgML + kSvgPlotW + 0.5) {
    lines.push_back("  <line x1=\"" + fmtNum(zx) + "\" y1=\"" + fmtNum(kSvgMT, "%.0f") + "\" x2=\"" +
                     fmtNum(zx) + "\" y2=\"" + fmtNum(kSvgMT + kSvgPlotH, "%.0f") +
                     "\" stroke=\"#888\" stroke-width=\"1\" stroke-dasharray=\"3 3\"/>");
  }

  for (int i = 0; i < n; ++i) {
    double sy = svgSy(n - i, y0, y1);
    lines.push_back("  <text x=\"" + fmtNum(kSvgML - 9, "%.0f") + "\" y=\"" + fmtNum(sy + 3.5) +
                     "\" text-anchor=\"end\" font-size=\"10\" fill=\"#444\">" + svgEscape(rows[static_cast<size_t>(i)].label) +
                     "</text>");
  }

  for (int i = 0; i < n; ++i) {
    const Row& r = rows[static_cast<size_t>(i)];
    double sy = svgSy(n - i, y0, y1);
    if (r.hasCI) {
      double sxLo = svgSx(r.ciLo, x0, x1), sxHi = svgSx(r.ciHi, x0, x1);
      double capLen = 4;
      lines.push_back("  <line x1=\"" + fmtNum(sxLo) + "\" y1=\"" + fmtNum(sy) + "\" x2=\"" + fmtNum(sxHi) +
                       "\" y2=\"" + fmtNum(sy) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
      lines.push_back("  <line x1=\"" + fmtNum(sxLo) + "\" y1=\"" + fmtNum(sy - capLen) + "\" x2=\"" + fmtNum(sxLo) +
                       "\" y2=\"" + fmtNum(sy + capLen) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
      lines.push_back("  <line x1=\"" + fmtNum(sxHi) + "\" y1=\"" + fmtNum(sy - capLen) + "\" x2=\"" + fmtNum(sxHi) +
                       "\" y2=\"" + fmtNum(sy + capLen) + "\" stroke=\"#1f77b4\" stroke-width=\"1.2\"/>");
    }
    if (std::isfinite(r.b)) {
      double sxB = svgSx(r.b, x0, x1);
      lines.push_back("  <circle cx=\"" + fmtNum(sxB) + "\" cy=\"" + fmtNum(sy) + "\" r=\"3\" fill=\"#1f77b4\"/>");
    }
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


static std::string buildSvgDispatch(const ojson& block);
  // fwd decl for combine's recursive call

static std::string buildSvgCombine(const ojson& block) {
  ojson grid = block.value("grid", ojson::object());
  int rows = std::max(1, grid.value("rows", 1));
  int cols = std::max(1, grid.value("cols", 1));
  double totalW = cols * kSvgW, totalH = rows * kSvgH;

  std::vector<std::string> lines = {"<svg viewBox=\"0 0 " + fmtNum(totalW, "%.0f") + " " +
                                     fmtNum(totalH, "%.0f") + "\" xmlns=\"http://www.w3.org/2000/svg\">"};

  // graphregion_color is always present in the block dict but may be JSON null
  // (graph_combine.py: unset graphregion(color(...)) opt leaves it None). A
  // null-but-present key makes nlohmann::json::value()'s default inapplicable
  // (defaults only cover an ABSENT key), so read it null-safely here — matching
  // the oracle's `if bg and bg != "none"` falsy-check (None/empty ⇒ no rect
  // painted, i.e. no default color substitution when unspecified).
  ojson bgJ = block.value("graphregion_color", ojson());
  std::string bg = bgJ.is_string() ? bgJ.get<std::string>() : std::string();
  if (!bg.empty() && bg != "none") {
    lines.push_back("  <rect x=\"0\" y=\"0\" width=\"" + fmtNum(totalW, "%.0f") + "\" height=\"" +
                     fmtNum(totalH, "%.0f") + "\" fill=\"" + bg + "\"/>");
  }

  std::string title = block.value("title", std::string());
  if (!title.empty()) {
    lines.push_back("  <text x=\"" + fmtNum(totalW / 2) + "\" y=\"20\" text-anchor=\"middle\" " +
                     "font-size=\"13\" font-weight=\"bold\" fill=\"#111\">" + svgEscape(title) + "</text>");
  }

  ojson panels = block.value("panels", ojson::array());
  for (size_t idx = 0; idx < panels.size(); ++idx) {
    int row = static_cast<int>(idx) / cols;
    int col = static_cast<int>(idx) % cols;
    if (row >= rows) break;
    double tx = col * kSvgW, ty = row * kSvgH;
    std::string subSvg;
    try {
      subSvg = buildSvgDispatch(panels[idx]);
    } catch (...) {
      subSvg = "<svg viewBox=\"0 0 520 360\" xmlns=\"http://www.w3.org/2000/svg\">"
                "<text x=\"10\" y=\"20\" font-size=\"10\" fill=\"red\">render error</text></svg>";
    }
    size_t bodyStart = subSvg.find('>');
    size_t bodyEnd = subSvg.rfind("</svg>");
    std::string inner = (bodyStart != std::string::npos && bodyEnd != std::string::npos && bodyEnd > bodyStart)
                             ? subSvg.substr(bodyStart + 1, bodyEnd - bodyStart - 1)
                             : std::string();
    // A panel widened past one cell (long legend labels) is scaled to fit its cell.
    std::string scaleAttr;
    double subW = kSvgW;
    size_t vb = subSvg.find("viewBox=\"0 0 ");
    if (vb != std::string::npos) subW = std::atof(subSvg.c_str() + vb + 13);
    if (subW > kSvgW + 0.5) scaleAttr = " scale(" + fmtNum(kSvgW / subW, "%.4f") + ")";
    lines.push_back("  <g transform=\"translate(" + fmtNum(tx, "%.0f") + "," + fmtNum(ty, "%.0f") + ")" +
                     scaleAttr + "\">");
    lines.push_back(inner);
    lines.push_back("  </g>");
  }

  lines.push_back("</svg>");
  std::string out;
  for (auto& l : lines) { out += l; out += "\n"; }
  if (!out.empty()) out.pop_back();
  return out;
}


static std::string buildSvgDispatch(const ojson& block) {
  std::string kind = block.value("kind", std::string("scatter"));
  if (kind == "histogram") return buildSvgHistogram(block);
  if (kind == "line") return buildSvgLine(block);
  // BS-055: xtline/tsline previously had NO branch here at all -- both fell
  // through to buildSvgScatter, which reads a top-level "points" key
  // neither block shape carries (their data lives in "series"). Result:
  // frame/axes/title rendered fine (kind-agnostic), zero data ever drawn.
  if (kind == "xtline" || kind == "tsline") return buildSvgSeriesPlot(block, /*asMarkers=*/false);
  if (kind == "twoway") return buildSvgTwoway(block);
  if (kind == "graph_combine") return buildSvgCombine(block);
  if (kind == "marginsplot") return buildSvgMarginsplot(block);
  if (kind == "coefplot") return buildSvgCoefplot(block);
  return buildSvgScatter(block);
}

}  // namespace piet::handlers::graph_detail

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace graph_detail;


std::string buildGraphSvg(const ojson& block) { return buildSvgDispatch(block); }

}  // namespace piet::handlers
