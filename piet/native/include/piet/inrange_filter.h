// BS-090 (BETA campaign, Phase G): shared row-range (`in a/b`) + `if` filter
// composition for every command whose row-filtering ultimately goes through
// `cobra eval --keepif` (as opposed to gen/replace's `--bygen`-native `_n`
// folding — egen.cpp's parseGenInBound/parseGenInRange, BUG-077).
//
// Why this file exists (not just a bigger tokenizer allow-list): Stata's
// `in a/b` folds naturally into a `_n`-based predicate (`_n>=a & _n<=b`),
// which is exactly what gen/replace's own bysort-fallback `--bygen` call
// already understands (BUG-077's runBysortGen). But op-cobra's expression
// evaluator (src/data/frame.cpp's ev()) throws "_n requires a bysort
// context" whenever its EvalCtx.order is null — and EVERY `--keepif` call
// in this codebase runs through Frame::keep_if(), which never sets .order.
// So forwarding a `_n`-based predicate as a `--keepif` argument (the naive
// generalization of the gen/replace trick) is a hard engine_error for every
// OTHER command. This instead materializes a REAL numeric row-index column
// via `cobra eval --bygen IDXVAR=_n --out tmp` first (an empty by-list
// gives the GLOBAL row index — the same BUG-077 precedent gen/replace's own
// standalone-_n path relies on), turns `in a/b` into an ordinary
// `IDXVAR>=a & IDXVAR<=b` numeric predicate over that materialized column
// (no `_n`/`_N` token left anywhere in the final --keepif expression), ANDs
// it with any real `if`, runs ONE more `--keepif`, then drops IDXVAR again
// before returning — 2-3 cobra round trips instead of gen/replace's zero
// extra, but the SAME AND-composition semantics every legal `in`-command
// already promises (D7).
#ifndef PIET_NATIVE_INRANGE_FILTER_H
#define PIET_NATIVE_INRANGE_FILTER_H

#include <cstdint>
#include <optional>
#include <string>

#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
}

namespace piet::handlers {

// Parsed `in a/b` bounds, ALREADY resolved to literal 1-based row numbers
// (f -> 1, l -> nRowsTotal, -k -> nRowsTotal-k+1). Every consumer of this
// struct needs a plain numeric bound, whether that's this file's own --keepif
// predicate, gen/replace's `_n` fold (egen.cpp), list's browse window
// (handlers_m3.cpp) or strmutate.cpp's row-wise mask. This is THE one parser
// of Stata's range grammar ([U] 11.1.4): `#1[/#2]`, each # a positive or
// negative integer (negative counts from the end, -1 = last row) or f/F/l/L.
// A resolved range is honoured only when 1 <= a <= b <= nRowsTotal; anything
// else is refused with r(198) and Stata's own sentence (see `refusal`).
struct InRangeBounds {
  bool ok = false;
  // Kept for the callers that still copy it into an InRangeFilterResult; no
  // longer set, since from-the-end bounds now resolve instead of refusing.
  bool negative = false;
  long long a = 0;
  long long b = 0;
  // Non-empty iff the grammar parsed but the resolved range is not a legal
  // one: Stata's own message ("observation numbers out of range", or "'0'
  // invalid observation number"). Empty with ok==false = a malformed string.
  std::string refusal;
};

InRangeBounds parseInRangeBounds(const std::string& raw, int64_t nRowsTotal);

struct InRangeFilterResult {
  bool ok = true;        // false -> malformed/out-of-range `in` bound; see inRangeRejectError()
  bool negative = false;  // legacy, no longer set (see InRangeBounds::negative)
  std::string refusal;    // InRangeBounds::refusal, carried through
  std::string dataPath;   // valid iff ok==true; == basePath unchanged when if/in both absent
};

// `basePath` must already exist on disk (a live session hbin or an earlier
// scratch file) and is never mutated by this call. On success (`ok==true`),
// `dataPath` is either `basePath` itself (neither `if` nor `in` present ->
// zero extra cobra calls, byte-identical to the pre-BS-090 per-file
// prefilter blocks this replaces) or a freshly-written scratch .hbin under
// session.workdir() that the CALLER owns (matches this codebase's
// established scratch-file convention: read-only callers leave it for the
// session workdir's normal lifecycle, mutation callers session.load() it).
//
// `complement` (default false): when true, keeps every row that does NOT
// match the combined (if AND in) condition instead of every row that does —
// `drop`'s semantics (BS-090), mirroring runDropIf's pre-existing
// `"!(" + ifExpr + ")"` trick for the if-only case, generalized to also
// cover a materialized in-range predicate.
//
// Any cobra subprocess failure (bygen/keepif/drop) is NOT caught here — it
// propagates as a CobraError, exactly like every existing single-call
// prefilter block this replaces, so callers keep their existing
// `try { ... } catch (const CobraError& ce) { return engineErrorBlock(...); }`
// wrapped around the call to this function instead of around a raw
// runCobraJson() call. Only a malformed/out-of-range `in` STRING (a user error,
// not an engine error) comes back as `ok=false` — never an exception.
// P2-PERF ARC-C C1-①: the materialized row-index column is a function of the
// BASE FILE ALONE — it does not depend on the range being sliced. A caller
// slicing the same base file G times (dispatcher.cpp's `by:`/`bysort:` grouped
// dispatch is the one such caller) builds it ONCE with buildRowIndexCache()
// and hands the result to every applyIfAndInFilter() call, which then costs a
// single --keepif instead of re-deriving `_n` over the whole dataset per group.
// `nRows` is carried alongside so the `f`/`l` bound resolution does not reopen
// the base either. Passing nullptr keeps the self-contained behaviour.
struct RowIndexCache {
  std::string path;    // basePath with the row-index column materialized
  int64_t nRows = 0;   // row count of basePath (for `f`/`l` bound resolution)
};

RowIndexCache buildRowIndexCache(SessionState& session, const std::string& basePath);

InRangeFilterResult applyIfAndInFilter(SessionState& session, const std::string& basePath,
                                       const std::optional<std::string>& ifExpr,
                                       const std::optional<std::string>& inRange,
                                       bool complement = false,
                                       const RowIndexCache* idx = nullptr);

// Standard BS-090 "in"-range reject error block, the ONE builder every `in`
// consumer uses, so a user sees the same sentence whichever command's `in`
// failed. Always r(198), Stata's code for both a malformed and an out-of-range
// `in`. `r.refusal` carries Stata's message when the caller had it; a caller
// that built `r` from InRangeBounds without copying it still gets an honest
// range message (the text is re-read here), never the "unsupported form" one.
// `cmdLabel` names the failing form in the malformed-string message (e.g.
// "duplicates report", "summarize").
nlohmann::ordered_json inRangeRejectError(const InRangeFilterResult& r, const std::string& inRangeRaw,
                                          const std::string& cmdLabel);

}  // namespace piet::handlers

#endif  // PIET_NATIVE_INRANGE_FILTER_H
