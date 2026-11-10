// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data duplicates.
#include "data_mutation_internal.h"
#include "piet/handlers/observations.h"
namespace piet::handlers::data_mutation_detail {


// ---------------------------------------------------------------------------
// duplicates report|drop|tag|list [VARLIST] — C++ port of duplicates.py.
// report/drop/tag route to cobra's native `duplicates` verb; `list` (no
// cobra native equivalent) is built from `duplicates tag` + a keepif filter
// on the tag column (group_size-1 > 0 <=> duplicate).



// _resolve_varlist: varlist or all names; validates each var exists
// ("variable `{v}' not found" — backtick-quote, matching Python's f-string).
std::optional<std::string> resolveVarlist(Datastore* ds,
                                          const std::optional<std::vector<std::string>>& varlist,
                                          std::vector<std::string>& outCols) {
  if (!varlist.has_value() || varlist->empty()) {
    outCols = ds->names();
    return std::nullopt;
  }
  for (const auto& v : *varlist) {
    if (!ds->has(v)) return "variable `" + v + "' not found";
  }
  outCols = *varlist;
  return std::nullopt;
}


// GA sweep 2026-08-22. The card-answered twin. Everything the Datastore version
// asks of the frame is METADATA — the column names, and whether a name exists —
// which the resident shadow card holds exactly, so a caller does not have to
// materialize the whole virtual head to ask it. Same idiom and the same safety
// rule as joinby's INSPECT-2608 fix and runSort's varlist check: the card answers
// only when it provably describes the current head, and a nullptr card means the
// caller falls back to the byte-identical Datastore path above.
//
// Measured cause (census47h, caller-attributed materialization log): of the 962
// head flushes triggered by a `datastore()` call in one 47-task run,
// runDuplicatesDrop was 320 — and its entry call existed only to resolve a
// varlist. Its TAIL flush was already removed in the 08-20 phase-2 work; this is
// the entry half the same census named.
std::optional<std::string> resolveVarlistFromCard(const std::vector<VarInfo>* card,
                                                  const std::optional<std::vector<std::string>>& varlist,
                                                  std::vector<std::string>& outCols) {
  if (card == nullptr) return std::string("__no_card__");  // sentinel: caller falls back
  if (!varlist.has_value() || varlist->empty()) {
    outCols.clear();
    outCols.reserve(card->size());
    for (const auto& vi : *card) outCols.push_back(vi.name);
    return std::nullopt;
  }
  for (const auto& v : *varlist) {
    bool found = false;
    for (const auto& vi : *card) if (vi.name == v) { found = true; break; }
    if (!found) return "variable `" + v + "' not found";
  }
  outCols = *varlist;
  return std::nullopt;
}


// duplicates.ado is rclass: every subcommand returns r(N), the observations it
// considered (`qui count if `touse''), report adds r(unique_value) and drop
// r(N_drop); none of them is posted when there is nothing to consider, which
// is `error 2000'. w2ah_data_verbs.log ah_dup_report_all (N=8, unique_value=7),
// ah_dup_list, ah_dup_tag, ah_dup_drop_all (N=8, N_drop=1), ah_dup_drop_none
// (N_drop=0). Main posted none of them, so every read was a named r(111).
// duplicates.ado's tag: `gen long g = Ngroup - 1 if touse' then `compress g' --
// the smallest integer type of the counts, with long's %12.0g kept
// (w2ah_data_verbs.log ah_dup_tag: byte %12.0g; main left no claim, so `: type'
// read float and the format %9.0g).
void recordDuplicatesTagType(SessionState& session, const std::string& genvar) {
  auto vals = headColumnValues(session, genvar);
  const std::string t = vals.has_value() ? compressedIntegerType(*vals) : std::string();
  session.implicitFloatVars.erase(genvar);
  VarMeta& vm = session.varMeta[genvar];
  vm.stataType = t.empty() ? std::string("long") : t;
  vm.format = std::string("%12.0g");
}


std::optional<ojson> postDuplicatesR(SessionState& session, int64_t n,
                                     std::initializer_list<std::pair<const char*, double>> more) {
  if (n <= 0) return rcErrorBlock("no observations", 2000);
  session.eval.rValues.set("N", static_cast<double>(n));
  std::vector<std::string> names = {"N"};
  for (const auto& [k, v] : more) {
    session.eval.rValues.set(k, v);
    names.emplace_back(k);
  }
  eval::declareRPosted(session.eval, "duplicates", names);
  return std::nullopt;
}




// The display columns `s` fills: one per character, two for the East Asian wide
// ranges (StataBE 19.5 lists "héllo" in five columns and "日本" in four).
size_t displayCols(const std::string& s) {
  size_t cols = 0;
  for (size_t i = 0; i < s.size();) {
    char32_t cp = 0;
    const size_t n = cecon::ident::decodeUtf8(s, i, &cp);
    if (n == 0) {  // a stray byte takes one column
      ++cols;
      ++i;
      continue;
    }
    const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                      (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
    cols += wide ? 2 : 1;
    i += n;
  }
  return cols;
}


// list's header abbreviation of a variable name to `n` columns: the leading
// characters that fit, `~`, then the last character (`household_id` -> `househ~d`,
// `ééééééééé` -> `éééééé~é`, StataBE 19.5). A name that fits is kept whole.
std::string abbrevName(const std::string& name, size_t n) {
  if (displayCols(name) <= n) return name;
  size_t last = name.size() - 1;
  while (last > 0 && (static_cast<unsigned char>(name[last]) & 0xC0) == 0x80) --last;
  const std::string tail = name.substr(last);
  const size_t budget = n - 1 - displayCols(tail);
  std::string head;
  for (size_t i = 0; i < last;) {
    const size_t len = cecon::ident::utf8SeqLen(static_cast<unsigned char>(name[i]));
    const std::string ch = name.substr(i, len);
    if (displayCols(head) + displayCols(ch) > budget) break;
    head += ch;
    i += len;
  }
  return head + "~" + tail;
}
}
namespace piet::handlers {
using namespace data_mutation_detail;


ojson runDuplicatesReport(const std::optional<std::vector<std::string>>& varlist,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }

