// `egen NEWVAR = fn(...) [, by(g) missing p(#) ...]` — C++ port of
// piet/backend/piet/egen.py (option-suffix rebuilding) + mutate.py's
// run_egen (the cobra --egen / --rowegen dispatch). M3b (ROADMAP "M3b
// scope" item 4 — bysort'd egen via bysort.py's by()-injection is the
// M3a-RED-cell fix; this file is the underlying `egen` command itself,
// which the bysort injection path also calls through dispatch()).
//
// Row functions (rowmean/rowsd/rowtotal/rowmax/rowmin/rowmiss/rownonmiss)
// route to `cobra eval --rowegen NEWVAR=fn(a,b,c)` (comma-separated args,
// converted from the space-separated Stata varlist form). Every other
// function (the original 8 stat fns mean/sum/total/count/min/max/median/sd
// plus the cobra 0.13.0 extended set tag/group/seq/rank/pctile/std/iqr)
// routes to `cobra eval --egen NEWVAR=fn(expr)[,opts][, by(g1,g2)]`.
#include "piet/handlers.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum
#include "piet/translate.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type

namespace {
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — egen's option vocabulary. Union of the
// base set and every per-function conditional key (see runEgen); registered so
// dispatcher.cpp's single resolution point can canonicalize an abbreviation
// BEFORE runEgen's own `k == "by"` / `k == "missing"` comparisons run.
const piet::eval::OptVocab& kVocabEgen = piet::eval::registerOptVocab(
    // The UNION across egen functions (BUG-233's rule): the dispatcher
    // canonicalizes an abbreviation against this set, and the per-function
    // check inside runEgen is what actually refuses a wrong pairing.
    {"egen"}, {{"by", "missing", "p", "from", "f", "to", "t", "block", "b",
               "punct", "format", "decode", "maxlength", "at", "group", "icodes", "label"}, {}});
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `trim(...)`/`lower(...)`
// call sites didn't need touching. Pure dedup, no behavior change.
std::string trim(const std::string& s) { return strutil::trim(s); }

ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// BUG-042 (egen if-wiring): `cobra merge`'s success path prints a plain-text
// status line to stderr (not JSON), so `runCobraJson`'s "parse stdout as
// JSON" contract doesn't fit — this needs the raw `runCobra()` + a manual
// CobraError build from stderr, same as handlers_data_verbs.cpp's own
// `errorFromRaw` (file-local copy per this codebase's established
// one-copy-per-TU convention for exactly this helper).
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  std::string firstLine = stderrText;
  auto nl = firstLine.find('\n');
  if (nl != std::string::npos) firstLine = firstLine.substr(0, nl);
  if (firstLine.empty()) firstLine = "cobra exited " + std::to_string(r.exit_code);
  return CobraError("engine_error", firstLine, stderrText);
}

std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// egen.py's _ROW_FUNS (mutate.py:187) — functions that route to --rowegen
// instead of --eigen.
const std::set<std::string>& rowFuns() {
  static const std::set<std::string> s = {"rowmean", "rowsd",  "rowtotal",
                                          "rowmax",  "rowmin", "rowmiss", "rownonmiss"};
  return s;
}

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — see the
// trim() wrapper above for why this stays a thin pass-through.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

