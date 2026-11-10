// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// variable copy rename.
#include "data_mutation_internal.h"
#include "piet/handlers/variables.h"
namespace piet::handlers::data_mutation_detail {


// ---------------------------------------------------------------------------
// rename OLDVAR NEWVAR — C++ port of mutate.py's run_rename (two-arg form
// only — wildcard/multi-var syntax is OUT of scope, matching the oracle's
// own commands.py guard).
//
// rename is pure column-metadata relabeling — it must never touch a row's
// VALUE. L1/M3 (CSV-shim retirement): this calls cobra's native `rename`
// verb (`--pairs old=new`), which renames the header cell in place and
// leaves every other column — and the renamed column's own values and
// factor dictionary — byte-identical. Numeric and string/factor columns take
// the IDENTICAL path, no dtype branch.
//
// History (superseded, kept for the honesty rationale): this was first a
// `cobra eval --gen NEW=OLD --drop OLD` value-copy that only worked for f64
// columns and leaked a raw "[internal] expr: ..." engine exception when OLD
// was string/factor (BUG B, W3 stabilization); the fix routed it through a
// whole-frame CSV/`cobra convert` round-trip (writeColumnCsvRoundTrip, since
// deleted) because piet/native then had no way to rename a header without an
// hbin writer. The native verb supersedes both — no round-trip, no
// re-factorization.

// rename's two refusals with Stata's text and rc (w2ah_data_verbs.log
// ah_rename_exist / ah_ren_exist: rc 110, the target untouched; ah_rename_absent
// / ah_ren_absent: rc 111). The target message used to read "already exists",
// which coarseRc maps to rc 1.
ojson renameAbsent(const std::string& oldvar) {
  return rcErrorBlock("variable " + oldvar + " not found", 111);
}

ojson renameTargetExists(const std::string& newvar) {
  return rcErrorBlock("variable " + newvar + " already defined", 110);
}


// ---------------------------------------------------------------------------
// clonevar NEWVAR = OLDVAR — value copy + metadata clone (BUG-003, 1.6.0).
// C++ port of dispatcher.py's clonevar handler: gen NEWVAR=OLDVAR (action
// stays "gen", not "clonevar" — the oracle's own quirk) + clone
// label/format/stata_type/value-label attachment.
//
// Numeric clonevar uses the fast `cobra eval --gen NEW=OLD` path (also the
// one place `clonevar ... if` is supported, folded into a cond()); the
// string/factor path — which `cobra eval --gen`'s numeric-only RHS cannot
// serve (it once leaked "[internal] expr: string/factor variable used where
// a number is required" on `clonevar st2 = state`, BUG-037) — now calls
// cobra's native `clonevar` verb (`--pairs old=new`, L1/M3): the clone is
// appended at the end with the source's factor dictionary SHARED, OLDVAR
// survives untouched. (History: BUG-037 first routed the string/factor path
// through a whole-frame CSV round-trip, writeColumnCsvRoundTrip, since
// deleted — the native verb supersedes it.)
//
// BS-031(c) (BETA campaign C2, silent-if-drop sweep): `ifExpr` was
// previously accepted by the tokenizer but never threaded to this handler
// at all — `clonevar inc2 = income if year==1982` silently cloned ALL 336
// rows instead of only the 48 in-sample ones (oracle-verified live this
// stint: 48 non-missing, 288 missing — clonevar is gen-class, so an
// if-excluded row's target is MISSING, same as plain `gen`). The numeric
// fast path folds `if` into cobra's own `cond()` the exact same way
// data_generate.cpp::runGen already does for plain `gen ... if` — no filtering/
// merge-back needed since this is a per-row passthrough. The string/factor
// path (`cobra clonevar` is a whole-column copy with no per-row conditional)
// serves `if` through a dispatched string `generate`, clonevar.ado's own body.
//
// C4-L4B (FIX-C4L4B-RESIDUALS ①): the encode recipe, applied to the residual
// wrap class the C4-L4 capture named. clonevar's entry reads a NAME and a
// DTYPE and nothing else — no column data — so the resident card answers both
// (the D1 rule) and the datastore opens LAZILY; the engine call takes the head
// TOKEN (`*session.currentHbin()`) instead of `ds->path()` and is wrapped in
// ResidentEligible, which routes it through the frame plane. `cobra clonevar`
// rides the SAME §26 acquire_data/commit_frame bridge as `cmd_encode`
// (apps/cmds/cmds_data.cpp:365/:368 vs :693/:697 — read cross-lane, zero cobra
// change), and the numeric arm is plain `eval --gen`, the hot resident path
// itself. Escape (`PIET_NO_RESIDENT_READ` for the C4 family,
// `PIET_NO_RESIDENT_SETCOL` for the C2-specific arm) restores the pre-C4 entry
// and `ds->path()` VERBATIM, so the floor RED arm reproduces the old path with
// one env, exactly as at the destring/encode sites.

// clonevar.ado after its `gen': the variable label, the value label and the
// format follow the source, and so does the type claim -- through the one carry
// rule (storage::carryTypeClaim, FU-243): `gen `type'' declares the source's
// type, so the clone of an untyped generate is a float that `replace` keeps
// float (w2ah_data_verbs.log ah_fu243_clone, ah_fu243_bigint).
void copyCloneMeta(SessionState& session, const std::string& oldvar, const std::string& newvar) {
  storage::carryTypeClaim(session, oldvar, newvar, /*move=*/false);
  auto metaIt = session.varMeta.find(oldvar);
  if (metaIt != session.varMeta.end()) {
    const VarMeta oldMeta = metaIt->second;
    VarMeta& newMeta = session.varMeta[newvar];
    if (oldMeta.label.has_value()) newMeta.label = oldMeta.label;
    if (oldMeta.format.has_value()) newMeta.format = oldMeta.format;
  }
  auto lblIt = session.varLblname.find(oldvar);
  if (lblIt != session.varLblname.end()) session.varLblname[newvar] = lblIt->second;
  auto vlIt = session.valueLabels.find(oldvar);
  if (vlIt != session.valueLabels.end()) session.valueLabels[newvar] = vlIt->second;
}
}
namespace piet::handlers {
using namespace data_mutation_detail;


ojson runRename(const std::string& oldvar, const std::string& newvar, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep S4: rename's entry does nothing but ask whether one
  // name exists and another does not — the exact question runGsort/runOrder/
  // runDrop/runKeep already answer from the resident shadow card. It was
  // answering it with session.datastore(), i.e. a full cecon::Dataset::open of
  // the current hbin (mmap + 192-column header parse). Falls back to the exact
  // prior behavior whenever the card cannot be trusted — see
  // session.shadowVarlistIfCurrent()'s doc comment (session_state.h).
  //
  // SCOPE, STATED HONESTLY. This is NOT where rename's measured ~0.19 s/call
  // goes. The dominant term is engine-side: Frame::rename sets structure_dirty_
  // (src/data/frame.cpp), which disqualifies write_hbin's header-patch path and
  // forces write_hbin_full — a complete 164 MB serialize to change one string in
  // a JSON header. That is a cobra-owned fix in a file this stint does not
  // touch; removing the wasted open here does not substitute for it and is not
  // claimed to.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    if (!cardHasName(*card, oldvar)) return renameAbsent(oldvar);
    if (cardHasName(*card, newvar)) return renameTargetExists(newvar);
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    const auto& names = ds->names();
    if (std::find(names.begin(), names.end(), oldvar) == names.end()) return renameAbsent(oldvar);
    if (std::find(names.begin(), names.end(), newvar) != names.end())
      return renameTargetExists(newvar);
  }
  // BUG C: shared validator (piet::strutil::isValidStataIdentifier, see its
  // header comment) — a rename TARGET is a new column name exactly like
  // gen's LHS, so it goes through the identical rule at the identical
  // helper (data_generate.cpp::runGen / string_generate_replace.cpp::runStringGen are the
  // other two call sites).
  if (!strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "rename: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  // BUG-1120: `rename x _cons' is "_cons invalid varname" r(198), and so are the
  // storage types and if/in/using/with (`rename x int'); a str# is "str5 invalid
  // variable name" (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar)) return rcErrorBlock(r->message, r->rc);

  // L1/M3: native `cobra rename` — renames the header cell in place and
  // byte-preserves every other column plus the source's own values and factor
  // dictionary (no whole-frame CSV round-trip, no re-factorization). Output
  // --out lands on a fresh scratch path under the "rename" prefix (that prefix
  // is what scratch_path_monotonic.py's rename-chain leg counts). Mirrors
  // runCollapse's own single-verb invocation machinery (runCobra +
  // errorFromRaw). Numeric and string/factor columns take the identical path.
  std::string outPath = nextPath(session.workdir(), "rename");
  // *session.currentHbin() rather than ds->path() — equivalent whenever data is
  // loaded (a Datastore is always constructed from *currentHbin_), and it is
  // what lets the card branch above skip building one at all. Same substitution
  // runGsort's own fix already made for its --data arg.
  std::vector<std::string> args = {"rename",  "--data",  *session.currentHbin(), "--out",
                                   outPath,   "--pairs", oldvar + "=" + newvar};
  {
    // P2-PERF ARC-B/B2: rename's `--out` becomes the head and is never read
    // back as a file — mutationResult renders from the shadow card, and the
    // card knows how to fold a rename (session_state.cpp CardFoldPlan::Rename).
    // That is exactly the invariant ResidentEligible asserts, and it is the
    // same opt-in gen/replace/egen/drop/keep have had all along. Without it the
    // call is a stateless mutation, and Frame::rename sets structure_dirty_, so
    // the engine re-serialises the WHOLE frame to change one string in a JSON
    // header: 18.6 ms per call, measured, on a 48,000 x 31 frame.
    SessionState::ResidentEligible residentRename(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);

  // Carry metadata: var-label/format/stata_type + value-label attachment, and
  // the type claim through the one carry rule (FU-243: an untyped generate's
  // implicit float used to stay behind under the old name, so `replace` on the
  // new name stored a double; w2ah_data_verbs.log ah_fu243_rename/ah_fu243_ren).
  storage::carryTypeClaim(session, oldvar, newvar, /*move=*/true);
  auto metaIt = session.varMeta.find(oldvar);
  if (metaIt != session.varMeta.end()) {
    session.varMeta[newvar] = metaIt->second;
    session.varMeta.erase(oldvar);
  }
  if (session.userFormattedVars.erase(oldvar) != 0) session.userFormattedVars.insert(newvar);
  auto lblIt = session.varLblname.find(oldvar);
  if (lblIt != session.varLblname.end()) {
    session.varLblname[newvar] = lblIt->second;
    session.varLblname.erase(oldvar);
  }
  auto vlIt = session.valueLabels.find(oldvar);
  if (vlIt != session.valueLabels.end()) {
    session.valueLabels[newvar] = vlIt->second;
    session.valueLabels.erase(oldvar);
  }

  return mutationResult(session, "rename", {newvar}, {oldvar});
}


ojson runClonevar(const std::string& newvar, const std::string& oldvar, SessionState& session,
                  const std::optional<std::string>& ifExpr,
                  const std::optional<std::string>& inRange) {
  // runSaveData's entry idiom: `datastore() == nullptr` is exactly
  // `!currentHbin().has_value()` (session_state.h), so hasData() is the same
  // question without the mmap + 192-column header parse (and, on a dirty
  // virtual head, without the EMH snapshot that open would force).
  if (piet::residentReadDisabled()) {
    if (session.datastore() == nullptr) return eval::noDataLoaded();
  } else if (!session.hasData()) {
    return eval::noDataLoaded();
  }
  // BUG-1096: clonevar.ado reads `[if] [in]` before anything else, so a range
  // outside the data is "observation numbers out of range" r(198) first; rows
  // outside the range are missing, as for `generate ... in` (StataBE 19.5:
  // `clonevar y = x in 2/3` keeps x's label, value label and format, and
  // "(3 missing values generated)"). It was refused by name.
  std::string rangeArg;  // "a/b" in literal row numbers, "" = no `in`
  if (inRange.has_value() && !inRange->empty()) {
    InRangeBounds rr = parseInRangeBounds(*inRange, session.macroObsCount());
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *inRange, "clonevar");
    }
    rangeArg = std::to_string(rr.a) + "/" + std::to_string(rr.b);
  }
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  // S4a's rule (runEncode): an unrecognized dtype spelling is REFUSED rather
  // than guessed — the card's vocabulary is exactly Datastore::dtype()'s
  // ("factor" | "f64"), and anything else falls through to the datastore for
  // the whole answer rather than being mapped by hand.
  auto lookupDtype = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      const VarInfo* vi = cardFind(*card, name);
      if (vi == nullptr) return std::nullopt;  // a current card is authoritative for absence
      if (vi->dtype == "factor" || vi->dtype == "f64") return vi->dtype;
    }
    Datastore* d = ensureDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  // BUG-1120: a reserved name is refused before anything is looked up, by
  // generate's rule -- `clonevar _cons = x' "_cons invalid varname" r(198),
  // `clonevar _skip = x' "_skip not allowed" r(101), `clonevar in = x' "'='
  // invalid observation number" r(198) (StataBE 19.5).
  if (auto r = strutil::generateNameRefusal(newvar)) return rcErrorBlock(r->message, r->rc);
  // clonevar.ado's order: `confirm new var newvar' (r(110)) before `confirm var
  // varname' (r(111)). w2ah_data_verbs.log ah_clone_exist: `clonevar cb = ci'
  // with cb present is rc 110 and cb keeps its byte values -- main replaced cb
  // with ci's values at rc 0.
  if (lookupDtype(newvar).has_value()) {
    return rcErrorBlock("variable " + newvar + " already defined", 110);
  }
  std::optional<std::string> srcDtype = lookupDtype(oldvar);
  if (!srcDtype.has_value()) {
    return rcErrorBlock("variable " + oldvar + " not found", 111);
  }
  // BUG C parity: clonevar's target is a new column name exactly like
  // gen's LHS / rename's target — this path never had the shared validator
  // wired at all before; mirror it here too (piet::strutil::
  // isValidStataIdentifier, see its header comment).
  if (!strutil::isValidStataIdentifier(newvar)) {
    return engineErrorBlock(
        "clonevar: invalid variable name `" + newvar + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");
  }
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();

  // C4-L4B: the residency switch, hoisted above both arms (the destring site's
  // shape). Resident path: `--data` rides the current head token and the
  // frame-run channel strips it. Escape/stateless path: pre-C4 verbatim —
  // materialize via ensureDs() and pass the real file.
  const bool residentClone = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                             !residentReadDisabled();
  std::string dataPath;
  if (residentClone) {
    dataPath = *session.currentHbin();
  } else {
    Datastore* d = ensureDs();
    if (d == nullptr) return eval::noDataLoaded();
    dataPath = d->path();
  }

  if (*srcDtype == "f64") {
    // Numeric fast path — UNCHANGED from before this fix when no `if` is
    // given; `if` folds into a cond() exactly like plain `gen ... if`.
    std::string outPath = nextPath(session.workdir(), "gen");
    std::string genExpr = haveIf ? (newvar + "=cond((" + *ifExpr + ")," + oldvar + ",.)")
                                  : (newvar + "=" + oldvar);
    std::vector<std::string> args = {"eval", "--data", dataPath, "--gen", genExpr, "--out",
                                      outPath};
    if (!rangeArg.empty()) args.insert(args.end(), {"--gen-in", rangeArg});
    std::optional<SessionState::ResidentEligible> cloneResident;
    if (residentClone) cloneResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
  } else {
    // String/factor path — the BUG-037 fix. BS-031(c): `cobra clonevar` is a
    // whole-column copy with no per-row conditional, so an `if` or `in` takes
    // the string generate path below instead of the verb.
    if (haveIf || !rangeArg.empty()) {
      // clonevar.ado's own body: `gen `type' newvar = varname `if'' -- a string
      // clone keeps the source's str# and is "" where the condition fails
      // (w2ah_data_verbs.log ah_clone_str_if: str12 %12s, [y1] then []). The
      // string generate path serves it; the metadata copy below runs as for
      // every clone.
      std::string typeTok;
      auto tIt = session.varMeta.find(oldvar);
      if (tIt != session.varMeta.end() && tIt->second.stataType.has_value() &&
          tIt->second.stataType->rfind("str", 0) == 0)
        typeTok = *tIt->second.stataType + " ";
      ojson genBlocks =
          dispatch("quietly generate " + typeTok + newvar + " = " + oldvar +
                       (haveIf ? " if " + *ifExpr : std::string()) +
                       (rangeArg.empty() ? std::string() : " in " + rangeArg),
                   session);
      if (genBlocks.is_array()) {
        for (const auto& b : genBlocks)
          if (b.is_object() && b.value("type", std::string()) == "error") return b;
      }
      ojson result = mutationResult(session, "gen", {newvar});
      copyCloneMeta(session, oldvar, newvar);
      return result;
    }
    // L1/M3: native `cobra clonevar` — appends the clone at the END of the
    // column order with the factor dictionary SHARED from the source (no
    // whole-frame CSV round-trip, no re-factorization); OLDVAR survives
    // untouched (a clone is a COPY, not a move). Output keeps the "gen"
    // scratch prefix (scratch_path_monotonic.py's clonevar-string leg counts
    // gen_*.hbin; the numeric fast path above shares that prefix).
    std::string outPath = nextPath(session.workdir(), "gen");
    std::vector<std::string> args = {"clonevar", "--data", dataPath, "--out",
                                     outPath,     "--pairs", oldvar + "=" + newvar};
    std::optional<SessionState::ResidentEligible> cloneResident;
    if (residentClone) cloneResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
  }

  // dispatcher.py's clonevar handler ORDERING BUG (verified against the
  // frozen 1.16.0 oracle directly, MI-XXX not filed but reproduced
  // bug-for-bug per Doctrine): `gen_result = run_gen(...)` computes its
  // `varlist_after` (via _mutation_result -> varlist_with_meta) BEFORE the
  // metadata-clone lines run below, and the handler returns that ALREADY-
  // COMPUTED gen_result unless it was None — so the returned block's
  // varlist_after reflects the label/format/etc BEFORE cloning (label=""
  // for the new var), even though session._var_meta[newvar] itself DOES
  // end up correctly populated (a later command's varlist_after — e.g. the
  // next `describe` — WILL show the cloned label; only THIS command's own
  // return block is stale). Compute the result block first, THEN clone
  // metadata, to reproduce this exact ordering.
  ojson result = mutationResult(session, "gen", {newvar});
  copyCloneMeta(session, oldvar, newvar);
  return result;
}
}
