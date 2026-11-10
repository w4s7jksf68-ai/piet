// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// data order.
#include "data_mutation_internal.h"
namespace piet::handlers {
using namespace data_mutation_detail;


// ---------------------------------------------------------------------------
// gsort [-]VAR [[-]VAR ...] — C++ port of gsort.py (parse_gsort_varlist +
// run_gsort, which delegates to editor.py's run_sort). Native cobra 0.18.0
// gsort's own error text diverges from the oracle's ("no column: X" vs
// "Variable 'X' not found.") — pre-validate here so error cells stay
// byte-identical; success path routes to cobra's native --vars "-a,b" verb.
ojson runGsort(const std::vector<std::string>& tokens, SessionState& session) {
  if (tokens.empty()) {
    return engineErrorBlock("gsort requires at least one variable");
  }
  std::vector<std::string> varnames;
  std::vector<bool> ascending;
  for (const auto& tok : tokens) {
    // [+-]?name, the name a (Unicode-permissive) identifier: bytes >= 0x80 are
    // identifier bytes (core/ident.h), an existing column is looked up byte-exactly.
    const bool signd = !tok.empty() && (tok[0] == '+' || tok[0] == '-');
    const std::string nm = signd ? tok.substr(1) : tok;
    bool ok = !nm.empty() && cecon::ident::isIdentStartByte(nm[0]);
    for (size_t i = 1; ok && i < nm.size(); ++i) ok = cecon::ident::isIdentByte(nm[i]);
    if (!ok) {
      return engineErrorBlock("gsort: invalid varspec '" + tok + "' — expected [-]varname");
    }
    varnames.push_back(nm);
    ascending.push_back(!(signd && tok[0] == '-'));
  }
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix (handlers_m3.cpp) — this varlist check only ever needs column
  // NAMES, so answer it from the resident shadow card when it is provably
  // current for the loaded hbin, instead of unconditionally forcing
  // session.datastore() to reopen the file from disk. Falls back to the
  // exact prior behavior (session.datastore() + ds->has()) otherwise.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varnames) {
      if (!cardHasName(*card, v)) return engineErrorBlock("Variable '" + v + "' not found.");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varnames) {
      if (!ds->has(v)) return engineErrorBlock("Variable '" + v + "' not found.");
    }
  }

  std::string joined;
  for (size_t i = 0; i < varnames.size(); ++i) {
    if (i) joined += ",";
    if (!ascending[i]) joined += "-";
    joined += varnames[i];
  }
  std::string outPath = nextPath(session.workdir(), "gsort");
  // fix/verb-overhead-sweep: *session.currentHbin() (not a ds->path() read)
  // — equivalent whenever data is loaded (Datastore is always constructed
  // from *currentHbin_, see SessionState::datastore()), and lets the fast
  // (card) branch above skip building a Datastore entirely, same choice
  // runSort's own fix already made for its --data arg.
  std::vector<std::string> args = {"gsort", "--data", *session.currentHbin(), "--vars", joined,
                                    "--out", outPath};
  // MAXPERF Stage 2 (item 3): single terminal row-reorder; --out never read back
  // as a file. Opt into the resident frame plane (no-op on the fallback plane).
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) return engineErrorBlock(cobraErrorStr(errorFromRaw(cr)), errorFromRaw(cr).code);
  session.load(outPath);
  return mutationResult(session, "sort");
}


