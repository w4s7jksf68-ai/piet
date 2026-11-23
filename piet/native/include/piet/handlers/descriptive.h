// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: describe, summarize, count, list, tabulate, table, tabstat, corr,
// ttest, panel summaries, compare. Implemented under piet/native/src/descriptive/;
// assert in src/datamgmt/.
#ifndef PIET_NATIVE_HANDLERS_DESCRIPTIVE_H
#define PIET_NATIVE_HANDLERS_DESCRIPTIVE_H

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `describe` / `desc` — VarInfo table, C++ port of describe.py's run_describe.
nlohmann::ordered_json runDescribe(const std::optional<std::vector<std::string>>& varlist,
                                    SessionState& session);

// What `describe` leaves in r() for the data in memory (StataBE 19.5, the
// vs-stata truth bank): r(N), r(k), r(width), r(N_max), r(k_max), r(widthmax),
// r(changed), and r(datalabel) when the dataset has a label -- posted in that
// order, so `return list` and `: r(scalars)` list them as Stata does (changed
// widthmax k_max N_max width k N). r(width) is left unposted (and so refused by
// name) when a string's width was never recorded. Declares the set for `cmd`:
// `describe` itself, and the commands whose ado ends in a `describe`
// (filelist's leftalign, reshape's Report1 `desc, short`), which leave its r().
void postDescribeResults(SessionState& session, const std::string& cmd = "describe");

// `describe [varlist] using FILE [, short simple varlist]`: FILE's description
// (read through `cobra import-dta`), the data in memory untouched. Posts the
// file's r(N), r(k), r(width), the edition limits, r(datalabel) when no varlist
// was given, and r(varlist)/r(sortlist) under `varlist`; never r(changed).
// `names` are the variables before `using` (names, patterns, abbreviations).
nlohmann::ordered_json runDescribeUsing(const std::vector<std::string>& names,
                                        const std::string& fileTyped, bool shortForm,
                                        bool simple, bool varlistOpt, SessionState& session);

// `summarize` / `sum [varlist] [if] [, detail]` — routes numeric vars to
// `cobra eval --summarize`, C++ port of summarize.py's run_summarize.
// `if COND` filtering: when ifExpr is set, the row filter is applied FIRST via
// `cobra eval --keepif COND` to a transient hbin (same validated keepif the
// data verbs use), then the ordinary summarize runs over the filtered data —
// so the numeric path (cobra --summarize) and the factor/fallback path both
// see the restricted sample uniformly (Doctrine ①: the qualifier reaches the
// engine, never silently dropped).
// `weightVar` (Step-1 D3): when set (tokenizer only lets this through for
// `[fw=]`/`[fweight=]`), forwarded as `--fweight weightVar` to the cobra call.
// gate-①: if a weight is set and the cobra call fails, the native numpyStats
// fallback is skipped and the cobra error is returned instead — an unweighted
// answer to a weighted question would be a silent-wrong-number parity bug.
// `inRange` (BS-090, BETA Phase G): `in a/b` row-range, composed with `ifExpr`
// via AND (piet::handlers::applyIfAndInFilter) before either the numeric
// cobra --summarize path or the factor/fallback path ever sees the data —
// same "restrict first, then compute" shape the `if`-only path already used.
nlohmann::ordered_json runSummarize(const std::optional<std::vector<std::string>>& varlist,
                                     bool detail, SessionState& session,
                                     const std::optional<std::string>& ifExpr = std::nullopt,
                                     const std::optional<std::string>& weightVar = std::nullopt,
                                     const std::optional<std::string>& inRange = std::nullopt);

// BS-092 (BETA campaign, Phase G): the pure per-Datastore stats computation
// runSummarize uses once it has resolved which Datastore to read — exposed
// so dispatcher.cpp's `by:`/`bysort: summarize` grouped-dispatch loop can
// invoke the EXACT SAME computation once per by-group's own scratch slice.
nlohmann::ordered_json summarizeCoreStats(Datastore& ds,
                                          const std::optional<std::vector<std::string>>& varlist,
                                          bool detail,
                                          const std::optional<std::string>& weightVar = std::nullopt);

