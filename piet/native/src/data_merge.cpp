// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data merge.
#include "data_combine_internal.h"
namespace piet::handlers {
using namespace data_combine_detail;


ojson runMerge(const std::string& mergeType, const std::vector<std::string>& keylist,
               const std::string& usingPathArg,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session, bool sequential, bool oldSyntax, bool oldSort) {
  if (!session.currentHbin().has_value()) {
    return engineErrorBlock("No dataset loaded. Load a dataset before merge.");
  }
  auto rcError = [](const std::string& code, const std::string& msg, const std::string& rc) {
    return ojson(eval::errorBlock(code, msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + rc + ")"));
  };

  // ---- parsing (merge.ado): no data are touched before it passes ----
  std::optional<std::string> keepusingOpt;
  bool nogenerate = false;
  std::optional<std::string> keepOpt, assertOpt;
  std::optional<std::string> generateOpt;
  bool updateOpt = false, replaceOpt = false, forceOpt = false, sortedOpt = false;
  bool noreportOpt = false;
  bool nonotesOpt = false;
  // BUG-148: resolve Stata option abbreviations to canonical spellings BEFORE reading them.
  static const std::unordered_set<std::string> kMergeCanonical = {
      "keepusing", "nogenerate", "keep",   "assert",   "generate", "update",
      "replace",   "force",      "sorted", "noreport", "nonotes", "nolabel"};
  std::vector<std::pair<std::string, eval::OptValue>> optsC(opts);  // `opts` is const&
  eval::canonicalizeOptionKeys(optsC, kMergeCanonical);
  bool nolabelOpt = false;
  for (const auto& [k, v] : optsC) {
    if (k == "keepusing" && !v.isBool) keepusingOpt = v.strValue;
    if (k == "nogenerate" && v.isBool && v.boolValue) nogenerate = true;
    if (k == "keep" && !v.isBool) keepOpt = v.strValue;
    if (k == "assert" && !v.isBool) assertOpt = v.strValue;
    if (k == "generate" && !v.isBool) generateOpt = trim(v.strValue);
    if (k == "update" && (!v.isBool || v.boolValue)) updateOpt = true;
    if (k == "replace" && (!v.isBool || v.boolValue)) replaceOpt = true;
    if (k == "force" && v.isBool && v.boolValue) forceOpt = true;
    if (k == "sorted" && v.isBool && v.boolValue) sortedOpt = true;
    if (k == "noreport" && v.isBool && v.boolValue) noreportOpt = true;
    if (k == "nonotes" && v.isBool && v.boolValue) nonotesOpt = true;
    if (k == "nolabel" && v.isBool && v.boolValue) nolabelOpt = true;
  }
  // BUG-105 (옵션망): any other residual option token is a named reject.
  if (auto err = eval::rejectUnknownOptions(optsC, kVocabMerge.known, kVocabMerge.unimplemented)) {
    return *err;
  }
  // resultlist("assert") / resultlist("keep") (merge).
  std::vector<int> keepCodes, assertCodes;
  if (assertOpt.has_value())
    if (auto err = parseMergeResults(*assertOpt, assertCodes)) return *err;
  if (keepOpt.has_value())
    if (auto err = parseMergeResults(*keepOpt, keepCodes)) return *err;
  // Stata: `generate()` RENAMES the marker; `nogenerate` suppresses it.
  if (generateOpt.has_value() && nogenerate) {
    return engineErrorBlock(
        "merge: `generate()` and `nogenerate` are alternatives — one names the marker variable, "
        "the other suppresses it.",
        "bad_input");
  }
  if (generateOpt.has_value() &&
      (generateOpt->empty() || !strutil::isValidStataIdentifier(*generateOpt))) {
    return rcError("syntax_error", "merge: generate() requires a variable name — got `" +
                                       generateOpt.value_or("") + "'",
                   "198");
  }
  // BUG-1120 review: a reserved marker name is refused by the new-variable rule
  // with _se and the r(table) row names ("_cons invalid varname", r(198)); alias
  // and _skip are "NAME not allowed" r(101) (StataBE 19.5).
  if (generateOpt.has_value()) {
    if (auto r = strutil::newVarNameRefusal(*generateOpt, /*resultRows=*/true))
      return rcError("syntax_error", r->message, std::to_string(r->rc));
    if (*generateOpt == "alias" || *generateOpt == "_skip")
      return rcError("syntax_error", *generateOpt + " not allowed", "101");
  }
  // `confirm new var` (merge): parse time, the data are not sorted yet
  // (dc_mg_gen_exists r(110), k=6 N=6 unchanged).
  auto hasVar = [&](const std::string& nm) {
    for (const auto& [n, dt] : currentNameDtypes(session))
      if (n == nm) return true;
    return false;
  };
  if (generateOpt.has_value() && hasVar(*generateOpt))
    return rcError("engine_error", "variable " + *generateOpt + " already defined", "110");
  // `replace` (merge): requires `update`.
  if (replaceOpt && !updateOpt) {
    return rcError("syntax_error",
                   "option replace not allowed\n    option replace requires you also specify "
                   "option update, thus demonstrating your understanding that an update will be "
                   "performed",
                   "198");
  }

  // Resolve the `using` file against session.cwd (native-paths stabilization
  // item 2) — session.workdir() is the internal hbin-chain scratch dir, not
  // where a user's own dataset files live.
  // [D] merge: "If filename is specified without an extension, .dta is
  // assumed" (w2d_uncovered.log merge_noforce_ctl: `using "w2d_mu"` reads
  // w2d_mu.dta -> r(106) on its str3 w).
  std::string usingShown = usingPathArg;
  std::string usingPath = resolveUsingDataset(session, usingPathArg, &usingShown);
  if (!fileExists(usingPath)) {
    // merge.ado sorts the master before `describe using` finds no
    // file, so the r(601) leaves the data sorted (dc_mx_nofile_d).
    if (!sortedOpt && !sequential && !oldSyntax) {
      for (const auto& k : keylist)
        if (!hasVar(k)) return rcError("engine_error", "variable " + k + " not found", "111");
      ojson sorted = runSort(keylist, session);
      if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
    }
    return rcError("engine_error", "file " + usingShown + " not found", "601");
  }

  Datastore* master = session.datastore();
  if (master == nullptr) return eval::noDataLoaded();

  // The using side may be .dta/.csv — pre-convert to hbin so we can
  // pre-validate keys/cardinality on it directly.
  std::string usingHbinPath = usingPath;
  std::string ext = lower(usingPath.substr(usingPath.find_last_of('.') == std::string::npos
                                               ? usingPath.size()
                                               : usingPath.find_last_of('.')));
  std::string usingMetaPath;  // xtset-file-persistence: the using side's char table
  if (ext == ".dta" || usingFileIsDta(usingPath)) {
    std::string conv = nextPath(session.workdir(), "merge_using_dta");
    usingMetaPath = conv + ".meta.json";
    registerScratchMint(usingMetaPath);  // BUG-389 (same sidecar shape as append's)
    CobraRunResult cr = runCobra({"import-dta", usingPath, conv, "--out-meta", usingMetaPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock("merge using: cannot convert '" + usingPath + "': " + ce.what());
    }
    usingHbinPath = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "merge_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult cr =
        runCobra({"convert", "--data", usingPath, "--out", conv, "--delim", std::string(1, delim)});
    if (!cr.ok) return delimitedUsingRefused(usingShown, 610);
    usingHbinPath = conv;
  }

  Datastore usingDs(usingHbinPath);

  for (const auto& k : keylist) {
    if (!master->has(k))
      return rcError("engine_error", "variable " + k + " not found", "111");
    if (!usingDs.has(k))
      return rcError("engine_error", "variable " + k + " not found in using data", "111");
  }

  // `sorted` (merge skips its sort): the built-in _merge then needs both
  // files marked sorted by the keys, else r(5) with the data as they were
  // (dc_ms_unsorted_master, dc_ms_unsorted_using).
  if ((sortedOpt && !sequential) || (oldSyntax && !oldSort)) {
    auto leads = [&](const std::string& claim) {
      std::istringstream in(claim);
      std::vector<std::string> c;
      for (std::string w; in >> w;) c.push_back(w);
      if (keylist.size() > c.size()) return false;
      for (size_t i = 0; i < keylist.size(); ++i)
        if (c[i] != keylist[i]) return false;
      return true;
    };
    if (!leads(master->sortedBy())) return rcError("engine_error", "master data not sorted", "5");
    if (!leads(usingDs.sortedBy())) return rcError("engine_error", "using data not sorted", "5");
  }

  // BUG-548: keepusing() is a varlist parsed against the USING names.
  std::vector<std::string> usingNonKeyKept;
  std::string keepusingCsv;
  std::set<std::string> keptUsing;  // the using names the merge brings (keys included)
  if (keepusingOpt.has_value()) {
    std::vector<std::string> expanded;
    if (auto err = expandUsingKeepList(*keepusingOpt, usingDs.names(), expanded)) return *err;
    for (size_t i = 0; i < expanded.size(); ++i) {
      if (i) keepusingCsv += ",";
      keepusingCsv += expanded[i];
    }
    std::set<std::string> full(keylist.begin(), keylist.end());
    full.insert(expanded.begin(), expanded.end());
    for (const auto& n : usingDs.names()) {
      if (full.count(n)) usingNonKeyKept.push_back(n);
    }
  } else {
    usingNonKeyKept = usingDs.names();
  }
  keptUsing.insert(usingNonKeyKept.begin(), usingNonKeyKept.end());

  const std::vector<std::string> masterNamesBefore = master->names();
  const std::set<std::string> keySet(keylist.begin(), keylist.end());
  // Shared non-key variables the using brings (keepusing permitting).
  std::vector<std::string> sharedVars;
  for (const auto& n : masterNamesBefore)
    if (!keySet.count(n) && keptUsing.count(n)) sharedVars.push_back(n);

  // ---- the checks the built-in _merge makes after merge.ado sorted the
  // master (:98-104): their refusals leave the master SORTED by the keys ----
  const std::string marker = nogenerate ? std::string() : generateOpt.value_or("_merge");
  std::optional<ojson> lateRefusal;
  if (!marker.empty() && hasVar(marker)) {
    lateRefusal = rcError("engine_error", "variable " + marker + " already defined", "110");
  }
  if (!lateRefusal) {
    for (const auto& k : keylist) {
      if (master->dtype(k) != usingDs.dtype(k)) {
        const bool mStr = master->dtype(k) == "factor";
        lateRefusal = rcError("engine_error",
                              "key variable " + k + " is " + (mStr ? "str" : "numeric") +
                                  " in master but " + (mStr ? "numeric" : "str") +
                                  " in using data",
                              "106");
        break;
      }
    }
  }
  std::vector<std::string> forcedVars;  // string/numeric clashes `force` lets through
  if (!lateRefusal) {
    for (const auto& n : sharedVars) {
      if (master->dtype(n) == usingDs.dtype(n)) continue;
      if (forceOpt) {
        forcedVars.push_back(n);
        continue;
      }
      const bool mStr = master->dtype(n) == "factor";
      lateRefusal = rcError(
          "engine_error",
          "variable " + n + " is " + (mStr ? "str" : "numeric") + " in master but " +
              (mStr ? "numeric" : "str") +
              " in using data\n    You could specify merge's force option to ignore this "
              "numeric/string mismatch.  The using variable would then be treated as if it "
              "contained \"\" or numeric missing value.",
          "106");
      break;
    }
  }
  if (!lateRefusal && !sequential) {
    auto dupRefusal = [&](Datastore& ds, const char* side) -> std::optional<ojson> {
      DupKeyResult dup = findDupKey(ds, keylist);
      if (!dup.found) return std::nullopt;
      std::string vars;
      for (size_t i = 0; i < keylist.size(); ++i) vars += (i ? " " : "") + keylist[i];
      return rcError("engine_error",
                     std::string(keylist.size() == 1 ? "variable " : "variables ") + vars +
                         (keylist.size() == 1 ? " does" : " do") +
                         " not uniquely identify observations in the " + side + " data",
                     "459");
    };
    if (mergeType == "1:1" || mergeType == "1:m") lateRefusal = dupRefusal(*master, "master");
    if (!lateRefusal && (mergeType == "1:1" || mergeType == "m:1")) {
      lateRefusal = dupRefusal(usingDs, "using");
      // The using side is checked after the master's keys were promoted to hold
      // the using's values (dc_mx_11_dupusing_d_v1 id int, from byte).
      if (lateRefusal) {
        const std::map<std::string, UsingVarMeta> um =
            usingVarMetaOf(session, usingPath, usingMetaPath, usingDs);
        for (const auto& k : keylist) {
          auto it = um.find(k);
          if (it == um.end() || !it->second.stataType.has_value() || master->dtype(k) != "f64" ||
              !isNumericStataType(*it->second.stataType))
            continue;
          const std::optional<std::string> m = numericTypeClaim(session, k);
          if (m.has_value() && !isNumericStataType(*m)) continue;
          const std::optional<std::string> promoted = appendPromotedType(m, it->second.stataType);
          if (promoted.has_value() && promoted != m) {
            VarMeta& vm = session.varMeta[k];
            if (!vm.format.has_value() || vm.format->empty()) vm.format = stataDefaultFormat(m);
            setNumericTypeClaim(session, k, promoted);
          }
        }
      }
    }
  }
  if (lateRefusal) {
    if (!sortedOpt && !sequential && !oldSyntax) {
      ojson sorted = runSort(keylist, session);
      if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
    }
    return *lateRefusal;
  }

  // ---- the merge: cobra's built-in _merge plan (MergeOpts::stata_plan) ----
  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  // nogenerate with update still needs the marker for r(N) (count of 4):
  // it is generated under a scratch name and dropped below.
  const bool scratchMarker = nogenerate && updateOpt && !noreportOpt;
  const std::string engineMarker = scratchMarker ? std::string("__000merge") : marker;
  auto mergeArgs = [&](bool withResults, const std::string& outPath) {
    std::vector<std::string> args = {"merge",     "--data", master->path(), "--using",
                                      usingHbinPath, "--type", mergeType,     "--stata-plan"};
    if (sequential) {
      args.push_back("--sequential");
    } else {
      args.push_back("--key");
      args.push_back(joinedKeys);
    }
    if (sortedOpt) args.push_back("--sorted");
    if (keepusingOpt.has_value()) {
      args.push_back("--keepusing");
      args.push_back(keepusingCsv);
    }
    if (engineMarker.empty()) {
      args.push_back("--nogenerate");
    } else if (engineMarker != "_merge") {
      args.push_back("--generate");
      args.push_back(engineMarker);
    }
    if (updateOpt) {
      args.push_back("--update");
      if (replaceOpt) args.push_back("--replace");
    }
    auto codesCsv = [](const std::vector<int>& c) {
      std::string s;
      for (int x : c) s += (s.empty() ? "" : " ") + std::to_string(x);
      return s;
    };
    if (withResults && !keepCodes.empty()) {
      args.push_back("--keep");
      args.push_back(codesCsv(keepCodes));
    }
    if (withResults && !assertCodes.empty()) {
      args.push_back("--assert");
      args.push_back(codesCsv(assertCodes));
    }
    args.push_back("--out");
    args.push_back(outPath);
    return args;
  };
  std::string outPath = nextPath(session.workdir(), "merge");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentMerge(session);
    cr = runCobra(mergeArgs(true, outPath));
  }
  bool assertFailed = false;
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    if (ce.code != "merge_assert") return engineErrorBlock(cobraErrorStr(ce), ce.code);
    // merge.ado checks assert() before keep() and on failure exits
    // r(9) with the merged result -- labelled, keep() not applied -- left in
    // memory (dc_ma_fail_3_d k=9 N=8, dc_ma_fail_keep_d N=8).
    assertFailed = true;
    outPath = nextPath(session.workdir(), "merge");
    SessionState::ResidentEligible residentMerge(session);
    cr = runCobra(mergeArgs(false, outPath));
    if (!cr.ok) {
      CobraError ce2 = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce2), ce2.code);
    }
  }

  // What the using file states about its variables, read while it is open.
  std::map<std::string, UsingVarMeta> uMeta = usingVarMetaOf(session, usingPath, usingMetaPath, usingDs);

  session.load(outPath);
  // xtset-file-persistence: the result keeps the MASTER's characteristics; the
  // using side's ts declaration fills in only when the master states none.
  // Oracle: w2_eststo_prefix.log:541 (B3, master un-xtset, using xtset -> xtreg
  // rc 0) and :582 (B3 CONTROL, master xtset, using not -> rc 0). Scoped to the
  // ts entries — no oracle here pins how merge folds the using side's other chars.
  const bool masterUndeclared = !(session.panelvar.has_value() && !session.panelvar->empty()) &&
                                !(session.timevar.has_value() && !session.timevar->empty());
  // BUG-659: a native checkpoint (`.hbin`/`.tmp`) this session saved states its
  // declaration beside its path rather than in a char table — same fill rule.
  const SessionState::CheckpointTsDecl* usingCk =
      (masterUndeclared && usingMetaPath.empty()) ? session.checkpointTsDecl(usingPath) : nullptr;
  if (usingCk != nullptr &&
      ((usingCk->panelvar.has_value() && !usingCk->panelvar->empty()) ||
       (usingCk->timevar.has_value() && !usingCk->timevar->empty()))) {
    session.panelvar = usingCk->panelvar;
    session.timevar = usingCk->timevar;
    session.delta = usingCk->delta;
    session.writeTsChars();
  }
  if (!usingMetaPath.empty() && masterUndeclared) {
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      nlohmann::json meta = nlohmann::json::parse(ms.str());
      if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
        bool adopted = false;
        for (const auto& ch : meta["characteristics"]) {
          if (!ch.is_object()) continue;
          const std::string vn = ch.value("varname", std::string());
          const std::string cn = ch.value("charname", std::string());
          if (vn != "_dta" || !SessionState::isTsCharName(cn) ||
              !ch.contains("value") || !ch["value"].is_string())
            continue;
          if (session.dtaChar(cn) != nullptr) continue;
          session.characteristics.push_back(
              DtaCharacteristic{vn, cn, ch["value"].get<std::string>()});
          adopted = true;
        }
        if (adopted) session.adoptTsFromChars();
      }
    } catch (const std::exception&) {
      // An unreadable sidecar leaves the master's (empty) declaration as it is —
      // the merge itself already succeeded on the engine's own read of the file.
    }
  }
  ojson result = mutationResult(session, "merge");
  // After mutationResult, which leaves the card current: appendUsingNotes reads
  // the result's variable names from it. Before, no card existed yet and the
  // names came from a Datastore, which wrote the whole merged frame to disk
  // (BUG-1089). The notes touch only the char table, which the result block
  // does not carry.
  if (!nonotesOpt) appendUsingNotes(session, usingPath, usingMetaPath);
  // Storage types, labels and formats (BUG-666, DC4, DC5).
  {
    const std::set<std::string> inMaster(masterNamesBefore.begin(), masterNamesBefore.end());
    const std::set<std::string> forced(forcedVars.begin(), forcedVars.end());
    std::vector<std::string> brought;
    for (const auto& [nm, dtype] : currentNameDtypes(session)) {
      auto uit = uMeta.find(nm);
      const UsingVarMeta* u = uit == uMeta.end() ? nullptr : &uit->second;
      if (!inMaster.count(nm)) {
        // A variable the merge brings in keeps the using file's storage type,
        // label and format (dc_m11_v6 yval `double|%10.3f|[y of u]`, dc_m11_v7
        // wtxt str4).
        brought.push_back(nm);
        if (u == nullptr) continue;
        if (dtype == "f64") {
          std::optional<std::string> t;
          if (u->stataType.has_value() && isNumericStataType(*u->stataType)) t = u->stataType;
          setNumericTypeClaim(session, nm, t);
        } else {
          if (const auto m = numericTypeClaim(session, nm); m && isNumericStataType(*m))
            setNumericTypeClaim(session, nm, std::nullopt);  // stale numeric claim on a string
          if (u->stataType.has_value() && !isNumericStataType(*u->stataType))
            session.varMeta[nm].stataType = *u->stataType;
        }
        if (!u->label.empty()) session.setVarLabel(nm, u->label);
        if (!u->format.empty()) {
          VarMeta& vm = session.varMeta[nm];
          if (!vm.format.has_value() || vm.format->empty()) vm.format = u->format;
        }
        continue;
      }
      // A key or a shared variable the using brings: merge promotes the
      // master's storage to hold the using's values (`(variable id was byte,
      // now int to accommodate using data's values)`, dc_m11_v1 id int, v int,
      // t str3). `force` keeps a clashing variable's master type.
      if (u == nullptr || (!keySet.count(nm) && !keptUsing.count(nm)) || forced.count(nm)) continue;
      if (!u->stataType.has_value()) continue;
      const std::optional<std::string> m = numericTypeClaim(session, nm);
      if (dtype == "f64") {
        if (!isNumericStataType(*u->stataType)) continue;
        if (m.has_value() && !isNumericStataType(*m)) continue;
        const std::optional<std::string> promoted = appendPromotedType(m, u->stataType);
        if (promoted.has_value() && promoted != m) {
          VarMeta& vm = session.varMeta[nm];
          if (!vm.format.has_value() || vm.format->empty()) vm.format = stataDefaultFormat(m);
          setNumericTypeClaim(session, nm, promoted);
        }
      } else if (m.has_value()) {
        const int mw = strWidthOf(*m), uw = strWidthOf(*u->stataType);
        if (*u->stataType == "strL" && *m != "strL") {
          session.varMeta[nm].stataType = std::string("strL");
        } else if (mw >= 0 && uw > mw) {
          VarMeta& vm = session.varMeta[nm];
          vm.stataType = *u->stataType;
        }
      }
      // Under update, a shared variable unlabelled in the master takes the
      // using's label (dc_mu_m1_update_replace v `[v of u]`; without update it
      // stays unlabelled, dc_mm1_v3 `[]`).
      if (updateOpt && !keySet.count(nm) && !u->label.empty()) {
        auto vit = session.varMeta.find(nm);
        const bool masterLabelled = vit != session.varMeta.end() && vit->second.label.has_value() &&
                                    !vit->second.label->empty();
        if (!masterLabelled) session.setVarLabel(nm, u->label);
      }
    }
    session.adoptVirtualSaveFormats(usingPath, brought);
    // Value labels (StataBE uni-oracle/vm.do; AER 007's Combined_waves1and2_final
    // lost all of Wave 2's 43 labelled variables): a variable the merge brings in
    // keeps its attachment; a shared one keeps the master's, even none; the
    // using's definitions are copied unless the master already defines the name
    // (`both` stays the master's), and `nolabel` copies no definition while the
    // attachments still come.
    adoptUsingValueLabels(session, usingPath, usingMetaPath, brought, !nolabelOpt);
    // The marker (merge.ado): byte %23.0g, value label `_merge`
    // (defined unless a `_merge` label already is), variable label "Matching
    // result from merge" (dc_m11_g `[_merge]`, dc_m11_gl, dc_m11_v9).
    if (!marker.empty() && oldSyntax) {
      // merge_10: a byte %8.0g marker with no labels (dc_mold_g `[]`, dc_mold_v9).
      setNumericTypeClaim(session, marker, std::string("byte"));
      session.varMeta[marker].format = std::string("%8.0g");
    } else if (!marker.empty()) {
      setNumericTypeClaim(session, marker, std::string("byte"));
      session.varMeta[marker].format = std::string("%23.0g");
      if (!session.labelDefs.count("_merge")) {
        session.labelDefs["_merge"] = {{"1", "Master only (1)"},
                                       {"2", "Using only (2)"},
                                       {"3", "Matched (3)"},
                                       {"4", "Missing updated (4)"},
                                       {"5", "Nonmissing conflict (5)"}};
      }
      std::map<int, std::string> numericMap;
      for (const auto& [code, text] : session.labelDefs["_merge"]) {
        try {
          numericMap[std::stoi(code)] = text;
        } catch (const std::exception&) {
        }
      }
      session.varLblname[marker] = "_merge";
      session.valueLabels[marker] = numericMap;
      session.setVarLabel(marker, "Matching result from merge");
    }
  }

  // Counts: the resident leg publishes them on the frame response, the
  // stateless leg in its stderr notice. They are the plan's, before keep().
  std::optional<int64_t> usingOnly;
  if (const CobraFrameMeta* mm = session.lastResidentMergeCounts()) {
    ojson counts;
    counts["master_only"] = mm->mergeMasterOnly;
    counts["using_only"] = mm->mergeUsingOnly;
    counts["matched"] = mm->mergeMatched;
    usingOnly = mm->mergeUsingOnly;
    if (!marker.empty()) result["merge_counts"] = counts;
  } else {
    static const std::regex reCounts(R"(master_only=(\d+)\s+using_only=(\d+)\s+matched=(\d+))");
    std::smatch m;
    if (std::regex_search(cr.stderr_text, m, reCounts)) {
      ojson counts;
      counts["master_only"] = std::stoll(m.str(1));
      counts["using_only"] = std::stoll(m.str(2));
      counts["matched"] = std::stoll(m.str(3));
      usingOnly = std::stoll(m.str(2));
      if (!marker.empty()) result["merge_counts"] = counts;
    }
  }

  // r(N) (merge's result table): `count if marker==2` after keep(),
  // or `count if marker==4` under update (dc_m11_r 2, dc_mk_master_r 0,
  // dc_mu_update_r 1). With noreport only a 1:1 merge counts (==2).
  // A failed assert() exits before both, leaving r() as it was (dc_ma_fail_3_r).
  if (!assertFailed && !oldSyntax && (!noreportOpt || mergeType == "1:1")) {
    const bool countFour = updateOpt && !noreportOpt;
    if (!countFour) {
      if (usingOnly.has_value()) {
        const bool kept2 = keepCodes.empty() ||
                           std::find(keepCodes.begin(), keepCodes.end(), 2) != keepCodes.end();
        session.eval.rValues.set("N", static_cast<double>(kept2 ? *usingOnly : 0));
      }
    } else if (!engineMarker.empty()) {
      // The update split (code 4) is not in the published counts: read the marker.
      if (Datastore* ds = session.datastore(); ds != nullptr && ds->has(engineMarker)) {
        const double* v = ds->col(engineMarker).f64();
        const int64_t n = ds->nRows();
        int64_t c = 0;
        for (int64_t r = 0; v != nullptr && r < n; ++r) c += v[r] == 4.0 ? 1 : 0;
        session.eval.rValues.set("N", static_cast<double>(c));
      }
    }
  }
  if (scratchMarker) {
    (void)mutationResult(session, "merge");
    ojson dropped = runDrop({engineMarker}, session);
    if (dropped.is_object() && dropped.value("type", std::string()) == "error") return dropped;
    session.forgetVarPlanes(engineMarker);
    result = mutationResult(session, "merge");
  }
  if (assertFailed) {
    std::string text;
    for (size_t i = 0; i < assertCodes.size(); ++i) {
      static const char* kText[] = {"", "from master", "from using", "matched",
                                    "matched and updated", "matched and conflicting"};
      const int c = assertCodes[i];
      const std::string t = (c >= 1 && c <= 5) ? kText[c] : "of unknown type";
      if (i) text += (i + 1 == assertCodes.size()) ? (assertCodes.size() > 2 ? ", or " : " or ") : ", ";
      text += t;
    }
    return rcError("engine_error",
                   "after merge, not all observations " + text + "\n(merged result left in memory)",
                   "9");
  }
  return result;
}
}
