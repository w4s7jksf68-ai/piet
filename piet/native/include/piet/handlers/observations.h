// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Command entry points: keep, drop, sort, sample, expand, duplicates, collapse, contract.
// Implemented under piet/native/src/datamgmt/.
#ifndef PIET_NATIVE_HANDLERS_OBSERVATIONS_H
#define PIET_NATIVE_HANDLERS_OBSERVATIONS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers {

// `drop VARLIST` / `keep VARLIST` (no `if`) — mutate.py's run_drop/run_keep.
nlohmann::ordered_json runDrop(const std::vector<std::string>& varlist, SessionState& session);
nlohmann::ordered_json runKeep(const std::vector<std::string>& varlist, SessionState& session);

// `drop if COND [in RANGE]` / `keep if COND [in RANGE]` — numeric-only
// vertical slice (routes to cobra eval --keepif); string/factor-referencing
// conditions are OUT of the M3a slice (strmutate.py's row-wise
// filter_rows_if is M3b). `ifExpr` may be an empty string when only
// `inRange` is present (BS-090, BETA Phase G) — piet::handlers::
// applyIfAndInFilter treats an empty ifExpr as absent, same as everywhere
// else in this codebase.
nlohmann::ordered_json runDropIf(const std::string& ifExpr, SessionState& session,
                                  const std::optional<std::string>& inRange = std::nullopt);

// BUG-532: `by BYVARS [(SORTVARS)]: keep if COND` (drop=false) / `drop if COND`
// (drop=true). COND is evaluated per by-group (group-local _n/_N, x[k], sum())
// in the rows' PHYSICAL order. `stataSortKeys` empty: the caller has checked
// that the rows are already in BYVARS++SORTVARS order (plain `by`, or a
// `bysort` with nothing to do). Non-empty: the rows are first sorted in place on
// those keys in STATA's order (stringKeyOrder below), the `bysort` half. One
// fused `eval --bygen --keepif --drop` call does the rest. A missing COND counts
// as true, as in Stata (COND's own `!`, `&`, `|` keep the engine's reading of
// a missing operand -- BUG-620, an engine fix). A comparison of a string
// variable with the blank literal "" is evaluated PIET-side (the engine folds
// `s==""` to a constant whenever "" is not a dictionary level — BUG-532); any
// other string condition is refused by name, r(199).
nlohmann::ordered_json runByKeepIf(SessionState& session, const std::string& ifExpr,
                                    const std::vector<std::string>& byVars,
                                    const std::vector<std::string>& stataSortKeys, bool drop);

// BUG-532: Stata orders a string by its bytes, the engine orders a factor by
// its dictionary CODE (first-seen order; a -1 code, a blank read from .dta or
// CSV, first). StringKeyOrder maps each code to the rank of its level in byte
// order: `rank[code]` for code >= 0, `blankRank` for code -1 (Stata reads it
// as ""). `codeOrderIsStata`: the dictionary is already strictly byte-ordered
// and has no "" level, so the engine's code order IS Stata's order.
// `blankLevel`: "" is also a level, so a -1 row and a "" row are one Stata
// value under two codes (a by-group the engine would split).
struct StringKeyOrder {
  std::vector<double> rank;
  double blankRank = 0;
  bool codeOrderIsStata = true;
  bool blankLevel = false;
};
StringKeyOrder stringKeyOrder(const std::vector<std::string>& levels);

nlohmann::ordered_json runKeepIf(const std::string& ifExpr, SessionState& session,
                                  const std::optional<std::string>& inRange = std::nullopt);

// `sort VARLIST` — editor.py's run_sort (ascending, stable). BUG-532: when the
// head's sort claim already begins with VARLIST the rows are not touched and
// the (longer) claim is kept, Stata's rule ([D] sort: a sort the data already
// satisfy does nothing).
nlohmann::ordered_json runSort(const std::vector<std::string>& varlist, SessionState& session);

// BUG-532: Stata's `bysort KEYS [(SORT)]:` half — sort the data in place on
// `keys` (KEYS then SORT) in STATA's order and leave them marked sorted by
// `keys`. A string key whose dictionary is not byte-ordered is sorted through a
// byte-rank scratch column (stringKeyOrder, the runByKeepIf mechanism; the
// engine's own sort orders a factor by its first-seen code) — the engine then
// mints no claim, as for every string key; every other key list goes through
// runSort. The engine's gsort moves no row when the marker already begins with
// `keys`, and the marker becomes `keys` (Frame::gsort; w2e_probes.log
// bo_sort_prefix_noop). Returns the error block, or nullopt.
std::optional<nlohmann::ordered_json> sortInStataOrder(SessionState& session,
                                                        const std::vector<std::string>& keys);

// BS-092 (BETA campaign, Phase G): NON-MUTATING sibling of runSort — sorts
// `basePath` by `byVars` into a fresh scratch .hbin without touching
// session state (unlike runSort/runGsort, which session.load() the
// result). Throws CobraError on failure. See the .cpp comment for why a
// read-only `by:`/`bysort:` grouped-dispatch loop needs this instead of the
// mutating sort handlers.
std::string sortForGrouping(SessionState& session, const std::string& basePath,
                            const std::vector<std::string>& byVars);

// `insobs # [, before(#) after(#)]`.
nlohmann::ordered_json runInsobs(const eval::ParsedCommand& parsed, SessionState& session);

