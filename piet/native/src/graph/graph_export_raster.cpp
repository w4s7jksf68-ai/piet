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

// ---- PNG ----

uint32_t crc32Of(const unsigned char* p, size_t n, uint32_t crc = 0xffffffffu) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return crc;
}

void put32(std::string& s, uint32_t v) {
  s += static_cast<char>((v >> 24) & 0xff);
  s += static_cast<char>((v >> 16) & 0xff);
  s += static_cast<char>((v >> 8) & 0xff);
  s += static_cast<char>(v & 0xff);
}

void pngChunk(std::string& out, const char* type, const std::string& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::string td = std::string(type, 4) + data;
  out += td;
  const uint32_t crc = crc32Of(reinterpret_cast<const unsigned char*>(td.data()), td.size()) ^ 0xffffffffu;
  put32(out, crc);
}

struct Canvas {
  int w, h;
  std::vector<unsigned char> px;  // RGB
  Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * static_cast<size_t>(h_) * 3, 255) {}
  void set(int x, int y, const Rgb& c) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
    px[i] = static_cast<unsigned char>(std::lround(c.r * 255));
    px[i + 1] = static_cast<unsigned char>(std::lround(c.g * 255));
    px[i + 2] = static_cast<unsigned char>(std::lround(c.b * 255));
  }
  void dot(double x, double y, double rad, const Rgb& c) {
    const int r = std::max(0, static_cast<int>(std::ceil(rad)));
    const int cx = static_cast<int>(std::lround(x)), cy = static_cast<int>(std::lround(y));
    for (int dy = -r; dy <= r; ++dy)
      for (int dx = -r; dx <= r; ++dx)
        if (r == 0 || dx * dx + dy * dy <= r * r) set(cx + dx, cy + dy, c);
  }
  void line(double x0, double y0, double x1, double y1, double width, const Rgb& c) {
    const double len = std::hypot(x1 - x0, y1 - y0);
    const int steps = std::max(1, static_cast<int>(std::ceil(len * 2)));
    const double rad = std::max(0.0, width / 2 - 0.5);
    for (int s = 0; s <= steps; ++s) {
      const double t = static_cast<double>(s) / steps;
      dot(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, rad, c);
    }
  }
  // even-odd scanline fill of closed polygons
  void fillPolys(const std::vector<std::vector<std::pair<double, double>>>& polys, const Rgb& c) {
    double ymin = 1e300, ymax = -1e300;
    for (const auto& p : polys)
      for (const auto& q : p) {
        ymin = std::min(ymin, q.second);
        ymax = std::max(ymax, q.second);
      }
    if (ymin > ymax) return;
    const int y0 = std::max(0, static_cast<int>(std::floor(ymin))), y1 = std::min(h - 1, static_cast<int>(std::ceil(ymax)));
    for (int y = y0; y <= y1; ++y) {
      const double sy = y + 0.5;
      std::vector<double> xs;
      for (const auto& p : polys) {
        for (size_t k = 0; k < p.size(); ++k) {
          const auto& a = p[k];
          const auto& b = p[(k + 1) % p.size()];
          if ((a.second <= sy && b.second > sy) || (b.second <= sy && a.second > sy)) {
            xs.push_back(a.first + (sy - a.second) * (b.first - a.first) / (b.second - a.second));
          }
        }
      }
      std::sort(xs.begin(), xs.end());
      for (size_t k = 0; k + 1 < xs.size(); k += 2) {
        for (int x = static_cast<int>(std::ceil(xs[k] - 0.5)); x <= static_cast<int>(std::floor(xs[k + 1] - 0.5)); ++x)
          set(x, y, c);
      }
    }
  }
};

