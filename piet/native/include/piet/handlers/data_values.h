// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: commands that compute values into variables.
// Implemented under piet/native/src/datamgmt/; geodist/geonear in src/utility/ and
// sctorezone in src/time/.
#ifndef PIET_NATIVE_HANDLERS_DATA_VALUES_H
#define PIET_NATIVE_HANDLERS_DATA_VALUES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `generate NEWVAR = EXPR [if COND]` — the NUMERIC route: `cobra eval --gen`.
// The dispatcher still detects string-typed (str#/strL) and bysort `_n`/`_N`
// expressions before this handler and routes them elsewhere — but they are no
// longer OUT OF SCOPE, which is what the TODO(M3) here used to say: they land in
// `strmutate::runStringGen` and `egen::runBysortGen`. Marker retired 2026-08-18
// (INSPECT-2608 census); the routing split it describes is current, the
// "unsupported_command" half was stale.
// `storeType` (BUG-471 T2): the resolved Stata storage type the engine stores
// the result as (`--store-type`); "" or "double" = the full double.
nlohmann::ordered_json runGen(const std::string& expr, const std::optional<std::string>& ifExpr,
                               SessionState& session, const std::string& storeType = "");

// `replace VARNAME = EXPR [if COND]` — numeric-only vertical slice, mirrors
// mutate.py's run_replace (cond()-based in-place rewrite via cobra eval).
// `storeType` (BUG-471 T2) is the variable's resolved type; an integer type
// widens by Stata's replace lattice unless `noPromote` (piet/storage_type.h).
nlohmann::ordered_json runReplace(const std::string& expr, const std::optional<std::string>& ifExpr,
                                   SessionState& session, const std::string& storeType = "",
                                   bool noPromote = false);

// ---- AER-SCOPE1 bundle F (RESHAPE, 2026-09-04) -----------------------------
// Seventeen shape/sample verbs on top of the established read/write pair:
// reading is `fetchColumns`, writing is a cobra verb + `session.load` +
// `mutationResult` (engine/mutation_result.cpp). **No new cobra verb** -- every one
// of these is a composition of setcol / convert / append / merge / eval, so the
// engine binary and the engine lane are untouched by this bundle.
//
// Randomness is Philox (`rng.h`) keyed on `session.seed`, never a global
// generator: `set seed 7` followed by the same command twice must give the same
// rows, and a per-row key makes that true without ordering assumptions.

// `split v, parse() gen() limit() destring` -- PIET-side decomposition, then k
// `setcol --new` calls. Deliberately NOT `gen stubj = word(v,j)`: `word()` is
// whitespace-only, and `parse("-")` is the corpus's most common form.
nlohmann::ordered_json runSplit(const eval::ParsedCommand& parsed, SessionState& session);

// `separate v, by(g) [shortlabel generate()]`.
nlohmann::ordered_json runSeparate(const eval::ParsedCommand& parsed, SessionState& session);
// `mvencode varlist, mv(#) [override]`.
nlohmann::ordered_json runMvencode(const eval::ParsedCommand& parsed, SessionState& session);

// `drawnorm v1 v2 ..., n() means() sds() corr()|cov() seed() clear` -- a small
// dense Cholesky; a non-PD matrix is refused by name rather than sampled from.
nlohmann::ordered_json runDrawnorm(const eval::ParsedCommand& parsed, SessionState& session);

// `cumul v, gen(c) [equal freq]`.
nlohmann::ordered_json runCumul(const eval::ParsedCommand& parsed, SessionState& session);

// `pctile new = v, nq()|percentiles() [genp()]` and the r-class `_pctile` form.
// Both use the SAME percentile rule as `xtile`/`winsor2` (transcribed from the
// engine's own stata_pctile), because two percentile rules in one product is a
// silently wrong number waiting for a fixture that does not exist.
nlohmann::ordered_json runPctile(const eval::ParsedCommand& parsed, SessionState& session);
nlohmann::ordered_json runPctileR(const eval::ParsedCommand& parsed, SessionState& session);

// `ihstrans varlist [if] [in] [, keepusing() prefix() casewise]` (SSC) -- new
// ihs_<var> columns = log(v+sqrt(v^2+1)).
nlohmann::ordered_json runIhstrans(const eval::ParsedCommand& parsed, SessionState& session);
// `sctorezone shift, force [only() exclude()]` (SSC scto) -- shift %tc variables
// by a fixed number of hours.
nlohmann::ordered_json runSctorezone(const eval::ParsedCommand& parsed, SessionState& session);

