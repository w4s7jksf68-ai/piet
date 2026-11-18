// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// estout and esttab: the value, cell, spec and model declarations their
// files share. The common table declarations come from tables_internal.h.
#pragma once
#include "tables_internal.h"
#include "piet/handlers/estimates_tables.h"

namespace piet::handlers::table_detail {
extern const double kEoNaN;


// Stata macro-list tokenization (`foreach x of local`, `word # of`, gettoken):
// blank-separated words; "..." and the compound `"..."' are ONE word with one
// level of quotes removed.
std::vector<std::string> eoTokens(const std::string& s);


// estout: a hook with no `"` is ONE
// line, verbatim; otherwise every word is a line.
std::vector<std::string> eoHookLines(const std::string& raw);


// Split `list , subopts` at the first comma outside quotes and parentheses.
void eoSplitComma(const std::string& raw, std::string* lhs, std::string* rhs);


// `name(arg) flag name2(arg2)` -> (lower(name), arg). Parentheses nest and
// quotes protect, so `prefix(\multicolumn{@span}{c}{)` keeps its braces.
std::vector<std::pair<std::string, std::optional<std::string>>> eoSubopts(const std::string& s);


// Stata `syntax` abbreviation: the CAPITALS of `spec` are the minimum, the
// whole word (lower-cased) the maximum.
bool eoAbbr(const std::string& given, const char* spec);


std::string eoReplaceAll(std::string s, const std::string& from, const std::string& to);


std::string eoTrim(const std::string& s);


// `di %-#s`, `di %#s`, `di %~#s`.
// Widths count CHARACTERS, not bytes (StataBE, scratch-r150/uni-v/p4.do).
std::string eoPadR(const std::string& s, int w);

std::string eoPadL(const std::string& s, int w);

std::string eoCentre(const std::string& s, int w);


// Stata's abbrev(s, n) for a NAME, as BUG-490 measured it (w1b_esttab_abbrev2)
// with the 32-character window estout's own Abbrev runs it on
// (estout; ek_e43 L8 `Second regressor w~l`): keep the trailing
// digit run (or the last character), as much of the head as fits, `~` between.
// Every length is in CHARACTERS (a Unicode name is cut and counted by code
// points, never mid-sequence): StataBE abbreviates by characters.
std::string eoStataAbbrev(std::string s, int w);


// estout Abbrev.
std::string eoAbbrev(int width, const std::string& value, bool abbrev);


// ---- numbers ----------------------------------------------------------------
// The value states estout's matrices carry: a number, `.` (missing -- printed),
// and `.y`/`.z` (not in this model -- the element's vacant() text).
struct EoV {
  enum Kind { kVacant, kMissing, kNum, kText } kind = kVacant;
  double v = 0.0;
  std::string text;
};

EoV eoNum(double v);


// estout vFormat (without par/dmarker/msign, which the caller adds).
// Returns false when the format is not one Stata accepts (r(198)).
// estout reads every cell through a macro first (`local value = St[r,m]`,
// estout), so it formats the value's 16-digit macro text read
// back: StataBE writes estadd's 0.715/5.2 (0x1.1999999999999p-3, which
// `display %9.3f` shows as 0.137) as 0.138 (uni-oracle/et2.do; AER 007's
// Table_A9_Panel_A beta/control mean).
bool eoFormatNumber(double vIn, const std::string& fmtIn, std::string* out);


// The full vFormat: number, then (when `lz` is off) the leading zero dropped,
// then par() around it.
bool eoVFormat(double v, const std::string& fmt, bool lz, const std::vector<std::string>& par,
               std::string* out, const std::string& dmarker = std::string(),
               const std::string& msign = std::string());


// ---- the cell elements ------------------------------------------------------
struct EoElem {
  std::string name;              // b se var t z p ci ci_l ci_u _star _sign _sigsign beta, or a matrix
  std::vector<std::string> fmt;  // per coefficient ROW (`word r of fmt`), last repeats
  bool star = false;
  bool hasPar = false;
  std::vector<std::string> par;  // ( ) by default, [ , ] for ci
  std::string label;
  bool labelSet = false;
  std::string vacant;
  std::string pvalue = "p";
  std::vector<std::string> pattern;
  bool abs = false;
};


struct EoCells {
  std::vector<EoElem> elems;
  // rows[i][j] = the elements `&`-joined in row i, column j; an empty vector is
  // the `.` placeholder.
  std::vector<std::vector<std::vector<int>>> rows;
  size_t ncols = 0;
  bool none = false;
};


// Suboptions of one element (estout ParseValueSubopts).
bool eoParseElemSubopts(const std::string& inner, EoElem* el, std::string* err);


// One row's text (`b se`, `b & se`, `b(fmt(3)) . se`): columns separated by
// blanks, elements of one column joined by `&`.
bool eoParseCellRow(const std::string& row, EoCells* out, std::string* err);


// estout (the cells() parse; none / CellsCheck): an
// unquoted bound word is a ROW of one element; a quoted word (or a
// parenthesised group) is a row of side-by-side columns.
bool eoParseCells(const std::string& raw, EoCells* out, std::string* err);


// ---- label options ----------------------------------------------------------
// estout ParseLabelsSubopts.
struct EoLabelOpt {
  bool given = false;
  bool none = false;
  std::vector<std::string> list;
  bool span = false;
  bool replace = false;
  bool numbers = false;
  bool titles = false, notitles = false;
  bool depvars = false, nodepvars = false;
  bool merge = false;
  int first = -1, last = -1;  // -1 unset; 0 no...; 1 given
  std::string prefix, suffix, erepeat, lhs;
  std::vector<std::string> begin, end, blist, elist;
  std::vector<std::string> pattern;
};


std::string eoStringArg(const std::optional<std::string>& a);


bool eoParseLabelOpt(const std::string& raw, EoLabelOpt* o, std::string* err);


// ---- coefficients and models --------------------------------------------------
struct EoCoefVals {
  double b = kEoNaN, se = kEoNaN, t = kEoNaN, p = kEoNaN, cil = kEoNaN, ciu = kEoNaN;
  bool hasCi = false;
  // e(V) holds 0 on this row (a base level, an omitted or a zero-variance
  // coefficient): estout's var is 0 there while se, t and p are missing.
  bool zeroVar = false;
};


using EoKey = std::pair<std::string, std::string>;
  // (equation, coefficient); "__" = none

struct EoModel {
  std::string name;              // as listed (a store name, or ".")
  const Model* model = nullptr;  // statistics; null in matrix mode
  std::string depname;           // first word of e(depvar)
  std::string title;             // e(_estimates_title)
  std::vector<EoKey> coefRows;   // e(b) order, `_cons` last within each equation
  std::map<EoKey, EoCoefVals> vals;
  // stat -> its rows (in the matrix's column order) and values
  std::map<std::string, std::vector<EoKey>> extraOrder;
  std::map<std::string, std::map<EoKey, double>> extra;
  std::map<std::string, double> beta;  // coefficient -> standardized beta (esttab, beta)
  // A row whose e(b) column carries a base or omitted mark: e(b)'s own name
  // (`0b.foreign`, `o.weight`, `0b.foreign#co.mpg`), which estout reads
  // without the marks and `estimates table` keeps for one model.
  std::map<EoKey, std::string> ebMarked;
};


bool eoIsCoefStat(const std::string& s);


EoV eoElemValue(const EoModel& m, const EoKey& key, const std::string& stat);


// One footer statistic (estout GetStats): aic/bic/rank computed, an e()
// scalar, `p` from Ftail (needs `session`) or chi2tail when e(p) is not posted,
// an e() macro as text (kText). The printed cell and r(stats) both read it.
EoV eoStatValue(const EoModel& em, const std::string& key, SessionState* session = nullptr);


// ---- the spec -----------------------------------------------------------------
struct EoStatRow {
  std::string key;
  std::string fmt;
  bool star = false;
};


struct EoIndicate {
  std::string name;
  std::string vars;
};


struct EoSpec {
  std::vector<EoModel> models;
  bool matrixMode = false;
  // matrix mode: the column names r(coefs) keeps (the matrix's own, or its
  // row names under transpose), one per cell column.
  std::vector<std::string> matrixColNames;
  EoCells cells;
  // coefficient selection
  std::optional<std::string> keep, drop, order;
  bool dropRelax = false;
  std::vector<EoIndicate> indicate;
  std::string indicateYes = "Yes", indicateNo = "No";
  std::vector<std::pair<std::string, std::string>> refcat;  // coef -> label
  bool refcatBelow = false;
  std::string refcatLabel = "ref.";
  bool omitted = true, baselevels = true;
  bool unstack = false;
  bool equationsGiven = false;
  // equations(): estout's AdjustRowEq spec ([name =] number-list, ...), applied
  // to the models' equation names before anything is merged.
  std::string equationsSpec;
  // head
  EoLabelOpt mgroups, mlabels, eqlabels, collabels, varlabels, statslabels;
  bool numbers = false;
  std::string numL = "(", numR = ")";
  bool label = false;
  // labcol2
  bool labcol2 = false;
  std::vector<std::string> labcol2Vals, labcol2Title;
  int labcol2Width = -1;
  // stats
  std::vector<EoStatRow> stats;
  // widths and frame
  std::vector<int> modelwidth{0};
  int varwidth = 0;
  bool abbrev = false;
  bool wrap = false;
  std::string begin, delimiter = "\t", end, lastend;
  bool lastendSet = false;
  std::string incelldelimiter = " ";
  std::vector<std::string> prehead, posthead, prefoot, postfoot;
  std::string title, note;
  std::vector<std::pair<std::string, std::string>> starlevels;  // symbol, level (verbatim)
  std::string starlevelsLabel = " p<", starlevelsDelim = ", ";
  bool legend = false;
  // style(mmd): a markdown divider row (`| ---- | :---: |`) follows the header (estout MakeMMDdef)
  bool mmd = false;
  // style(smcl): rules across the table (`{hline}`) above the header, below it, between equations, above
  // the statistics and at the foot (estout smclrules, smclmidrules, smcleqrules)
  bool smclRules = false, smclMidRules = false, smclEqRules = false;
  // dropped[(str)]: a coefficient whose e(b) and e(V) are 0 reads `str` (default `(dropped)`) in the
  // first cell of its model, the rest of its cells blank
  bool dropped = false;
  std::string droppedText = "(dropped)";
  bool stardetach = false;
  std::vector<std::pair<std::string, std::string>> substitute;
  std::string interaction = " # ";
  bool lz = true;
  // dmarker(str): the decimal marker written in place of `.`; msign(str): the
  // minus sign written in place of a leading `-` (estout vFormat).
  std::string dmarker, msign;
  // starkeep(list) / stardrop(list): the coefficients whose stars are shown /
  // not shown (matched as `var` or `eq:var`).
  std::vector<std::string> starKeep, starDrop;
  bool hasStarKeep = false;
  // margin: estout's marginal-effects switch. The effects are read from e(Xmfx_*)
  // or the like, which only an `mfx`-style fit posts; none is read here, so the
  // coefficients stay as they are. `discrete`/`discrete2` are the legend text
  // (the `(d)` marker and its explanation) that @discrete stands for.
  bool margin = false;
  std::string discrete, discrete2;
  std::string hlinechar = "-";
  std::vector<int> extracols;
  std::vector<std::pair<std::string, std::string>> rename;  // [eq:]from -> to
  int level = 95;
  SessionState* session = nullptr;
};


struct EoError {
  std::string msg;
  int rc = 198;
};




// estout Stars.
std::string eoStars(const std::vector<std::pair<std::string, std::string>>& lv, const EoV& p);


// estout CheckStarvals (the legend text).
std::string eoStarLegend(const EoSpec& sp);


// ---- CompileVarl (estout): a coefficient's label under `label` -------
std::string eoVarLabel(SessionState& session, const std::string& var, bool* exists);


std::string eoCompileVarl(SessionState& session, const std::string& vnameIn,
                          const std::string& interaction);


// ---- coefficient selection: ExpandEqVarlist / DropOrKeep / Order ---------------
// A spec item: (eq, name) where eq "" = any equation and name "" = the whole
// equation (`eq:`).
using EoSpecItem = std::pair<std::string, std::string>;


bool eoExpandEqVarlist(const std::string& list, const std::vector<EoKey>& rows, bool append,
                       std::vector<EoSpecItem>* out, std::vector<EoKey>* appendRows,
                       EoError* err);


bool eoSpecHits(const std::vector<EoSpecItem>& spec, const EoKey& row);


void eoDropOrKeep(bool keep, const std::vector<EoSpecItem>& spec, std::vector<EoKey>* rows);


// estout Order.
void eoOrder(const std::string& specList, const std::vector<EoSpecItem>& expanded,
             std::vector<EoKey>* rows);


// mat_capp ..., cons (estout): rows of `src` not yet in `dst` join at
// the end of their equation -- before that equation's `_cons` -- and a new
// equation joins at the end.
void eoCapp(std::vector<EoKey>* dst, const std::vector<EoKey>& src);


// Stata's `piece(n, w, s), nobreak`: the n-th word-wrapped chunk of s.
std::string eoPiece(int n, int width, const std::string& s);


// What estout leaves in r() beside r(names)/r(nmodels)/r(cmdline)
// (estout, _estout_getres): r(coefs) -- the coefficient rows that survive
// order()/indicate()/keep()/drop(), one column per model and coefficient
// statistic, `.z` where a model lacks the row and `.y` where it lacks the
// statistic --, r(stats), and the per-model macros (m#_depname,
// m#_estimates_title, m#_<stat> for a statistic that is an e() macro).
struct EoRPost {
  bool hasCoefs = false;
  eval::EvalMatrix coefs;
  bool hasStats = false;
  eval::EvalMatrix stats;
  std::vector<std::pair<std::string, std::string>> macros;
};


// estout's coefficient statistics (the coefs() list _estout_getres reads):
// one per cells() element in order, `ci` as ci_l ci_u, then each pvalue() a
// starred element names that is not itself an element.
std::vector<std::string> eoCoefColumns(const EoCells& cells);


// A matrix stripe as Stata stores it: `eq:name`, the empty equation written
// `_` only when another row names one, and a factor term whose position holds
// two or more levels and no base marks its lowest level `bn`.
std::vector<std::string> eoStripeNames(const std::vector<EoKey>& rows);


// Fill `post` from the write phase's state: `models` after rename() and
// TableIsAMess, `mrows` each model's own rows, `B` the rows that survived
// order()/indicate()/keep()/drop(), `coefCols` eoCoefColumns(sp.cells).
void eoCollectRPost(const EoSpec& sp, const std::vector<EoModel>& models,
                    const std::vector<std::vector<EoKey>>& mrows, const std::vector<EoKey>& B,
                    const std::vector<std::string>& coefCols, EoRPost* post);


// The whole write phase. `out` receives the file text (every line ends `\n`).
// `post`, when given, receives the r() results estout stores.
bool eoRender(const EoSpec& sp, std::string* out, EoError* err, int* ccolsOut = nullptr,
              EoRPost* post = nullptr);


// The estimator's equation naming in e(b) -- the rules of eval/macros.cpp's
// kEStripeTable that a table needs: logit/probit name the one equation after
// the depvar, poisson after the depvar with `.` as `_`, mlogit one equation per
// outcome, everything else none (`__`, estout's spelling of `_`).
std::string eoEquationOf(const std::string& estimator, const std::string& depname);


std::string eoOutcomeName(const ojson& o);


double eoJ(const ojson& r, const char* k);


// One stored model as the write phase reads it: e(b)'s rows with b/se/t/p/ci,
// the estadd/estpost matrices, and the transform() applied the way
// _estout_getres does (b, se and the interval; t and p stay untransformed).
EoModel eoModelFrom(const Model& m, const std::string& listedName, SessionState& session,
                    const TransformSpec* tr, size_t mi, bool dropOmitted, bool withBase = true);

// margin / nomargin / discrete() / nodiscrete (estout, esttab): sets sp->margin and the
// legend text; false with `err` set for margin(x) where x is not margin/u/c/p.
bool eoParseMargin(const std::vector<std::pair<std::string, eval::OptValue>>& opts, EoSpec* sp,
                   EoError* err);

// topfile()/bottomfile(): prepend/append the named files' lines to the rendered table.
bool eoWrapWithFiles(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                     SessionState& session, std::string* text, EoError* err);

// level(#) other than 95: the interval is recomputed from b and se (estout's own rule).
void eoApplyLevel(const Model& m, EoModel& em, int level, SessionState& session);


// ---- esttab's per-mode table (esttab), the modes this build writes --
struct EtMode {
  std::string mode;   // fixed tab csv tex booktabs
  std::string mode0;  // fixed tab csv tex
  std::vector<std::string> open, close, caption, toprule, midrule, bottomrule, topgap, midgap,
      bottomgap, eqruleBegin, open0, close0;
  std::string incelldel;
  bool hasEqrule = false;
  std::vector<std::string> ssl, lsl;
  std::vector<std::pair<std::string, std::string>> starlevels;
  std::string starlevlab;  // the label() of starlevels, or empty
  bool starlevlabSet = false;
  std::string begin, delimiter, end;
  bool beginSet = false, delimiterSet = false, endSet = false;
  int varwidth = -1, modelwidth = -1;  // -1 = none
  bool abbrev = false;
  std::vector<std::pair<std::string, std::string>> substitute;
  std::string interaction = " # ";
  std::string tstatlab = "t statistics", zstatlab = "z statistics", pvallab = "p-values", cilab;
};


EtMode esttabMode(const std::string& mode, bool label, bool plain, int level);


// `star(* 0.10 ** 0.05)` -> pairs; false when the list is not symbol/level pairs.
bool eoParseStarPairs(const std::string& raw, std::vector<std::pair<std::string, std::string>>* out,
                      std::string* labelOut, std::string* delimOut, EoError* err);


std::vector<int> eoIntList(const std::string& raw);


// estout's stats(keys, fmt() labels() star) (estout ParseStatsSubopts,
// ProcessStatslayout): per-key formats run forward, the default being the
// first cell element's first format.
bool eoParseStats(const std::string& raw, const std::string& defaultFmt, EoSpec* sp,
                  EoError* err);


// estout's own refusals of an option's ARGUMENT, read with estout's spellings
// (Stats() min 1, VARwidth() min 3, MARgin() min 2) whatever key PIET resolved:
// varwidth() is `numlist max=1 int >=0` (`var(mpg "Miles")` r(121)); a stats()
// list is checked with `confirm names` (`s(* 0.1 ** 0.05)` r(7)); margin() takes
// u, c or p (`ma(b 2)` r(198)); esttab refuses noconstant with constant (r(198)).
// nullopt = no refusal. vs-stata bank 2026-11-10 (ids at the definition).
std::optional<EoError> eoArgumentError(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, bool esttab);


// esttab, beta: b * sd(x)/sd(y) over e(sample) (estadd, called by
// esttab). PIET's store carries no e(sample) mask, so the sample is
// RECONSTRUCTED as the complete cases of {depvar, regressors} and CHECKED
// against e(N): a mismatch refuses by name rather than printing a number from
// the wrong rows (internal notes esttab-beta).
bool esttabBetaVals(SessionState& session, const std::vector<Model>& models,
                    std::vector<std::map<std::string, double>>* out, std::string* errOut);


// estout posts r() (estout): r(cmdline), r(names),
// r(nmodels), r(ccols) -- absent when no coefficient row survived --, and what
// `post` carries (r(coefs), r(stats), the per-model macros).
void eoPostR(SessionState& session, const std::string& cmdline, const std::vector<EoModel>& models,
             int ccols, const EoRPost* post = nullptr);


ojson esttabImpl(const ojson& esttabBlock, const std::string& path,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session, bool toFile);





// estout's MatrixMode (estout; esttab hands the same namelist
// on): one model named after the matrix, one side-by-side cell column per
// matrix column labelled with its name, the rows the matrix's rows, fmt()
// cycling over the columns.
bool eoMatrixModel(SessionState& session, const std::string& spec, EoSpec* sp, std::string* err);


// `matrix(A, fmt(...))` written where a namelist goes; returns its argument.
std::optional<std::string> eoMatrixNamelist(const std::vector<std::string>& words);
}  // namespace piet::handlers::table_detail