Canvas rasterize(const Scene& sc, int width, int height) {
  if (width <= 0 && height <= 0) width = static_cast<int>(std::lround(sc.width * 2));
  if (width <= 0) width = static_cast<int>(std::lround(sc.width * height / sc.height));
  if (height <= 0) height = static_cast<int>(std::lround(sc.height * width / sc.width));
  width = std::clamp(width, 1, 16000);
  height = std::clamp(height, 1, 16000);
  Canvas cv(width, height);
  const double sx = width / sc.width, sy = height / sc.height;
  const double s = std::min(sx, sy);
  for (const Prim& p : sc.prims) {
    if (p.kind == Prim::kText) continue;
    if (p.kind == Prim::kCircle) {
      if (!p.fill.none) cv.dot(p.cx * sx, p.cy * sy, p.r * s, p.fill);
      if (!p.stroke.none) {
        const int n = std::max(16, static_cast<int>(p.r * s * 6));
        for (int k = 0; k < n; ++k) {
          const double a0 = 2 * M_PI * k / n, a1 = 2 * M_PI * (k + 1) / n;
          cv.line((p.cx + p.r * std::cos(a0)) * sx, (p.cy + p.r * std::sin(a0)) * sy,
                  (p.cx + p.r * std::cos(a1)) * sx, (p.cy + p.r * std::sin(a1)) * sy, p.width * s, p.stroke);
        }
      }
      continue;
    }
    std::vector<std::vector<std::pair<double, double>>> scaled;
    for (const auto& sp : p.subpaths) {
      std::vector<std::pair<double, double>> v;
      for (const auto& q : sp) v.emplace_back(q.first * sx, q.second * sy);
      scaled.push_back(v);
    }
    if (!p.fill.none) cv.fillPolys(scaled, p.fill);
    if (!p.stroke.none && p.width > 0) {
      for (size_t k = 0; k < scaled.size(); ++k) {
        const auto& v = scaled[k];
        for (size_t q = 0; q + 1 < v.size(); ++q)
          cv.line(v[q].first, v[q].second, v[q + 1].first, v[q + 1].second, p.width * s, p.stroke);
        if (p.closed[k] && v.size() > 2)
          cv.line(v.back().first, v.back().second, v.front().first, v.front().second, p.width * s, p.stroke);
      }
    }
  }
  return cv;
}

}  // namespace

std::string toPng(const Scene& sc, int widthIn, int heightIn) {
  const Canvas cv = rasterize(sc, widthIn, heightIn);
  const int width = cv.w, height = cv.h;
  // raw scanlines (filter 0) in stored deflate blocks
  std::string raw;
  raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 3 + 1));
  for (int y = 0; y < height; ++y) {
    raw += '\0';
    raw.append(reinterpret_cast<const char*>(&cv.px[static_cast<size_t>(y) * static_cast<size_t>(width) * 3]),
               static_cast<size_t>(width) * 3);
  }
  std::string z;
  z += static_cast<char>(0x78);
  z += static_cast<char>(0x01);
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    z += static_cast<char>(last ? 1 : 0);
    z += static_cast<char>(n & 0xff);
    z += static_cast<char>((n >> 8) & 0xff);
    z += static_cast<char>(~n & 0xff);
    z += static_cast<char>((~n >> 8) & 0xff);
    z.append(raw, pos, n);
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (unsigned char ch : raw) {
    a = (a + ch) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  std::string out = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  put32(ihdr, static_cast<uint32_t>(width));
  put32(ihdr, static_cast<uint32_t>(height));
  ihdr += static_cast<char>(8);  // bit depth
  ihdr += static_cast<char>(2);  // RGB
  ihdr += '\0';
  ihdr += '\0';
  ihdr += '\0';
  pngChunk(out, "IHDR", ihdr);
  pngChunk(out, "IDAT", z);
  pngChunk(out, "IEND", std::string());
  return out;
}