// `geodist lat1 lon1 lat2 lon2, gen(newvar) [miles sphere radius()]` --
// Vincenty on WGS84 by default; `sphere` is haversine.
nlohmann::ordered_json runGeodist(const eval::ParsedCommand& parsed, SessionState& session);
// `geonear nid nlat nlon using F, neighbors(...) [nearcount() miles ...]`.
nlohmann::ordered_json runGeonear(const eval::ParsedCommand& parsed, SessionState& session);

// ---- AER-SCOPE1 bundle A (2026-09-03) --------------------------------------
// Cox's `winsor VAR, gen(NEW) p(#) | h(#) [highonly|lowonly]`. Deliberately NOT
// routed to runWinsor2: winsor2 clamps at the `_pctile` value, which averages
// the two neighbours when N*p lands on an integer, while winsor.ado replaces
// the h lowest observations with the (h+1)-th ORDER STATISTIC. Same intent,
// different number, and the difference is invisible in the output -- an alias
// here would be a silent wrong answer rather than a missing feature.
nlohmann::ordered_json runWinsorCox(const eval::ParsedCommand& parsed, SessionState& session);

// -- M3b: MI-066 data verbs + egen (ROADMAP M3b) -----------------------------

// `egen NEWVAR = fn(...) [if COND] [in RANGE] [, by(g) missing p(#) ...]` —
// C++ port of egen.py + mutate.py's run_egen. Row functions (rowmean/
// rowsd/...) route to cobra eval --rowegen; everything else routes to
// --egen. `by` is the already-extracted by()-clause value (bysort's
// inline-injection path AND a plain `egen X=fn(y), by(g)` both funnel
// through here with `by` populated from opts["by"] or the bysort prefix).
// `inRange` (BS-090, BETA Phase G): composes with `ifExpr` via AND, reusing
// the SAME synthetic row-index column this handler already materializes
// for the if-merge-back key (see the .cpp comment).
// `storeType` (BUG-471 T2): the resolved result type; withheld from tag()/
// seq()/mode(), whose ados ignore it.
// `fixedTypeOut`: the result's type where the function, not the declaration,
// decides it -- for seq() the type Stata's widening replace leaves (BUG-471 T2
// fix-up F3: egen seq() generates a byte, then widens, chosen from the stored
// min/max the computing call itself summarizes); for mode() of a string the
// str# its compress leaves (BUG-1109); for cut() and the rest of
// runEgenAdoFunction the type they recorded for the result (BUG-1108). Untouched
// otherwise.
// `gtools`: the line was gtools' gegen, whose own total()/sum()/mean() sum in
// data order where official egen reads summarize's exact sum.
nlohmann::ordered_json runEgen(const std::string& expr, const std::string& by,
                                const std::optional<std::string>& ifExpr,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                SessionState& session,
                                const std::optional<std::string>& inRange = std::nullopt,
                                const std::string& storeType = "",
                                std::string* fixedTypeOut = nullptr, bool gtools = false);

// `[bysort VARLIST:] gen/replace newvar = expr` — C++ port of bysort.py's
// run_bysort_gen (cobra eval --bygen, native _n/_N group-context support).
// byVars=[] means global (no by()/sort() clause). Called by the dispatcher's
// bysort/by prefix interception (any gen/replace sub-command, not just ones
// referencing _n/_N — matches the oracle's "keep separate for clarity"
// design, see egen.cpp) AND by the direct (non-bysort) gen/replace dispatch
// whenever `_n`/`_N` appears OR an `in a/b` range is present (BUG-077).
// `inRange` (default nullopt) is the raw tokenizer-captured qualifier string
// (e.g. "1/10", "f/l"); only the direct-dispatch call sites pass it — the
// bysort/by-prefix call site never does (a by-group `_n` can't express
// Stata's GLOBAL `in` range; see the tokenizer's inRangeAllowed comment).
// `sortVars` (default nullopt = preserve the old sortFirst-driven default)
// is BS-002's explicit parenthetical sort key (`bysort byvar (sortvar):`) —
// only the bysort/by-prefix call site ever passes a non-empty value.
// BUG-532: `bysort KEYS (SORT):`'s in-place sort for generate/replace, in
// Stata's order and with Stata's marker; skipped when KEYS SORT already lead
// the marker. Returns the error block, or nullopt.
std::optional<nlohmann::ordered_json> sortForBysort(
    SessionState& session, const std::vector<std::string>& byVars,
    const std::optional<std::vector<std::string>>& sortVars);

nlohmann::ordered_json runBysortGen(
    SessionState& session, const std::string& expr, const std::vector<std::string>& byVars,
    const std::optional<std::string>& ifExpr, bool sortFirst, bool isReplace,
    const std::optional<std::string>& inRange = std::nullopt,
    const std::optional<std::vector<std::string>>& sortVars = std::nullopt,
    const std::string& storeType = "", bool noPromote = false);

