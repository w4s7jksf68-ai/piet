// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `[bysort VARLIST:] generate/replace` (and a direct generate/replace with _n or
// `in`) through `cobra eval --bygen`: the in-place bysort sort, the masked replace
// store-type probe and the store. Split from egen.cpp without changing behavior.
#include "piet/handlers/data_values.h"
#include "piet/handlers/observations.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum
#include "piet/translate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"  // isIdentByte: cut()'s argument
#include "data/missing.h"  // missing_kind: string() of a .a cell
#include "io/win_compat.h"  // to_native_path, native_exists, native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // cut's value label and the any* note are ordinary commands
#include "piet/display_hardening.h"  // exprUnboundNameRefusal: cut()'s =/exp
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // BUG-1001: expandVarlist, the one varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "egen_internal.h"

namespace piet::handlers {

using namespace egen_detail;

namespace egen_detail {

// bysort.py's _make_bygen_expr: "newvar=rhs_expr, by(v1,v2), sort(s1,s2)".
// by_vars=[] (global) omits both by()/sort(); sort_vars defaults to by_vars.
std::string makeBygenExpr(const std::string& expr, const std::vector<std::string>& byVars,
                          const std::optional<std::vector<std::string>>& sortVars) {
  std::string s = expr;
  if (!byVars.empty()) {
    std::string joined;
    for (size_t i = 0; i < byVars.size(); ++i) {
      if (i) joined += ",";
      joined += byVars[i];
    }
    s += ", by(" + joined + ")";
    const std::vector<std::string>& sv = sortVars.has_value() ? *sortVars : byVars;
    if (!sv.empty()) {
      std::string sjoined;
      for (size_t i = 0; i < sv.size(); ++i) {
        if (i) sjoined += ",";
        sjoined += sv[i];
      }
      s += ", sort(" + sjoined + ")";
    }
  }
  return s;
}

}  // namespace egen_detail

namespace {

// Does `e` call function `fn`? exprHasRunningSum's scan (exprfns.h) for any
// name: quote-aware and word-bounded, so `rowgroup(` or a "group(" literal do
// not match.
bool exprCallsFn(const std::string& e, const std::string& fn) {
  char quote = 0;
  for (size_t i = 0; i < e.size(); ++i) {
    const char c = e[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (e.compare(i, fn.size(), fn) != 0) continue;
    if (i > 0) {
      const char p = e[i - 1];
      if (cecon::ident::isIdentByte(p) || p == '.') continue;  // core/ident.h
    }
    size_t j = i + fn.size();
    while (j < e.size() && (e[j] == ' ' || e[j] == '\t')) ++j;
    if (j < e.size() && e[j] == '(') return true;
  }
  return false;
}

// BUG-841/842: the replace store-type decision (BUG-471 T2) when the folded
// probe of storage::decideReplaceStoreType would not read the values the store
// writes: a running sum under a qualifier (the fold accumulates the excluded
// rows) or group() under `in` (the fold numbers over the dataset). Measured:
// `generate byte b = 0` / `replace b = sum(x) if _n > 1` with x = -100, 150
// read 50 from the fold and kept byte, so the store wrote `.` for 150. This
// probe is the store's own --bygen with the target swapped for the scratch name
// and the same qualifier flags (no --gen-replace: untouched rows stay missing),
// read-only, so it summarizes exactly the stored values. The second --bygen
// marks the fractional ones under the same groups and qualifiers.
struct MaskedTypeDecision {
  std::string storeType;
  std::optional<ojson> error;
};
MaskedTypeDecision maskedReplaceStoreType(SessionState& session, const std::string& resolved,
                                          bool noPromote, const std::string& rhs,
                                          const std::vector<std::string>& byVars,
                                          const std::optional<std::vector<std::string>>& bygenSort,
                                          const std::vector<std::string>& qualifierArgs,
                                          const std::string& tsExpr) {
  MaskedTypeDecision d;
  d.storeType = resolved;
  if (!storage::isIntegerType(resolved) || noPromote) return d;  // as decideReplaceStoreType
  Datastore* ds = session.datastore();
  if (ds == nullptr) {
    d.error = ojson(eval::noDataLoaded());
    return d;
  }
  const std::string pv = storage::kReplaceProbeVar;
  const std::string pf = pv + "frac";
  const std::string probeArg = makeBygenExpr(pv + "=" + rhs, byVars, bygenSort);
  std::vector<std::string> args = {
      "eval", "--data", ds->path(), "--bygen", probeArg, "--bygen",
      makeBygenExpr(pf + "=(" + pv + "!=int(" + pv + "))", byVars, bygenSort)};
  args.insert(args.end(), qualifierArgs.begin(), qualifierArgs.end());
  args.push_back("--summarize");
  args.push_back(pv + "," + pf);
  args = translate::injectTsContextForExpr(args, tsExpr, session);
  // The store draws from the stream this probe would consume: hand the probe
  // that stream and give it back, as decideReplaceStoreType does.
  const long long savedStream = session.rngStream;
  args = translate::injectRngContextForExpr(args, probeArg + " " + tsExpr, session);
  session.rngStream = savedStream;
  ojson out;
  {
    SessionState::ResidentSuspend suspend(session);
    SessionState::StatelessRead statelessRead(session);
    try {
      out = runCobraJson(args);
    } catch (const CobraError& ce) {
      d.error = engineErrorBlock(cobraErrorStr(ce), ce.code);
      return d;
    }
  }
  const ojson summ = out.value("summarize", ojson::object());
  const ojson sv = summ.value(pv, ojson::object());
  const ojson sf = summ.value(pf, ojson::object());
  const auto num = [](const ojson& o, const char* k) {
    return o.contains(k) && o[k].is_number() ? o[k].get<double>() : 0.0;
  };
  if (!(num(sv, "N") > 0)) return d;  // nothing non-missing is stored: no widening
  d.storeType = storage::promotedType(resolved, num(sf, "max") > 0.0, num(sv, "min"),
                                      num(sv, "max"));
  return d;
}

}  // namespace

// BUG-532: Stata's `bysort KEYS (SORT): generate|replace` SORTS THE DATA by
// KEYS SORT and leaves them sorted (marker KEYS SORT) before it computes, so
// every later row-order read (`_n`, `in`, `list`, `x[_n-1]`, `save`, a plain
// `by`) sees Stata's order (w2_bykeep.log by_p_after_bysgen: a plain `by g
// (b):` runs after `bysort g (b): generate`). The rows are sorted in place, in
// Stata's order (a string key by its bytes; the engine moves no row when the
// marker already begins with KEYS SORT, and the marker becomes KEYS SORT
// either way).
std::optional<ojson> sortForBysort(SessionState& session, const std::vector<std::string>& byVars,
                                   const std::optional<std::vector<std::string>>& sortVars) {
  std::vector<std::string> keys = byVars;
  if (sortVars.has_value()) keys.insert(keys.end(), sortVars->begin(), sortVars->end());
  // bysort skips a redundant sort when its keys already lead the marker;
  // unlike an explicit `sort g`, it then keeps a more-specific `g b` claim
  // (w2c_functions sortedby_bysort_then_by_gb).
  if (Datastore* ds = session.datastore()) {
    std::istringstream marker(ds->sortedBy());
    bool alreadySorted = true;
    for (const auto& key : keys) {
      std::string marked;
      if (!(marker >> marked) || marked != key) { alreadySorted = false; break; }
    }
    if (alreadySorted) return std::nullopt;
  }
  return sortInStataOrder(session, keys);
}

// bysort.py's run_bysort_gen — `[bysort VARLIST:] gen/replace newvar = expr`
// via cobra eval --bygen (which natively supports _n/_N in group context;
// used unconditionally here even for exprs WITHOUT _n/_N, matching the
// oracle's own "keep separate for clarity" design note). `inRange` (BUG-077)
// is only ever non-nullopt from the DIRECT gen/replace dispatch path — the
// bysort/by-prefix call site (dispatcher.cpp) never passes it, since a
// by-group `_n` can't express Stata's GLOBAL `in` range (intentionally left
// rejected by the tokenizer's inRangeAllowed gate for bysort/by).
ojson runBysortGen(SessionState& session, const std::string& expr,
                   const std::vector<std::string>& byVars, const std::optional<std::string>& ifExpr,
                   bool sortFirst, bool isReplace, const std::optional<std::string>& inRange,
                   const std::optional<std::vector<std::string>>& sortVars,
                   const std::string& storeType, bool noPromote) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  auto eqPos = expr.find('=');
  if (eqPos == std::string::npos) {
    return engineErrorBlock("bysort gen: expression must be `newvar = expr`");
  }
  std::string newvar = trim(expr.substr(0, eqPos));
  std::string rhs = trim(expr.substr(eqPos + 1));
  // replace writes an existing variable only. This route never names the
  // target in the expression when the `if` rides --gen-mask, so the engine
  // cannot refuse it there: `replace y = runiform() if _n > 1` and
  // `bysort g: replace y = 1` on an absent y are r(111) in StataBE 19.5.
  if (isReplace) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && !ds->has(newvar)) {
      return ojson(eval::errorBlock("syntax_error", "variable " + newvar + " not found",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    }
  }

  // BUG-077 / BUG-842: `in a/b` rides the engine's own range channel (eval
  // --gen-in), never the expression: group() numbers within an `in` range
  // (w2m_bymask G05 :514) but not within an `if` that names the same rows
  // (G02 :482), so the two must reach the engine apart. Rows outside the range
  // are missing for gen and keep their value for replace (--gen-replace below).
  bool haveRange = inRange.has_value() && !inRange->empty();
  std::string rangeArg;
  std::string rangePred;  // the replace type probe's folded form (see below)
  if (haveRange) {
    // BUG-672/673: the shared [U] 11.1.4 resolver (inrange_filter.cpp), not a
    // private grammar. The range becomes literal global row numbers (this path
    // never runs with a by-list; see above). `_N` is the frame's row count as
    // the macro layer sees it — card first, so a dirty virtual head is not
    // snapshotted.
    InRangeBounds rr = parseInRangeBounds(*inRange, session.macroObsCount());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "gen/replace");
    }
    rangeArg = std::to_string(rr.a) + "/" + std::to_string(rr.b);
    rangePred = "_n>=" + std::to_string(rr.a) + " & _n<=" + std::to_string(rr.b);
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveCombined = haveIf || haveRange;

