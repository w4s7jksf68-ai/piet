// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
//
// M4a item 3: stored-estimates surface — eststo/esttab/coefplot. C++ port
// of piet/backend/piet/eststo.py + esttab.py (make_snapshot/render_esttab/
// parse_esttab_opts) + coefplot.py (run_coefplot), and dispatcher.py's
// "eststo"/"eststo_dir"/"eststo_manage"/"esttab"/"coefplot" native-handler
// arms. See piet/native/include/piet/handlers.h for the per-handler
// mapping.
#include "piet/handlers/data_values.h"
#include "piet/handlers/estimates_tables.h"
#include "piet/handlers/observations.h"
#include "piet/handlers/variables.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <memory>
#include <cstdio>
#include <fstream>
#include <regex>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>

#include "core/ident.h"       // the shared Unicode name rule
#include "piet/datastore.h"   // kEsampleColumn
#include "piet/dispatcher.h"  // `estimates esample:` marks its rows through `generate`
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"  // eScalarKey(): the ONE e()-name table
#include "estimation/estimation_internal.h"  // e(sample) T2's mask writer (esampleRewriteHeadMask)
#include "io/win_compat.h"     // cecon::native_exists / to_native_path
#include "stored_estimates_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace stored_estimates_detail;

namespace {

// ---------------------------------------------------------------------------
// e(sample) T3 (internal note esample/SPEC.md §6, corrected toward
// the StataBE 19.5 records of tests/eval/golden/aer_w2/w2_esample.log section G
// and toward Stata's own estimates.ado, which the records follow):
//  * `estimates store NAME` (and `eststo NAME`) drops any `_est_NAME`, then
//    writes e(sample) into a new byte variable `_est_NAME` labelled
//    "esample() from estimates store" at the end of the data (estimates.ado
//    Store: `capture drop _est_NAME`, `_est hold NAME, ... varname(_est_NAME)`;
//    G0b `0|28|`, G0c, G0d r(k) 19, G0e `... zb27 _est_A|`, G9 `0|0|28|`).
//  * `estimates restore NAME` of results that are NOT the active stored ones
//    reads e(sample) back from `_est_NAME` -- nonzero and nonmissing is in --
//    and leaves `_est_NAME` as the LAST variable (est_unhold drops it,
//    est_hold re-creates it; G2 28, G12 `28|31|`, I1). When NAME already IS the
//    active results Restore does nothing at all (`if ! inlist("NAME", ".",
//    e(_estimates_name))`), so the live mask keeps following the rows: G3
//    after `sort` 28, G4 after `drop` 22, G4b after `expand` 28 (the copies
//    are outside the sample, as for any fit, w2_esample.log:1076 E6).
//  * `estimates drop`/`clear` and `eststo clear` drop `_est_NAME` (G8b, G9b
//    r(111)) and leave the active e(sample) alone (G8 28, G10 `0|28|28|`).
// The DATA side is a visible user variable, so it follows every row verb the
// way Stata's does. Layers: none of these touch an expression evaluator; the
// mask they write is read through the row rewrite (dispatcher.cpp
// rewriteEsampleToken) and the macro layer's obs-1 hook, unchanged.
std::string estVarName(const std::string& name) { return "_est_" + name; }

bool dataHasVar(SessionState& session, const std::string& var) {
  if (!session.hasData()) return false;
  try {
    Datastore* ds = session.datastore();
    return ds != nullptr && ds->has(var);
  } catch (const std::exception&) {
    return false;
  }
}

// Writes `_est_NAME` = e(sample) (byte, 1/0). True when it was written. The
// old variable goes first in every case, as in Stata's estimates store: a stale
// `_est_NAME` from an earlier store of the same name would otherwise answer for
// these results. No block: Stata's store prints nothing about the variable.
bool esampleStoreVar(const std::string& name, SessionState& session) {
  if (!session.hasData()) return false;
  const std::string var = estVarName(name);
  if (dataHasVar(session, var)) {
    const ojson d = runDrop({var}, session);
    if (d.is_object() && d.value("type", std::string()) == "error") return false;
  }
  const SessionState::EsampleReading r = session.esampleReading();
  std::string expr;
  switch (r.kind) {
    case SessionState::EsampleReading::Kind::kColumn:
      expr = std::string("(") + kEsampleColumn + "==" + std::to_string(r.serial) + ")";
      break;
    case SessionState::EsampleReading::Kind::kZero:
      expr = "0";
      break;
    case SessionState::EsampleReading::Kind::kRefused:
      return false;  // PIET does not have these results' sample: no variable
  }
  const ojson g = runGen(var + " = " + expr, std::nullopt, session, "byte");
  if (!g.is_object() || g.value("type", std::string()) == "error") return false;
  // describe: `_est_A  byte  %8.0g  esample() from estimates store`
  // (w2_esample.log:1375 G0c).
  session.varMeta[var].stataType = "byte";
  session.varMeta[var].format = "%8.0g";
  session.setVarLabel(var, "esample() from estimates store");
  return true;
}

// The `_est_NAME` variables PIET wrote for these stored names, dropped
// together (estimates.ado Drop: `_est drop NAME` takes the held variable).
void esampleDropStoreVars(const std::vector<std::string>& names, SessionState& session) {
  std::vector<std::string> doomed;
  for (const auto& n : names) {
    const auto it = session.estimatesModels.find(n);
    if (it == session.estimatesModels.end() || !it->second || !it->second->esampleVar) continue;
    if (dataHasVar(session, estVarName(n))) doomed.push_back(estVarName(n));
  }
  if (!doomed.empty()) (void)runDrop(doomed, session);
}

}  // namespace

