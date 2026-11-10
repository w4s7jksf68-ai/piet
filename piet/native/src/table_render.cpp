// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Shared table output, extracted without changing renderer behavior from
// handlers_aer_tables.cpp. Keep command parsing and file/session effects out.
#include "piet/table_render.h"

#include <sstream>

#include "core/ident.h"

namespace piet::tables {
namespace {

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte name stays aligned.
std::string rpad(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}

std::string lpad(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

// LaTeX's ten specials. `nofix` (texsave) turns this off; everything else in
// this file escapes unconditionally, because an unescaped `_` in a variable
// name is the single most common way a generated .tex file fails to compile.
std::string texEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '_': case '&': case '%': case '$': case '#': case '{': case '}':
        out += '\\';
        out += c;
        break;
      case '~': out += "\\textasciitilde{}"; break;
      case '^': out += "\\textasciicircum{}"; break;
      case '\\': out += "\\textbackslash{}"; break;
      default: out += c;
    }
  }
  return out;
}

// A delimited cell. Comma output quotes anything containing the delimiter or a
// quote; tab output does not, because outreg2's own `.txt` ledger is read back
// by this build's `append` parser and an added quote would not round-trip.
std::string delimCell(const std::string& s, char delim) {
  if (delim != ',') return s;
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

}  // namespace

std::string renderTex(const Table& t, const TexStyle& st) {
  const size_t k = t.ncols();
  auto esc = [&](const std::string& s) { return st.escape ? texEscape(s) : s; };
  auto rowOut = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = esc(label);
    for (size_t c = 0; c < k; ++c) {
      line += " & ";
      if (c < cells.size()) line += esc(cells[c]);
    }
    line += " \\\\";
    return line;
  };

  std::ostringstream o;
  const std::string top = st.booktabs ? "\\toprule" : (st.doubleTop ? "\\hline\\hline" : "\\hline");
  const std::string mid = st.booktabs ? "\\midrule" : "\\hline";
  const std::string bot = st.booktabs ? "\\bottomrule" : "\\hline";

  if (!st.fragment) {
    if (st.standalone) {
      o << "\\documentclass{article}\n\\begin{document}\n";
    }
    if (!t.title.empty()) o << "\\caption{" << esc(t.title) << "}\n";
    std::string align = t.align;  // texsave's align() wins outright
    if (align.empty()) {
      if (st.align == TexStyle::kStar) {
        align = "l*{" + std::to_string(k) + "}{" + st.alignLetter + "}";
      } else if (st.align == TexStyle::kSpaced) {
        align = "l";
        for (size_t c = 0; c < k; ++c) align += " c";
      } else {
        align = "l";
        for (size_t c = 0; c < k; ++c) align += "c";
      }
    }
    o << "\\begin{tabular}{" << align << "}\n";
    o << top << "\n";
  }

  if (!t.colNums.empty()) {
    o << rowOut("", t.colNums) << "\n";
  }
  if (!t.colHeads.empty() || !t.stubHead.empty()) {
    o << rowOut(t.stubHead, t.colHeads) << "\n";
  }
  // The cell-name row travels with the table, so it appears in every style.
  // Its PLACEMENT in tex is not separately captured -- w1b_tables/e7.tex is a
  // `collabels(none)` arm and carries no such row -- but a table model that
  // holds a row the tex writer silently drops is the worse of the two.
  if (!t.colLabels.empty()) {
    o << rowOut("", t.colLabels) << "\n";
  }
  if (!st.fragment && (!t.colNums.empty() || !t.colHeads.empty() || !t.stubHead.empty())) {
    o << mid << "\n";
  }

  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) {
      o << mid << "\n";
      continue;
    }
    o << rowOut(r.label, r.cells) << "\n";
  }

  if (!t.foot.empty()) {
    if (!st.fragment) o << mid << "\n";
    for (const auto& r : t.foot) {
      if (r.kind == TRow::kRule) {
        o << mid << "\n";
        continue;
      }
      o << rowOut(r.label, r.cells) << "\n";
    }
  }

  if (!st.fragment) {
    o << bot << "\n";
    o << "\\end{tabular}\n";
    for (const auto& n : t.notes) {
      // \multicolumn would need the column count inside the tabular; outside it
      // a note is ordinary text, which is what both estout and outreg2 emit.
      o << "\\begin{tablenotes}\n\\item " << esc(n) << "\n\\end{tablenotes}\n";
    }
    if (st.standalone) o << "\\end{document}\n";
  }
  return o.str();
}

