// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "estimators/postest_names.h"  // interaction_key (test/lincom name rule)
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_exists / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // estadd mean: `summarize ... if e(sample)`
#include "piet/display_hardening.h"  // piet::evalScalarExpr
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // eval::applyStataFmt (estout vFormat)
#include "piet/eval/macros.h"  // piet::eval::eScalarKey
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // loops::expandVarlist (texsave varlist)
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/table_render.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// The `known` sets are the plan's command scope table verbatim; the
// `unimplemented` sets are the plan's "refusal options (named)" column. The
// split matters: an option in NEITHER set reads as "unknown option" (which is
// the wrong diagnosis for a real Stata option this build simply does not do),
// and an option wrongly placed in `known` is accepted and silently ignored,
// which is the defect this whole apparatus exists to prevent.


namespace piet::eval {
// eval/macros.cpp: the text a `local v = <real>` stores. Declared here as
// handlers_lb_leebounds.cpp declares it (macros.h does not export it).
std::string stataMacroRealText(double d);
}  // namespace piet::eval


namespace piet::handlers {
bool parseTransformOpt(const std::string& raw, TransformSpec* out, std::string* err);
TransformCell transformOneCell(const TransformSpec& spec, size_t modelIdx,
                               const std::string& coefName, double b,
                               const nlohmann::ordered_json& snap, SessionState& session);
void mirrorIntoEStores(SessionState& session,
                       const std::vector<std::pair<std::string, double>>& scalars,
                       const std::vector<std::pair<std::string, std::string>>& locals,
                       const std::vector<std::pair<std::string, nlohmann::ordered_json>>& matrices);
nlohmann::ordered_json runEstadd(const eval::ParsedCommand& parsedIn, SessionState& session);
nlohmann::ordered_json runEstpost(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json factorBaseLevels(const nlohmann::ordered_json& coeftable, SessionState& session);
nlohmann::ordered_json writeEsttabFile(const nlohmann::ordered_json& esttabBlock, const std::string& path,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                      SessionState& session);
nlohmann::ordered_json esttabScreen(const nlohmann::ordered_json& esttabBlock,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   SessionState& session);
nlohmann::ordered_json runEstout(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runOutreg2(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runTexsave(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runTexdoc(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runTexdocLine(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runFrmttable(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runRegsave(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runParmest(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runMat2txt(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runErepost(const eval::ParsedCommand& parsed, SessionState& session);
}
namespace piet::handlers::table_detail {
extern const piet::eval::OptVocab& kVocabEstadd;
extern const piet::eval::OptVocab& kVocabEstpost;
extern const piet::eval::OptVocab& kVocabEstout;
extern const piet::eval::OptVocab& kVocabOutreg2;
extern const piet::eval::OptVocab& kVocabTexsave;
extern const piet::eval::OptVocab& kVocabTexdoc;
extern const piet::eval::OptVocab& kVocabTexLine;
extern const piet::eval::OptVocab& kVocabFrmttable;
extern const piet::eval::OptVocab& kVocabRegsave;
extern const piet::eval::OptVocab& kVocabParmest;
extern const piet::eval::OptVocab& kVocabMat2txt;
extern const piet::eval::OptVocab& kVocabErepost;
extern const piet::eval::OptVocab& kVocabTabout;


using ojson = nlohmann::ordered_json;


// ===========================================================================
// Errors
// ===========================================================================

ojson tblError(const std::string& message, const std::string& code = "engine_error");


// A USER SYNTAX error. `syntax_error` is what coarseRc maps to rc 198; the
// default `engine_error` maps to rc 1, and bundle C had to repair six sites
// that had made exactly that substitution.
ojson tblSyntax(const std::string& message);


// An error whose Stata rc is KNOWN but which coarseRc's message rules do not
// recognise. `reason` is the only channel that carries an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson tblErrorRc(const std::string& message, int rc);


ojson displayBlock(const std::string& text);


ojson echoBlock(const std::string& text);


bool isErrorBlock(const ojson& b);


// ===========================================================================
// Option access
// ===========================================================================

const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key);


bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key);


std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key);


// ===========================================================================
// Small text helpers
// ===========================================================================

std::string dequote(const std::string& s);


std::vector<std::string> splitWs(const std::string& s);


// Split at TOP-LEVEL whitespace only: `b(star fmt(3)) se(par)` is two items,
// not five. Quotes count as depth too, so `label("Std. err.")` survives.
std::vector<std::string> splitTopLevel(const std::string& s, char sep = ' ');


std::string joinWith(const std::vector<std::string>& v, const std::string& sep);


// The whole string is `r( NAME )`. NAME is read with core/ident.h's permissive
// tier (a byte >= 0x80 is a name byte), by hand rather than by regex: a narrow
// `char` bracket range over the high bytes is not portable across standard
// libraries.
bool parseWholeRName(const std::string& s, std::string* name);


// `<integer>.<name>`: a factor-level coefficient (`2.قيمة`).
bool parseLevelName(const std::string& s, long long* level, std::string* var);
inline constexpr const char* kMissingCell = ".";


// `fmt(3)` means `%9.3f`; `fmt(%9.2f)` is passed through. `fmt(a3)` is Stata's
// "adaptive" width and is REFUSED by the callers rather than silently rendered
// as a fixed 3 decimals -- refusing is the plan's decision 4.
std::string normalizeFmt(const std::string& raw, bool* okOut);


// The `%[-][w].[p][fge]` subset the corpus exercises. Anything else falls back
// to three decimals, which is Stata's own table default.
// Defined below, next to the other number shapes; declared here because the
// `a#` branch of fmtNum needs it and fmtNum comes first in this file.
std::string sigDigits(double v, int d, int minDec, bool comma);


std::string fmtNum(double v, const std::string& fmt);


// --- estout's `a#` adaptive format -----------------------------------------
// estout `SignificantDigits` ("idea stolen from outreg2.ado",
// says its own comment -- which is why outreg2's `auto(#)` shares this body).
// `minDec` is the one place the two callers differ: esttab floors at ONE
// decimal (1946.069 -> 1946.1) and outreg2 floors at ZERO (-> 1,946). That
// single integer is the whole difference between t_plain.txt and o_plain.txt
// on the same coefficient, so it is a parameter rather than two copies.
// --- outreg2 `adec(#)`: three regimes of one width-12 format ---------------
// outreg2 `_addstat_parse` renders each added statistic as
//   string(value, "%12.<adec><afmt>")            (afmt defaults to `fc`)
// and splices the result back into the COMMA-DELIMITED addstat() string that
// outreg2 then re-parses. A thousands separator therefore comes back as a FIELD
// separator, which is why the corpus's own line -- `addstat(Mean dep var,
// `mdp') adec(2)`, 106 uses across three papers -- is r(198) in this Stata
// whenever that mean reaches four integer digits (w1b_adec_rule5.log,
// W1BAD5REC|corpus_macro|198|, beside W1BAD5REC|corpus_rmean_ok|0| on a mean of
// 21.3). The refusal belongs to the caller; this builds the string and counts
// the separators that survived.
//
// The `%12.` WIDTH is what makes this three regimes rather than two, and each
// one is measured (w1b_adec_rule{1,3,4,5}.log):
//   plain "%.<adec>f" over 12 chars -> e-notation, min(adec,5) decimals, and
//                                      adec(0) also five; no separator, rc 0
//   separated spelling within 12    -> separators kept -> outreg2 FAILS
//   separated spelling over 12      -> separators dropped, rc 0
// 6165.257 flips between adec(6) (`6,165.257000`, 12 chars, r(198)) and adec(7)
// (`6,165.2570000`, 13 chars, rc 0, rendering 6165.2570000). Testing |value| >=
// 1000 instead would get BOTH ends wrong: 999.96 at adec(1) IS `1,000.0` and
// refuses (W1BADREC|v999_96_adec1|198|), and 9999999 at adec(3) does not.
struct AdecCell {
  std::string text;
  int seps = 0;
};


AdecCell outreg2AdecCell(double v, int adec);


std::string sigDigits(double v, int d, int minDec, bool comma);


std::string fmtJson(const ojson& v, const std::string& fmt);


// An observation count renders as an integer (Stata's `%9.0g` on an integral
// value), never as "30.000".
std::string fmtCount(const ojson& v);


struct StarLevel {
  const char* mark;
  double cutoff;
};


// estout/esttab's default ladder and outreg2's are DIFFERENT, and this is not
// cosmetic: the same p = 0.03 gets one star from estout and two from outreg2.
// The plan's Stata-semantics section fixes both explicitly for that reason.
const std::vector<StarLevel>& estoutStars();

const std::vector<StarLevel>& outreg2Stars();


std::string starsFor(const ojson& p, const std::vector<StarLevel>& ladder);


std::string starNote(const std::vector<StarLevel>& ladder);


// `starlevels(* 0.10 ** 0.05 *** 0.01)` -- pairs of mark/threshold. A malformed
// list keeps the default rather than half-applying, which is the same rule
// eststo_esttab.cpp's parseStarOpt already follows for `star()`.
std::vector<StarLevel> parseStarLevels(const std::string& spec,
                                       const std::vector<StarLevel>& fallback,
                                       std::vector<std::string>* markStore);


// Shared presentation-only types and renderers live in table_model.h and
// table_render.cpp. Commands below retain model construction and side effects.
using tables::Table;

using tables::TRow;

using tables::renderFixed;


// ===========================================================================
// File writing
// ===========================================================================

// SIBLING TEMP + rename(2), never a write into the destination. Copied in shape
// from `export delimited`'s writer (handlers_data_verbs3.cpp:1245-1340): a
// failure never leaves a truncated file where a readable one used to be, and a
// destination that is ALSO a source (outreg2's `append` ledger) is safe because
// the caller has already read it fully into memory before calling this.
//
// `userPath` is what the user TYPED and is the only spelling that ever reaches
// the output; `resolved` is what gets opened. Bundle D echoed the resolved
// absolute path and baked a build machine's workdir into a golden.
ojson writeTextFileAtomic(const std::string& cmd, const std::string& resolved,
                          const std::string& userPath, const std::string& bytes);


std::string readWholeFile(const std::string& path, bool* okOut);


// The existence/replace/append gate every file-writing verb here shares.
// Returns a non-null error block when the write must not happen.
ojson guardOverwrite(const std::string& cmd, const std::string& resolved,
                     const std::string& userPath, bool replace, bool append);


// `using f` with no extension gets one. Stata's table commands each have their
// own default and getting it wrong writes a file the user cannot find.
std::string ensureExt(const std::string& path, const std::string& ext);


std::string lowerExt(const std::string& path);


// `using FILE` split out of a varlist. Returns the raw (still user-spelled)
// path, and erases `using` and everything after it from `words`.
std::optional<std::string> takeUsing(std::vector<std::string>& words);


// ===========================================================================
// Stored estimates: the bag, the e() scalars, and the staleness rule
// ===========================================================================

ojson emptyBag();


// Freeze the e() scalars a stored estimate should carry. `eScalarKey()` is the
// ONE table of Stata-spelling -> cobra-key (eval/macros.h); it was exposed at
// namespace scope for this call precisely so a third copy would not appear.
ojson buildEScalars(const ojson& est);


// A stored estimate plus everything a renderer needs from it.
struct Model {
  std::string name;
  ojson snap;       // the eststo snapshot
  ojson added;      // {scalars, locals, matrices}
  ojson escalars;   // Stata-spelled e() scalars
};


// The snapshot's own `escalars`/`added` -- what `runEststo` attaches -- WIN.
// The fallback below is a backstop for an estimate that reached the store by
// some other route (a snapshot from before this bundle, or one built from
// LastModel's cached result), and it fires only when the shared fingerprint
// agrees. A mismatch discards rather than bleeds: attaching another model's
// additions is exactly the failure the fingerprint exists to stop.
Model materializeModel(SessionState& session, const std::string& name, const ojson& snap);


// Resolve a namelist (possibly empty -> every stored estimate) into models.
// On failure `errOut` is a real error block and the return is empty.
bool activeModel(SessionState& session, const std::string& cmd, Model* out, ojson* errOut);


std::vector<Model> collectModels(SessionState& session, const std::string& cmd,
                                 const std::vector<std::string>& namesIn, ojson* errOut);


// The coefficient rows of one model, keyed by name.
const ojson* coefRow(const ojson& snap, const std::string& name);


// Union of coefficient names in first-appearance order across models, then
// keep()/drop()/order() applied in Stata's own precedence: `order()` promotes,
// `keep()` restricts, `drop()` removes.
// The ONE coefficient-selection filter. There were two: `coefNames` for
// estout/outreg2 and a second copy inside `writeEsttabFile` for esttab, and the
// glob fix below reached only the first one -- `keep(*2*)` selected `x2` on
// estout and emptied the table on esttab in the same build. A command whose row
// selection is implemented twice is a command with two answers about what `*`
// means, which is the shadow-evaluator class this codebase keeps paying for.
void applyCoefSelection(std::vector<std::string>* names,
                        const std::optional<std::string>& keepOpt,
                        const std::optional<std::string>& dropOpt,
                        const std::optional<std::string>& orderOpt, std::string* errOut);


// BUG-496: every one of `keep()`/`drop()`/`order()`'s tokens is a PATTERN, not a
// name. `keep(*romer*)` is how all four of paper 015's transform lines select
// their coefficients and `keep(3:0.MI*#c.romer*)` how another does, and exact
// matching answered them with an EMPTY body at rc 0. `keep(mp?)` is `mpg` too
// (w1b_keepglob.log ka), so it is Stata's `match()`, not a prefix rule. `errOut`
// carries the other half: a keep() token that matches nothing is r(111)
// "coefficient zz* not found" (kg_glob_none), not a table with no rows.
std::vector<std::string> coefNames(const std::vector<Model>& models,
                                   const std::optional<std::string>& keepOpt,
                                   const std::optional<std::string>& dropOpt,
                                   const std::optional<std::string>& orderOpt,
                                   std::string* errOut = nullptr);


// `varlabels(x "Income" y "Age")` / `coeflabels(...)` -- pairs. Plus `label`,
// which asks for the DATASET's variable labels instead.
std::map<std::string, std::string> parseLabelPairs(const std::string& spec);


std::string labelFor(const std::string& name, const std::map<std::string, std::string>& explicitMap,
                     bool useVarLabels, SessionState& session);


// BUG-486 removed this file's last caller of a `statLabel` auto-label map --
// estout labels a footer row with the key AS TYPED (`r2`, not `R-squared`;
// w1b_estout_defaults.log g3), and a pretty label comes from
// `stats(..., labels(...))`, which is the user's text and needs no map. The map
// is deleted rather than parked: a dead table of guesses is how a later reader
// concludes the guesses were measured. esttab's footer is built by its own
// writer and its labels were byte-verified by the 2026-09-09 capture.

// One footer statistic for one model: e() scalars first, then estadd's scalars,
// then estadd's locals (a TEXT row -- "Controls Yes"). Absent = blank cell, not
// zero: a missing statistic is not a statistic equal to nothing.
std::string statCell(const Model& m, const std::string& key, const std::string& fmt,
                     bool* isTextOut);


// ---------------------------------------------------------------------------
// `transform()`. Declared in piet/handlers.h; see there for the grammar and for
// why it is shared rather than per-writer.
// ---------------------------------------------------------------------------


// Stata's `gettoken` with no `bind` splits on WHITESPACE only, so `exp(@)` and
// `@/(-_b[weight])` are each one token and `pattern(1 1 0 0)` is FOUR. That last
// one is not a detail: it is why paper 083's `transform(@/M 1/M pattern(1 1 0 0)
// @*100 100 pattern(0 0 1 1))` -- no comma, so pattern() cannot be a suboption --
// reads `pattern(1` as a coefficient pattern and silently transforms nothing in
// Stata (w1b_transform.log tr_083_verb, rc 0 and byte-identical to the control).
std::vector<std::string> trTokens(const std::string& s);


// Split at the FIRST top-level comma: `transform(<list> [, pattern(...)])`.
void trSplitSubopts(const std::string& raw, std::string* list, std::string* subs);


// estout `MatchCoef`. A token with no colon matches the NAME; one with
// a colon matches equation and name separately, and an empty half is `*`. This
// engine's coeftable carries no equation for a single-equation model, and
// `transform(*:*mpg* @*2 2)` still transforms `mpg` there (w1b_transform.log
// r9) -- `globMatch("*", "")` is true, so that falls out rather than needing a
// case.
bool trMatch(const std::string& pat, const std::string& eq, const std::string& name);


// `@` stands for a MATRIX ELEMENT in the ado (`subinstr "@" "\`b'"`, where `b` is
// `bc[i,j]`), which is atomic -- so the substituted literal is PARENTHESISED.
// Without that, `@^2` on a negative coefficient would parse as -(x^2) here and
// as (+x)^2 there, and the sign of every squared transform would differ.
std::string trSubstAt(const std::string& expr, double at);


// Make ONE model's estimates current for the duration of an expression, because
// `_b[]` in a transform resolves per model (y1). Save/restore rather than a
// second `_b[]` resolver: there are already two of those (macros.cpp and
// display_hardening.cpp) and a third keyed to a different context is how they
// come to disagree.
// eb-order T-P3: `_b[]`/`_se[]` read EvalSession::coefSource(), not
// lastModelResult, so the guard also sets the scoped coefficient source
// (coefOverride) to the stored model; the lastModelResult swap stays for the
// fitted e-scalars (e(N), e(r2)) that resolveEScalar reads from it.
struct TrScopedModel {
  piet::eval::EvalSession& s;
  std::optional<nlohmann::ordered_json> saved;
  const nlohmann::ordered_json* savedOverride;
  TrScopedModel(piet::eval::EvalSession& s_, const nlohmann::ordered_json& snap)
      : s(s_), saved(s_.lastModelResult), savedOverride(s_.coefOverride) {
    s.lastModelResult = snap;
    s.coefOverride = &*s.lastModelResult;
  }
  ~TrScopedModel() {
    s.lastModelResult = saved;
    s.coefOverride = savedOverride;
  }
  TrScopedModel(const TrScopedModel&) = delete;
  TrScopedModel& operator=(const TrScopedModel&) = delete;
};




// ===========================================================================
// estout's cells() sub-grammar (plan design decision 4)
//
//   cells(ITEM ...)  /  cells("ITEM ...")
//   ITEM := name | name(sub ...)
//   name := b | se | t | p | <an estadd matrix name, for estpost tables>
//   sub  := star | par | par(L R) | fmt(#|%fmt) | label(text)
//
// Each ITEM is its own ROW (stacked), which is estout's default layout. The
// forms deliberately NOT accepted, each refused BY NAME rather than ignored:
// `&` (side-by-side), `[ ]` (grouping), `pvalue()`, and equation notation --
// every one of them changes the SHAPE of the table, so accepting and ignoring
// would silently produce a different table than the one asked for.
// ===========================================================================

struct CellSpec {
  std::string name;
  bool star = false;
  bool par = false;
  std::string parL = "(";
  std::string parR = ")";
  std::string fmt = "%9.3f";
  bool fmtGiven = false;
  std::string label;
  // outreg2's DEFAULT number shape: significant digits, floor 0 decimals, with
  // thousands separators (outreg2 `dec` defaults to 3 and
  // `fmt` defaults to `fc`; turns the adaptive path off
  // the moment `dec()`/`bdec()` is given). `autoD < 0` means "use fmt".
  int autoD = -1;
};


// Split `name(inner)` into its two halves. Returns false when the parentheses
// do not balance, which is a syntax error rather than a name.
bool splitCall(const std::string& item, std::string* head, std::string* inner);


bool parseCells(const std::string& raw, std::vector<CellSpec>* out, std::string* errOut,
                bool* sideBySideOut = nullptr);


// ===========================================================================
// The shared coefficient-body builder
// ===========================================================================

struct BodyOpts {
  std::vector<CellSpec> cells;
  // BUG-487: estout's cells() QUOTE is an operator. `cells(a b)` stacks the
  // items as ROWS; `cells("a b")` puts them SIDE BY SIDE -- k*n columns, one
  // body row per coefficient, ordered model-major (m1.b m1.se m2.b m2.se;
  // tests/eval/golden/aer_w1/w1b_estout_cells.log h2). With ONE item the two
  // shapes coincide, which the capture also pins (h5 == h6).
  bool sideBySide = false;
  std::vector<StarLevel> stars;
  std::optional<std::string> keep, drop, order;
  std::map<std::string, std::string> labels;
  bool useVarLabels = false;
  bool nocons = false;
  // o_plain.txt puts the intercept LAST and calls it `Constant` whether or not
  // `label` was given. Both default OFF so `estout`'s frozen fixture keeps its
  // bytes; only outreg2 turns them on.
  bool consLast = false;
  std::string consLabel;
};


// One body row per (coefficient x cell-spec). A model that does not estimate a
// given coefficient gets a BLANK cell, never a zero -- a mixed-estimator table
// is the plan's own edge case and a zero there is a claim of no effect.
void buildBody(Table* t, const std::vector<Model>& models, const BodyOpts& bo,
               SessionState& session, std::string* errOut = nullptr);


// The footer: `stats(N r2 ...)` in the order given. A key that is an estadd
// LOCAL renders as text ("Controls Yes"), which is decision 2's whole point.
void buildFoot(Table* t, const std::vector<Model>& models, const std::vector<std::string>& stats,
               const std::string& fmt, size_t stride = 1);


// Column heads: `mlabels()`/`mtitles()` if given, else the model's depvar (what
// esttab prints), else the stored name.
// `nameHeads` picks which side of the model a derived head comes from. estout
// heads its columns with the STORED NAME (`m1`, `m2` -- e2.txt/e_plain.txt row
// 1); esttab heads them with the dependent variable. This build derived the
// depvar for both, which is BUG-486's second axis.
void buildHeads(Table* t, const std::vector<Model>& models,
                const std::optional<std::string>& mlabels, bool numbers, bool heads,
                bool nameHeads = false);


// ===========================================================================
// Raw-argument recovery
//
// `parsed.varlist` is WHITESPACE-split (tokenizer.cpp:685), so
// `estadd local controls "Yes, with FE"` arrives as four tokens with the quotes
// broken across two of them. Every place below that needs a user STRING reads
// it out of `parsed.raw` instead. The comma cut uses the tokenizer's own
// quote-aware scanner rather than a plain find(','), because a comma inside the
// text is not an option separator.
// ===========================================================================

std::string argsAfterHead(const std::string& raw);


// First `:` outside quotes and parens -- estadd's `... : m1 m2` namelist split.
long long findUnquotedColon(const std::string& s);


// Peel the leading word off a string, returning it and advancing `rest`.
std::string takeWord(std::string* rest);


// ===========================================================================
// Fit-sample column values (estadd ysumm)
//
// The estimation sample is not the loaded dataset: `regress y x if region<=4`
// fits on a subset, and Stata's `estadd ysumm` summarises e(sample), not the
// frame. LastModel records the two shapes this codebase uses for that -- a
// materialized scratch file (`fitDataPath`) and/or an engine-side `--keepif`
// expression -- and this reproduces the same reconstruction handlers_resample.
// cpp:480-500 does, rather than reading the head and being quietly wrong by a
// few hundred observations (BUG-163's measured failure was 3,106 vs 4,649).
// ===========================================================================

bool fitSampleValues(SessionState& session, const std::string& var,
                     std::vector<double>* out, std::string* errOut);



// `r(name)` -> its number, for the expression estadd evaluates itself: the
// macro layer leaves bare r() on an estadd line alone (eval/macros.cpp), so
// `estadd r(mean)` and `estadd scalar r(sd)` reach the handler as written.
std::string estaddSubstR(const std::string& expr, SessionState& session);


// ===========================================================================
// estpost -- the pseudo-estimate (plan design decision 3)
//
// Stata's estpost posts a summary table into e() so that `esttab, cells(mean)`
// can read it. The posted objects are 1xk MATRICES named after the statistic
// (e(mean), e(sd), ...), with the variables as column names -- which is exactly
// the shape `estadd matrix` already stores, so estpost reuses that bag rather
// than inventing a second one.
//
// The numbers come from the SAME cores the native commands use:
//   summarize -> handlers::summarizeCoreStats (handlers.cpp)
//   tabstat   -> the `cobra tabstat --tidy` argv, re-assembled here
// `runTabstatCore` (estimation_m4.cpp:967-1013) is in an anonymous namespace
// and is therefore NOT callable from this TU. Re-assembling its six-line argv
// is cheaper than exposing a symbol out of that hot file, and the SOURCE of the
// numbers -- the engine -- is identical either way, which is the property that
// matters. A native re-implementation of the statistics would not be.
// ===========================================================================



// A 1xk matrix in the `added.matrices` shape.
ojson rowMatrix(const std::vector<std::string>& colnames, const std::vector<double>& values);


double jsonNum(const ojson& v);


// The statistic names estpost summarize posts, in Stata's own order. `N` is
// renamed to `count` because that is the e() name esttab's `cells(count)`
// asks for; the rest keep the summarize block's spelling.
struct SummStat {
  const char* eName;
  const char* blockKey;
  bool detailOnly;
};

const std::vector<SummStat>& summStats();


// Turn the posted matrices into the on-screen table. One row per variable, one
// column per statistic -- `columns(variables)` transposes it, which is what
// that option means and why it is not a silent no-op here.
Table estpostTable(const std::vector<std::string>& vars,
                   const std::vector<std::string>& statNames, const ojson& matrices,
                   bool byVariables, const std::string& title);


// Install the pseudo-estimate. `lastEstBlock` is what `eststo` snapshots, what
// `estadd` fingerprints, and -- since bundle E -- what a bare `esttab` falls
// back to. estpost therefore has to write it for anything downstream to see
// the table at all.
//
// `session.eststo(...)` is deliberately NOT called: Stata's estpost posts to
// e() and never stores, and minting an `m1` as a side effect would also change
// what a LATER `eststo` numbers itself.
// `setLastModel` is not called either: estpost is not a fitted model, and
// claiming one would let `predict`/`test` address a summary table. eb-order
// T-P3: nor may the PREVIOUS fit stay behind it -- with its LastModel alive,
// `_b[]`, test/lincom/nlcom/margins/predict and `estimates save` all answered
// from that fit at rc 0 (SPEC C, REPORT a12/a13) -- so the whole e() is reset
// first, as `clear results` does.
//
// THE COEFTABLE IS NOT EMPTY, and this is a deliberate departure from the
// plan's literal `coeftable:[]`. Stata's `ereturn post` requires a b vector,
// and estpost supplies one -- which is why a bare `esttab` after `estpost
// summarize` prints the means at all. An empty coeftable renders an EMPTY
// TABLE at rc 0, with the statistics sitting unreachable in e(mean). So e(b)
// carries the primary statistic and e(se) the dispersion one, and the 1xk
// matrices are posted alongside exactly as the plan specifies.
struct PseudoCoef {
  std::string name;
  double b = std::numeric_limits<double>::quiet_NaN();
  double se = std::numeric_limits<double>::quiet_NaN();
};


void postPseudoEstimate(SessionState& session, const std::string& estimator, double nObs,
                        const ojson& matrices, const std::vector<PseudoCoef>& coefs);


// The two statistics that become e(b) and e(se). `mean`/`sd` when they were
// computed, else the first two requested -- so `estpost tabstat x, stat(sum)`
// still posts something addressable rather than a blank column.
void pickPrimary(const std::vector<std::string>& statNames, std::string* bStat,
                 std::string* seStat);


// Read one cell out of a posted 1xk matrix by column position.
double matCell(const ojson& matrices, const std::string& stat, size_t j);




// The active e() as a Model, with the intercept renamed the way `makeSnapshot`
// does -- `_cons` is the spelling every table and every user writes.
bool activeModel(SessionState& session, const std::string& cmd, Model* out, ojson* errOut);


// One place where a rendered table becomes a file. `append` reads the existing
// bytes FIRST and hands the concatenation to the atomic writer, so the
// destination is never opened for writing while it is still a source (the
// dest==source law, BUG-138).
ojson emitTable(const std::string& cmd, SessionState& session, const std::string& userPath,
                const std::string& bytes, bool replace, bool append);
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
               std::string* out);


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
};


bool eoIsCoefStat(const std::string& s);


EoV eoElemValue(const EoModel& m, const EoKey& key, const std::string& stat);


// One footer statistic (estout GetStats): e() scalars, then estadd scalars,
// then estadd LOCALS as text (`return(m#_stat)`, estout).
EoV eoStatValue(const EoModel& em, const std::string& key);


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
  std::vector<std::pair<std::string, std::string>> substitute;
  std::string interaction = " # ";
  bool lz = true;
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


bool eoMatrixModel(SessionState& session, const std::string& spec, EoSpec* sp, std::string* err);


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


// The whole write phase. `out` receives the file text (every line ends `\n`).
bool eoRender(const EoSpec& sp, std::string* out, EoError* err, int* ccolsOut = nullptr);


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


// ---- esttab's per-mode table (esttab), the modes this build writes --
struct EtMode {
  std::string mode;   // fixed tab csv tex booktabs
  std::string mode0;  // fixed tab csv tex
  std::vector<std::string> open, close, caption, toprule, midrule, bottomrule, topgap, midgap,
      bottomgap, eqruleBegin;
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


// esttab, beta: b * sd(x)/sd(y) over e(sample) (estadd, called by
// esttab). PIET's store carries no e(sample) mask, so the sample is
// RECONSTRUCTED as the complete cases of {depvar, regressors} and CHECKED
// against e(N): a mismatch refuses by name rather than printing a number from
// the wrong rows (internal notes esttab-beta).
bool esttabBetaVals(SessionState& session, const std::vector<Model>& models,
                    std::vector<std::map<std::string, double>>* out, std::string* errOut);


// estout posts r() (estout): r(cmdline), r(names),
// r(nmodels), r(ccols) -- missing when no coefficient statistic resolved.
void eoPostR(SessionState& session, const std::string& cmdline, const std::vector<EoModel>& models,
             int ccols);


ojson esttabImpl(const ojson& esttabBlock, const std::string& path,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session, bool toFile);



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


// Portions derived from texsave 1.6.1 (MIT).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// texsave -- the DATASET as a LaTeX table
// ===========================================================================


// A variable's display format, as `list`/outsheet render it (graph_m4.cpp
// displayFormatOf's pattern).
std::string displayFormatOfVar(SessionState& session, Datastore& ds, const std::string& var);
extern const char* const kTexdocPathKey;


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
std::string frmtCell(double v, int dec);


// `rtitles("a" \ "b")` / `ctitles("", "x" \ "", "y")`: rows are separated by a
// top-level backslash and cells within a row by a comma. Quote-aware on both,
// which is what lets a cell hold LaTeX -- `"\makecell[l]{\it{Panel A}}"` is one
// cell and its backslashes are not separators (names i7).
std::vector<std::vector<std::string>> frmtTitleRows(const std::string& raw);


// Stata's own default names for an unnamed matrix, which frmttable treats as no
// names at all.
bool frmtDefaultNames(const std::vector<std::string>& names, const char* stem);
extern const char* const kFrmtStorePrefix;


// One table's inputs, as stored for `replay` (statmat copied by value -- the
// matrix may change before the replay).
ojson frmtInputs(const eval::ParsedCommand& parsed, const eval::EvalMatrix& mat);


// Render stored inputs as frmttable's LaTeX (the measured rules above).
bool frmtRenderTex(const ojson& in, bool fragment, bool nocenter, std::string* out,
                   std::string* err);


// ===========================================================================
// regsave / parmest -- the estimates as a DATASET (plan design decision 9)
//
// One builder, two column vocabularies. The rows go out as CSV, `cobra convert`
// turns them into an .hbin, and then either `export-dta` writes the requested
// file (session untouched) or `session.load` makes the table the dataset in
// memory. The CI columns come from the coeftable the ENGINE produced -- this
// build does not own an inverse-t, and recomputing a 90% interval from a 95%
// fit with a normal quantile would be a quietly wrong number, which is why a
// `level()` other than the fitted one is refused below rather than approximated.
// ===========================================================================



struct DsColumn {
  std::string name;
  bool isString = false;
  std::vector<double> num;
  std::vector<std::string> str;
};


// CSV escaping for the convert step. `cobra convert --delim ,` reads a plain
// RFC4180 file; a variable name or label containing a comma would otherwise
// shift every later column by one.
std::string csvCell(const std::string& s);


// Build the .hbin. Returns the path, or an error block through `errOut`.
bool materializeDataset(SessionState& session, const std::string& cmd,
                        const std::vector<DsColumn>& cols, size_t nRows, std::string* pathOut,
                        ojson* errOut);


// Common tail: `using`/`saving()` writes a .dta and leaves the session alone;
// otherwise the built table BECOMES the dataset in memory.
ojson finishDataset(SessionState& session, const std::string& cmd, const std::string& hbin,
                    const std::optional<std::string>& outUser, bool replace, bool appendUsing,
                    const std::vector<std::string>& newVars);


// The coefficient rows of the ACTIVE estimation, for both builders.
struct CoefRowView {
  std::string name;
  double b = std::numeric_limits<double>::quiet_NaN();
  double se = std::numeric_limits<double>::quiet_NaN();
  double t = std::numeric_limits<double>::quiet_NaN();
  double p = std::numeric_limits<double>::quiet_NaN();
  double lo = std::numeric_limits<double>::quiet_NaN();
  double hi = std::numeric_limits<double>::quiet_NaN();
};


// The rows in e(b)'s order and with e(b)'s names (eval::stataEbColumns): `_cons`
// last, and a factor's base level as a row of its own, b 0 and se 0 with t and
// p missing and both limits 0 (w2at_table_writers.log:4726-4773 wt_p01 `x1 x2
// 0b.g 1.g 2.g _cons`, row 3 `0|0|.|.|0|0`; regsave wt_r01 row 3 `0b.g` 0 0 0 0).
std::vector<CoefRowView> coefViews(const ojson& snap);


// `level(#)` other than the level the model was FITTED at cannot be honoured
// without an inverse-t this build does not have. Refused by name; the fix the
// message names (refit with level(#)) produces the right interval.
std::optional<ojson> rejectForeignLevel(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& cmd);
}