void clearStoredEstimates(SessionState& session) {
  esampleDropStoreVars(session.estimatesNames(), session);
  session.estimatesClear();
}

// ===========================================================================
// eststo / est store / estimates store

ojson runEststo(const std::optional<std::string>& name,
                const std::optional<std::string>& title, SessionState& session,
                const std::string& autoPrefix, bool viaEststo) {
  const auto& lmOpt = session.lastModel();
  bool hasEstBlock = session.lastEstBlock.has_value();
  // FU-1176: estimates.ado Store refuses only an EMPTY e(cmd) (r(301), the
  // G5b case below). An `ereturn post` e() whose eclass program set e(cmd)
  // (w2an_coefplot_family.do cf_lpost: `ereturn post cfb cfV` + `ereturn local
  // cmd "lpost"`, then `estimates store lp`, rc 0 in Stata: cf_at_eq) is
  // stored as that posting.
  const std::string* postedCmd = session.eval.eStrings.find("cmd");
  // BUG-1045: eststo first fills an empty e(cmd) with "." whenever e() holds
  // anything (eststo's placeholder `eret local cmd "."`), then stores it; only
  // `estimates store` itself keeps the empty-e(cmd) refusal below.
  if (viaEststo && !hasEstBlock && !lmOpt.has_value() && session.ePostedLive &&
      (postedCmd == nullptr || postedCmd->empty()) &&
      (!session.eval.eScalars.items().empty() || !session.eval.eStrings.items().empty() ||
       !session.eval.eMatrices.empty())) {
    session.eval.setEString("cmd", ".");
    postedCmd = session.eval.eStrings.find("cmd");
  }
  const bool postedWithCmd = !hasEstBlock && !lmOpt.has_value() && session.ePostedLive &&
                             postedCmd != nullptr && !postedCmd->empty();
  if (postedWithCmd) {
    ojson snapshot;
    if (session.eval.lastModelResult.has_value() && session.eval.lastModelResult->is_object())
      snapshot = makeSnapshot(*session.eval.lastModelResult);
    if (!snapshot.contains("coeftable")) snapshot["coeftable"] = ojson::array();
    const std::string* dv = session.eval.eStrings.find("depvar");
    snapshot["depvar"] = dv != nullptr ? *dv : std::string();
    snapshot["estimator"] = *postedCmd;
    const double* nE = session.eval.eScalars.find("N");
    snapshot["n_obs"] = nE != nullptr ? ojson(*nE) : ojson(nullptr);
    snapshot["escalars"] = ojson::object();
    snapshot["added"] = emptyAddedBag();
    if (title.has_value()) snapshot["title"] = *title;
    std::string storedName = session.eststo(name, snapshot, autoPrefix, viaEststo);
    auto sm = std::make_shared<SessionState::StoredModel>();
    sm->lastModelResult = session.eval.lastModelResult;
    sm->eScalars = session.eval.eScalars;
    sm->eStrings = session.eval.eStrings;
    sm->eMatrices = session.eval.eMatrices;
    sm->eOnFitNames = session.eOnFitNames;
    sm->postedE = true;
    sm->postedEsampleSerial = session.ePostedEsampleSerial;
    if (dataHasVar(session, estVarName(storedName)))
      (void)runDrop({estVarName(storedName)}, session);  // never another results' sample
    session.estimatesModelSet(storedName, std::move(sm));
    session.activeEstimatesName = storedName;
    ojson out;
    out["type"] = "display";
    out["text"] = "(stored as " + storedName + ")";
    return out;
  }
  if (!hasEstBlock && lmOpt.has_value() && !lmOpt->ePosted) {
    // e15d_eststo2 (w2g_ebmat.log:2497-2504): an e() that no `ereturn post`
    // produced (rddensity, LastModel::ePosted) is not storable. estimates store
    // exits 301 on its empty e(cmd); eststo fills
    // e(cmd) "." first, and the hold step then refuses the
    // unposted e() with r(301) "last estimates not found".
    return ojson(eval::errorBlock(
        "engine_error",
        viaEststo ? "last estimates not found"
                  : "last estimation results not found, nothing to store",
        std::nullopt, std::nullopt, std::nullopt, std::string("r(301)")));
  }
  if (!hasEstBlock && !lmOpt.has_value()) {
    // FU-580: estimates.ado Store exits 301 when e(cmd) is empty -- after
    // `ereturn post` too (w2_esample.log:1507 G5b `301|...`). The rc rides the
    // errors.h builder; a bare engine error surfaced as rc 1.
    return ojson(eval::errorBlock(
        "engine_error",
        viaEststo ? "eststo: no estimation result in memory. Run a regression first."
                  : "last estimation results not found, nothing to store",
        std::nullopt, std::nullopt, std::nullopt, std::string("r(301)")));
  }

  ojson snapshot;
  if (hasEstBlock) {
    snapshot = makeSnapshot(*session.lastEstBlock);
    // mlogit's block names no depvar (its outcomes each carry one); e(depvar)
    // is the fitted model's, which esttab heads the column with (ek_e34 `k3`).
    // The block (and so e(depvar)) is empty for mlogit; the fit's own --y is
    // the dependent variable.
    if (snapshot.value("depvar", std::string()).empty() && snapshot.contains("outcomes") &&
        lmOpt.has_value()) {
      snapshot["depvar"] = !lmOpt->depvar.empty() ? lmOpt->depvar : lmOpt->y;
    }
  } else if (lmOpt.has_value() && lmOpt->result.has_value()) {
    snapshot = makeSnapshot(*lmOpt->result);
  } else {
    // _coeftable_from_last_model fallback: LastModel caches no b/se values
    // directly, so this is always an empty coeftable (mirrors the oracle's
    // own documented "rare no-cache case" guard).
    snapshot["coeftable"] = ojson::array();
    snapshot["n_obs"] = nullptr;
    snapshot["r2"] = nullptr;
    snapshot["depvar"] = lmOpt.has_value() ? lmOpt->depvar : std::string();
    snapshot["estimator"] = lmOpt.has_value() ? lmOpt->cobraCmd : std::string();
    snapshot["fe"] = nullptr;
  }

  // W2AK BASELEV: the factor base levels, read while this fit's data and
  // e(sample) are still the live ones (tables/table_model_context.cpp factorBaseLevels).
  if (snapshot.contains("coeftable")) {
    const ojson bl = factorBaseLevels(snapshot["coeftable"], session);
    if (!bl.empty()) snapshot["base_levels"] = bl;
  }

  // AER-SCOPE1 bundle E: e() scalars + whatever `estadd` attached to the ACTIVE
  // estimate. Stata's order is estadd-then-eststo (the corpus writes it that way
  // 5,725 times), so the bag is picked up HERE rather than by a later `estadd`
  // reaching into the store.
  snapshot["escalars"] = hasEstBlock ? escalarsFrom(*session.lastEstBlock) : ojson::object();

  // The bag only travels if it still belongs to THIS estimate. `lastEstBlock` is
  // assigned directly by five different TUs, so there is no single place to
  // clear the bag from when a new regression replaces e() -- staleness is
  // therefore decided at READ time, here, against the shared fingerprint.
  // Discarding silently would be the wrong half of honest: an `estadd`ed row
  // would just be missing from the table, rc 0.
  std::string note;
  const std::string fp =
      hasEstBlock ? SessionState::estAddFingerprint(*session.lastEstBlock) : std::string();
  bool bagHasContent = false;
  for (const char* section : {"scalars", "locals", "matrices"}) {
    if (session.estAdded.contains(section) && !session.estAdded[section].empty()) {
      bagHasContent = true;
      break;
    }
  }
  if (bagHasContent && !session.estAddedFp.empty() && session.estAddedFp == fp) {
    snapshot["added"] = session.estAdded;
  } else {
    snapshot["added"] = emptyAddedBag();
    if (bagHasContent) {
      note = "\n(estadd: previous additions discarded -- a new estimation replaced e())";
      session.estAddedClear();
    }
  }

  // B4-W3b: `estimates store NAME, title("...")`. Stored ONLY when the option
  // was written — an absent key and an empty string are different things here,
  // because the corpus's `est store r`i', title("`lab'")` can legitimately pass
  // an empty macro and Stata keeps that as an empty title, not as no title.
  if (title.has_value()) snapshot["title"] = *title;

  std::string storedName = session.eststo(name, snapshot, autoPrefix, viaEststo);

  // W2 esttabnames (BUG-553): stash the MODEL beside the table, so `estimates
  // restore NAME` can make it the active one again -- the LastModel, the e()
  // block, and the macro layer's e() stores as they stand NOW (estadd's
  // scalars/locals/matrices live there, not in LastModel::result). The e() it
  // came from now carries this name (estimates.ado Store: `ereturn hidden
  // local _estimates_name`), which is what a later `esttab .` expands to.
  {
    auto sm = std::make_shared<SessionState::StoredModel>();
    // The LastModel is stashed only when it is the model BEHIND this e()
    // block. `estpost` (tables/table_estpost.cpp) posts a block and leaves the
    // previous fit's LastModel in place, and so does `csdid`; pairing the two
    // would let a restore publish the old fit's e(cmd)/_b[] under the new
    // results' name. Both the depvar and, where the fit's result records one,
    // e(N) must agree. Without a model the restore refuses by name instead
    // (estimatesRestore).
    bool modelMatchesBlock = session.lastModel().has_value();
    if (modelMatchesBlock && hasEstBlock) {
      const ojson& blk = *session.lastEstBlock;
      const auto& lm = *session.lastModel();
      modelMatchesBlock = blk.value("depvar", std::string()) == lm.depvar;
      if (modelMatchesBlock && lm.result.has_value() && lm.result->is_object() &&
          lm.result->contains("n_obs") && (*lm.result)["n_obs"].is_number() &&
          blk.contains("n_obs") && blk["n_obs"].is_number()) {
        modelMatchesBlock = (*lm.result)["n_obs"].get<double>() == blk["n_obs"].get<double>();
      }
    }
    if (modelMatchesBlock) {
      sm->lastModel = session.lastModel();
      sm->lastModelResult = session.eval.lastModelResult;
      sm->eNameContract = session.eNameContract;
    }
    // eb-order T-P3: the e() stores belong to the live e() whether or not a
    // fitted model stands behind its block -- an estpost posting has none, and
    // `estimates restore` installs such a store as the posting it was
    // (SessionState::estimatesRestore), its e(N)/e(cmd)/e(mean) included.
    if (modelMatchesBlock || hasEstBlock) {
      sm->eScalars = session.eval.eScalars;
      sm->eStrings = session.eval.eStrings;
      sm->eMatrices = session.eval.eMatrices;
    }
    sm->estBlock = session.lastEstBlock;
    // A posted e() keeps its `ereturn post ..., esample()` mask serial.
    if (!session.lastModel().has_value() && session.ePostedLive)
      sm->postedEsampleSerial = session.ePostedEsampleSerial;
    // e(sample) T3: `_est_NAME`, for results whose model PIET holds (a posting
    // with no model behind it has no sample PIET could write).
    if (modelMatchesBlock) {
      sm->esampleVar = esampleStoreVar(storedName, session);
    } else if (dataHasVar(session, estVarName(storedName))) {
      (void)runDrop({estVarName(storedName)}, session);  // never another results' sample
    }
    session.estimatesModelSet(storedName, std::move(sm));
    session.activeEstimatesName = storedName;
  }
  ojson out;
  out["type"] = "display";
  // One block, never an array: the `eststo` arm pushes this result directly, and
  // an array reaching the block stream is read with value("type") downstream and
  // throws type_error.306 (the class that aborted the selftest binary in bundle
  // D). The note is folded into the text for exactly that reason.
  out["text"] = "(stored as " + storedName + ")" + note;
  return out;
}