// `count [if COND] [in RANGE]` — numeric-only vertical slice (cobra eval
// --keepif then row count); string-referencing conditions are OUT of the
// M3a slice. `inRange` (BS-090, BETA Phase G) composes with `ifExpr` via AND
// (piet::handlers::applyIfAndInFilter).
nlohmann::ordered_json runCount(const std::optional<std::string>& ifExpr, SessionState& session,
                                 const std::optional<std::string>& inRange = std::nullopt);

// `assert EXP [if COND] [in RANGE]` (P4 wave) — Stata's claim-verifier. Silent
// on success; on failure reports the contradiction count and errors with r(9).
// Deliberately implemented ON TOP of runCount (`count if !(EXP)`) rather than
// with its own evaluator: `count if` already routes through BOTH the native
// CiParser fast path and the cobra keepif fallback, so assert inherits whatever
// those two agree on. A second expression evaluator here would be a second place
// for the two layers to disagree — the exact defect class that makes a wrong
// answer silent.
nlohmann::ordered_json runAssert(const std::string& expr, SessionState& session,
                                 const std::optional<std::string>& ifExpr = std::nullopt,
                                 const std::optional<std::string>& inRange = std::nullopt);

// `list [VARLIST] [if COND] [in RANGE]` — browse.py's run_browse in list mode
// (numeric-only if/in; factor-referencing conditions are OUT of scope).
nlohmann::ordered_json runList(const std::optional<std::vector<std::string>>& varlist,
                                const std::optional<std::string>& ifExpr,
                                const std::optional<std::string>& inRange, SessionState& session,
                                bool noLabel = false);

// Stata `list`'s layout options ([D] list), parsed once for `list` and for
// `duplicates list|examples` (which hand their options to list). The frontend's
// listing renderer (piet/frontend/js/list-format.js) draws the table from the
// `listing` object buildListing() attaches; this struct is what it is told.
struct ListLayout {
  bool noobs = false, clean = false, divider = false, compress = false, nocompress = false;
  bool table = false, display = false, header = false, noheader = false, footer = false;
  bool nolabel = false, notrim = false, nodotz = false, subvarname = false;
  bool absolute = false, relative = false, doubleSpace = false;
  long long separator = 5;  // 0 = none
  long long headerEvery = 0;
  long long abbreviate = -1, stringWidth = -1, linesize = -1;
  std::vector<std::string> sepby;
  // mean/sum/N: absent = not asked, empty = every listed numeric variable.
  std::optional<std::vector<std::string>> mean, sum, count;
  std::optional<std::string> labvar;
  std::optional<std::vector<std::string>> constant;  // empty = every listed variable
};

// Parses list's options (already abbreviation-resolved, as in kVocabList) into
// *out; `raw` is the command line as typed (the N option is case-sensitive).
// Returns Stata's refusal for a malformed one.
std::optional<nlohmann::ordered_json> parseListLayout(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& raw,
    ListLayout* out);

// A computed column listed before the variables (duplicates list's Group and Obs,
// examples' #): its header and one value per listed row.
struct ListLeadCol {
  std::string head;
  std::vector<double> values;
  long long fmtw = 9;  // the width of its display format (nocompress draws it that wide)
  std::vector<std::string> texts;  // a string column: these cells instead of `values`
  std::string format = "%9.0g";    // a numeric column's display format
  bool trailing = false;           // listed after the variables instead of before them
};

// The `listing` object for `rows` (0-based rows of `ds`, listed in that order and
// numbered `obs`) of `vars`: every cell as Stata shows it under the variable's
// display format and value label, the column headers, the mean/sum/N lines, the
// sepby() breaks and the constant() split. On a refusal *err is set and the
// return value is null.
nlohmann::ordered_json buildListing(SessionState& session, const Datastore& ds,
                                    const std::vector<int64_t>& rows,
                                    const std::vector<int64_t>& obs,
                                    const std::vector<std::string>& vars, const ListLayout& lo,
                                    nlohmann::ordered_json* err,
                                    const std::vector<ListLeadCol>& lead = {});