// egen.py's build_opts_suffix: opts dict (excluding `by`) -> cobra-syntax
// suffix, e.g. {"p":"90"} -> ",p(90)"; {"missing":true} -> ",missing".
// Preserves ParsedCommand::opts insertion order (parity with Python dict
// iteration order — both reflect tokenizer left-to-right option order).
std::string buildOptsSuffix(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                            const std::set<std::string>& exclude) {
  std::string suffix;
  bool any = false;
  for (const auto& [key, val] : opts) {
    if (exclude.count(key)) continue;
    if (val.isBool) {
      if (!val.boolValue) continue;  // Python: `val is True` only
      suffix += (any ? "," : ",") + key;
      any = true;
    } else if (!val.strValue.empty()) {
      suffix += "," + key + "(" + val.strValue + ")";
      any = true;
    }
  }
  return suffix;
}

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

}  // namespace

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

  // BUG-077: fold `in a/b` into the same cond() the if-expr already builds,
  // ANDed together so a row must satisfy BOTH to take the rhs branch; rows
  // outside the range fall to the existing false branch (`.` for gen — a
  // new var is missing outside the range; `newvar` unchanged for replace).
  bool haveRange = inRange.has_value() && !inRange->empty();
  std::string rangePred;
  if (haveRange) {
    // BUG-672/673: the shared [U] 11.1.4 resolver (inrange_filter.cpp), not a
    // private grammar. The range becomes literal global row numbers, which the
    // empty-by-list bygen context's `_n` counts in (this path never runs with
    // a by-list; see above). `_N` is the frame's row count as the macro layer
    // sees it — card first, so a dirty virtual head is not snapshotted.
    InRangeBounds rr = parseInRangeBounds(*inRange, session.macroObsCount());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "gen/replace");
    }
    rangePred = "_n>=" + std::to_string(rr.a) + " & _n<=" + std::to_string(rr.b);
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  std::string combinedIf;
  if (haveIf && haveRange) {
    // BUG-622 (its egen.cpp sites): the `if` half is normalised with `!=0`
    // before the AND, as applyIfAndInFilter does (BUG-534). The engine's `&`
    // reads a missing operand as false, so `gen w = 1 if z in 1/4` left the
    // missing-z rows unset; Stata's `if` treats a missing condition as true,
    // which is what the if-only cond() below already does.
    combinedIf = "((" + *ifExpr + ")!=0) & (" + rangePred + ")";
  } else if (haveIf) {
    combinedIf = *ifExpr;
  } else if (haveRange) {
    combinedIf = rangePred;
  }
  bool haveCombined = haveIf || haveRange;

  // AER scope 2 / B3(c). Folding `if` into cond() is exactly equivalent for every
  // function whose answer depends only on its own row -- but sum() is a RUNNING
  // total, and a folded qualifier makes it accumulate across the rows the
  // qualifier excluded (measured: Stata gives . 2 5 9 . 6 13 21 for paper 015's
  // shape, folding gives . 3 6 10 . 11 18 26). When the right-hand side carries a
  // running sum the condition rides separately as --gen-mask and the engine
  // applies it to the accumulation itself.
  const bool runningSum = piet::eval::exprHasRunningSum(rhs);
  const bool maskOut = haveCombined && runningSum && !isReplace;
  std::string bygenRhs;
  if (isReplace) {
    bygenRhs = haveCombined ? ("cond((" + combinedIf + "),(" + rhs + ")," + newvar + ")") : rhs;
  } else if (maskOut) {
    bygenRhs = rhs;
  } else {
    bygenRhs = haveCombined ? ("cond((" + combinedIf + "),(" + rhs + "),.)") : rhs;
  }
  std::string bygenFullExpr = newvar + "=" + bygenRhs;
  // BS-002: an explicit sortVars (the `(sortvar)` parenthetical) always
  // wins over the sortFirst-driven default — it is a strictly more specific
  // instruction from the same `bysort`/`by` prefix line.
  std::optional<std::vector<std::string>> bygenSort =
      sortVars.has_value() ? sortVars : (sortFirst ? std::optional(byVars) : std::nullopt);
  // BUG-532: Stata's `bysort KEYS (SORT): generate|replace` SORTS THE DATA by
  // KEYS SORT and leaves them sorted (marker KEYS SORT) before it computes;
  // --bygen alone only computes through a sort order and leaves the rows where
  // they were, so every later row-order read (`_n`, `in`, `list`, `x[_n-1]`,
  // `save`, a plain `by`) saw PIET's order, not Stata's (w2_bykeep.log
  // by_p_after_bysgen: a plain `by g (b):` runs after `bysort g (b):
  // generate`). The rows are sorted in place first, in Stata's order (a string
  // key by its bytes; the engine moves no row when the marker already begins
  // with KEYS SORT, and the marker becomes KEYS SORT either way); --bygen
  // then groups on by() alone, whose stable sort keeps each group's rows in
  // that physical order — also for a string sort key, whose engine code order
  // is not Stata's. A plain `by` (sortFirst false) never sorts.
  if (sortFirst && !byVars.empty()) {
    std::vector<std::string> keys = byVars;
    if (sortVars.has_value()) keys.insert(keys.end(), sortVars->begin(), sortVars->end());
    if (std::optional<ojson> sortErr = sortInStataOrder(session, keys)) return *sortErr;
    bygenSort = std::vector<std::string>{};
  }
  std::string bygenArg = makeBygenExpr(bygenFullExpr, byVars, bygenSort);

  // BUG-471 T2: a replace widens an integer type by Stata's lattice before the
  // one store (see runReplace, handlers.cpp). The probe is the same --bygen with
  // the target swapped for the scratch name and the untouched rows missing, run
  // after the in-place sort above so it groups exactly as the store will.
  std::string finalStoreType = storeType;
  if (isReplace) {
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
  if (maskOut) { baseArgs.push_back("--gen-mask"); baseArgs.push_back(combinedIf); }
  storage::appendStoreTypeArg(baseArgs, finalStoreType);  // BUG-471 T2
  std::vector<std::string> args =
      translate::injectTsContextForExpr(baseArgs, bygenArg, session);
  args = translate::injectRngContextForExpr(args, bygenArg, session);
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
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  if (isReplace) storage::recordReplaceType(session, newvar, storeType, finalStoreType);
  return mutationResult(session, "gen", newvar.empty() ? std::vector<std::string>{}
                                                        : std::vector<std::string>{newvar});
}