  // AER scope 2 / B3(c). Folding `if` into cond() is exactly equivalent for every
  // function whose answer depends only on its own row -- but sum() is a RUNNING
  // total, and a folded qualifier makes it accumulate across the rows the
  // qualifier excluded (measured: Stata gives . 2 5 9 . 6 13 21 for paper 015's
  // shape, folding gives . 3 6 10 . 11 18 26). When the right-hand side carries a
  // running sum the condition rides separately as --gen-mask and the engine
  // applies it to the accumulation itself -- under replace too (BUG-841:
  // `replace c09 = sum(x) if _n > 1` summed the unselected row into every later
  // total, -1 60 120 130 160 210 at rc 0, oracle w2m_bymask C09 :361
  // -1 40 100 110 140 190). --gen-replace makes the rows a separate qualifier
  // excludes keep their value, as replace does.
  const bool runningSum = piet::eval::exprHasRunningSum(rhs);
  // A draw rides the mask for the same reason: the engine evaluates both branches
  // of cond(), as Stata does (BUG-1050), so a folded `if` would draw on the rows
  // it excludes, which Stata's qualifier never evaluates.
  const bool rhsDraws = translate::exprHasDrawToken(rhs);
  const bool maskOut = haveIf && (runningSum || rhsDraws);
  const bool keepUnselected = isReplace && (maskOut || haveRange);
  std::string bygenRhs = rhs;
  if (haveIf && !maskOut) {
    bygenRhs = "cond((" + *ifExpr + "),(" + rhs + ")," + (isReplace ? newvar : ".") + ")";
  }
  std::string bygenFullExpr = newvar + "=" + bygenRhs;
  // BS-002: an explicit sortVars (the `(sortvar)` parenthetical) always
  // wins over the sortFirst-driven default — it is a strictly more specific
  // instruction from the same `bysort`/`by` prefix line.
  std::optional<std::vector<std::string>> bygenSort =
      sortVars.has_value() ? sortVars : (sortFirst ? std::optional(byVars) : std::nullopt);
  // BUG-532: --bygen alone only computes through a sort order and leaves the
  // rows where they were (see sortForBysort); after the in-place sort it groups
  // on by() alone, whose stable sort keeps each group's rows in that physical
  // order — also for a string sort key, whose engine code order is not
  // Stata's. A plain `by` (sortFirst false) never sorts.
  if (sortFirst && !byVars.empty()) {
    if (std::optional<ojson> sortErr = sortForBysort(session, byVars, sortVars)) return *sortErr;
    bygenSort = std::vector<std::string>{};
  }
  std::string bygenArg = makeBygenExpr(bygenFullExpr, byVars, bygenSort);