// A number as `display %fmt` shows it, without the field's padding (list's cells,
// summarize's `format`).
std::string listDisplayNumber(double v, const std::string& fmt);

// `list` with Stata's layout: runList's browse block plus the `listing` object.
nlohmann::ordered_json runListStata(const std::vector<std::string>& vars,
                                    const eval::ParsedCommand& parsed, SessionState& session,
                                    const ListLayout& lo);

// `levelsof VAR [, local(NAME) clean]` — distinct sorted values; numeric
// vertical slice (factor/string levelsof is M3b).
nlohmann::ordered_json runLevelsof(const std::string& varname,
                                    const std::optional<std::string>& localName, bool clean,
                                    const std::optional<std::string>& ifExpr, SessionState& session);

// `levelsof VAR` / `glevelsof VARLIST` [if] [in] [, local() clean missing
// separate() colseparate() silent]: the level text and the counts each command's
// own ado publishes (levels_command.cpp has the rules and their sources).
struct LevelsofRequest {
  std::vector<std::string> vars;
  std::optional<std::string> ifExpr;
  std::optional<std::string> inRange;
  std::optional<std::string> localName;
  bool clean = false;
  bool missing = false;
  bool silent = false;
  bool gtools = false;  // glevelsof
  std::optional<std::string> separate;
  std::optional<std::string> colseparate;
  // glevelsof (vs-stata stage 4): numfmt(%fmt) spells numeric levels in that
  // C printf format (gtools'); nolocalvar stores no levels; desc[j] sorts variable j descending
  // (`-var` in the varlist).
  std::optional<std::string> numfmt;
  bool noLocalvar = false;
  std::vector<bool> desc;
};
nlohmann::ordered_json runLevelsofCommand(const LevelsofRequest& req, SessionState& session);

// `compare v1 v2` -- display only.
nlohmann::ordered_json runCompare(const eval::ParsedCommand& parsed, SessionState& session);

nlohmann::ordered_json runInspect(const std::vector<std::string>& varlist,
                                  const std::optional<std::string>& ifExpr,
                                  const std::optional<std::string>& inRange, SessionState& session);
// BUG-371 — panel summaries. Both PIET-native (no engine verb exists), and both
// require xtset: the whole content of xtsum is the BETWEEN/WITHIN split, which is
// meaningless without knowing what a unit is.
nlohmann::ordered_json runXtsum(const std::vector<std::string>& varlist, SessionState& session,
                                const std::optional<std::string>& ifExpr = std::nullopt,
                                const std::optional<std::string>& inRange = std::nullopt,
                                // i(varname): the panel variable named on the line.
                                const std::string& idOverride = std::string());
nlohmann::ordered_json runXttab(const std::vector<std::string>& varlist, SessionState& session,
                                const std::optional<std::string>& ifExpr = std::nullopt,
                                const std::optional<std::string>& inRange = std::nullopt,
                                const std::string& idOverride = std::string());