  // BUG-042: `duplicates report ... if COND` — cobra's `duplicates` verb has
  // no --keepif of its own, so pre-filter via the same row-filter infra
  // summarize/count already use (`cobra eval --keepif` into a temp hbin,
  // then point the report at the filtered copy instead of the live data).
  // BS-090 (BETA Phase G): `in` now composes with `if` via AND here too
  // (piet::handlers::applyIfAndInFilter).
  std::string dataPath = ds->path();
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, dataPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "duplicates report");
    dataPath = fr.dataPath;
  }

  ojson raw;
  try {
    raw = runCobraJson({"duplicates", "report", "--data", dataPath, "--vars", joined});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  bool allVars = (!varlist.has_value() || varlist->empty());
  std::string colStr = allVars ? "(all variables)" : joined;
  {
    // oracle's col_str is SPACE-joined, not comma.
    colStr = allVars ? "(all variables)" : std::string();
    if (!allVars) {
      for (size_t i = 0; i < cols.size(); ++i) {
        if (i) colStr += " ";
        colStr += cols[i];
      }
    }
  }

  std::ostringstream out;
  out << "Duplicates in terms of " << colStr << "\n\n";
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%8s  %14s  %9s", "copies", "observations", "surplus");
  out << buf << "\n";
  out << std::string(38, '-') << "\n";

  int64_t totalSurplus = 0;
  int64_t nObs = 0, nUnique = 0;
  if (raw.contains("rows") && raw["rows"].is_array()) {
    for (const auto& row : raw["rows"]) {
      int64_t copies = row.value("copies", 0);
      int64_t observations = row.value("observations", 0);
      int64_t surplus = row.value("surplus", 0);
      totalSurplus += surplus;
      nObs += observations;
      if (copies > 0) nUnique += observations / copies;
      std::snprintf(buf, sizeof(buf), "%8lld  %14lld  %9lld", static_cast<long long>(copies),
                    static_cast<long long>(observations), static_cast<long long>(surplus));
      out << buf << "\n";
    }
  }
  out << std::string(38, '-') << "\n";
  std::snprintf(buf, sizeof(buf), "%24s  %9lld", "Total surplus:",
               static_cast<long long>(totalSurplus));
  out << buf;

  if (auto bad = postDuplicatesR(session, nObs, {{"unique_value", static_cast<double>(nUnique)}}))
    return *bad;
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}


