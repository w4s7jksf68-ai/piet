// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Shared table contents. Cells have already been formatted by the command;
// this model carries presentation only, with no dataset or session ownership.
#ifndef PIET_NATIVE_TABLE_MODEL_H
#define PIET_NATIVE_TABLE_MODEL_H

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace piet::tables {

struct TRow {
  enum Kind { kCoef, kSe, kStat, kText, kRule };
  Kind kind = kCoef;
  std::string label;
  std::vector<std::string> cells;  // one per column; "" renders as blank
};

struct Table {
  std::string title;
  std::vector<std::string> colNums;   // "(1)", "(2)", ... ; empty = suppressed
  std::vector<std::string> colHeads;  // mtitles / ctitles / depvars
  // The head of the STUB column (the leftmost, label-bearing one). Blank for an
  // estimates table, whose stub holds coefficient names and needs no head;
  // texsave's leftmost column is a data column and its variable name would
  // otherwise be dropped on the floor.
  std::string stubHead;
  // BUG-486: the CELL-NAME row. estout writes one under the model heads naming
  // which statistic each column carries (`b`, or `b/se` for a stacked pair) --
  // `tests/eval/golden/aer_w1/w1b_tables/e2.txt` row 2, and e_plain.txt row 2
  // from the 09-09 capture. `collabels(none)` is exactly the option that
  // removes it (w1b_tables/e7.tex, which carries the model names and no cell
  // row). Empty for every other writer, whose renderers therefore do not
  // change shape.
  std::vector<std::string> colLabels;
  std::vector<TRow> body;
  std::vector<TRow> foot;
  std::vector<std::string> notes;
  std::string align;  // texsave's align(); empty = derived

  size_t ncols() const {
    size_t n = colHeads.size();
    n = std::max(n, colNums.size());
    n = std::max(n, colLabels.size());
    for (const auto& r : body) n = std::max(n, r.cells.size());
    for (const auto& r : foot) n = std::max(n, r.cells.size());
    return n;
  }
};

}  // namespace piet::tables

#endif  // PIET_NATIVE_TABLE_MODEL_H