// `sample # [, count]` -- Philox key per row, keep the k smallest.
nlohmann::ordered_json runSample(const eval::ParsedCommand& parsed, SessionState& session);
// `splitsample [, generate() nsplit() rseed() show]`.
nlohmann::ordered_json runSplitsample(const eval::ParsedCommand& parsed, SessionState& session);

// `bsample [#] [, cluster() idcluster()]`.
// Whether `bsample` takes bsample.ado's fast method (no #, no cluster/strata/
// idcluster/weight): Stata's own rows from the session stream, with e(sample)
// kept on the drawn originals and cleared on the copies.
bool bsampleIsStataFastPath(const eval::ParsedCommand& parsed);
nlohmann::ordered_json runBsample(const eval::ParsedCommand& parsed, SessionState& session);

// `collapse (stat) varlist [(stat) varlist...] [, by(g1 g2)] [if COND] [in RANGE]`.
// `weightVar` (Step-1 D3): when set (tokenizer only admits this for
// `[fw=]`/`[fweight=]`), forwarded as `--fweight weightVar` to cobra's
// `collapse` alongside `--by` — cobra does the actual Σw math and rejects
// unsupported combinations (e.g. weighted median/p##) itself. `inRange`
// (BS-090, BETA Phase G): `in a/b`, composed with `ifExpr` via AND
// (piet::handlers::applyIfAndInFilter) BEFORE cobra's collapse call, rather
// than forwarded as a raw `--keepif` (see the .cpp for why).
// `opts`: the line's options as the dispatcher resolved them; the option words
// (cw, clabel, nocompress, append, smart, fast) are read off their keys.
nlohmann::ordered_json runCollapse(const std::string& rawLine,
                                    const std::optional<std::string>& ifExpr,
                                    SessionState& session,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                    const std::optional<std::string>& weightVar = std::nullopt,
                                    const std::optional<std::string>& inRange = std::nullopt,
                                    const std::string& flavor = "collapse",
                                    const std::optional<std::string>& weightType = std::nullopt);

// `gsort [-]VAR ...` — generalised per-key ascending/descending sort.
nlohmann::ordered_json runGsort(const std::vector<std::string>& tokens, SessionState& session);

// `duplicates report|drop|tag|list [VARLIST] [if COND] [in RANGE] [, gen(NEWVAR)]`.
// `ifExpr` (BUG-042, BS-031(a)): report/tag/list all honor `if` (tag/list
// fixed this stint — see their own .cpp comments); `drop` honestly rejects
// `if` instead of ignoring it (see runDuplicatesDrop's .cpp comment for why
// that one stayed a reject rather than a full fix this stint). `inRange`
// (BS-090, BETA Phase G) follows the SAME split: report/tag/list now honor
// `in` (composed with `if` via AND); `drop` honestly rejects `in` too, for
// the identical reason it already rejects `if` (see runDuplicatesDrop's
// .cpp comment — restricting the sample before dedup-and-splice-back needs
// more machinery than a simple filter, for `in` exactly as much as `if`).
nlohmann::ordered_json runDuplicates(const std::string& sub,
                                      const std::optional<std::vector<std::string>>& varlist,
                                      const std::optional<std::string>& genvar,
                                      SessionState& session,
                                      const std::optional<std::string>& ifExpr = std::nullopt,
                                      const std::optional<std::string>& inRange = std::nullopt);

// `isid VARLIST` — assert that VARLIST uniquely identifies the observations.
// Silent (no output, rc 0) when it does; Stata's r(459) with its own wording
// when it does not, r(111) for an unknown variable. Oracle: StataBE 19.5 —
// `isid make` on auto.dta → rc 0, `isid foreign` → "variable foreign does not
// uniquely identify the observations" + r(459), `isid nosuchvar` → r(111).
// PIET-native (the engine has no isid verb): a single pass over the key
// columns' mmap'd payloads, no scratch file written.
nlohmann::ordered_json runIsid(const std::vector<std::string>& varlist, SessionState& session,
                                bool missok = false, bool sort = false);

// P1 wave data verbs — thin forwarders onto the engine subcommands of the same
// name (the joinby/tsfill shape: PIET validates and resolves the `using` file,
// the engine does the transformation). Oracle pins live on the engine side.
//   `expand N`               — each observation becomes N copies
//   `fillin v1 v2 [...]`     — rectangularize + `_fillin` flag
//   `cross using FILE`       — Cartesian product (master wins a shared name)
//   `contract varlist`       — distinct combinations + `_freq`
// B4-W3b: `generate(newvar)` marks each row 0 (an original) or 1 (a copy the
// expansion added). Measured (StataBE 19.5, pins_oracle_g.log:227-233,
// `sysuse auto` + `expand 2, generate(dupflag)`): _N 74 -> 148, dupflag[1]==0,
// and `tabulate dupflag` is 74 "Original observation" / 74 "Duplicated
// observation" — i.e. the copies are APPENDED, which cobra's own expand does
// too (verified on this build: an `origrow=_n` column reads 1,2,... at rows
// 1,2 and again at rows N+1,N+2).
nlohmann::ordered_json runExpand(const std::string& countArg,
                                 const std::optional<std::string>& genVar,
                                 SessionState& session);
nlohmann::ordered_json runFillin(const std::vector<std::string>& varlist, SessionState& session);
nlohmann::ordered_json runCross(const std::string& usingPathArg, SessionState& session);
nlohmann::ordered_json runContract(const std::vector<std::string>& varlist, SessionState& session);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_HANDLERS_OBSERVATIONS_H