// `recode VARNAME (rules...) [if COND] [, gen(NEWVAR)]` — value recoding.
// `if` requires `gen()` in this build (BS-031(b) — see .cpp comment).
nlohmann::ordered_json runRecode(const std::string& rawLine, SessionState& session,
                                  const std::optional<std::string>& ifExpr = std::nullopt);

// BUG-371 — `decode` (the inverse of encode: value-label codes back to strings)
// and `inspect` (the numeric composition of one column). Both are PIET-native:
// decode reads session.valueLabels and writes ONE column via `cobra setcol`, and
// inspect is arithmetic over an mmap'd column, the same reason `isid` is native.
nlohmann::ordered_json runDecode(const std::string& varname,
                                 const std::optional<std::string>& genVar, SessionState& session,
                                 size_t maxLength = 32000);

// BUG-370 — xtile / ipolate / tssmooth ma. The engine half of all three already
// shipped (`cobra eval --xtile/--ipolate/--tsma`, Stata-parity tested in
// tests/unit/test_xtipo.cpp and test_tsma.cpp); what was missing was the command
// table row pointing at it. Each of these is argument validation plus one engine
// call, and each refuses BY NAME the Stata options the engine has no equivalent
// for, rather than dropping them silently.

nlohmann::ordered_json runXtile(const std::string& newvar, const std::string& srcvar,
                                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                const std::optional<std::string>& ifExpr, SessionState& session,
                                const std::optional<std::string>& weightType = std::nullopt,
                                const std::optional<std::string>& weightVar = std::nullopt);
// xtile's weighted form (datamgmt/pctile.cpp, beside the weighted
// percentile rule it shares with pctile/_pctile).
nlohmann::ordered_json runXtileWeighted(const std::string& newvar, const std::string& srcvar,
                                        int nq, const std::string& weightVar,
                                        const std::string& weightType, SessionState& session);
nlohmann::ordered_json runIpolate(const std::vector<std::string>& varlist,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::optional<std::string>& ifExpr, SessionState& session);
nlohmann::ordered_json runTssmoothMa(const std::string& newvar, const std::string& srcvar,
                                     const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                     SessionState& session);

// `encode STRVAR, gen(NEWVAR) [label(LBLNAME)]` — string(factor)->numeric
// (1..K, alphabetical) with a value-label set registered + attached. C++
// port of encode_op.py's run_encode. Blocking the debug_all.do M4a depth
// boundary at line 157 — folded in alongside the M4a estimation surface
// even though it predates this campaign's explicit item list, since it
// unblocks stream-replay depth for near-zero marginal scope.
// BUG-042: `ifExpr` restricts BOTH which rows get a real encoded value
// (matching Stata's `encode ... if COND` — rows failing COND come back
// missing on `newvar`) AND nothing else (the level->code mapping itself
// still comes from the whole variable — see egen.cpp's runEgen for why the
// mapping and the per-row gating are legitimately different scopes).
nlohmann::ordered_json runEncode(const std::string& strvar, const std::string& newvar,
                                  const std::optional<std::string>& lblname, SessionState& session,
                                  const std::optional<std::string>& ifExpr = std::nullopt,
                                  bool noExtend = false);

// `destring VAR, {replace | gen(NEWVAR)} [ignore("chars") force float percent
// dpcomma]` -- BS-003/D4 (BETA B-data): PIET-native string(factor)->numeric
// conversion of ONE variable; the dispatcher's `destring`/`tostring` arm runs
// the varlist loop and the option checks, and calls this once per variable.
// Behaviour as measured on StataBE 19.5. In short: an
// already-numeric VAR, or one holding a value that does not read as a number
// (without `force`), is declined with a `display` note at rc 0 and nothing is
// created; otherwise the parsed f64 column replaces VAR or is added as NEWVAR
// right after VAR, and VarMeta records the storage type Stata's destring
// reports (byte/int/long when every value is integral and in range, else
// double, or float under `float`). `opts` carries the three options beyond
// replace/generate/ignore/force: float (create as float instead of double),
// dpcomma (comma as the decimal separator) and percent (ignore `%` and divide
// by 100).
struct DestringOpts {
  bool asFloat = false;
  bool dpcomma = false;
  bool percent = false;
};
nlohmann::ordered_json runDestring(const std::string& varname, bool doReplace,
                                    const std::optional<std::string>& genVar,
                                    const std::optional<std::string>& ignoreChars, bool force,
                                    SessionState& session, const DestringOpts& opts = {});