ojson runEgen(const std::string& exprIn, const std::string& by,
              const std::optional<std::string>& ifExpr,
              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
              SessionState& session, const std::optional<std::string>& inRange,
              const std::string& storeTypeIn, std::string* seqTypeOut) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  std::string expr = trim(exprIn);
  std::string outPath = nextPath(session.workdir(), "egen");

  auto eqPos = expr.find('=');
  std::string newvar = eqPos == std::string::npos ? "" : trim(expr.substr(0, eqPos));
  std::string rhs = eqPos == std::string::npos ? expr : trim(expr.substr(eqPos + 1));

  // BUG-471 T2: the result's storage type rides the ONE call that computes it
  // (never the if/in plumbing calls). tag()/seq()/mode() ignore the requested
  // type in Stata (DIAGNOSIS section B15-B17), so they get no flag at all. The
  // replace-built results (rowtotal/rowmax/rowmin, default rank, cut with a
  // fractional edge) widen an integer type in Stata; the engine refuses that
  // with bad_input, and the refusal is surfaced as an r(198) option refusal —
  // never retried without the flag, which would store a double under an
  // integer label.
  const std::string egenFn = storage::egenFunctionName(rhs);
  const std::string storeType = storage::egenIgnoresStoreType(egenFn) ? std::string() : storeTypeIn;
  auto egenStoreError = [&](const CobraError& ce) -> ojson {
    if (storage::isIntegerType(storeType) && ce.code == "bad_input" &&
        std::string(ce.what()).find("--store-type") != std::string::npos) {
      return ojson(eval::unsupportedOption(
          storeType, "egen " + egenFn + "() builds its result with replace, which widens a " +
                         storeType + " in Stata; PIET cannot widen it yet. Declare the result " +
                         "float or double instead."));
    }
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  };

  // BUG-042: `egen NEWVAR = fn(...) if COND` — Stata restricts BOTH the
  // computation (fn() sees only the matching rows: e.g. mean() is the
  // subset's mean, group()/tag() only consider the subset's distinct
  // values) AND the assignment (non-matching rows come back missing) to
  // the if-subset. Mechanical fix (row-filter infra reuse, mirroring
  // summarize/count's keepif prefilter): tag every row with a stable
  // index, filter a COPY to the if-subset, run the *exact same* egen op
  // there, then merge the freshly-computed column back onto the FULL
  // (untouched) dataset by that index — unmatched rows come back missing
  // for NEWVAR (cobra merge's standard unmatched-master semantics), while
  // every OTHER variable is untouched (unlike a whole-dataset keepif,
  // which would also drop rows from every other column). This is uniform
  // across every egen function family (row/stat/tag/group/seq) since it
  // never special-cases which fn was requested — it just re-runs whatever
  // cobra call was going to happen anyway, against the filtered subset.
  // No ifExpr/inRange: dataPath == the live hbin, zero extra cobra calls,
  // byte-identical to the pre-fix behavior (freeze-compatible).
  //
  // BS-090 (BETA Phase G): `in a/b` now composes with `if` via AND here
  // too — reusing the SAME kIdxVar this block already materializes for the
  // if-merge-back key (it is already the GLOBAL row index, `_n` over the
  // whole dataset), so unlike piet::handlers::applyIfAndInFilter's general
  // case, no SEPARATE idx materialization is needed.
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  // BUG-369 follow-up, E1 guard: with if/in, this handler computes fn() on a
  // row-FILTERED copy (the whole point of the BUG-042 fix above). But a TS
  // operator inside fn() must be resolved against the FULL tsset series before
  // any sample restriction (E1 — the same law cmds_ts.cpp/tsops enforces), and
  // on the filtered copy a boundary row's lag would silently come back missing
  // or skip across removed rows. A silently-wrong number is the one class this
  // codebase refuses to ship — so the combination is an honest reject with the
  // working recipe, until someone builds a pre-materialize step. `egen m =
  // mean(L.y)` WITHOUT if/in stays fully supported (whole-frame evaluation,
  // E1 holds by construction).
  if ((haveIf || (inRange.has_value() && !inRange->empty())) &&
      translate::exprHasTsToken(rhs)) {
    return engineErrorBlock(
        "egen with if/in cannot take a time-series operator inside the function yet: "
        "the operator must see the full tsset series BEFORE the sample restriction "
        "(materialize it first — `gen tmp = L.x`, then egen over tmp with your if/in)",
        "unimplemented");
  }
  bool haveIn = inRange.has_value() && !inRange->empty();
  // Stage 1b: the common (no if/in) egen is a clean single mutation whose --out
  // becomes the head and is never read back as a file — opt it into the
  // resident frame plane. The if/in path instead builds intermediate scratch
  // hbins and reads them back AS FILES out of resident order (the `Datastore
  // srcDs` below; the merge-back references the idx temp created several steps
  // earlier), so it stays on the default stateless path (real temp files, and
  // its `--data currentHbin` idx call already materializes any virtual head).
  std::optional<SessionState::ResidentEligible> egenResident;
  if (!(haveIf || haveIn)) egenResident.emplace(session);
  std::string egenDataPath = *session.currentHbin();
  std::string idxFullPath;
  static const char* kIdxVar = "__egen_ifidx__";
  if (haveIf || haveIn) {
    idxFullPath = nextPath(session.workdir(), "egen_ifidx");
    std::vector<std::string> idxArgs = {"eval", "--data", *session.currentHbin(), "--bygen",
                                        std::string(kIdxVar) + "=_n", "--out", idxFullPath};
    try {
      runCobraJson(idxArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string combinedIf = ifExpr.value_or("");
    if (haveIn) {
      Datastore srcDs(*session.currentHbin());
      InRangeBounds rr = parseInRangeBounds(*inRange, srcDs.nRows());
      if (!rr.ok) {
        InRangeFilterResult fr;
        fr.ok = false;
        fr.refusal = rr.refusal;
        return inRangeRejectError(fr, *inRange, "egen");
      }
      std::string rangePred = std::string(kIdxVar) + ">=" + std::to_string(rr.a) + " & " +
                              std::string(kIdxVar) + "<=" + std::to_string(rr.b);
      // BUG-622 (its egen.cpp sites): `!=0` on the `if` half, as in
      // runBysortGen above.
      combinedIf = haveIf ? ("((" + *ifExpr + ")!=0) & (" + rangePred + ")") : rangePred;
    }
    std::string filteredPath = nextPath(session.workdir(), "egen_iffilter");
    std::vector<std::string> filterArgs = translate::injectTsContextForExpr(
        {"eval", "--data", idxFullPath, "--keepif", combinedIf, "--out", filteredPath},
        combinedIf, session);  // BUG-369 follow-up: the if itself may carry L.
    try {
      runCobraJson(filterArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    egenDataPath = filteredPath;
  }

  // Row-function detection: `fn(a b c)` (space-separated Stata varlist) with
  // fn in _ROW_FUNS -> --rowegen NEWVAR=fn(a,b,c) (comma-separated).
  static const std::regex reRowFn(R"(^(\w+)\s*\(([^)]+)\)\s*$)");
  std::smatch m;
  std::string computedPath;
  ojson generatedLabels = ojson::object();
  if (std::regex_match(rhs, m, reRowFn) && rowFuns().count(lower(m.str(1)))) {
    std::string fnName = m.str(1);
    std::string argsStr = trim(m.str(2));
    // space-separated -> comma-separated (mutate.py: ",".join(args_str.split()))
    std::string argsCsv;
    std::istringstream iss(argsStr);
    std::string tok;
    bool first = true;
    while (iss >> tok) {
      if (!first) argsCsv += ",";
      argsCsv += tok;
      first = false;
    }
    std::string rowegenExpr = newvar + "=" + fnName + "(" + argsCsv + ")";
    std::vector<std::string> args = {"eval", "--data", egenDataPath, "--rowegen",
                                      rowegenExpr, "--out", outPath};
    storage::appendStoreTypeArg(args, storeType);  // BUG-471 T2
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      return egenStoreError(ce);
    }
    computedPath = outPath;
  } else {
    // Non-row function: --eigen path (cobra's --egen flag). BS-097: cobra's
    // own `--egen` CLI parsing (apps/cmds/cmds_data.cpp) never validates the
    // comma-suffix tail at all — it only ever substring-scans for the
    // specific keys it reads (by()/missing/p()) and drops everything else on
    // the floor (live-verified: `--egen "x=mean(v),bogusopt(5)"` -> exit 0,
    // x computed, bogusopt silently discarded). Verbatim passthrough via
    // buildOptsSuffix therefore does NOT reach any rejecting net downstream
    // — this PIET-layer check is the ONLY place an unsupported egen option
    // can be caught, so it must run BEFORE the suffix is forwarded.
    static const std::regex reEgenFnName(R"(^(\w+)\s*\()");
    std::smatch fnNameMatch;
    std::string egenFnName;
    if (std::regex_search(rhs, fnNameMatch, reEgenFnName)) egenFnName = lower(fnNameMatch.str(1));
    // BUG-233: `missing`/`p` are per-egen-function, so they stay conditional
    // here (the reject net must still refuse `missing` for a non-group egen);
    // the registered vocabulary below carries the union so the dispatcher can
    // canonicalize `mis` before this loop's raw-key comparisons ever run.
    std::unordered_set<std::string> kEgenKnownOpts = {"by"};
    if (egenFnName == "group") kEgenKnownOpts.insert("missing");
    else if (egenFnName == "pctile") kEgenKnownOpts.insert("p");
    // BUG-433: seq()'s own three. Both spellings of each, because Stata
    // documents `from()/to()/block()` and every corpus line writes `f()/t()/b()`.
    // Per-function, like `missing` and `p` above: `egen m = mean(x), b(2)` must
    // still be refused.
    else if (egenFnName == "seq")
      kEgenKnownOpts.insert({"from", "f", "to", "t", "block", "b"});
    else if (egenFnName == "concat")
      kEgenKnownOpts.insert({"punct", "p", "format", "f", "decode", "maxlength"});
    else if (egenFnName == "cut")
      kEgenKnownOpts.insert({"at", "group", "icodes", "label"});
    if (auto err = eval::rejectUnknownOptions(opts, kEgenKnownOpts)) {
      return *err;
    }
    if (egenFnName == "concat" || egenFnName == "cut") {
      std::set<std::string> seen;
      for (const auto& [key, value] : opts) {
        const std::string canonical = egenFnName == "concat" && key == "p" ? "punct"
            : egenFnName == "concat" && key == "f" ? "format" : key;
        if (!seen.insert(canonical).second)
          return eval::unsupportedOption(key, "This option may only be specified once.");
        if (key == "by")
          return eval::unsupportedOption(key, "egen " + egenFnName + "() cannot be combined with by.");
        const bool flag = key == "decode" || key == "icodes" || key == "label";
        if (value.isBool != flag)
          return eval::unsupportedOption(key, flag ? "Use this option without parentheses."
                                                  : "This option requires parentheses.");
        if (!flag && key != "punct" && key != "p" && trim(value.strValue).empty())
          return eval::unsupportedOption(key, "This option requires a nonempty argument.");
      }
    }
    // The suffix carries every option EXCEPT by() (appended separately below),
    // so seq()'s f/t/b ride through verbatim to cobra's own parser.
    auto forwardedOpts = opts;
    if (egenFnName == "concat") {
      for (auto& [key, value] : forwardedOpts) {
        (void)value;
        if (key == "f") key = "format";
      }
    }
    std::string optsSuffix = buildOptsSuffix(forwardedOpts, {"by"});
    std::string cobraExpr = expr + optsSuffix;

    if (!by.empty()) {
      // cobra eval --egen already parses by() out of the expr; append it.
      cobraExpr += ", by(" + by + ")";
    }

    std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", egenDataPath, "--egen", cobraExpr, "--out", outPath},
        cobraExpr, session);  // BUG-369 follow-up: `egen m = mean(L.y)`
    storage::appendStoreTypeArg(args, storeType);  // BUG-471 T2
    const bool decodedConcat = egenFnName == "concat" &&
        std::any_of(opts.begin(), opts.end(), [](const auto& option) {
          return option.first == "decode" && option.second.isBool && option.second.boolValue;
        });
    if (decodedConcat) {
      ojson labelInput = ojson::object();
      for (const auto& [var, labels] : session.valueLabels) {
        ojson codes = ojson::object();
        for (const auto& [code, text] : labels) codes[std::to_string(code)] = text;
        labelInput[var] = std::move(codes);
      }
      if (!labelInput.empty()) {
        args.push_back("--value-labels");
        args.push_back(labelInput.dump());
      }
    }
    // BUG-471 T2 fix-up (F3): seq() is `gen byte` then a replace that widens
    // (_gseq.ado:42, :45-49), so its type follows the values it stored. The
    // computing call summarizes the new column after the egen (eval's
    // --summarize reads the final frame), so the min/max cost no extra engine
    // call and no dispatch(): r() and e() are untouched. Under if/in this frame
    // is the filtered copy, i.e. exactly the rows the replace touched.
    const bool seqSummary = seqTypeOut != nullptr && egenFnName == "seq" && !newvar.empty();
    if (seqSummary) {
      args.push_back("--summarize");
      args.push_back(newvar);
    }
    ojson egenOut;
    try {
      egenOut = runCobraJson(args);
    } catch (const CobraError& ce) {
      return egenStoreError(ce);
    }
    generatedLabels = egenOut.value("egen_value_labels", ojson::object());
    if (seqTypeOut != nullptr && egenFnName == "cut") {
      const ojson types = egenOut.value("egen_store_types", ojson::object());
      if (types.contains(newvar) && types[newvar].is_string())
        *seqTypeOut = types[newvar].get<std::string>();
    }
    if (seqSummary) {
      const ojson summ = egenOut.value("summarize", ojson::object()).value(newvar, ojson::object());
      const auto num = [&](const char* k) {
        return summ.contains(k) && summ[k].is_number() ? summ[k].get<double>() : 0.0;
      };
      // No stored value (every row excluded): the `gen byte` stands.
      *seqTypeOut = num("N") > 0 ? storage::promotedType("byte", false, num("min"), num("max"))
                                 : std::string("byte");
    }
    computedPath = outPath;
  }

  std::string finalPath = computedPath;
  if (haveIf || haveIn) {
    // Merge NEWVAR back onto the untouched full dataset by the stable idx
    // (--keepusing restricts the using-side to idx+NEWVAR only — merging
    // the full column set back in would immediately fail cobra's
    // both-sides-have-this-column conflict check). --nogenerate skips the
    // _merge indicator (this isn't a user-visible merge, just plumbing).
    std::string mergedPath = nextPath(session.workdir(), "egen_ifmerge");
    std::vector<std::string> mergeArgs = {
        "merge",       "--data", idxFullPath, "--using",     computedPath, "--type", "1:1",
        "--key",       kIdxVar,  "--keepusing", newvar,       "--nogenerate", "--out", mergedPath};
    CobraRunResult mcr = runCobra(mergeArgs);
    if (!mcr.ok) {
      CobraError ce = errorFromRaw(mcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    std::string dropIdxPath = nextPath(session.workdir(), "egen_ifdropidx");
    std::vector<std::string> dropArgs = {"eval", "--data", mergedPath, "--drop", kIdxVar, "--out",
                                         dropIdxPath};
    try {
      runCobraJson(dropArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    finalPath = dropIdxPath;
  }

  session.load(finalPath);
  if (generatedLabels.contains(newvar) && generatedLabels[newvar].is_object()) {
    std::map<std::string, std::string> definition;
    std::map<int, std::string> labels;
    for (auto it = generatedLabels[newvar].begin(); it != generatedLabels[newvar].end(); ++it) {
      definition[it.key()] = it.value().get<std::string>();
      labels[std::stoi(it.key())] = it.value().get<std::string>();
    }
    session.labelDefs[newvar] = std::move(definition);
    session.varLblname[newvar] = newvar;
    session.valueLabels[newvar] = std::move(labels);
  }
  return mutationResult(session, "egen",
                        newvar.empty() ? std::vector<std::string>{} : std::vector<std::string>{newvar});
}

}  // namespace piet::handlers