// ---------------------------------------------------------------------------
// order VARLIST [, first|last|before(VAR)|after(VAR)] — C++ port of
// order.py's run_order. Pre-validates everything the oracle validates
// (anchor existence, anchor-not-in-varlist, mode exclusivity) so error text
// matches byte-for-byte, then calls cobra's native order verb for the
// actual column-permutation write.
ojson runOrder(const std::vector<std::string>& varlistIn,
              const std::vector<std::pair<std::string, eval::OptValue>>& opts,
              SessionState& session) {
  std::vector<std::string> varlist = varlistIn;
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix (handlers_m3.cpp) — order's own entry validation (varlist +
  // before()/after() anchor existence, all below) only ever needs column
  // NAMES, so answer it from the resident shadow card when it is provably
  // current, instead of unconditionally forcing session.datastore() open.
  // Falls back to the exact prior behavior (session.datastore() +
  // ds->names()) whenever the card cannot be trusted.
  //
  // NOTE on scope: order's own Frame-level mutation (src/data/frame.cpp,
  // Frame::order()'s rvalue overload) unconditionally sets
  // structure_dirty_=true ("a column permutation can't be patched onto the
  // source"), so the engine reports card_stable=false on every single call
  // (apps/cecon_cli.cpp's card_stable computation) and mutationResult's own
  // post-mutation Datastore rebuild further down is NEVER skippable via the
  // resident fast path — unlike gsort/sort, whose take_rows-based Frame
  // mutation never sets that bit. This fix removes only the AVOIDABLE
  // entry-side reopen (immediately below); the unavoidable post-mutation
  // rebuild is untouched (and untouchable from this file — it lives in
  // mutation_result.cpp).
  const auto* card = session.shadowVarlistIfCurrent();
  Datastore* ds = nullptr;
  if (card == nullptr) {
    ds = session.datastore();
    if (ds == nullptr) return engineErrorBlock("No dataset loaded. Use `use` first.", "no_data_loaded");
  }
  if (varlist.empty()) return engineErrorBlock("order: variable list required");

  // BUG-105 (옵션망): order accepts only first/last/before()/after() — any
  // other residual option token (a Stata option this build doesn't
  // implement, or a typo) must be a named reject, not a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownOrderOpts = kVocabOrder.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnownOrderOpts)) {
    return *err;
  }

  auto hasName = [&](const std::string& n) {
    if (card != nullptr) return cardHasName(*card, n);
    const std::vector<std::string>& allNames = ds->names();
    return std::find(allNames.begin(), allNames.end(), n) != allNames.end();
  };
  // `alphabetic` (min `alpha`) sorts the named variables before moving them,
  // and `_all`/`*` name every variable (aer_w2 w2b_dataio order_alpha
  // `order *, alpha` -> big g id num s x year; order_alphabetic `order _all,
  // alphabetic`, w2b_dataio.log:809-845).
  bool alphabetic = false;
  for (const auto& [k, v] : opts)
    if (k == "alphabetic") alphabetic = true;
  {
    std::vector<std::string> all;
    if (card != nullptr) {
      for (const auto& info : *card) all.push_back(info.name);
    } else {
      all = ds->names();
    }
    std::vector<std::string> expanded;
    for (const auto& v : varlist) {
      if (v == "_all" || v == "*") {
        for (const auto& n : all)
          if (std::find(expanded.begin(), expanded.end(), n) == expanded.end()) expanded.push_back(n);
        continue;
      }
      if (std::find(expanded.begin(), expanded.end(), v) == expanded.end()) expanded.push_back(v);
    }
    varlist = std::move(expanded);
    if (alphabetic) std::sort(varlist.begin(), varlist.end());
  }
  for (const auto& v : varlist) {
    if (!hasName(v)) return engineErrorBlock("order: variable '" + v + "' not found in dataset");
  }

  bool first = false, last = false;
  bool beforeIsBool = false, afterIsBool = false;
  std::optional<std::string> beforeVar, afterVar;
  for (const auto& [k, v] : opts) {
    if (k == "first" && v.isBool && v.boolValue) first = true;
    if (k == "last" && v.isBool && v.boolValue) last = true;
    if (k == "before") {
      if (v.isBool) beforeIsBool = true;
      else beforeVar = v.strValue;
    }
    if (k == "after") {
      if (v.isBool) afterIsBool = true;
      else afterVar = v.strValue;
    }
  }
  if (beforeIsBool) {
    return engineErrorBlock("order, before(): requires a variable name, e.g. before(weight)");
  }
  if (afterIsBool) {
    return engineErrorBlock("order, after(): requires a variable name, e.g. after(make)");
  }
  if (beforeVar.has_value()) *beforeVar = trim(*beforeVar);
  if (afterVar.has_value()) *afterVar = trim(*afterVar);
  if (beforeVar.has_value() && beforeVar->empty()) beforeVar.reset();
  if (afterVar.has_value() && afterVar->empty()) afterVar.reset();

  int nModes = (first ? 1 : 0) + (last ? 1 : 0) + (beforeVar.has_value() ? 1 : 0) +
              (afterVar.has_value() ? 1 : 0);
  if (nModes > 1) {
    return engineErrorBlock("order: specify at most one of first, last, before(), after()");
  }
  if (nModes == 0) first = true;

  std::string mode = "first";
  std::string anchor;
  if (beforeVar.has_value()) {
    if (!hasName(*beforeVar)) {
      return engineErrorBlock("order, before(): variable '" + *beforeVar + "' not found");
    }
    if (std::find(varlist.begin(), varlist.end(), *beforeVar) != varlist.end()) {
      return engineErrorBlock("order, before(" + *beforeVar +
                              "): anchor variable must not be in the reorder list (it is "
                              "ambiguous where '" +
                              *beforeVar + "' would land)");
    }
    mode = "before";
    anchor = *beforeVar;
  } else if (afterVar.has_value()) {
    if (!hasName(*afterVar)) {
      return engineErrorBlock("order, after(): variable '" + *afterVar + "' not found");
    }
    if (std::find(varlist.begin(), varlist.end(), *afterVar) != varlist.end()) {
      return engineErrorBlock("order, after(" + *afterVar +
                              "): anchor variable must not be in the reorder list (it is "
                              "ambiguous where '" +
                              *afterVar + "' would land)");
    }
    mode = "after";
    anchor = *afterVar;
  } else if (last) {
    mode = "last";
  }

  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::string outPath = nextPath(session.workdir(), "order");
  // fix/verb-overhead-sweep: *session.currentHbin() (not a ds->path() read)
  // — equivalent whenever data is loaded, and lets the fast (card) branch
  // above skip building a Datastore entirely, same choice runSort's own fix
  // and this file's runGsort fix already made for their --data args.
  std::vector<std::string> args = {"order", "--data", *session.currentHbin(), "--vars", joined};
  if (mode == "first") {
    args.push_back("--first");
  } else if (mode == "last") {
    args.push_back("--last");
  } else if (mode == "before") {
    args.push_back("--before");
    args.push_back(anchor);
  } else if (mode == "after") {
    args.push_back("--after");
    args.push_back(anchor);
  }
  args.push_back("--out");
  args.push_back(outPath);
  {
    // MAXPERF Stage 2 (item 3): pure column reorder — single terminal mutation,
    // --out never read back as a file. Opt into the resident frame plane.
    SessionState::ResidentEligible resident(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  session.load(outPath);
  return mutationResult(session, "order");
}
}