namespace {

// ---------------------------------------------------------------------------
// JPEG (baseline, 4:4:4, quality-scaled standard tables). `graph export x.jpg`
// is a raster format Stata writes (007's figures export .jpg). This encoder
// follows the baseline process of ITU-T T.81 with the example quantization and
// Huffman tables of its Annex K, scaled by quality the way the IJG library
// defines it; no external library.

const int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                         12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                         58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const int kLumQ[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                       14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                       18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                       49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const int kChrQ[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                       24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                       99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                       99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
// Annex K.3 tables: code counts per length 1..16, then the symbols.
const unsigned char kDcLumBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const unsigned char kDcLumVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const unsigned char kDcChrBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const unsigned char kDcChrVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const unsigned char kAcLumBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const unsigned char kAcLumVal[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
const unsigned char kAcChrBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const unsigned char kAcChrVal[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct HuffTable {
  uint16_t code[256] = {};
  uint8_t len[256] = {};
};

HuffTable buildHuff(const unsigned char* bits, const unsigned char* vals) {
  HuffTable t;
  uint16_t code = 0;
  int k = 0;
  for (int l = 1; l <= 16; ++l) {
    for (int i = 0; i < bits[l - 1]; ++i, ++k) {
      t.code[vals[k]] = code++;
      t.len[vals[k]] = static_cast<uint8_t>(l);
    }
    code <<= 1;
  }
  return t;
}

struct BitWriter {
  std::string* out;
  uint32_t acc = 0;
  int n = 0;
  void put(uint32_t bits, int len) {
    for (int i = len - 1; i >= 0; --i) {
      acc = (acc << 1) | ((bits >> i) & 1u);
      if (++n == 8) {
        out->push_back(static_cast<char>(acc & 0xff));
        if ((acc & 0xff) == 0xff) out->push_back('\0');  // byte stuffing
        acc = 0;
        n = 0;
      }
    }
  }
  void flush() {
    while (n != 0) put(1, 1);  // pad with 1-bits
  }
};

void putMarker16(std::string& s, int v) {
  s.push_back(static_cast<char>((v >> 8) & 0xff));
  s.push_back(static_cast<char>(v & 0xff));
}

}  // namespace

std::string toJpeg(const Scene& sc, int widthIn, int heightIn, int quality) {
  const Canvas cv = rasterize(sc, widthIn, heightIn);
  const int W = cv.w, H = cv.h;
  quality = std::clamp(quality, 1, 100);
  const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
  int qY[64], qC[64];
  for (int i = 0; i < 64; ++i) {
    qY[i] = std::clamp((kLumQ[i] * scale + 50) / 100, 1, 255);
    qC[i] = std::clamp((kChrQ[i] * scale + 50) / 100, 1, 255);
  }
  std::string out;
  out += "\xFF\xD8";                                              // SOI
  out += "\xFF\xE0";                                              // APP0 JFIF
  putMarker16(out, 16);
  out += std::string("JFIF\0", 5);
  out += "\x01\x01";
  out += '\0';                                                    // no units
  putMarker16(out, 1);
  putMarker16(out, 1);
  out += '\0';
  out += '\0';
  for (int t = 0; t < 2; ++t) {                                   // DQT
    out += "\xFF\xDB";
    putMarker16(out, 67);
    out += static_cast<char>(t);
    for (int i = 0; i < 64; ++i) out += static_cast<char>((t == 0 ? qY : qC)[kZigzag[i]]);
  }
  out += "\xFF\xC0";                                              // SOF0
  putMarker16(out, 17);
  out += static_cast<char>(8);
  putMarker16(out, H);
  putMarker16(out, W);
  out += static_cast<char>(3);
  for (int c = 0; c < 3; ++c) {
    out += static_cast<char>(c + 1);
    out += static_cast<char>(0x11);                               // 4:4:4
    out += static_cast<char>(c == 0 ? 0 : 1);
  }
  auto dht = [&](int cls, int id, const unsigned char* bits, const unsigned char* vals, int nvals) {
    out += "\xFF\xC4";
    putMarker16(out, 19 + nvals);
    out += static_cast<char>((cls << 4) | id);
    for (int i = 0; i < 16; ++i) out += static_cast<char>(bits[i]);
    for (int i = 0; i < nvals; ++i) out += static_cast<char>(vals[i]);
  };
  dht(0, 0, kDcLumBits, kDcLumVal, 12);
  dht(1, 0, kAcLumBits, kAcLumVal, 162);
  dht(0, 1, kDcChrBits, kDcChrVal, 12);
  dht(1, 1, kAcChrBits, kAcChrVal, 162);
  out += "\xFF\xDA";                                              // SOS
  putMarker16(out, 12);
  out += static_cast<char>(3);
  for (int c = 0; c < 3; ++c) {
    out += static_cast<char>(c + 1);
    out += static_cast<char>(c == 0 ? 0x00 : 0x11);
  }
  out += '\0';
  out += static_cast<char>(63);
  out += '\0';

  const HuffTable dcY = buildHuff(kDcLumBits, kDcLumVal), acY = buildHuff(kAcLumBits, kAcLumVal);
  const HuffTable dcC = buildHuff(kDcChrBits, kDcChrVal), acC = buildHuff(kAcChrBits, kAcChrVal);
  double cosT[8][8];
  for (int x = 0; x < 8; ++x)
    for (int u = 0; u < 8; ++u) cosT[x][u] = std::cos((2 * x + 1) * u * M_PI / 16.0);
  BitWriter bw{&out};
  int prevDc[3] = {0, 0, 0};
  auto magnitude = [](int v, int* nbits) -> uint32_t {
    int a = v < 0 ? -v : v;
    int n = 0;
    while (a) {
      ++n;
      a >>= 1;
    }
    *nbits = n;
    return static_cast<uint32_t>(v < 0 ? v + (1 << n) - 1 : v);
  };
  for (int by = 0; by < H; by += 8) {
    for (int bx = 0; bx < W; bx += 8) {
      double blk[3][64];
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          const int px = std::min(bx + x, W - 1), py = std::min(by + y, H - 1);
          const size_t i = (static_cast<size_t>(py) * static_cast<size_t>(W) + static_cast<size_t>(px)) * 3;
          const double r = cv.px[i], g = cv.px[i + 1], b = cv.px[i + 2];
          blk[0][y * 8 + x] = 0.299 * r + 0.587 * g + 0.114 * b - 128;
          blk[1][y * 8 + x] = -0.168736 * r - 0.331264 * g + 0.5 * b;
          blk[2][y * 8 + x] = 0.5 * r - 0.418688 * g - 0.081312 * b;
        }
      }
      for (int c = 0; c < 3; ++c) {
        const int* q = c == 0 ? qY : qC;
        int coef[64];
        for (int v = 0; v < 8; ++v) {
          for (int u = 0; u < 8; ++u) {
            double sum = 0;
            for (int y = 0; y < 8; ++y)
              for (int x = 0; x < 8; ++x) sum += blk[c][y * 8 + x] * cosT[x][u] * cosT[y][v];
            const double cu = u == 0 ? M_SQRT1_2 : 1.0, cv2 = v == 0 ? M_SQRT1_2 : 1.0;
            const double f = 0.25 * cu * cv2 * sum;
            coef[v * 8 + u] = static_cast<int>(std::lround(f / q[v * 8 + u]));
          }
        }
        const HuffTable& dc = c == 0 ? dcY : dcC;
        const HuffTable& ac = c == 0 ? acY : acC;
        int nb = 0;
        const int diff = coef[0] - prevDc[c];
        prevDc[c] = coef[0];
        const uint32_t dbits = magnitude(diff, &nb);
        bw.put(dc.code[nb], dc.len[nb]);
        if (nb) bw.put(dbits, nb);
        int run = 0;
        for (int k = 1; k < 64; ++k) {
          const int v = coef[kZigzag[k]];
          if (v == 0) {
            ++run;
            continue;
          }
          while (run > 15) {
            bw.put(ac.code[0xF0], ac.len[0xF0]);  // ZRL
            run -= 16;
          }
          const uint32_t abits = magnitude(v, &nb);
          const int sym = (run << 4) | nb;
          bw.put(ac.code[sym], ac.len[sym]);
          bw.put(abits, nb);
          run = 0;
        }
        if (run > 0) bw.put(ac.code[0x00], ac.len[0x00]);  // EOB
      }
    }
  }
  bw.flush();
  out += "\xFF\xD9";                                              // EOI
  return out;
}

}  // namespace graph_export_detail

}  // namespace piet::handlers
