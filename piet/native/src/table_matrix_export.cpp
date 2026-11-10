// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// table matrix export.
#include "tables_internal.h"
namespace piet::handlers::table_detail {


// ===========================================================================
// frmttable -- a session MATRIX as a LaTeX table (TeX only)
// ===========================================================================

// --- frmttable's own LaTeX, which is not estout's ---------------------------
// This writer used to hand its Table to `renderTex`, the estout/esttab emitter,
// and the two shapes are not the same table. Measured against the ado
// (tests/eval/golden/aer_w1/w1b_frmttable_{base,names,cells2}.log), on the
// SIMPLEST possible call -- `frmttable using f.tex, statmat(N) sdec(2) tex`:
//   oracle  \begin{tabular}{ccc}
//           \hline \noalign{\smallskip}1.00 & 2.00 & 3.00\\
//   before  \begin{tabular}{lccc}
//           \hline
//            & c1 & c2 & c3 \\
//           \hline
//           r1 & 1.00 & 2.00 & 3.00 \\
// -- a stub column and a header row that the oracle does not emit at all, plus a
// different rule idiom. Four things had to be measured before this could be
// written, and each one is a rule no amount of reading the option list gives:
//
//  1. DEFAULT-SPELLED matrix names count as ABSENT. Every Stata matrix has
//     names, and `matrix N = (1,2,3\4,5,6)` carries `r1 r2` / `c1 c2 c3` -- for
//     which the oracle emits no stub and no header (base h1). Setting those SAME
//     names explicitly changes nothing (names i2), and naming them anything else
//     turns the stub or the header on (i3 rownames only, i4 colnames only).
//  2. The TABLE WIDTH is the wider of the data (plus stub) and the longest
//     ctitles row: `ctitles("", "c1", "c2", "c3")` over a three-column matrix
//     with no stub gives `{cccc}` and pads every data row with a trailing empty
//     cell (base h5), while a SHORTER ctitles pads the header instead (i8).
//  3. `rtitles` longer than the matrix ADDS rows (ia: a third row of empty data).
//  4. The default numeric format carries THOUSANDS SEPARATORS -- 1234.5 at
//     sdec(2) is `1,234.50` with no sfmt() at all (cells2 k6), which this build
//     was dropping. And the sdec() list's LAST entry repeats for the remaining
//     columns (k2: `sdec(0,1)` over three columns is 0, 1, 1), while extra
//     entries are ignored (k4).


// frmttable's `%fc`: fixed decimals with thousands separators on the integer
// part. A missing value is an EMPTY cell, which is not a flourish -- paper 004
// builds three of its rows out of `J(1,7,.)` spacers (base j1/j2).
std::string frmtCell(double v, int dec) {
  if (std::isnan(v)) return std::string();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string out(buf);
  const size_t dot = out.find('.');
  const size_t start = (!out.empty() && out[0] == '-') ? 1 : 0;
  const size_t end = (dot == std::string::npos) ? out.size() : dot;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    out.insert(at, ",");
  }
  return out;
}


// `rtitles("a" \ "b")` / `ctitles("", "x" \ "", "y")`: rows are separated by a
// top-level backslash and cells within a row by a comma. Quote-aware on both,
// which is what lets a cell hold LaTeX -- `"\makecell[l]{\it{Panel A}}"` is one
// cell and its backslashes are not separators (names i7).
std::vector<std::vector<std::string>> frmtTitleRows(const std::string& raw) {
  std::vector<std::vector<std::string>> rows;
  // SPLIT FIRST, DEQUOTE EACH CELL -- the same rule `parseLabelPairs` states a
  // few hundred lines up, and the first cut of this function broke it. Dequoting
  // `"a" \ "b"` FIRST strips the outer pair and leaves `a" \ "b`, whose remaining
  // quotes make the splitter's quote tracking swallow the separator, so every
  // row collapsed into one cell reading `a" \ "b`. It reproduced on nine of the
  // nineteen oracle arms at once.
  for (const auto& r : splitTopLevel(raw, '\\')) {
    std::vector<std::string> cells;
    for (const auto& c : splitTopLevel(r, ',')) cells.push_back(dequote(strutil::trim(c)));
    rows.push_back(cells);
  }
  return rows;
}


// Stata's own default names for an unnamed matrix, which frmttable treats as no
// names at all.
bool frmtDefaultNames(const std::vector<std::string>& names, const char* stem) {
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] != stem + std::to_string(i + 1)) return false;
  }
  return !names.empty();
}


