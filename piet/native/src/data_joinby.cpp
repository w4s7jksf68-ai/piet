// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data joinby.
#include "data_io_internal.h"
namespace piet::handlers::data_io_detail {


JoinbyUsingResult resolveJoinbyUsing(SessionState& session, const std::string& usingPathArg) {
  JoinbyUsingResult r;
  // cross hands the file over still quoted; a message names it without them
  // (`cross using "nof"`: StataBE `file nof.dta not found`).
  std::string usingShown = stripOneQuotePair(trim(usingPathArg));
  std::string usingPath = resolveUsingDataset(session, usingPathArg, &usingShown);
  r.usingPath = usingPath;
  if (!fileExists(usingPath)) {
    // `use using` finds no file: r(601) (w2aw_data_combine_family.log dc_j_nofile),
    // naming the NAME.dta a bare stem stood for.
    r.errBlock = ojson(eval::errorBlock("engine_error", "file " + usingShown + " not found",
                                        std::nullopt, std::nullopt, std::nullopt, "r(601)"));
    return r;
  }
  std::string ext = extOf(usingPath);
  if (ext == ".dta" || usingFileIsDta(usingPath)) {
    std::string conv = nextPath(session.workdir(), "joinby_using_dta");
    r.metaPath = conv + ".meta.json";
    registerScratchMint(r.metaPath);
    CobraRunResult cr = runCobra({"import-dta", usingPath, conv, "--out-meta", r.metaPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      r.errBlock =
          engineErrorBlock("joinby using: cannot convert '" + usingPath + "': " + ce.what());
      return r;
    }
    r.hbinPath = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "joinby_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult cr = runCobra(
        {"convert", "--data", usingPath, "--out", conv, "--delim", std::string(1, delim)});
    if (!cr.ok) {
      r.errBlock = delimitedUsingRefused(usingShown, 610);
      return r;
    }
    r.hbinPath = conv;
  } else {
    r.hbinPath = usingPath;  // already .hbin
  }
  r.ok = true;
  return r;
}


// The variables only the using file has keep what it states about them -- its
// variable label, format, storage type and value label (dc_j_base_v5 uval
// `float|%7.1f|[u val]`; BUG-1157: joinby and cross of a file whose int u
// carries ul list u through ul, StataBE).
void adoptUsingOwnVars(SessionState& session, const JoinbyUsingResult& ur,
                       const std::map<std::string, UsingVarMeta>& uMeta,
                       const std::vector<std::string>& usingOnly) {
  for (const auto& nm : usingOnly) {
    auto it = uMeta.find(nm);
    if (it == uMeta.end()) continue;
    const UsingVarMeta& u = it->second;
    if (!u.label.empty()) session.setVarLabel(nm, u.label);
    if (!u.format.empty()) {
      VarMeta& vm = session.varMeta[nm];
      if (!vm.format.has_value() || vm.format->empty()) vm.format = u.format;
    }
    if (u.stataType.has_value()) {
      session.implicitFloatVars.erase(nm);
      session.varMeta[nm].stataType = *u.stataType;
    }
  }
  adoptUsingValueLabels(session, ur.usingPath, ur.metaPath, usingOnly, true);
}
}
namespace piet::handlers {
using namespace data_io_detail;


ojson runJoinby(const std::vector<std::string>& keylist, const std::string& usingPathArg,
                SessionState& session, const std::string& unmatched,
                const std::optional<std::string>& mergeVar) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // INSPECT-2608: everything this function asks of the MASTER frame is metadata
  // — does a key exist, what are the non-key names, how many rows — and
  // `session.datastore()` was answering it with a full open of the frame. The
  // engine reads the file itself for the actual join, so that open was pure
  // overhead: measured on the acceptance corpus's controls=yes arm, joinby's
  // 34.2 s carried 19.8 s of engine and 5.5 GB of PIET-side column reads for
  // questions the resident shadow card already holds. Same idiom, and the same
  // safety rule, as runSort's varlist check (handlers_m3.cpp): the card answers
  // only when it provably describes the current head, and a nullptr falls back
  // to the exact prior behaviour.
  const std::vector<VarInfo>* card = session.shadowVarlistIfCurrent();
  std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
  Datastore* master = nullptr;
  if (!card || !cardRows.has_value()) {
    master = session.datastore();
    if (master == nullptr) return eval::noDataLoaded();
  }
  auto masterHas = [&](const std::string& nm) {
    if (master) return master->has(nm);
    for (const auto& info : *card)
      if (info.name == nm) return true;
    return false;
  };
  auto masterNames = [&]() {
    std::vector<std::string> out;
    if (master) return master->names();
    out.reserve(card->size());
    for (const auto& info : *card) out.push_back(info.name);
    return out;
  };
  if (keylist.empty()) return engineErrorBlock("joinby: key variable list required");
  for (const auto& k : keylist) {
    if (!masterHas(k)) return engineErrorBlock("joinby: variable '" + k + "' not found");
  }