ojson runEststoDirNames(const std::vector<std::string>& namelist, SessionState& session);
ojson runEststoDir(SessionState& session) { return runEststoDirNames({}, session); }

// W2 esttabnames (BUG-552): `estimates dir A_*` lists only what the namelist
// expands to (no namelist = every stored name). Declared here and
// in dispatcher.cpp's forward block, not in handlers.h (outside this lane).
ojson runEststoDirNames(const std::vector<std::string>& namelist, SessionState& session) {
  std::vector<std::string> names = session.estimatesNames();
  if (!namelist.empty()) {
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(namelist);
    if (ex.rc != 0) return estNamelistError(ex);
    names.clear();
    for (const auto& n : ex.names) {
      if (n != ".") names.push_back(n);  // unstored active results have no dir row
    }
  }
  if (names.empty()) {
    ojson out;
    out["type"] = "display";
    out["text"] = "(no stored estimates)";
    return out;
  }
  std::string text = "Stored estimation results:";
  for (const auto& n : names) {
    const ojson* snap = session.estimatesGet(n);
    std::string depvar = (snap && snap->contains("depvar") && (*snap)["depvar"].is_string())
                             ? (*snap)["depvar"].get<std::string>()
                             : std::string();
    text += "\n  " + n + "  (" + depvar + ")";
    // B4-W3b: the ONE place a stored `title()` is observable. Appended only
    // when the option was actually written, so every pre-B4 store — and every
    // freeze fixture captured off one — prints the byte-identical line it did
    // before (`title` answered r(198) until this delta, so no captured store
    // can carry the key).
    if (snap && snap->contains("title") && (*snap)["title"].is_string()) {
      text += "  \"" + (*snap)["title"].get<std::string>() + "\"";
    }
  }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

ojson runEststoManage(const std::string& sub, const std::optional<std::string>& name,
                      SessionState& session) {
  if (sub == "drop") {
    if (!name.has_value()) return engineErrorBlockEt("estimates drop: requires a model name");
    // BUG-149 (P2-PERF ARC-P). `estimates drop _all` is a documented Stata form
    // and it was being looked up as a literal store NAME, so it always missed.
    // `_all` means every stored estimate, which is what `clear` already does.
    if (*name == "_all") {
      esampleDropStoreVars(session.estimatesNames(), session);  // e(sample) T3
      session.estimatesClear();
      ojson out;
      out["type"] = "display";
      out["text"] = "(dropped all stored estimates)";
      return out;
    }
    // W2 esttabnames (BUG-552): the operand is a NAMELIST:
    // `estimates drop A_*` / `estimates drop m1 m2` drop every
    // match, `.` is skipped, and a token matching nothing is r(111)
    // "estimation result NAME not found" -- also for a plain misspelt name,
    // which BUG-149 had answered at rc 1 with a message of its own. Nothing is
    // dropped when any token misses (the expansion fails first).
    std::vector<std::string> toks;
    std::istringstream tss(*name);
    for (std::string t; tss >> t;) toks.push_back(t);
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(toks);
    if (ex.rc != 0) return estNamelistError(ex);
    esampleDropStoreVars(ex.names, session);  // e(sample) T3: G8b r(111)
    std::string dropped;
    for (const auto& n : ex.names) {
      if (n == "." || !session.estimatesDrop(n)) continue;
      dropped += (dropped.empty() ? "" : " ") + n;
    }
    ojson out;
    out["type"] = "display";
    out["text"] = "(dropped " + dropped + ")";
    return out;
  }
  if (sub == "clear") {
    // e(sample) T3: `_est_*` go with the stores (G9b r(111)); the active
    // e(sample) stays (w2_esample.log:1621 G10 `0|28|28|`).
    clearStoredEstimates(session);
    ojson out;
    out["type"] = "display";
    out["text"] = "(estimates cleared)";
    return out;
  }
  return engineErrorBlockEt("eststo: unknown subcommand '" + sub + "'");
}

// ===========================================================================
// e(sample) T3: estimates restore NAME (the namelist is expanded by the
// dispatcher's restore arm, which owns the namelist's r(111)/r(198)). See
// the T3 note above esampleStoreVar for the Stata rules this follows.
ojson runEstimatesRestore(const std::string& name, SessionState& session) {
  // estimates.ado Restore: NAME already the active results -> nothing happens,
  // so the live e(sample) keeps following the rows (G3/G4/G4b). Decided BEFORE
  // the re-activation below, which renames the active results.
  const bool wasActive = session.activeStoredName() == name;
  const std::optional<LastModel> live = session.lastModel();
  if (!session.estimatesRestore(name)) {
    return ojson(eval::errorBlock(
        "unsupported_option",
        "estimates restore: " + name + " was stored from results this version of PIET "
        "cannot make active again (no fitted model and no estimation table behind "
        "them); nothing was changed"));
  }
  ojson note;
  note["type"] = "display";
  note["text"] = "(results " + name + " are active now)";
  const auto it = session.estimatesModels.find(name);
  if (it == session.estimatesModels.end() || !it->second || !it->second->lastModel.has_value() ||
      !session.lastModel().has_value())
    return note;
  const LastModel lm = *session.lastModel();  // the restored model (kUnavailable)
  if (wasActive && live.has_value()) {
    session.setLastModelEsample(live->esample, live->esampleSerial, live->esampleInstallPath,
                                lm.dataPath);
    return note;
  }
  if (!it->second->esampleVar) return note;  // stored without a sample: the named r(198)
  const std::string var = estVarName(name);
  if (!dataHasVar(session, var)) {
    session.setLastModelEsample(LastModel::EsampleState::kStale, 0, std::string(), lm.dataPath);
    return note;
  }
  const bool changed = session.datasetChanged;
  // est_unhold drops `_est_NAME`, est_hold writes it anew: it ends up last.
  {
    eval::OptValue on;
    on.isBool = true;
    on.boolValue = true;
    (void)runOrder({var}, {{"last", on}}, session);
  }
  // The restored rows: `_est_NAME` nonzero and nonmissing (est_hold: `replace
  // touse = 0 if missing(touse)`, then `ereturn repost, esample(touse)`). They
  // are written under the model's own serial, so a frame that still carries
  // this model's original marker (a `preserve` taken before the store) reads
  // the same results' sample.
  std::vector<double> cells;
  unsigned long long serial = lm.esampleSerial;
  bool read = false;
  try {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(var) && ds->col(var).dtype == cecon::DType::F64) {
      if (serial == 0) serial = ++session.esampleColumnInstalls;
      const cecon::Column& c = ds->col(var);
      const double* v = c.f64();
      cells.resize(static_cast<size_t>(ds->nRows()));
      for (int64_t i = 0; i < ds->nRows(); ++i)
        cells[static_cast<size_t>(i)] =
            (!std::isnan(v[i]) && v[i] != 0.0) ? static_cast<double>(serial) : 0.0;
      read = true;
    }
  } catch (const std::exception&) {
    read = false;
  }
  if (!read || !esampleRewriteHeadMask(session, cells)) {
    session.datasetChanged = changed;
    return note;  // kUnavailable: the named r(198), never another model's rows
  }
  session.datasetChanged = changed;
  session.setLastModelEsample(LastModel::EsampleState::kColumn, serial, lm.esampleInstallPath,
                              lm.dataPath);
  return note;
}