// One table's inputs, as stored for `replay` (statmat copied by value -- the
// matrix may change before the replay).
ojson frmtInputs(const eval::ParsedCommand& parsed, const eval::EvalMatrix& mat) {
  ojson in;
  in["data"] = ojson::array();
  for (const auto& row : mat.data) {
    ojson r = ojson::array();
    for (double v : row) r.push_back(std::isnan(v) ? ojson(nullptr) : ojson(v));
    in["data"].push_back(r);
  }
  in["rownames"] = mat.rownames;
  in["colnames"] = mat.colnames;
  for (const char* k : {"sdec", "sfmt", "rtitles", "ctitles", "title", "note"}) {
    if (auto v = optStr(parsed.opts, k)) in[k] = *v;
  }
  return in;
}


// Render stored inputs as frmttable's LaTeX (the measured rules above).
bool frmtRenderTex(const ojson& in, bool fragment, bool nocenter, std::string* out,
                   std::string* err) {
  eval::EvalMatrix m;
  for (const auto& r : in["data"]) {
    std::vector<double> row;
    for (const auto& v : r) row.push_back(v.is_number() ? v.get<double>() : kEoNaN);
    m.data.push_back(row);
  }
  m.rownames = in.value("rownames", std::vector<std::string>{});
  m.colnames = in.value("colnames", std::vector<std::string>{});
  const eval::EvalMatrix* mat = &m;
  auto opt = [&](const char* k) -> std::optional<std::string> {
    if (in.contains(k) && in[k].is_string()) return in[k].get<std::string>();
    return std::nullopt;
  };
  // sdec(): a comma list, per column, LAST ENTRY REPEATING (cells2 k2/k4).
  std::vector<int> sdec;
  if (auto sd = opt("sdec")) {
    for (const auto& tok : splitTopLevel(dequote(*sd), ',')) {
      try {
        size_t pos = 0;
        const int n = std::stoi(strutil::trim(tok), &pos);
        if (n >= 0 && n <= 12) sdec.push_back(n);
      } catch (const std::exception&) {
      }
    }
  }
  if (sdec.empty()) sdec.push_back(3);
  auto decFor = [&](size_t c) { return sdec[std::min(c, sdec.size() - 1)]; };
  // sfmt(): per column, last repeating, an empty trailing item ignored; `f`
  // is fixed WITHOUT thousands separators, `fc` (the default) with them
  // (w2at wt_f03 `2001.000` under sfmt(f,f,f,)).
  std::vector<std::string> sfmt;
  if (auto sf = opt("sfmt")) {
    for (const auto& tok : splitTopLevel(dequote(*sf), ',')) {
      const std::string t = strutil::trim(tok);
      sfmt.push_back(t);
    }
    while (!sfmt.empty() && sfmt.back().empty()) sfmt.pop_back();
    for (const auto& f : sfmt) {
      if (f != "f" && f != "fc" && !f.empty()) {
        *err = "frmttable: sfmt(" + f + ") is not implemented in this build; f and fc are";
        return false;
      }
    }
  }
  auto cellFor = [&](double v, size_t c) {
    const std::string f = sfmt.empty() ? std::string("fc") : sfmt[std::min(c, sfmt.size() - 1)];
    if (std::isnan(v)) return std::string();
    if (f == "f") {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.*f", decFor(c), v);
      return std::string(buf);
    }
    return frmtCell(v, decFor(c));
  };

  std::vector<std::vector<std::string>> ctRows;
  if (auto ct = opt("ctitles")) ctRows = frmtTitleRows(*ct);
  std::vector<std::string> rtitles;
  if (auto rt = opt("rtitles")) {
    for (const auto& row : frmtTitleRows(*rt)) rtitles.push_back(row.empty() ? std::string() : row[0]);
  }
  const bool rowNamesReal = !mat->rownames.empty() && !frmtDefaultNames(mat->rownames, "r");
  const bool colNamesReal = !mat->colnames.empty() && !frmtDefaultNames(mat->colnames, "c");
  const bool stub = !rtitles.empty() || rowNamesReal;
  if (ctRows.empty() && colNamesReal) {
    std::vector<std::string> head;
    if (stub) head.push_back(std::string());
    for (const auto& c : mat->colnames) head.push_back(c);
    ctRows.push_back(head);
  }
  const size_t dataCols = mat->data.empty() ? mat->colnames.size() : mat->data[0].size();
  size_t width = dataCols + (stub ? 1 : 0);
  for (const auto& r : ctRows) width = std::max(width, r.size());
  const size_t nRows = std::max(mat->data.size(), rtitles.size());
  auto joinRow = [&](std::vector<std::string> cells) {
    cells.resize(width);
    return joinWith(cells, " & ") + "\\\\";
  };
  std::vector<std::string> lines;
  if (!fragment) lines.push_back("\\documentclass[]{article}\\pagestyle{empty}\\begin{document}");
  if (!nocenter) lines.push_back("\\begin{center}");
  if (auto ti = opt("title")) {
    lines.push_back("\\smallskip\\begin{large}" + dequote(*ti) + "\\end{large}\\\\");
    lines.push_back("\\smallskip");
  }
  std::string spec = stub ? "l" : "";
  for (size_t c = (stub ? 1 : 0); c < width; ++c) spec += "c";
  lines.push_back("\\begin{tabular}{" + spec + "}");
  bool first = true;
  for (const auto& hr : ctRows) {
    lines.push_back((first ? "\\hline \\noalign{\\smallskip}" : "") + joinRow(hr));
    first = false;
  }
  for (size_t r = 0; r < nRows; ++r) {
    std::vector<std::string> cells;
    if (stub) {
      cells.push_back(r < rtitles.size()
                          ? rtitles[r]
                          : (r < mat->rownames.size() && rowNamesReal ? mat->rownames[r]
                                                                      : std::string()));
    }
    if (r < mat->data.size()) {
      for (size_t c = 0; c < mat->data[r].size(); ++c) cells.push_back(cellFor(mat->data[r][c], c));
    }
    std::string prefix;
    if (first) prefix = "\\hline \\noalign{\\smallskip}";
    else if (r == 0 && !ctRows.empty()) prefix = "\\noalign{\\smallskip}\\hline \\noalign{\\smallskip}";
    lines.push_back(prefix + joinRow(cells));
    first = false;
  }
  lines.push_back("\\noalign{\\smallskip}\\hline\\end{tabular}\\\\");
  if (auto n = opt("note")) {
    lines.push_back("\\begin{footnotesize}" + dequote(*n) + "\\end{footnotesize}\\\\");
    lines.push_back("\\smallskip");
  }
  if (!nocenter) lines.push_back("\\end{center}");
  if (!fragment) lines.push_back("\\end{document}");
  out->clear();
  for (const auto& l : lines) *out += l + "\n";
  return true;
}
}
namespace piet::handlers {
using namespace table_detail;


ojson runFrmttable(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFrmttable.known,
                                            kVocabFrmttable.unimplemented)) {
    return ojson(*err);
  }
  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  const std::optional<std::string> statmat = optStr(parsed.opts, "statmat");
  ojson inputs;
  if (statmat.has_value()) {
    // With statmat() the table is built from it; replay() is ignored and
    // store() names it (frmttable; w2at wt_f04).
    const eval::EvalMatrix* mat = session.eval.findMatrix(dequote(*statmat));
    // An absent matrix ends in Mata's conformability error, r(3200) (wt_f06).
    if (mat == nullptr) return tblErrorRc("conformability error", 3200);
    inputs = frmtInputs(parsed, *mat);
    const std::string storeName =
        optStr(parsed.opts, "store").has_value() ? strutil::trim(dequote(*optStr(parsed.opts, "store")))
                                                 : std::string();
    session.eval.globals.set(kFrmtStorePrefix + storeName, inputs.dump());
  } else if (hasFlag(parsed.opts, "replay")) {
    const std::string name = optStr(parsed.opts, "replay").has_value()
                                 ? strutil::trim(dequote(*optStr(parsed.opts, "replay")))
                                 : std::string();
    const std::string blob = session.eval.globals.get(kFrmtStorePrefix + name);
    if (blob.empty()) return tblErrorRc("frmttable: no stored table" + (name.empty() ? std::string() : " " + name), 198);
    try {
      inputs = ojson::parse(blob);
    } catch (...) {
      return tblErrorRc("frmttable: the stored table could not be read", 198);
    }
  } else {
    return tblSyntax("frmttable: statmat(MATRIX) or replay is required by this build -- the "
                     "forms that build a table WITHOUT one (addrows) need frmttable's accumulator, "
                     "which is not implemented");
  }
  if (!usingPath.has_value() || usingPath->empty()) {
    // The screen form: the table is built and kept as the stored table, rc 0
    // (w2at wt_f01a, log 4395-4410).
    return ojson::array();
  }
  // frmttable's DEFAULT file is RTF (.doc) -- base h8 -- which this build has no
  // writer for; refused BY NAME rather than writing LaTeX under a .doc name.
  if (!hasFlag(parsed.opts, "tex")) {
    return ojson(eval::refusedCommand("frmttable", "no_rtf_writer"));
  }
  std::string body, rerr;
  if (!frmtRenderTex(inputs, hasFlag(parsed.opts, "fragment"), hasFlag(parsed.opts, "nocenter"),
                     &body, &rerr)) {
    return ojson(eval::unsupportedOption("sfmt", rerr));
  }
  return emitTable("frmttable", session, ensureExt(*usingPath, ".tex"), body,
                   hasFlag(parsed.opts, "replace"), false);
}
}