  // joinby: the marker is `_merge` unless _merge() names it, and
  // exists only when unmatched() is not none or _merge() is given; a name in
  // use is `confirm new var` r(110) (dc_j_merge_exists).
  std::string marker;
  if (mergeVar.has_value()) marker = *mergeVar;
  else if (unmatched != "none") marker = "_merge";
  if (!marker.empty() && masterHas(marker))
    return ojson(eval::errorBlock("engine_error", "variable " + marker + " already defined",
                                  std::nullopt, std::nullopt, std::nullopt, "r(110)"));

  JoinbyUsingResult ur = resolveJoinbyUsing(session, usingPathArg);
  if (!ur.ok) return ur.errBlock;
  Datastore usingDs(ur.hbinPath);
  // What the using file states about its variables (labels, formats, str#),
  // adopted by the variables the join brings in (dc_j_base_v5 uval
  // `float|%7.1f|[u val]`).
  const std::map<std::string, UsingVarMeta> uMeta =
      usingVarMetaOf(session, ur.usingPath, ur.metaPath, usingDs);

  for (const auto& k : keylist) {
    if (!usingDs.has(k)) {
      return engineErrorBlock("joinby: key variable '" + k + "' not found in using dataset");
    }
  }

  // Column-name conflict check (non-key columns present on both sides) —
  // mirrors merge's own established rule: this build performs a bare inner
  // key-join only (D5), with no update/replace value-reconciliation, so an
  // overlapping non-key name is ambiguous and rejected honestly rather than
  // silently picking a side.
  std::set<std::string> keySet(keylist.begin(), keylist.end());
  std::set<std::string> masterNonKeys;
  const std::vector<std::string> masterNameList = masterNames();
  for (const auto& nm : masterNameList) {
    if (!keySet.count(nm)) masterNonKeys.insert(nm);
  }
  std::vector<std::string> overlap;
  for (const auto& nm : usingDs.names()) {
    if (keySet.count(nm)) continue;
    if (masterNonKeys.count(nm)) overlap.push_back(nm);
  }
  if (!overlap.empty()) {
    std::sort(overlap.begin(), overlap.end());
    std::string listStr = "[";
    for (size_t i = 0; i < overlap.size(); ++i) {
      if (i) listStr += ", ";
      listStr += "'" + overlap[i] + "'";
    }
    listStr += "]";
    // Master wins a shared non-key name ([D] joinby's default without update/
    // replace) — oracle-pinned on the P1 wave fixture. Not an error; the
    // engine drops the using's copy and the report below must match.
    (void)listStr;
  }

  int64_t nMaster = master ? master->nRows() : *cardRows;
  int64_t nUsing = usingDs.nRows();

  std::vector<std::string> usingExtra;  // using's non-key columns, appended
  for (const auto& nm : usingDs.names()) {
    // master wins a shared name, so it is NOT a newly added column
    if (!keySet.count(nm) && !masterHas(nm)) usingExtra.push_back(nm);
  }