  // BUG-471 T2: a replace widens an integer type by Stata's lattice before the
  // one store (see runReplace, data_generate.cpp). The probe is the same --bygen with
  // the target swapped for the scratch name and the untouched rows missing, run
  // after the in-place sort above so it groups exactly as the store will.
  std::string finalStoreType = storeType;
  // The qualifiers as the store sends them (--gen-replace aside).
  std::vector<std::string> qualifierArgs;
  if (maskOut) qualifierArgs.insert(qualifierArgs.end(), {"--gen-mask", *ifExpr});
  if (haveRange) qualifierArgs.insert(qualifierArgs.end(), {"--gen-in", rangeArg});
  const bool foldedProbeExact =
      !((runningSum || rhsDraws) && haveCombined) &&
      !(haveRange && (exprCallsFn(rhs, "group") || (haveIf && exprCallsFn(*ifExpr, "group"))));
  if (isReplace && !foldedProbeExact) {
    MaskedTypeDecision dec =
        maskedReplaceStoreType(session, storeType, noPromote, rhs, byVars, bygenSort,
                               qualifierArgs, rhs + " " + ifExpr.value_or(std::string()));
    if (dec.error.has_value()) return *dec.error;
    finalStoreType = dec.storeType;
  } else if (isReplace) {
    // decideReplaceStoreType's probe is one --bygen value, so it reads the
    // qualifiers folded (the `if` half `!=0`-normalised, BUG-622); for every
    // shape that reaches here the fold computes the stored values exactly.
    std::string combinedIf;
    if (haveIf && haveRange) {
      combinedIf = "((" + *ifExpr + ")!=0) & (" + rangePred + ")";
    } else if (haveIf) {
      combinedIf = *ifExpr;
    } else if (haveRange) {
      combinedIf = rangePred;
    }
    const std::string probeRhs =
        haveCombined ? ("cond((" + combinedIf + "),(" + rhs + "),.)") : rhs;
    const std::string probeArg = makeBygenExpr(
        std::string(storage::kReplaceProbeVar) + "=" + probeRhs, byVars, bygenSort);
    std::string keyNames;
    for (const auto& b : byVars) keyNames += b + " ";
    if (bygenSort.has_value())
      for (const auto& b : *bygenSort) keyNames += b + " ";
    storage::ReplaceTypeDecision dec = storage::decideReplaceStoreType(
        session, storeType, noPromote, newvar, rhs, haveCombined, "--bygen", probeArg,
        {rhs, combinedIf, keyNames}, /*drawContext=*/true);
    if (dec.error.has_value()) return *dec.error;
    finalStoreType = dec.storeType;
  }