ojson runDuplicatesDrop(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange) {
  // Card first (see resolveVarlistFromCard): this entry used to open the frame —
  // i.e. materialize the virtual head — purely to resolve a varlist. `ds` stays
  // null on the card path and every later use of it below is guarded.
  Datastore* ds = nullptr;
  std::vector<std::string> cols;
  auto err = resolveVarlistFromCard(session.shadowVarlistIfCurrent(), varlist, cols);
  if (err.has_value() && *err == "__no_card__") {
    ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    err = resolveVarlist(ds, varlist, cols);
  }
  if (err.has_value()) return engineErrorBlock(*err);

  // BS-031 (BETA campaign C2, silent-if-drop sweep): `duplicates drop ...
  // if COND` previously ignored `if` entirely (dedup ran over the WHOLE
  // dataset, live-verified this stint: `duplicates drop year if year==1982`
  // silently collapsed all 336 rows down to 7 — one per year — instead of
  // leaving the 288 out-of-sample rows untouched). A correct fix needs to
  // dedup ONLY the if-sample and re-splice the untouched out-of-sample rows
  // back in at their original relative position (row-id tag + append +
  // re-sort) — meaningfully more moving parts than tag/list's fixes below,
  // and NOT exercised by any oracle-pinned scenario this stint (real
  // Stata's `duplicates drop VARLIST` also requires `,force` whenever
  // VARLIST narrows from "all variables" -- enforced at the dispatcher arm
  // since data-verbs-effects-rename-recode-label -- so almost every real
  // `if`-qualified call already errors on that ground first). Converting the silent-wrong
  // into an honest reject here (rather than shipping an unverified
  // multi-step engine choreography with no oracle to pin it against) is the
  // deliberate, disclosed scope boundary for this item.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    return eval::unsupportedOption(
        "if",
        "`duplicates drop` with `if` is not yet supported in this build (dropping duplicates "
        "within an if-restricted sample while leaving other rows untouched needs more than a "
        "simple filter) — `duplicates report`/`tag`/`list` all honor `if`.");
  }
  // BS-090 (BETA Phase G): `in` needs the EXACT same dedup-then-splice-back
  // machinery `if` does (a plain filter would silently dedup only the
  // in-range rows and drop the out-of-range ones' own duplicates too) — the
  // SAME disclosed scope boundary as the `if` reject just above, not a new
  // gap this phase introduces.
  if (inRange.has_value() && !inRange->empty()) {
    return eval::unsupportedOption(
        "in " + *inRange,
        "`duplicates drop` with `in` is not yet supported in this build (dropping duplicates "
        "within an in-restricted sample while leaving other rows untouched needs more than a "
        "simple filter) — `duplicates report`/`tag`/`list` all honor `in`.");
  }

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }
  std::string outPath = nextPath(session.workdir(), "dup_drop");
  // On the card path `ds` is deliberately null (no head was materialized), so the
  // two facts this call still needs come from the session instead: the head PATH
  // it would have reported, and the pre-mutation row count. Both are exactly the
  // substitutions the prior phase-2 work established for this file — a `--data`
  // that only needs a path takes `*currentHbin()`, and a row count the card holds
  // is read from the card. The Datastore path below is byte-identical to before.
  const std::string dataPath = ds ? ds->path() : *session.currentHbin();
  std::vector<std::string> args = {"duplicates", "drop", "--data", dataPath, "--vars", joined,
                                    "--out", outPath};
  std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
  if (ds == nullptr && !cardRows.has_value()) {
    // The card answered the varlist but cannot answer the row count. Rather than
    // report a wrong `nBefore`, fall back to the exact prior behaviour.
    ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
  }
  int64_t nBefore = ds ? ds->nRows() : *cardRows;  // pre-mutation row count.
  {
    // MAXPERF Stage 2 (item 3): single terminal row-dedup mutation; --out never
    // read back as a file. Opt into the resident frame plane.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);
  // Phase 2 (2026-08-20). This used to be `session.datastore()->nRows()`, which
  // MATERIALIZED the whole virtual head — the entire dedup output written to
  // disk — to learn ONE integer, and it did so BEFORE mutationResult ran, so the
  // card fold below could never save the flush it had already paid for. Measured
  // by the Phase 0 caller-attributed census: runDuplicatesDrop forced 100 of the
  // run's 476 head materializations (21%), second only to rename's 220.
  //
  // mutationResult already computes that integer, on BOTH of its paths, and the
  // ordering is the whole point: taking it from the result means the number
  // shipped as `n_dropped` is derived from the very card being shipped as
  // `varlist_after`, instead of from a separate read that could disagree with it.
  // Fold path: `out["n_rows_after"] = delta.rows` (session_state.cpp), the engine's
  // own post-mutation count. Rebuild path: `ds->nRows()` — the identical value
  // this line used to fetch, just no longer fetched twice.
  ojson result = mutationResult(session, "duplicates_drop");
  const int64_t nAfter = result.value("n_rows_after", int64_t{0});
  result["n_dropped"] = nBefore - nAfter;
  if (auto bad = postDuplicatesR(session, nBefore,
                                 {{"N_drop", static_cast<double>(nBefore - nAfter)}}))
    return *bad;
  return result;
}