// `tabulate`/`tab`/`tab1`/`tab2` VARLIST [, chi2 sum(V)|summarize(V)] — C++
// port of tabulate.py's run_tabulate (oneway/twoway freq, tab1/tab2, the
// summarize()-mode per-category mean/sd/N table).
// `weightVar` (Step-1 D3): when set (tokenizer only admits this for
// `[fw=]`/`[fweight=]` on tabulate/tab/tab1/tab2), PIET reads/validates it
// itself (no cobra call underlies native tabulate) and weights every cell
// freq by Σw; missing-weight rows are dropped, non-integer weights error.
// NOT yet honoured for the `sum()` per-category mean/sd/N variant — that
// combination returns an explicit unsupported_option error instead of a
// silent unweighted answer.
nlohmann::ordered_json runTabulate(Datastore& ds, const std::vector<std::string>& varlist,
                                    bool chi2, const std::string& cmd,
                                    const std::optional<std::string>& sumvar,
                                    const std::optional<std::string>& weightVar = std::nullopt,
                                   // BUG-371: `tabulate, missing` — count missing as its OWN
                                   // category rather than dropping those rows. Defaulted so
                                   // every pre-existing call site is byte-identical.
                                   bool includeMissing = false,
                                   // B5-D-TAB-SORT: one-way `, sort` — rows by DESCENDING
                                   // frequency instead of ascending category value. Defaulted
                                   // so every pre-existing call site is byte-identical.
                                   bool sortByFreq = false,
                                   // The typed weight (fw/aw/iw and their long names);
                                   // nullopt reads `weightVar` as an fweight.
                                   const std::optional<std::string>& weightType = std::nullopt,
                                   // BUG-1514: the two-way statistics beyond chi2, a mask of
                                   // the kTab* bits below. 0 keeps every call site byte-identical.
                                   unsigned twoWayStats = 0,
                                   // `exact(#)`: the enumeration workspace (Stata's megabytes,
                                   // default 1); a larger # allows a larger table.
                                   double exactWorkspace = 1.0,
                                   // tab2 `firstonly`: the first variable against each other one.
                                   bool firstOnly = false,
                                   // one-way `subpop(varname)`: counts only the rows where the
                                   // variable is nonzero; the levels are those of the whole sample.
                                   const std::string& subpopVar = std::string());

// BUG-1514: tabulate_twoway's lrchi2, V, gamma and taub ([R] tabulate twoway,
// Methods and formulas), the bits of runTabulate's `twoWayStats`.
constexpr unsigned kTabLrchi2 = 1u;
constexpr unsigned kTabCramersV = 2u;
constexpr unsigned kTabGamma = 4u;
constexpr unsigned kTabTaub = 8u;
constexpr unsigned kTabExact = 16u;  // Fisher's exact (2 x 2 closed form, r x c enumerated)

// `table ROWVAR [COLVAR] [, statistic(STAT VAR)]` — C++ port of table.py's
// run_table (aggregate statistic by cross-classification).
// BUG-160 CLOSURE: delegates to runTableMulti (below) with a single stat
// spec, so the one-statistic and multi-statistic paths share ONE Total-
// margin/label-header computation — see runTableMulti's own comment for why
// both paths were wrong the same way.
// What `table`'s options decide about the shape of the result: which margins are drawn, and
// whether the dimension variables' missing values are categories of their own. `presentation`
// carries the options that only paint the table (title, note, nformat, sformat, showcounts ...)
// as typed, for the renderer.
struct TableLayout {
  bool includeMissing = false;
  bool showRowTotal = true;  // the Total row (collapses the row variable)
  bool showColTotal = true;  // the Total column (collapses the column variable)
  bool zeroCounts = false;  // zerocounts: an empty cell of a count or frequency is 0, not empty
  // [fw|aw|pw|iw=weightVar]: frequencies and shares are sums of weights, means and sums are
  // weighted, count / min / max / raw* statistics are not.
  std::string weightVar;
  std::string weightType;  // "fw", "aw", "pw" or "iw"
  // A third dimension (`table a b c`): one table per level of `pageVar`. With `pageLevel` NaN
  // the call only lists the levels (`page_levels` in the result); with a level it draws that
  // page. A row must have `pageVar` present to be in the table whichever page is drawn.
  std::string pageVar;
  double pageLevel = std::numeric_limits<double>::quiet_NaN();
};

nlohmann::ordered_json runTable(Datastore& ds, const std::string& rowvar,
                                 const std::optional<std::string>& colvar, const std::string& stat,
                                 const std::optional<std::string>& statvar,
                                 SessionState* session = nullptr, const TableLayout* layout = nullptr);