// e(sample) T3: `estimates esample` (report) and `estimates esample: [varlist]
// [if] [in] [, replace]` (set) -- Stata's estimates_sample.ado, read for this
// port. The set form marks the rows that satisfy if/in with every variable of
// varlist nonmissing (marksample), refuses r(322) "no; e(sample) already set"
// when the active results carry one and `replace` is absent, posts the rows as
// e(sample) and sets e(_estimates_sample) "user" (w2_esample.log:1542 G7
// `0|28|` after `estimates use`, whose results carry none). Weights,
// stringvars() and zeroweight are not implemented: refused by name.
ojson runEstimatesEsample(bool colon, const std::vector<std::string>& varlistIn,
                          const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& inRange, bool hasWeight, bool replace,
                          SessionState& session) {
  const SessionState::EsampleReading r = session.esampleReading();
  // BUG-1211: an `ereturn post` result takes an e(sample) too (estimates_sample.ado
  // reposts it), on the posting's own marker serial.
  const bool posting = !session.lastModel().has_value() && session.ePostedLive;
  const bool userSet = (session.lastModel().has_value() || posting) &&
                       session.eval.eStrings.get("_estimates_sample", "") == "user";
  const bool hasSample = (session.lastModel().has_value() || posting) &&
                         r.kind != SessionState::EsampleReading::Kind::kZero;
  // The varlist as Stata's marksample reads it: wildcards, ranges and unique
  // abbreviations (`x*`, `x1-x2`, `longn`; BUG-1211).
  std::vector<std::string> varlist = varlistIn;
  if (colon) {
    if (auto err = resolveByKeyNames(session, varlist)) return *err;
  }
  if (!colon) {
    if (!varlist.empty() || ifExpr.has_value() || inRange.has_value() || hasWeight)
      return ojson(eval::engineError("estimates esample: `estimates esample` takes nothing "
                                     "without the colon (`estimates esample: [varlist] [if] [in]`)",
                                     "syntax_error"));
    std::string who, text;
    if (userSet && hasSample) {
      who = "user";
      text = "e(sample) set by user";
    } else if (hasSample) {
      who = "cmd";
      text = "e(sample) set by estimation command";
    } else {
      who = "zero'd";
      text = "e(sample) not set (0 assumed)";
    }
    session.eval.rValues = eval::OrderedFloatDict();
    session.eval.rStrings.clear();
    session.eval.rStrings.set("who", who);
    ojson out;
    out["type"] = "display";
    out["text"] = text;
    return out;
  }
  if (hasWeight)
    return ojson(eval::errorBlock(
        "unsupported_option",
        "estimates esample: weights are not supported in this version of PIET; nothing was set.",
        std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  if (!session.lastModel().has_value() && !posting)
    return ojson(eval::errorBlock("engine_error", "last estimates not found", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(301)")));
  if (!replace && hasSample)
    return ojson(eval::errorBlock("engine_error", "no; e(sample) already set", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(322)")));
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  for (const auto& v : varlist) {
    if (!ds->has(v) || isHiddenColumn(v))
      return ojson(eval::errorBlock("engine_error", "variable " + v + " not found", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(111)")));
    if (ds->col(v).dtype != cecon::DType::F64)
      return ojson(eval::errorBlock(
          "unsupported_option",
          "estimates esample: string variable " + v + " in the varlist (stringvars()) is not "
          "supported in this version of PIET; nothing was set.",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }
  const bool changed = session.datasetChanged;
  // The if/in rows, marked by `generate` itself so the condition goes through
  // the one row rewrite every `if` takes (an `if e(sample)` reads the CURRENT
  // mask). A missing condition is true, as in Stata: `1 if .` is 1.
  // A scratch name no variable holds (BUG-1211: a user's `__piet_esw__` was dropped).
  std::string tmp = "__piet_esw__";
  for (int k = 1; ds->has(tmp); ++k) tmp = "__piet_esw" + std::to_string(k) + "__";
  std::string line = "quietly generate byte " + tmp + " = 1";
  if (ifExpr.has_value() && !ifExpr->empty()) line += " if " + *ifExpr;
  if (inRange.has_value() && !inRange->empty()) line += " in " + *inRange;
  const ojson gen = piet::dispatch(line, session);
  if (gen.is_array()) {
    for (const auto& b : gen) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        if (dataHasVar(session, tmp)) (void)runDrop({tmp}, session);
        session.datasetChanged = changed;
        return b;
      }
    }
  }
  std::vector<double> cells;
  const unsigned long long serial = session.esampleColumnInstalls + 1;
  bool marked = false;
  try {
    ds = session.datastore();
    if (ds != nullptr && ds->has(tmp)) {
      const int64_t n = ds->nRows();
      cells.assign(static_cast<size_t>(n), 0.0);
      const double* m = ds->col(tmp).f64();
      std::vector<const double*> cols;
      for (const auto& v : varlist) cols.push_back(ds->col(v).f64());
      for (int64_t i = 0; i < n; ++i) {
        bool in = m[i] == 1.0;
        for (const double* c : cols) in = in && !std::isnan(c[i]);
        if (in) cells[static_cast<size_t>(i)] = static_cast<double>(serial);
      }
      marked = true;
    }
  } catch (const std::exception&) {
    marked = false;
  }
  if (dataHasVar(session, tmp)) (void)runDrop({tmp}, session);
  if (!marked || !esampleRewriteHeadMask(session, cells)) {
    session.datasetChanged = changed;
    return engineErrorBlockEt(
        "estimates esample: the estimation-sample marker could not be written; nothing was set.");
  }
  session.esampleColumnInstalls = serial;
  session.datasetChanged = changed;
  if (posting) {
    session.ePostedEsampleSerial = serial;  // esampleReading's posting branch reads it
  } else {
    const LastModel& lm = *session.lastModel();
    session.setLastModelEsample(LastModel::EsampleState::kColumn, serial, lm.esampleInstallPath,
                                lm.dataPath);
  }
  // `ereturn repost` takes e(_estimates_name) away; estimates_sample.ado then
  // posts e(_estimates_sample) "user", which the next fit drops.
  session.activeEstimatesName.clear();
  session.eval.setEString("_estimates_sample", "user");
  session.noteEOnFit("_estimates_sample");
  ojson out;
  out["type"] = "echo";
  out["text"] = "";
  return out;
}

}  // namespace piet::handlers