// BS-031(a) (BETA campaign C2, silent-if-drop sweep): `duplicates tag ...
// if COND, gen(NEWVAR)` previously ignored `if` entirely — the sub-command
// dispatch in runDuplicates() below never even forwarded ifExpr to this
// function. Live-verified this stint (StataBE): tag is computed WITHIN the
// if-sample only, and if-EXCLUDED rows get NEWVAR = MISSING (oracle:
// `duplicates tag year if year==1982, gen(dtag)` -> dtag has N=48 (matches
// the if-sample size), mean=min=max=47, and the other 288 rows are
// missing). Unlike recode, `duplicates tag` is a genuine AGGREGATE over the
// CURRENT rows (group size depends on which OTHER rows are present) — a
// "compute unrestricted, mask after" shortcut would silently give the
// WRONG group sizes whenever `if` doesn't happen to align with whole
// dedup-key groups, so this must actually restrict the input before
// tagging. cobra's own `duplicates tag --keepif EXPR` (apps/cmds/
// cmds_data.cpp) does the restricting, but its `--out` is then only the
// N_sub-row filtered frame — reintegrating that into the FULL N-row
// dataset (untouched rows kept, tag missing for them) needs a join key:
// stamp every row with its own position (`_n`) before filtering, tag the
// filtered subset, then `cobra merge 1:1` the tagged subset's `genvar`
// back onto the full id-stamped frame by that same position (an ordinary
// master-keeps-all merge — unmatched rows get missing "for free", exactly
// Stata's semantics) and drop the synthetic id column again.
ojson runDuplicatesTag(const std::optional<std::vector<std::string>>& varlist,
                       const std::string& genvar, SessionState& session,
                       const std::optional<std::string>& ifExpr,
                       const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);
  // BUG-1120 review: generate() becomes the name of a `generate`, so generate's
  // rule answers it: `duplicates tag x, gen(_N)' is "_N invalid varname" r(198),
  // gen(byte) "too few variables specified" r(102) (StataBE 19.5).
  if (auto r = strutil::generateNameRefusal(genvar)) return rcErrorBlock(r->message, r->rc);
  if (ds->has(genvar)) {
    return engineErrorBlock("variable `" + genvar + "' already defined");
  }

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (!haveIf && !haveIn) {
    std::string outPath = nextPath(session.workdir(), "dup_tag");
    std::vector<std::string> args = {"duplicates", "tag",    "--data", ds->path(), "--vars",
                                      joined,       "--gen",  genvar,   "--out",    outPath};
    // MAXPERF Stage 2 (item 3): the unrestricted `duplicates tag` is a single
    // terminal pure-ADD mutation (--out becomes the head, never read back as a
    // file). Opt into the resident frame plane. The if/in path below stays
    // STATELESS: it stamps _n, filters, tags, and merges back through
    // intermediate temps that are read AS FILES.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
    recordDuplicatesTagType(session, genvar);
    ojson tagged = mutationResult(session, "duplicates_tag", {genvar});
    if (auto bad = postDuplicatesR(session, tagged.value("n_rows_after", int64_t{0}), {}))
      return *bad;
    return tagged;
  }

  const std::string ridCol = "__dup_rid__";
  std::string idPath = nextPath(session.workdir(), "dup_tag_id");
  try {
    // egen seq() is the engine's row-number op — bare `_n` in engine eval
    // requires a bysort context (rejected), unlike PIET-side gen.
    runCobraJson({"eval", "--data", ds->path(), "--egen", ridCol + "=seq()", "--out", idPath});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // BS-090 (BETA Phase G): `ridCol` (already the GLOBAL row index, 1..N,
  // materialized above for the merge-back key regardless of `in`) doubles
  // as the `in a/b` predicate's target — no SEPARATE idx materialization
  // needed here, unlike piet::handlers::applyIfAndInFilter's general case.
  std::string combinedIf = ifExpr.value_or("");
  if (haveIn) {
    InRangeBounds rr = parseInRangeBounds(*inRange, ds->nRows());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.negative = rr.negative;
      return inRangeRejectError(fr, *inRange, "duplicates tag");
    }
    std::string rangePred = ridCol + ">=" + std::to_string(rr.a) + " & " + ridCol + "<=" +
                            std::to_string(rr.b);
    combinedIf = haveIf ? ("(" + *ifExpr + ") & (" + rangePred + ")") : rangePred;
  }
  std::string taggedPath = nextPath(session.workdir(), "dup_tag_sample");
  {
    std::vector<std::string> args = {"duplicates", "tag",     "--data", idPath, "--vars",
                                      joined,       "--gen",   genvar,   "--keepif", combinedIf,
                                      "--out",      taggedPath};
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string mergedPath = nextPath(session.workdir(), "dup_tag_merged");
  {
    // merge prints a plain-text "merge 1:1: wrote ..." line, not JSON —
    // runCobra (ok-check), never runCobraJson (same idiom as the
    // duplicates-tag call above).
    CobraRunResult cr = runCobra({"merge", "--data", idPath, "--using", taggedPath, "--type",
                                  "1:1", "--key", ridCol, "--keepusing", genvar, "--nogenerate",
                                  "--out", mergedPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string finalPath = nextPath(session.workdir(), "dup_tag_final");
  {
    CobraRunResult cr = runCobra({"eval", "--data", mergedPath, "--drop", ridCol, "--out",
                                  finalPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  const int64_t nSample = Datastore(taggedPath).nRows();
  // An empty if/in sample is r(2000) BEFORE the new variable exists
  // (w2ao_label_family.log:3177/:3193 lb_duptag_empty: `confirm variable dd`
  // is r(111) afterwards).
  if (nSample == 0)
    if (auto bad = postDuplicatesR(session, nSample, {})) return *bad;
  session.load(finalPath);
  recordDuplicatesTagType(session, genvar);
  ojson tagged = mutationResult(session, "duplicates_tag", {genvar});
  if (auto bad = postDuplicatesR(session, nSample, {})) return *bad;
  return tagged;
}


ojson runDuplicatesList(const std::optional<std::vector<std::string>>& varlist,
                        SessionState& session, const std::optional<std::string>& ifExpr,
                        const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  std::string joined;
  for (size_t i = 0; i < cols.size(); ++i) {
    if (i) joined += ",";
    joined += cols[i];
  }
  std::string tagVar = "__dup_tag__";
  std::string tagPath = nextPath(session.workdir(), "dup_list_tag");
  {
    // BS-031(a) (BETA campaign C2, silent-if-drop sweep): `duplicates list
    // ... if COND` previously ignored `if` (tag was always computed over
    // the FULL dataset). `list` is read-only/display-only — unlike tag's
    // gen() target, there is no full-N column to preserve, so pre-filtering
    // the SOURCE data (BS-090, BETA Phase G: piet::handlers::
    // applyIfAndInFilter — composes `if`/`in` via AND, replacing the old
    // direct `--keepif` forward on this internal `duplicates tag` call,
    // which could not have evaluated an `in`-derived `_n` predicate anyway)
    // is enough: it restricts BOTH which rows are considered for the dedup
    // AND which rows land in the (already scratch, never session-loaded)
    // `tagPath` output — the browse below then naturally only ever sees the
    // if/in-sample. Live-verified this stint (StataBE): `duplicates list
    // state if year==1982` correctly reports 0 duplicates (each state
    // appears once per year), `duplicates list year if year==1982` lists
    // all 48 in-sample rows (all share year 1982). Known, disclosed
    // cosmetic gap: real Stata's list table shows each row's ORIGINAL
    // (pre-filter) observation number in its "Obs" column — this handler's
    // browse()-based output has no such column at all (pre-existing, not
    // introduced by this fix), so that particular renumbering nuance isn't
    // addressed here.
    std::string dupListDataPath = ds->path();
    if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
      InRangeFilterResult fr;
      try {
        fr = applyIfAndInFilter(session, dupListDataPath, ifExpr, inRange);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      if (!fr.ok) return inRangeRejectError(fr, *inRange, "duplicates list");
      dupListDataPath = fr.dataPath;
    }
    std::vector<std::string> args = {"duplicates", "tag",  "--data", dupListDataPath, "--vars",
                                      joined,       "--gen", tagVar,  "--out",         tagPath};
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  std::string filteredPath = nextPath(session.workdir(), "dup_list_filtered");
  try {
    runCobraJson({"eval", "--data", tagPath, "--keepif", tagVar + ">0", "--out", filteredPath});
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  {
    const int64_t nSample = Datastore(tagPath).nRows();
    if (auto bad = postDuplicatesR(session, nSample, {})) return *bad;
  }
  Datastore filteredDs(filteredPath);
  if (filteredDs.nRows() == 0) {
    ojson block;
    block["type"] = "display";
    block["text"] = "No duplicates found.";
    return block;
  }
  std::optional<std::vector<std::string>> browseVars;
  if (varlist.has_value() && !varlist->empty()) browseVars = *varlist;
  // Value labels live on the SESSION keyed by varname, so they apply to the
  // cobra-filtered temp dataset too (same column names) — an empty map here
  // rendered value-labeled f64 columns as raw codes, diverging from the
  // oracle's duplicates.py (value_label_map=session.get_value_label_map()).
  // format-display-default-lifecycle (FU-1015): and every column's display
  // format from the one resolver, as list carries it (handlers_m3 runList).
  ojson block = filteredDs.browse(0, 200, browseVars, session.valueLabelMapForBrowse(),
                                  session.displayFormatMap(filteredDs.formatCard(browseVars)));
  block["editable"] = false;
  block["note"] = std::to_string(filteredDs.nRows()) + " duplicate observations";
  return block;
}


// BUG-1096: `duplicates examples [varlist] [if] [in]` -- one line per group of
// observations that share their varlist values (two or more of them in the
// sample): the group's size, the observation number of its first member and
// the shared values, groups in the varlist's sort order. duplicates.ado lists
// them in `list`'s box (Group only when there are two groups or more), after
// "Duplicates in terms of x" ("... all variables" without a varlist); with no
// such group it says "(0 observations are duplicates)". r(N) is the sample size,
// and an empty sample is r(2000). StataBE 19.5 (x = 1 2 1 3 2 1):
//   +--------------------------+
//   | Group   #   e.g. obs   x |
//   |--------------------------|
//   |     1   3          1   1 |
//   |     2   2          2   2 |
//   +--------------------------+
// Each column is as wide as its header or widest cell (a value label for a
// labelled code, a missing code as written), in display columns, and a rule
// follows every fifth line, as `list` draws it. A variable's header is its name
// unless that is wider than both eight columns and the widest cell, when list
// abbreviates it to the wider of the two (`household_id` over 1 and 2 is
// `househ~d`, `averylongvariablename` over 12-column strings `averylongv~e`); a
// variable whose format is left-justified (%-12s, %-9.0g) is left-aligned in its
// header and cells, every other column right-aligned (StataBE 19.5, %~12s too).
// list picks this table only when its lines fit c(linesize); PIET keeps no line
// width (`set linesize` is ignored), so it prints the table Stata prints at a
// linesize wide enough, and refuses by name a table wider than 255, the widest
// line Stata allows, which Stata always lists in its display format.
ojson runDuplicatesExamples(const std::optional<std::vector<std::string>>& varlist,
                            SessionState& session, const std::optional<std::string>& ifExpr,
                            const std::optional<std::string>& inRange) {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  std::vector<std::string> cols;
  auto err = resolveVarlist(ds, varlist, cols);
  if (err.has_value()) return engineErrorBlock(*err);

  // The sample, with each row's observation number in the whole data.
  std::optional<Datastore> sampleFile;
  const Datastore* sample = ds;
  std::vector<int64_t> obs;
  if ((ifExpr.has_value() && !ifExpr->empty()) || (inRange.has_value() && !inRange->empty())) {
    const std::string ridCol = "__dup_rid__";
    const std::string idPath = nextPath(session.workdir(), "dup_ex_id");
    InRangeFilterResult fr;
    try {
      runCobraJson({"eval", "--data", ds->path(), "--egen", ridCol + "=seq()", "--out", idPath});
      fr = applyIfAndInFilter(session, idPath, ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, inRange.value_or(std::string()), "duplicates examples");
    sampleFile.emplace(fr.dataPath);
    sample = &*sampleFile;
    const double* rid = sample->col(ridCol).f64();
    for (int64_t i = 0; i < sample->nRows(); ++i) obs.push_back(static_cast<int64_t>(rid[i]));
  } else {
    for (int64_t i = 0; i < sample->nRows(); ++i) obs.push_back(i + 1);
  }
  if (auto bad = postDuplicatesR(session, sample->nRows(), {})) return *bad;

  // Group the rows by their cells (a number by its bits, so `.a` is not `.`).
  std::vector<const cecon::Column*> keyCols;
  for (const auto& c : cols) keyCols.push_back(&sample->col(c));
  auto cellText = [](const cecon::Column& c, int64_t i) -> std::string {
    if (c.dtype == cecon::DType::Factor) {
      const int32_t code = c.codes()[i];
      return code < 0 || static_cast<size_t>(code) >= c.levels.size()
                 ? std::string()
                 : std::string(c.levels[static_cast<size_t>(code)]);
    }
    uint64_t bits;
    const double v = c.f64()[i] == 0.0 ? 0.0 : c.f64()[i];
    std::memcpy(&bits, &v, sizeof bits);
    return std::to_string(bits);
  };
  struct Group {
    int64_t first = 0;  // the sample row of its first member
    int64_t size = 0;
  };
  std::map<std::vector<std::string>, Group> groups;
  for (int64_t i = 0; i < sample->nRows(); ++i) {
    std::vector<std::string> key;
    for (const cecon::Column* c : keyCols) key.push_back(cellText(*c, i));
    auto [it, fresh] = groups.emplace(std::move(key), Group{i, 0});
    (void)fresh;
    ++it->second.size;
  }
  std::vector<Group> dups;
  for (const auto& [key, g] : groups)
    if (g.size > 1) dups.push_back(g);
  // Stata's sort order of the varlist: numbers ascending then the missing
  // codes, strings by their bytes.
  std::sort(dups.begin(), dups.end(), [&](const Group& a, const Group& b) {
    for (const cecon::Column* c : keyCols) {
      int d = 0;
      if (c->dtype == cecon::DType::Factor) {
        d = cellText(*c, a.first).compare(cellText(*c, b.first));
      } else {
        d = cecon::stata_cmp(c->f64()[a.first], c->f64()[b.first]);
      }
      if (d != 0) return d < 0;
    }
    return false;
  });

  const bool allVars = !varlist.has_value() || varlist->empty();
  std::string text = "Duplicates in terms of ";
  if (allVars) {
    text += "all variables";
  } else {
    for (size_t i = 0; i < cols.size(); ++i) text += (i ? " " : "") + cols[i];
  }
  if (dups.empty()) {
    ojson block;
    block["type"] = "display";
    block["text"] = text + "\n\n(0 observations are duplicates)";
    return block;
  }

  std::vector<std::string> head;
  if (dups.size() > 1) head.push_back("Group");
  head.push_back("#");
  head.push_back("e.g. obs");
  const size_t firstVar = head.size();
  for (const auto& c : cols) head.push_back(c);
  // Each variable's display format, from the one resolver list uses.
  std::map<std::string, std::string> fmtOf;
  for (const auto& [name, fmt] : session.displayFormatMap(ds->formatCard(cols)))
    fmtOf[name] = fmt;
  std::vector<bool> left(head.size(), false);
  for (size_t k = 0; k < cols.size(); ++k)
    left[firstVar + k] = fmtOf[cols[k]].compare(0, 2, "%-") == 0;
  std::vector<std::vector<std::string>> rows;
  for (size_t g = 0; g < dups.size(); ++g) {
    std::vector<std::string> row;
    if (dups.size() > 1) row.push_back(std::to_string(g + 1));
    row.push_back(std::to_string(dups[g].size));
    row.push_back(std::to_string(obs[static_cast<size_t>(dups[g].first)]));
    for (size_t k = 0; k < cols.size(); ++k) {
      const cecon::Column& c = *keyCols[k];
      if (c.dtype == cecon::DType::Factor) {
        row.push_back(cellText(c, dups[g].first));
        continue;
      }
      const double v = c.f64()[dups[g].first];
      const int kind = cecon::missing_kind(v);
      if (kind >= 0) {
        row.push_back(cecon::missing_text(kind));
        continue;
      }
      auto vl = session.valueLabels.find(cols[k]);
      if (vl != session.valueLabels.end() && v == std::floor(v)) {
        auto lab = vl->second.find(static_cast<int>(v));
        if (lab != vl->second.end()) {
          row.push_back(lab->second);
          continue;
        }
      }
      row.push_back(trim(eval::applyStataFmt(v, fmtOf[cols[k]])));
    }
    rows.push_back(std::move(row));
  }
  std::vector<size_t> width(head.size());
  for (size_t j = 0; j < head.size(); ++j) {
    size_t widest = 0;
    for (const auto& r : rows) widest = std::max(widest, displayCols(r[j]));
    if (j >= firstVar) head[j] = abbrevName(head[j], std::max<size_t>(8, widest));
    width[j] = std::max(displayCols(head[j]), widest);
  }
  auto line = [&](const std::vector<std::string>& cells) {
    std::string s = "  |";
    for (size_t j = 0; j < cells.size(); ++j) {
      const std::string pad(width[j] - displayCols(cells[j]), ' ');
      s += (j ? "   " : " ") + (left[j] ? cells[j] + pad : pad + cells[j]);
    }
    return s + " |\n";
  };
  size_t inner = 2 + 3 * (head.size() - 1);
  for (size_t w : width) inner += w;
  if (inner + 4 > 255) {
    return eval::errorBlock(
        "unsupported_command",
        "duplicates examples: these groups make a table " + std::to_string(inner + 4) +
            " columns wide, wider than Stata's widest line (linesize 255), so Stata lists them "
            "in list's display format, which this build does not implement; name fewer "
            "variables",
        std::string("duplicates"), std::nullopt, std::nullopt, "refused:list-display-format");
  }
  const std::string box = "  +" + std::string(inner, '-') + "+\n";
  const std::string rule = "  |" + std::string(inner, '-') + "|\n";
  text += "\n\n" + box + line(head) + rule;
  for (size_t r = 0; r < rows.size(); ++r) {
    if (r > 0 && r % 5 == 0) text += rule;
    text += line(rows[r]);
  }
  text += box;
  text.pop_back();  // the block carries no trailing newline
  ojson block;
  block["type"] = "display";
  block["text"] = text;
  return block;
}
ojson runDuplicates(const std::string& sub, const std::optional<std::vector<std::string>>& varlist,
                    const std::optional<std::string>& genvar, SessionState& session,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange) {
  std::string subLower = lower(sub);
  if (subLower == "report") return runDuplicatesReport(varlist, session, ifExpr, inRange);
  if (subLower == "drop") return runDuplicatesDrop(varlist, session, ifExpr, inRange);
  if (subLower == "tag") {
    if (!genvar.has_value() || genvar->empty()) {
      return engineErrorBlock("duplicates tag: ,gen(NEWVAR) required");
    }
    return runDuplicatesTag(varlist, *genvar, session, ifExpr, inRange);
  }
  if (subLower == "list") return runDuplicatesList(varlist, session, ifExpr, inRange);
  if (subLower == "examples") return runDuplicatesExamples(varlist, session, ifExpr, inRange);
  return engineErrorBlock(
      "duplicates " + sub +
      ": unknown subcommand. Use: duplicates report|examples|drop|tag|list [VARLIST]");
}
}