// BUG-160: `table (row) (col), statistic(A) statistic(B) ...` — ONE
// `table_result` with every requested statistic nested under each row
// level, `Total` row/column margins, and value-label-aware column/row
// headers (Stata's `table` prints all three for ONE statistic too, which is
// why runTable above just forwards here with a one-element list rather than
// duplicating the margin/label logic). `statSpecs` is (normalized stat name,
// optional stat variable) in the order the caller wants them nested/emitted.
// `session`, when non-null, supplies value-label lookups
// (session->valueLabels[rowvar-or-colvar]) for a NUMERIC column carrying
// Stata value labels (e.g. `treated` 0/1 -> "no"/"yes") — distinct from a
// genuine factor/string column, which already carries its own dictionary
// via GroupCol::levels. Passing nullptr skips label substitution (numeric
// codes render as their own string).
nlohmann::ordered_json runTableMulti(
    Datastore& ds, const std::string& rowvar, const std::optional<std::string>& colvar,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& statSpecs,
    SessionState* session = nullptr, const TableLayout* layout = nullptr);

// `tabstat VARLIST [, by(g) stats(mean sd ...)]` — direct cobra argv (B9/C10
// translate.py path), estimation-block wrapped. `ifExpr` (BUG-042): tabstat's
// own cobra CLI has no --keepif of its own, so this pre-filters via the same
// `cobra eval --keepif` row-filter infra summarize/count already use.
// B4-W3b: `wantGroups` carries the engine's per-group block onto the result
// (`save`/`nototal` only — see runTabstatCore's comment for why it is not
// unconditional), `noTotal` drops the overall row for `nototal`.
nlohmann::ordered_json runTabstat(const std::vector<std::string>& varlist,
                                   const std::optional<std::string>& byVal,
                                   const std::optional<std::string>& statsVal,
                                   SessionState& session,
                                   const std::optional<std::string>& ifExpr = std::nullopt,
                                   bool wantGroups = false, bool noTotal = false,
                                   // `in a/b`, composed with `if` (tabstat takes both).
                                   const std::optional<std::string>& inRange = std::nullopt);

// W:P5 — `by G: tabstat VARLIST, statistics(...)`. The by-prefix driver in
// dispatcher.cpp has already materialized this group's slice (its rows ANDed
// with the sub-command's own `if`), so this entry point skips the row-filter
// step runTabstat performs and shares the rest of the body with it. `by()` is
// absent by construction: the prefix owns grouping, and Stata rejects both
// grouping axes at once anyway.
nlohmann::ordered_json runTabstatOnData(const std::vector<std::string>& varlist,
                                         const std::optional<std::string>& statsVal,
                                         SessionState& session, const std::string& dataPath);

// `corr`/`correlate`/`pwcorr` VARLIST [, sig obs pairwise] — C++ port of
// translate.py's build_corr_argv. `ifExpr` (BUG-042): same keepif prefilter
// as runTabstat above (corr's cobra CLI has no --keepif of its own either).
// `inRange` (BS-090, BETA Phase G): `in a/b`, composed with `ifExpr` via AND.
// correlate's means, pwcorr's bonferroni/sidak (adjust 1/2), star(), print() (as
// fractions, -1 unset) and listwise: runCorrStata's options beyond the core ones.
struct CorrExtra {
  bool means = false;
  int adjust = 0;
  double star = -1, print = -1;
  bool listwise = false;
};
nlohmann::ordered_json runCorr(const std::vector<std::string>& varlist, bool pairwise, bool sig,
                                SessionState& session,
                                const std::optional<std::string>& ifExpr = std::nullopt,
                                const std::optional<std::string>& inRange = std::nullopt);

// `ttest VAR [== MU] [, by(G) unequal level(#)]` / `ttest V1 == V2 [, paired]`
// — C++ port of translate.py's build_ttest_argv.
nlohmann::ordered_json runTtest(const eval::ParsedCommand& parsed, SessionState& session);
// ttesti (descriptive/descriptive_statistics.cpp): ttest from summary statistics.
nlohmann::ordered_json runTtesti(const eval::ParsedCommand& parsed, SessionState& session);

// `xtdescribe [if] [in] [, patterns()]` -- r() are summarize, detail of T_i.
nlohmann::ordered_json runXtdescribe(const eval::ParsedCommand& parsed, SessionState& session);

// `cf varlist using file [, verbose]` -- rc 9 on any difference.
nlohmann::ordered_json runCf(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_DESCRIPTIVE_H
