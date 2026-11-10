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

Rgb parseColor(const std::string& s0) {
  Rgb c;
  std::string s = s0;
  s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }),
          s.end());
  if (s.empty() || s == "none" || s == "transparent") {
    c.none = true;
    return c;
  }
  auto hex = [](char h) -> int {
    if (h >= '0' && h <= '9') return h - '0';
    h = static_cast<char>(std::tolower(static_cast<unsigned char>(h)));
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    return 0;
  };
  if (s[0] == '#') {
    if (s.size() == 7) {
      c.r = (hex(s[1]) * 16 + hex(s[2])) / 255.0;
      c.g = (hex(s[3]) * 16 + hex(s[4])) / 255.0;
      c.b = (hex(s[5]) * 16 + hex(s[6])) / 255.0;
    } else if (s.size() == 4) {
      c.r = hex(s[1]) * 17 / 255.0;
      c.g = hex(s[2]) * 17 / 255.0;
      c.b = hex(s[3]) * 17 / 255.0;
    }
    return c;
  }
  if (s.compare(0, 4, "rgb(") == 0) {
    std::istringstream is(s.substr(4));
    char comma;
    int r = 0, g = 0, b = 0;
    is >> r >> comma >> g >> comma >> b;
    c.r = r / 255.0;
    c.g = g / 255.0;
    c.b = b / 255.0;
    return c;
  }
  static const std::map<std::string, std::array<int, 3>> kNamed = {
      {"black", {0, 0, 0}},      {"white", {255, 255, 255}}, {"red", {255, 0, 0}},
      {"green", {0, 128, 0}},    {"blue", {0, 0, 255}},      {"gray", {128, 128, 128}},
      {"grey", {128, 128, 128}}, {"navy", {0, 0, 128}},      {"orange", {255, 165, 0}},
      {"maroon", {128, 0, 0}},   {"lightgray", {211, 211, 211}}};
  auto it = kNamed.find(s);
  if (it != kNamed.end()) {
    c.r = it->second[0] / 255.0;
    c.g = it->second[1] / 255.0;
    c.b = it->second[2] / 255.0;
  }
  return c;
}

// An affine map (a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f.
struct Affine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  void apply(double x, double y, double* ox, double* oy) const {
    *ox = a * x + c * y + e;
    *oy = b * x + d * y + f;
  }
  Affine then(const Affine& in) const {  // this ∘ in
    Affine r;
    r.a = a * in.a + c * in.b;
    r.b = b * in.a + d * in.b;
    r.c = a * in.c + c * in.d;
    r.d = b * in.c + d * in.d;
    r.e = a * in.e + c * in.f + e;
    r.f = b * in.e + d * in.f + f;
    return r;
  }
  double scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
};

using Attrs = std::map<std::string, std::string>;

double num(const Attrs& a, const char* k, double dflt = 0) {
  auto it = a.find(k);
  if (it == a.end() || it->second.empty()) return dflt;
  return std::atof(it->second.c_str());
}
std::string str(const Attrs& a, const char* k, const std::string& dflt = std::string()) {
  auto it = a.find(k);
  return it == a.end() ? dflt : it->second;
}

Affine parseTransform(const std::string& t) {
  Affine m;
  size_t i = 0;
  while (i < t.size()) {
    const size_t open = t.find('(', i);
    if (open == std::string::npos) break;
    const size_t close = t.find(')', open);
    std::string name = t.substr(i, open - i);
    name.erase(std::remove_if(name.begin(), name.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }),
               name.end());
    std::string args = t.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
    for (char& ch : args)
      if (ch == ',') ch = ' ';
    std::istringstream is(args);
    std::vector<double> v;
    double x;
    while (is >> x) v.push_back(x);
    Affine step;
    if (name == "translate") {
      step.e = v.size() > 0 ? v[0] : 0;
      step.f = v.size() > 1 ? v[1] : 0;
    } else if (name == "scale") {
      step.a = v.size() > 0 ? v[0] : 1;
      step.d = v.size() > 1 ? v[1] : step.a;
    } else if (name == "rotate" && !v.empty()) {
      const double th = v[0] * M_PI / 180.0;
      step.a = std::cos(th);
      step.b = std::sin(th);
      step.c = -std::sin(th);
      step.d = std::cos(th);
    } else if (name == "matrix" && v.size() == 6) {
      step = Affine{v[0], v[1], v[2], v[3], v[4], v[5]};
    }
    m = m.then(step);
    i = close == std::string::npos ? t.size() : close + 1;
  }
  return m;
}