std::string renderFixed(const Table& t, const EstoutHooks& hk) {
  const size_t k = t.ncols();
  std::vector<size_t> width(k + 1, 0);
  auto measure = [&](const std::string& label, const std::vector<std::string>& cells) {
    width[0] = std::max(width[0], cecon::ident::codePointCount(label));  // characters
    for (size_t c = 0; c < k; ++c) {
      if (c < cells.size())
        width[c + 1] = std::max(width[c + 1], cecon::ident::codePointCount(cells[c]));
    }
  };
  measure("", t.colNums);
  measure("", t.colHeads);
  measure("", t.colLabels);
  for (const auto& r : t.body) measure(r.label, r.cells);
  for (const auto& r : t.foot) measure(r.label, r.cells);
  for (size_t c = 1; c <= k; ++c) width[c] = std::max<size_t>(width[c], 8);
  width[0] = std::max<size_t>(width[0], 12);

  size_t total = width[0];
  for (size_t c = 1; c <= k; ++c) total += 1 + width[c];
  const std::string rule(total, '-');

  std::ostringstream o;
  if (!t.title.empty()) o << t.title << "\n";
  auto emit = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = rpad(label, width[0]);
    for (size_t c = 0; c < k; ++c) {
      line += " " + lpad(c < cells.size() ? cells[c] : std::string(), width[c + 1]);
    }
    // Trailing blanks carry no information and would make every golden
    // sensitive to a column that happens to be empty.
    while (!line.empty() && line.back() == ' ') line.pop_back();
    o << hk.begin << line << "\n";
  };
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  o << rule << "\n";
  if (!t.colNums.empty()) emit("", t.colNums);
  if (!t.colHeads.empty() || !t.stubHead.empty()) emit(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) emit("", t.colLabels);
  o << rule << "\n";
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) { o << rule << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (!t.foot.empty()) {
    o << rule << "\n";
    if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) {
      if (r.kind == TRow::kRule) { o << rule << "\n"; continue; }
      emit(r.label, r.cells);
    }
  }
  o << rule << "\n";
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << n << "\n";
  return o.str();
}

// --- estout's style(tex) --------------------------------------------------
// BUG-486's SIXTH axis, and the largest single block left in this bundle: 148 of
// the corpus's 219 estout lines carry tex, over 11 papers. This writer handed
// them to `renderTex` -- the esttab emitter -- so all 148 got
// `\begin{tabular}{l*{2}{c}}`, `\hline\hline` and ` & e1 & e2 \\`, where the ado
// writes a bare FRAGMENT of fixed-width right-aligned cells and no rules at all:
//
//     `            &          t1&          t2\\`
//     `mpg         &   -49.51222&   -238.8943\\`
//
// (tests/eval/golden/aer_w1/w1b_estout_tex.log r1, and w1b_tables/e7.tex from the
// 2026-09-14 capture, which shows the same thing and was never diffed -- the
// "captured but not compared" shape BUG-486 already records about itself.)
//
// Three widths are the whole implementable content, and each is measured:
//   stub  = varwidth() if given, else 20 when `label` is in effect and 12 when it
//           is not. It is the LABEL FLAG and not the content: a two-character
//           label still gets 20 and a 25-character one overflows unpadded
//           (w1b_estout_tex2.log s12..s25 against the no-label control).
//   cell  = modelwidth() if given, else 12, right-aligned, also a MINIMUM (a
//           13-character number overflows: w1b_estout_tex.log rc).
//   star  = 3, LEFT-aligned after the cell, so `1.746559** ` keeps its trailing
//           space where `-238.8943***` has none (r4). Padding value+star together
//           to 15 would get the first of those wrong and the second right.
std::string renderEstoutTex(const Table& t, size_t stubW, size_t cellW,
                            size_t starW, const EstoutHooks& hk) {
  const size_t k = t.ncols();
  auto padR = [](const std::string& v, size_t w) { return rpad(v, w); };
  auto padL = [](const std::string& v, size_t w) { return lpad(v, w); };
  // The star mark is concatenated into the cell text upstream (`cellFor` appends
  // it), and the two halves pad differently -- so it is split back off here. A
  // trailing run of `*`/`+` is the mark: no number this writer formats ends in
  // either, and `(omitted)` does not.
  auto splitStar = [](const std::string& c, std::string* star) {
    size_t e = c.size();
    while (e > 0 && (c[e - 1] == '*' || c[e - 1] == '+')) --e;
    *star = c.substr(e);
    return c.substr(0, e);
  };
  auto line = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string out = padR(label, stubW);
    for (size_t c = 0; c < k; ++c) {
      std::string star;
      const std::string v = c < cells.size() ? splitStar(cells[c], &star) : std::string();
      out += "&" + padL(v, cellW) + padR(star, starW);
    }
    return out + "\\\\";
  };

  std::ostringstream o;
  auto row = [&](const std::string& label, const std::vector<std::string>& cells) {
    o << hk.begin << line(label, cells) << "\n";
  };
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  // `numbers` puts the (1)/(2) row ABOVE the model names (r a), which is the
  // opposite of esttab's order.
  if (!t.colNums.empty()) row(std::string(), t.colNums);
  if (!t.colHeads.empty()) row(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) row(std::string(), t.colLabels);
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) row(r.label, r.cells);
  if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) row(r.label, r.cells);
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << n << "\n";
  return o.str();
}

std::string renderDelim(const Table& t, char delim, const EstoutHooks& hk) {
  const size_t k = t.ncols();
  std::ostringstream o;
  auto emit = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = delimCell(label, delim);
    for (size_t c = 0; c < k; ++c) {
      line += delim;
      line += delimCell(c < cells.size() ? cells[c] : std::string(), delim);
    }
    o << hk.begin << line << "\n";
  };
  if (!t.title.empty()) o << delimCell(t.title, delim) << "\n";
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  if (!t.colNums.empty()) emit("", t.colNums);
  if (!t.colHeads.empty() || !t.stubHead.empty()) emit(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) emit("", t.colLabels);
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) { o << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) {
    if (r.kind == TRow::kRule) { o << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << delimCell(n, delim) << "\n";
  return o.str();
}

}  // namespace piet::tables