// `tostring VAR, {gen(NEWVAR) | replace} [usedisplayformat]` — BS-003/D4
// (BETA B-data): PIET-NATIVE numeric->string(factor) conversion, the mirror
// of runDestring above. Default rendering matches datamgmt/row_evaluation.cpp's own
// coerceStr() convention (integer -> plain digits, non-integer ->
// eval::gFormat, missing -> ""); `usedisplayformat` instead renders via
// eval::applyStataFmt() against the variable's OWN session.varMeta[...].
// format (BS-005/D6's same field; "%9.0g" default when unset) — note
// applyStataFmt only recognizes the numeric %{f,g,e} families, so a
// date/time display format falls back to its own generic-number rendering
// (a pre-existing limitation of that shared helper, not newly introduced
// here). Scoped to exactly ONE variable per call, same rationale as
// runDestring.
//
// BUG-665: `opts.stataGate` (set by the `tostring` command arm; left off by
// import excel's allstring, which converts unconditionally) switches on
// tostring's own rules: the rendering format is format() when
// given, the variable's display format under usedisplayformat, else %12.0g;
// the value rendered is the STORED value (a float-typed variable is rounded
// to float first -- w2b_dataio.log tostring_gen_force renders float 7.01 as
// "7.010000229"); a variable any of whose rendered values does not read back
// equal is left as is with "VAR cannot be converted reversibly; no
// replace|generate" (rc 0) unless `force`; an already-string variable is a
// note, not an error; `replace` on a value-labelled variable is refused by
// note unless `force`. `forcedLoss`, when given, says whether a conversion
// went through only because of `force` (some value does not read back), for
// the ado's "VAR was forced to string; some loss of information" line.
struct TostringOpts {
  bool stataGate = false;
  std::optional<std::string> format;
  bool force = false;
};
nlohmann::ordered_json runTostring(const std::string& varname, bool doReplace,
                                    const std::optional<std::string>& genVar, bool useDisplayFormat,
                                    SessionState& session, const TostringOpts& opts = {},
                                    bool* forcedLoss = nullptr);

// -- M4a: output-completion surface (ROADMAP M4a) ----------------------------

// `winsor2 VARLIST [, cuts(p_lo p_hi) trim by(G) suffix(SUF) replace]` — C++
// port of winsor2.py's run_winsor2 (SSC winsorize via cobra eval --winsor).
nlohmann::ordered_json runWinsor2(SessionState& session, const std::vector<std::string>& varlist,
                                   double cutLo, double cutHi, bool trim, const std::string& by,
                                   const std::string& suffix, bool replace);

// `mkspline stub = varname [, cubic [nknots(#) | knots(v1,v2,...)]]` — BS-087
// (part). ONLY the `cubic` (restricted cubic spline) form is supported —
// real Stata's linear/piecewise-linear default (no `cubic` keyword) has no
// engine equivalent (live-verified: the engine ALWAYS computes a cubic
// basis regardless of the keyword) and is honest-rejected rather than
// silently answered with a cubic result. `if` is honest-rejected too (the
// engine's --keepif DROPS rows from the whole dataset rather than leaving
// the new spline vars missing, unlike real Stata's if-qualified generate-
// family semantics). See the .cpp header comment for the nknots(#) ENGINE
// BUG disclosure (flagged for Auto Patcher, not fixed here).
nlohmann::ordered_json runMkspline(const std::string& stubIn, const std::string& srcVarIn,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::optional<std::string>& ifExpr, SessionState& session);

//
// datamgmt/mvdecode.cpp, integ.cpp, sencode.cpp, fillmissing.cpp, astile.cpp -- verbs that write a column.
// `mvdecode varlist, mv(numlist[=.] [\ numlist[=.]])`.
nlohmann::ordered_json runMvdecode(const eval::ParsedCommand& parsed, SessionState& session);
// `integ yvar xvar [if] [in] [, generate() trapezoid]` -- sets r(integral).
nlohmann::ordered_json runInteg(const eval::ParsedCommand& parsed, SessionState& session);
// `sencode strvar [if] [in], generate()|replace [label() gsort()]` (Newson).
nlohmann::ordered_json runSencode(const eval::ParsedCommand& parsed, SessionState& session);
// `fillmissing varlist [if] [in] [, with()]` (Shah); the by-prefix arrives as by().
nlohmann::ordered_json runFillmissing(const eval::ParsedCommand& parsed, SessionState& session);
// `astile newvar = var [if] [in] [, nq() by()]` (Shah); the by-prefix arrives as by().
nlohmann::ordered_json runAstile(const eval::ParsedCommand& parsed, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_DATA_VALUES_H
