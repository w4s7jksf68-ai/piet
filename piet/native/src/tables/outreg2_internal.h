// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// outreg2: the declarations its file shares with the other table writers.
// The common table declarations come from tables_internal.h.
#pragma once
#include "tables_internal.h"

namespace piet::handlers::table_detail {
// Portions derived from outreg2 2.3.2 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// outreg2
//
// outreg2 reports the ACTIVE estimation (or the stored ones named in
// `outreg2 [m1 m2] using ...`), and its
// `append` crosses SESSIONS -- run the same do-file twice and the second run's
// append lands on the first run's file. So the `.txt` ledger IS the source of
// truth for append (plan decision 7); an in-memory ledger would answer
// differently on the second run, which is the rejected alternative.
// ===========================================================================



// `addtext(Controls, Yes, Year FE, Yes)` / `addstat(F, e(F), R2adj, e(r2_a))`
// -- both are FLAT comma lists of alternating name and value.
std::vector<std::pair<std::string, std::string>> parseCommaPairs(const std::string& spec);


// ---------------------------------------------------------------------------
// outreg2 2.3.2 (17aug2014), derived from its own source rather than from the
// shape of its output. The ado works on a STRING DATASET whose rows are the
// ledger's lines and whose columns are its tab-separated cells, and every
// file it writes is a rendering of that one grid:
//
//   makeFile     builds the new column (report / reportCol / rowtype2)
//   appendFile   merges it into the ledger already on disk, by a sort key
//   cleanFile    adds the LABELS column and finds the head/bottom borders
//   _texout      the .tex file, from the label-merged grid
//   _xmlout      the Excel-2003 workbook, from the same grid
//
// So this port keeps a grid (O2Grid) and derives each stage, following
// the ado's own structure. The oracle is tests/eval/golden/aer_w2/w2at_table_writers
// (every file read back line by line) and aer_b4/tables/o_*.txt.
// ---------------------------------------------------------------------------

using O2Grid = std::vector<std::vector<std::string>>;
extern const double kO2Missing;


bool o2IsMissing(double v);


// `round(x - int(round(x, 1e-10)), 1e-10) == 0` -- the integer test autogen
// and autodigits2 share (outreg2).
bool o2Whole(double v);


// string(v, "%12.<d>e")
std::string o2Exp(double v, int d);


// string(v, "%12.<dec>fc") -- the width-12 comma format every fixed cell uses
// (fmtgen). Its three overflow regimes are outreg2AdecCell's, measured.
std::string o2Fixed(double v, int dec);


// The digit count autogen (:6655) and autodigits2 (:6771) derive. They differ
// in ONE branch: past `auto` integer digits autogen gives auto-left-1 (then
// floors at 0) and autodigits2 gives 0 -- the same number after the floor.
int o2AutoDec(double v, int autoD, int less);


// autogen (:6655-6740): %12.<aadec>fc, (auto-1)e from seven decimals on, auto e
// beyond +-1e6. A MISSING value gets an empty format and `string(., "")` is
// empty -- which is why o_stats.txt's depvar row is blank and o09's (bdec(3),
// so fmtgen) reads `.`.
std::string o2AutoCell(double v, int autoD, int less);


// autodigits2 as _addstat_parse uses it (:10420): string(v, "%12.<n>f") -- no
// separators, because r(valstr) is `<n>f`.
std::string o2AutoDigits2Cell(double v, int autoD, int less);


// Split an option body at top-level commas, keeping EMPTY items (a leading,
// doubled or trailing comma is an empty ctitle row, :6029-6050). Each item is
// trimmed and loses one pair of outer double quotes, as gettoken strips them.
std::vector<std::string> o2SplitCommas(const std::string& s);


bool o2IsFactorPart(const std::string& p);


// Stata's e(b) spelling of an interaction: factor parts first, continuous parts
// as `c.<var>` (w2at o21: `regress y c.x1#i.g` writes `1.g#c.x1`). This build's
// coeftable keeps the typed order and drops the `c.` (`x1#1.g`).
std::string o2StataName(const std::string& n);


// fvts_label (:10145-10241): the LABELS cell of one e(b) name.
std::string o2FvtsLabel(const std::string& name, SessionState& session);


// `fvtsunab <token>, onebyone` (:612-622, :10081): a keep()/drop() token
// unabbreviated against the DATASET's variables. A token that is not a
// variable list (`_cons`) expands to nothing -- which is why `drop(_cons)`
// keeps the Constant row (w2at o10).
struct O2Token {
  std::string text;      // a name to match exactly (Stata spelling)
  bool anyLevel = false; // `i.var`: every level of var
};

std::vector<O2Token> o2ExpandTokens(const std::string& spec, SessionState& session);


bool o2TokenHits(const std::vector<O2Token>& toks, const std::string& stataName);


// One row of the new column -- makeFile's report / reportCol / varname /
// rowtype2 (-1 header, 0 top blank, 1 body, 2 bottom blank, 3 statistics and
// notes; :6091-6098).
struct O2Row {
  std::string report;
  std::string cell;
  std::string varname;
  int rowtype = 1;
};


// The sort key Stata's `sort` uses: missing sorts after every number.
int o2Cmp(double a, double b);


int o2CmpKeys(const std::vector<double>& a, const std::vector<double>& b);


std::string o2TrimCell(const std::string& s);


// The Vorder header pass both halves of appendFile run (:3019-3025, :3487-3496):
// the top blank row is 0, each header row above it one less, and the topmost
// of them -99 -- so header rows merge by their DISTANCE from the body, and the
// column-number row always meets the column-number row.
void o2HeaderOrder(const std::vector<int>& rowtype, std::vector<double>* vorder);


// sortvar() tokens: `tsunab` each one, keeping it literally when that fails
// (:545-568).
std::vector<std::string> o2SortvarList(const std::string& spec, SessionState& session);


// appendFile (:2991-3843). `f1` is the ledger as it stands, LABELS column and
// title rows already stripped; `f2` the new column. Returns the merged raw
// grid: f1's columns, then the new one.
O2Grid o2AppendFile(const O2Grid& f1, const std::vector<O2Row>& f2In,
                    const std::vector<std::string>& sortvar,
                    std::vector<std::string>* bodyNamesOut);


// cleanFile's two borders (:9178-9200).
void o2Borders(const O2Grid& g, size_t* headRow, size_t* bottomRow);


std::string o2RenderLedger(const O2Grid& g);


O2Grid o2ParseLedger(const std::string& text);


std::string o2Subinstr(std::string s, const std::string& from, const std::string& to);


// has_eqn: two or more `$` (:7212-7215).
bool o2HasEqn(const std::string& s);


struct O2TexOpts {
  bool fragment = false;
  bool landscape = false;
};


// _texout (:7059-7383) over the label-merged grid (1-based borders).
std::string o2RenderTex(O2Grid g, size_t headBorder, size_t bottomBorder, const O2TexOpts& o);


// _xmlout + xmlstack (:7652-8112), without `legible`: one line, no newline.
std::string o2RenderXml(const O2Grid& g, size_t headBorder, size_t bottomBorder);
}  // namespace piet::handlers::table_detail