std::string xmlUnescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      const size_t semi = s.find(';', i);
      if (semi != std::string::npos) {
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else out += '?';
        i = semi;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

}  // namespace

// Reads the drawing elements of a structural SVG into a display list in the
// root viewBox's coordinates.
Scene parseSvg(const std::string& svg) {
  Scene sc;
  struct Frame {
    Affine m;
    Rgb fill;
    Rgb stroke;
    bool hasFill = false, hasStroke = false;
    double width = 1;
    bool hasWidth = false;
    std::string tag;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{});
  bool root = true;
  int skipDepth = 0;  // inside <clipPath>/<defs>
  size_t i = 0;
  while (i < svg.size()) {
    const size_t lt = svg.find('<', i);
    if (lt == std::string::npos) break;
    const size_t gt = svg.find('>', lt);
    if (gt == std::string::npos) break;
    std::string tagText = svg.substr(lt + 1, gt - lt - 1);
    i = gt + 1;
    if (tagText.empty() || tagText[0] == '!' || tagText[0] == '?') continue;
    if (tagText[0] == '/') {
      std::string name = tagText.substr(1);
      if (name == "clipPath" || name == "defs") {
        if (skipDepth > 0) --skipDepth;
        continue;
      }
      if ((name == "g" || name == "svg") && stack.size() > 1) stack.pop_back();
      continue;
    }
    const bool selfClose = !tagText.empty() && tagText.back() == '/';
    if (selfClose) tagText.pop_back();
    size_t ns = 0;
    while (ns < tagText.size() && !std::isspace(static_cast<unsigned char>(tagText[ns]))) ++ns;
    const std::string name = tagText.substr(0, ns);
    Attrs at;
    {
      size_t p = ns;
      while (p < tagText.size()) {
        while (p < tagText.size() && std::isspace(static_cast<unsigned char>(tagText[p]))) ++p;
        const size_t eq = tagText.find('=', p);
        if (eq == std::string::npos) break;
        std::string key = tagText.substr(p, eq - p);
        key.erase(std::remove_if(key.begin(), key.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }),
                  key.end());
        size_t q = eq + 1;
        while (q < tagText.size() && std::isspace(static_cast<unsigned char>(tagText[q]))) ++q;
        if (q >= tagText.size()) break;
        const char quote = tagText[q];
        if (quote != '"' && quote != '\'') break;
        const size_t end = tagText.find(quote, q + 1);
        at[key] = tagText.substr(q + 1, end == std::string::npos ? std::string::npos : end - q - 1);
        p = end == std::string::npos ? tagText.size() : end + 1;
      }
      // style="a:b; c:d" as attributes
      auto st = at.find("style");
      if (st != at.end()) {
        std::istringstream is(st->second);
        std::string kv;
        while (std::getline(is, kv, ';')) {
          const size_t colon = kv.find(':');
          if (colon == std::string::npos) continue;
          std::string k = kv.substr(0, colon), v = kv.substr(colon + 1);
          k.erase(std::remove_if(k.begin(), k.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }), k.end());
          while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
          at[k] = v;
        }
      }
    }
    if (name == "clipPath" || name == "defs") {
      if (!selfClose) ++skipDepth;
      continue;
    }
    if (skipDepth > 0) continue;
    Frame top = stack.back();
    auto inherit = [&](Frame& f) {
      if (at.count("fill")) {
        f.fill = parseColor(at["fill"]);
        f.hasFill = true;
      }
      if (at.count("stroke")) {
        f.stroke = parseColor(at["stroke"]);
        f.hasStroke = true;
      }
      if (at.count("stroke-width")) {
        f.width = std::atof(at["stroke-width"].c_str());
        f.hasWidth = true;
      }
    };
    if (name == "svg") {
      Frame f = top;
      std::istringstream vb(str(at, "viewBox"));
      double vx = 0, vy = 0, vw = 0, vh = 0;
      vb >> vx >> vy >> vw >> vh;
      if (root) {
        if (vw > 0 && vh > 0) {
          sc.width = vw;
          sc.height = vh;
        }
        Affine m;
        m.e = -vx;
        m.f = -vy;
        f.m = m;
        root = false;
      } else {
        const double x = num(at, "x"), y = num(at, "y");
        const double w = num(at, "width", vw), h = num(at, "height", vh);
        Affine m;
        const double sx = vw > 0 ? w / vw : 1, sy = vh > 0 ? h / vh : 1;
        m.a = sx;
        m.d = sy;
        m.e = x - vx * sx;
        m.f = y - vy * sy;
        f.m = top.m.then(m);
      }
      inherit(f);
      if (!selfClose) stack.push_back(f);
      continue;
    }
    if (name == "g") {
      Frame f = top;
      if (at.count("transform")) f.m = top.m.then(parseTransform(at["transform"]));
      inherit(f);
      if (!selfClose) stack.push_back(f);
      continue;
    }
    Frame f = top;
    if (at.count("transform")) f.m = top.m.then(parseTransform(at["transform"]));
    inherit(f);
    Prim pr;
    pr.fill = f.hasFill ? f.fill : Rgb{0, 0, 0, false};
    pr.stroke = f.hasStroke ? f.stroke : Rgb{0, 0, 0, true};
    pr.width = (f.hasWidth ? f.width : 1) * f.m.scale();
    auto pt = [&](double x, double y) {
      double ox, oy;
      f.m.apply(x, y, &ox, &oy);
      return std::make_pair(ox, oy);
    };
    if (name == "line") {
      pr.kind = Prim::kPath;
      pr.subpaths.push_back({pt(num(at, "x1"), num(at, "y1")), pt(num(at, "x2"), num(at, "y2"))});
      pr.closed.push_back(false);
      if (!f.hasStroke) pr.stroke = Rgb{0, 0, 0, false};
      pr.fill.none = true;
      sc.prims.push_back(pr);
    } else if (name == "rect") {
      const double x = num(at, "x"), y = num(at, "y"), w = num(at, "width"), h = num(at, "height");
      pr.kind = Prim::kPath;
      pr.subpaths.push_back({pt(x, y), pt(x + w, y), pt(x + w, y + h), pt(x, y + h)});
      pr.closed.push_back(true);
      sc.prims.push_back(pr);
    } else if (name == "circle") {
      pr.kind = Prim::kCircle;
      auto c = pt(num(at, "cx"), num(at, "cy"));
      pr.cx = c.first;
      pr.cy = c.second;
      pr.r = num(at, "r") * f.m.scale();
      sc.prims.push_back(pr);
    } else if (name == "polyline" || name == "polygon") {
      std::string pts = str(at, "points");
      for (char& ch : pts)
        if (ch == ',') ch = ' ';
      std::istringstream is(pts);
      std::vector<std::pair<double, double>> sp;
      double x, y;
      while (is >> x >> y) sp.push_back(pt(x, y));
      pr.kind = Prim::kPath;
      pr.subpaths.push_back(sp);
      pr.closed.push_back(name == "polygon");
      if (name == "polyline" && !f.hasFill) pr.fill.none = true;
      sc.prims.push_back(pr);
    } else if (name == "path") {
      std::string d = str(at, "d");
      std::vector<std::pair<double, double>> cur;
      double px = 0, py = 0;
      char cmd = 'M';
      size_t q = 0;
      auto readNum = [&](double* out) {
        while (q < d.size() && (std::isspace(static_cast<unsigned char>(d[q])) || d[q] == ',')) ++q;
        if (q >= d.size()) return false;
        if (!(std::isdigit(static_cast<unsigned char>(d[q])) || d[q] == '-' || d[q] == '.' || d[q] == '+'))
          return false;
        char* end = nullptr;
        *out = std::strtod(d.c_str() + q, &end);
        q = static_cast<size_t>(end - d.c_str());
        return true;
      };
      auto flush = [&](bool closed) {
        if (cur.size() >= 2) {
          pr.subpaths.push_back(cur);
          pr.closed.push_back(closed);
        }
        cur.clear();
      };
      while (q < d.size()) {
        while (q < d.size() && (std::isspace(static_cast<unsigned char>(d[q])) || d[q] == ',')) ++q;
        if (q >= d.size()) break;
        if (std::isalpha(static_cast<unsigned char>(d[q]))) {
          cmd = d[q++];
          if (cmd == 'Z' || cmd == 'z') {
            flush(true);
            continue;
          }
        }
        double x = 0, y = 0;
        switch (cmd) {
          case 'M':
          case 'm':
            if (!readNum(&x) || !readNum(&y)) {
              q = d.size();
              break;
            }
            flush(false);
            if (cmd == 'm') {
              x += px;
              y += py;
            }
            px = x;
            py = y;
            cur.push_back(pt(px, py));
            cmd = cmd == 'm' ? 'l' : 'L';
            break;
          case 'L':
          case 'l':
            if (!readNum(&x) || !readNum(&y)) {
              q = d.size();
              break;
            }
            if (cmd == 'l') {
              x += px;
              y += py;
            }
            px = x;
            py = y;
            cur.push_back(pt(px, py));
            break;
          case 'H':
          case 'h':
            if (!readNum(&x)) {
              q = d.size();
              break;
            }
            px = cmd == 'h' ? px + x : x;
            cur.push_back(pt(px, py));
            break;
          case 'V':
          case 'v':
            if (!readNum(&y)) {
              q = d.size();
              break;
            }
            py = cmd == 'v' ? py + y : y;
            cur.push_back(pt(px, py));
            break;
          default:
            // curves and arcs: their end point only
            {
              std::vector<double> nums;
              double v;
              while (readNum(&v)) nums.push_back(v);
              if (nums.size() >= 2) {
                x = nums[nums.size() - 2];
                y = nums.back();
                if (std::islower(static_cast<unsigned char>(cmd))) {
                  x += px;
                  y += py;
                }
                px = x;
                py = y;
                cur.push_back(pt(px, py));
              } else {
                q = d.size();
              }
            }
        }
      }
      flush(false);
      pr.kind = Prim::kPath;
      if (!f.hasFill) pr.fill.none = true;
      if (!pr.subpaths.empty()) sc.prims.push_back(pr);
    } else if (name == "text") {
      const size_t close = svg.find("</text>", i);
      std::string content = svg.substr(i, close == std::string::npos ? std::string::npos : close - i);
      // drop nested tags (tspan)
      std::string plain;
      bool inTag = false;
      for (char ch : content) {
        if (ch == '<') inTag = true;
        else if (ch == '>') inTag = false;
        else if (!inTag) plain += ch;
      }
      i = close == std::string::npos ? svg.size() : close + 7;
      pr.kind = Prim::kText;
      auto p0 = pt(num(at, "x"), num(at, "y"));
      pr.cx = p0.first;
      pr.cy = p0.second;
      pr.size = num(at, "font-size", 10) * f.m.scale();
      pr.anchor = str(at, "text-anchor", "start");
      pr.text = xmlUnescape(plain);
      if (!f.hasFill) pr.fill = Rgb{0, 0, 0, false};
      sc.prims.push_back(pr);
    }
  }
  return sc;
}

}  // namespace graph_export_detail

}  // namespace piet::handlers