  // One engine call (`cobra joinby`, L1b) replaces the old in-C++ within-key
  // cross-product + whole-frame CSV + `cobra convert` round-trip. The engine
  // verb implements the same inner unmatched(none) join: factor keys matched
  // by LEVEL STRING via a unified table (each hbin's private level table can
  // number the same string differently); unmatched keys DROPPED both sides;
  // output = master columns + using non-key columns, master-row-order
  // deterministic. One accepted+logged divergence: ±0.0 f64 keys now MATCH
  // (the engine-wide make_key ±0-normalize convention) where the old
  // text-repr keyer treated them as distinct.
  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  std::string outPath = nextPath(session.workdir(), "joinby");
  CobraRunResult cr;
  {
    // GA sweep leg ②/④, 2026-08-22. joinby was the single largest engine
    // coordinate in the census of record — 452.6 s, 27.3% of engine wall, of
    // which the phase probe put 363.6 s (77%) in the OUTPUT WRITE, at 330.9 MB
    // of scratch per call and 52.94 GB across one 47-task run. The kernel is
    // 106.9 s; the join itself was never the cost.
    //
    // The cost is that this call was leaving the router by the stateless leg,
    // so one joinby moved the whole frame across disk THREE times: the master
    // head had to be materialized to a real file to be named as `--data`, the
    // engine wrote the full result to `--out`, and `session.load(outPath)` read
    // all of it back. The resident plane exists precisely to delete all three —
    // `acquire_data("")` hands back the session's zero-copy view and
    // `commit_frame(..., "")` stages instead of writing (apps/cmds/common.cpp),
    // and both `data_missing`/`out_missing` already tolerate the empty argument
    // in resident mode. Nothing in the engine needed changing.
    //
    // ResidentEligible asserts exactly the invariant that holds here, the same
    // one rename/gen/replace/egen/drop/keep assert: this `--out` BECOMES the
    // head and is never read back as a file. `session.load(outPath)` below then
    // records the virtual chain rather than opening anything, and
    // mutationResult renders from the resulting card. joinby is safe to opt in
    // where `merge` is not: PIET never parses joinby's stderr (it checks
    // `cr.ok` and the card, never the "wrote N rows" notice), so no status
    // count depends on the one-shot leg being taken.
    SessionState::ResidentEligible residentJoinby(session);
    std::vector<std::string> jargs = {"joinby", "--data",
                                      master ? master->path() : *session.currentHbin(),
                                      "--using", ur.hbinPath, "--keys", joinedKeys,
                                      "--out", outPath};
    if (unmatched != "none") {
      jargs.push_back("--unmatched");
      jargs.push_back(unmatched);
    }
    if (!marker.empty()) {
      jargs.push_back("--merge-var");
      jargs.push_back(marker);
    }
    cr = runCobra(jargs);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // The disk-full guard still applies to the STATELESS leg, where `--out` is a
  // real file and a silent short write is exactly the BUG-350 failure mode. On
  // the resident leg no file is written by design, so the check would fire on
  // every success — it is therefore asked only when a file was expected.
  if (!session.lastRunWasResident() && !cecon::native_exists(outPath))
    return engineErrorBlock("joinby: failed writing output dataset (disk full or I/O error)");

  session.load(outPath);
  // e(sample) T2: Stata keeps the marker on ONE copy of each matched master row
  // (w2_esample_copies.log:470 X4 `0|19|72|m0=38|m0_diff=19|`); which copy
  // follows its own sort tie order (X4b `not_first_of_run=8`), and PIET keeps
  // the first of each master row's run (handlers.cpp esampleKeepFirstCopy). A
  // frame whose keys cannot be read withdraws the marker by name.
  const std::map<std::string, int64_t> runLen = esampleUsingRunLengths(usingDs, keylist);
  if (!esampleKeepFirstCopy(session, keylist, &runLen) &&
      !esampleMarkHeadLost(session, EsampleLostBy::kJoinby)) {
    return engineErrorBlock(
        "joinby: the data were joined, but the estimation-sample marker could not be "
        "withdrawn from the result; e(sample) is not reliable until the next estimation.");
  }
  adoptUsingOwnVars(session, ur, uMeta, usingExtra);
  // The marker (joinby.ado DefLabel): a byte with value label
  // __MERGE (1-3 and the 9999 "signature" entry), no variable label, displayed
  // as wide as "both in master and using data" (dc_j_both_v5 `byte|%29.0g|[]`).
  if (!marker.empty()) {
    session.implicitFloatVars.erase(marker);
    VarMeta& vm = session.varMeta[marker];
    vm.stataType = std::string("byte");
    vm.format = std::string("%29.0g");
    vm.label.reset();
    session.labelDefs["__MERGE"] = {{"1", "only in master data"},
                                    {"2", "only in using data"},
                                    {"3", "both in master and using data"},
                                    {"9999", "signature"}};
    session.varLblname[marker] = "__MERGE";
    session.valueLabels[marker] = {{1, "only in master data"},
                                   {2, "only in using data"},
                                   {3, "both in master and using data"},
                                   {9999, "signature"}};
  }
  ojson result = mutationResult(session, "joinby", usingExtra);
  result["n_master"] = nMaster;
  result["n_using"] = nUsing;
  return result;
}
}
