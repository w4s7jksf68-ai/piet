// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "graph_export_scene.h"

namespace piet::handlers {

namespace graph_export_detail {

namespace {

std::string fnum(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.3f", v);
  std::string s = b;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s == "-0") s = "0";
  return s;
}

std::string escapeParen(const std::string& s) {
  std::string out;
  for (unsigned char ch : s) {
    if (ch == '(' || ch == ')' || ch == '\\') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch < 32 || ch > 126) {
      out += '?';
    } else {
      out += static_cast<char>(ch);
    }
  }
  return out;
}

// Helvetica's average advance (0.5 em) for the anchor offset.
double approxWidth(const Prim& p) { return 0.5 * p.size * static_cast<double>(p.text.size()); }

// The shared vector program: PDF and PostScript differ only in operator
// spellings, given here per format. The y axis is flipped once (SVG is y-down).
std::string vectorProgram(const Scene& sc, bool pdf) {
  std::ostringstream o;
  auto color = [&](const Rgb& c, bool fill) {
    o << fnum(c.r) << ' ' << fnum(c.g) << ' ' << fnum(c.b) << ' ' << (pdf ? (fill ? "rg" : "RG") : "setrgbcolor")
      << '\n';
  };
  auto Y = [&](double y) { return sc.height - y; };
  for (const Prim& p : sc.prims) {
    if (p.kind == Prim::kText) {
      if (p.text.empty()) continue;
      double x = p.cx;
      if (p.anchor == "middle") x -= approxWidth(p) / 2;
      else if (p.anchor == "end") x -= approxWidth(p);
      const Rgb c = p.fill.none ? Rgb{} : p.fill;
      color(c, true);
      if (pdf) {
        o << "BT /F1 " << fnum(p.size) << " Tf " << fnum(x) << ' ' << fnum(Y(p.cy)) << " Td (" << escapeParen(p.text)
          << ") Tj ET\n";
      } else {
        o << "/Helvetica findfont " << fnum(p.size) << " scalefont setfont " << fnum(x) << ' ' << fnum(Y(p.cy))
          << " moveto (" << escapeParen(p.text) << ") show\n";
      }
      continue;
    }
    auto pathOut = [&]() {
      if (p.kind == Prim::kCircle) {
        if (pdf) {
          const double k = 0.5522847498 * p.r;
          const double cx = p.cx, cy = Y(p.cy), r = p.r;
          o << fnum(cx + r) << ' ' << fnum(cy) << " m\n";
          o << fnum(cx + r) << ' ' << fnum(cy + k) << ' ' << fnum(cx + k) << ' ' << fnum(cy + r) << ' ' << fnum(cx)
            << ' ' << fnum(cy + r) << " c\n";
          o << fnum(cx - k) << ' ' << fnum(cy + r) << ' ' << fnum(cx - r) << ' ' << fnum(cy + k) << ' '
            << fnum(cx - r) << ' ' << fnum(cy) << " c\n";
          o << fnum(cx - r) << ' ' << fnum(cy - k) << ' ' << fnum(cx - k) << ' ' << fnum(cy - r) << ' ' << fnum(cx)
            << ' ' << fnum(cy - r) << " c\n";
          o << fnum(cx + k) << ' ' << fnum(cy - r) << ' ' << fnum(cx + r) << ' ' << fnum(cy - k) << ' '
            << fnum(cx + r) << ' ' << fnum(cy) << " c h\n";
        } else {
          o << "newpath " << fnum(p.cx) << ' ' << fnum(Y(p.cy)) << ' ' << fnum(p.r) << " 0 360 arc closepath\n";
        }
        return;
      }
      if (!pdf) o << "newpath\n";
      for (size_t s = 0; s < p.subpaths.size(); ++s) {
        const auto& sp = p.subpaths[s];
        for (size_t k = 0; k < sp.size(); ++k) {
          o << fnum(sp[k].first) << ' ' << fnum(Y(sp[k].second)) << ' '
            << (k == 0 ? (pdf ? "m" : "moveto") : (pdf ? "l" : "lineto")) << '\n';
        }
        if (p.closed[s]) o << (pdf ? "h\n" : "closepath\n");
      }
    };
    if (!p.fill.none) {
      color(p.fill, true);
      pathOut();
      o << (pdf ? "f\n" : "fill\n");
    }
    if (!p.stroke.none && p.width > 0) {
      color(p.stroke, false);
      o << fnum(p.width) << (pdf ? " w\n" : " setlinewidth\n");
      pathOut();
      o << (pdf ? "S\n" : "stroke\n");
    }
  }
  return o.str();
}

}  // namespace

std::string toPdf(const Scene& sc) {
  const std::string content = vectorProgram(sc, true);
  std::vector<std::string> objs;
  objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + fnum(sc.width) + " " + fnum(sc.height) +
                 "] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
  objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
  objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<size_t> offs;
  for (size_t k = 0; k < objs.size(); ++k) {
    offs.push_back(out.size());
    out += std::to_string(k + 1) + " 0 obj\n" + objs[k] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
  for (size_t o : offs) {
    char b[32];
    std::snprintf(b, sizeof b, "%010zu 00000 n \n", o);
    out += b;
  }
  out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return out;
}

std::string toEps(const Scene& sc) {
  std::string out = "%!PS-Adobe-3.0 EPSF-3.0\n%%Creator: PIET\n%%BoundingBox: 0 0 " +
                    std::to_string(static_cast<int>(std::ceil(sc.width))) + " " +
                    std::to_string(static_cast<int>(std::ceil(sc.height))) + "\n%%Pages: 1\n%%EndComments\n";
  out += "gsave\n1 setlinejoin 1 setlinecap\n";
  out += vectorProgram(sc, false);
  out += "grestore\nshowpage\n%%EOF\n";
  return out;
}

}  // namespace graph_export_detail

}  // namespace piet::handlers