  std::string outPath = nextPath(session.workdir(), "bysort_gen");
  // BUG-369 follow-up: `bysort g: gen lb = L.y` — the operator resolves over
  // the FULL tsset series (E1) and the by-prefix only scopes _n/_N, so the
  // whole-frame bygen evaluation is exactly Stata's semantics; it just needs
  // the tsset metadata to ride the call.
  std::vector<std::string> baseArgs = {"eval", "--data", *session.currentHbin(),
                                      "--bygen", bygenArg, "--out", outPath};
  baseArgs.insert(baseArgs.end(), qualifierArgs.begin(), qualifierArgs.end());
  if (keepUnselected) baseArgs.push_back("--gen-replace");
  storage::appendStoreTypeArg(baseArgs, finalStoreType);  // BUG-471 T2
  // The `if` sent as --gen-mask is not inside bygenArg, so the context scan
  // reads it too: a mask with `L.z` or a draw needs the tsset metadata or the
  // draw coordinates exactly as a folded one does (the engine parses the mask
  // against the same frame).
  const std::string ctxExpr =
      maskOut ? bygenArg + " " + *ifExpr : bygenArg;
  std::vector<std::string> args =
      translate::injectTsContextForExpr(baseArgs, ctxExpr, session);
  args = translate::injectRngContextForExpr(args, ctxExpr, session);
  // ARC-B1 (P2PERF): opt this verb into the resident plane, the same way its
  // siblings runGen/runReplace/runEgen already are. Without the opt-in the call
  // routes STATELESS — the engine re-reads the whole dataset from disk, writes a
  // whole new file, and the resident token is cleared, so the NEXT resident verb
  // pays a full re-adoption on top. The W0 counters put the benchmark at 3449
  // stateless trips and 2701 re-adoptions, and a `bysort: gen` in a loop is one
  // of the verbs feeding both.
  //
  // Sound because `--bygen` is frame-shaped like every other eval op: cmd_eval
  // builds its output column against the in-RAM frame and commits it, and
  // Frame::bygen computes through a sort ORDER vector without ever permuting the
  // frame's rows — so residency changes no row order, no column identity, and no
  // value. (The `bysort` half's in-place sort above is its own mutation,
  // done before this call.) The sort parenthetical is already folded into the single --bygen
  // expression above, so there is no second call to keep in step.
  SessionState::ResidentEligible bygenResident(session);
  nlohmann::json response;
  try {
    response = runCobraJson(args);
    translate::absorbRngDraws(response, session);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  if (isReplace) {
    storage::recordReplaceType(session, newvar, storeType, finalStoreType);
    // A replace rewrites the target in place: it goes in the rewrittenVars slot,
    // as runReplace's does, never in newVars, which the console reads as
    // "(variable created)".
    return mutationResult(session, "gen", {}, {}, {newvar});
  }
  ojson block = mutationResult(session, "gen", newvar.empty() ? std::vector<std::string>{}
                                                               : std::vector<std::string>{newvar});
  translate::carryStoreOverflow(response, block);  // BUG-1123
  return block;
}

}  // namespace piet::handlers
