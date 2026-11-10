// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: variable card after a mutation, seeding side (n_missing carry, shadow-card seeding, worker varlist fetch).
#include "piet/session_state.h"
#ifndef _WIN32
#include <execinfo.h>
#include <dlfcn.h>
#endif
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>   // C4-L1 window trace (stderr fprintf, env-gated)
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>  // std::greater<> (BUG-381 census sort)
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "piet/bcal.h"  // bcal::clearCache (resetProcessCaches)
#include "piet/cclass.h"  // BUG-1006: resolveCValue (macroCValue)
#include "piet/counters.h"
#include "piet/eval/errors.h"  // eval::coarseRc (the r(N) of a logged error)
#include "piet/eval/macros.h"  // eScalarKey (eNameOrderFor)
#include "piet/display_hardening.h"  // BUG-718: readVarObservation (macroVarObs)
#include "piet/scratch_seq.h"  // BUG-389: scratchMintSeq (sweep throttle)
#include "piet/storage_type.h"  // stataDefaultFormat (displayFormat)
#include "session_resident_internal.h"

namespace piet {
using namespace session_detail;

void SessionState::seedNMissForPureAdd(Datastore* ds, const std::string& action,
                                       const std::vector<std::string>& newVars,
                                       const std::vector<std::string>& droppedVars,
                                       const std::vector<std::string>& rewrittenVars) {
  if (std::getenv("PIET_NO_NMISS_CACHE")) return;  // C2-F13a escape hatch + A/B correctness gate
  if (ds == nullptr) return;

  // Start from the full cached table; each branch below carries the columns it
  // can PROVE byte-identical and drops/rescans the rest. Anything not matched
  // hits the final return and rescans fully — the conservative default.
  std::unordered_map<std::string, int64_t> seed = nMissCache_;

  // (1) C2-F13a pure-ADD {gen,egen,duplicates_tag,recode-gen}: adds newVars and
  //     leaves every existing column byte-identical. Carry all; new cols scan
  //     fresh. (Same condition as the pre-F9 code — behaviour unchanged here.)
  //     C1-CARD-REBUILD-REDUCE A11 siblings: encode/destring-gen/tostring-gen
  //     (mutationResult path) and winsor2/mkspline (hand-rebuild path) are all
  //     the SAME shape — the engine (encode/setcol/eval --winsor/--mkspline)
  //     appends or in-place-patches only the reported column(s) and byte-
  //     preserves every other column AND its dictionary. winsor2 passes the
  //     rewritten source(s) IN newVars (its emitted new_vars already lists them,
  //     replace or suffix), so the `erase(nv)` below rescans exactly the changed
  //     column and carries the rest. Proven byte-identical against a forced full
  //     scan (PIET_NO_NMISS_CACHE=1) per verb — the same A/B gate string
  //     gen/replace ships under.
  static const std::set<std::string> kPureAdd = {
      "gen", "egen", "duplicates_tag", "recode",
      "encode", "destring", "tostring", "winsor2", "mkspline"};
  if (!newVars.empty() && droppedVars.empty() && kPureAdd.count(action)) {
    for (const auto& nv : newVars) seed.erase(nv);  // new/changed cols scanned fresh
    ds->seedNMissing(std::move(seed));
    return;
  }

  // (2) F9 replace-style (empty newVars): rewrites rewrittenVars IN PLACE and
  //     touches nothing else (replace never adds/removes rows). Carry all EXCEPT
  //     the rewritten target(s). DORMANT until the call site passes rewrittenVars
  //     — with it empty we cannot tell WHICH column changed, so we fall through
  //     to a full rescan and never seed a stale count onto the rewritten column
  //     (the exact pre-F9 `replace` behaviour). Actions: string gen/replace
  //     (strmutate, action "gen") plus the C1 A11 in-place siblings destring
  //     replace / tostring replace (single-column `cobra setcol --var TARGET`,
  //     every other column byte-identical) — all row-invariant, single-column
  //     rewrites that pass rewrittenVars={target}.
  static const std::set<std::string> kRewriteInPlace = {"gen", "destring", "tostring"};
  if (kRewriteInPlace.count(action) && newVars.empty() && !rewrittenVars.empty()) {
    for (const auto& rv : rewrittenVars) seed.erase(rv);  // rewritten col scans fresh
    ds->seedNMissing(std::move(seed));
    return;
  }

  // (3) F9 variable drop/keep (action "drop"/"keep": a COLUMN subset, rows
  //     untouched). Every SURVIVING column is byte-identical — carry its count
  //     and just drop the removed keys. The observation filters `drop if`/
  //     `keep if` use action "keepif" (row-changing) and correctly miss this
  //     branch. `runKeep` already passes droppedVars = the REMOVED (not kept)
  //     columns, so the same erase-the-removed-keys logic serves both.
  if ((action == "drop" || action == "keep") && newVars.empty()) {
    for (const auto& dv : droppedVars) seed.erase(dv);
    ds->seedNMissing(std::move(seed));
    return;
  }

  // (4) F9 rename (action "rename", newVars={new}, droppedVars={old}): pure key
  //     move — the column data is byte-identical, only its name changed. Move
  //     the cached count old->new and carry the rest. If `old` was not cached
  //     we have nothing trustworthy to carry for `new`, so seed nothing.
  if (action == "rename" && newVars.size() == 1 && droppedVars.size() == 1) {
    auto it = seed.find(droppedVars[0]);
    if (it != seed.end()) {
      int64_t carried = it->second;
      seed.erase(it);
      seed[newVars[0]] = carried;
      ds->seedNMissing(std::move(seed));
    }
    return;
  }

  // (5) L4 O(1)-cards: reorderers sort/gsort (rows permuted, action "sort") and
  //     order (columns permuted, action "order"). A permutation of the rows or
  //     of the columns leaves every surviving column byte-identical — the same
  //     values, hence the same missing-count, level set, and type — and neither
  //     verb adds, drops, or filters rows (runOrder forwards only
  //     --first/--last/--before/--after, never --keepif; runSort/runGsort
  //     forward a bare varlist). Carry the whole cache; nothing is rescanned.
  //     Without this, the card rebuild's varlist() faults in and NaN-scans all N
  //     columns per reorder — the full-frame "card tax" (~1.9 s on the 108K×192
  //     corpus, ~35x the 8-col cost) this branch removes.
  if (action == "sort" || action == "order") {
    ds->seedNMissing(std::move(seed));
    return;
  }

  // Everything else: full rescan (seed nothing). Genuinely row- or column-set-
  // changing verbs (merge, reshape, expand, collapse, keepif, append, …)
  // deliberately land here.
}

void SessionState::seedShadowFromFrameVars(const CobraFrameMeta& meta) {
  const std::vector<CobraFrameVar>& vars = meta.vars;
  // BUG-190. The frame meta the worker sends back describes DATA — name, type,
  // missing count, level count. It carries no display FORMAT and no variable
  // LABEL, because the engine has no such concept: those live entirely on the
  // PIET side. Rebuilding the card from it therefore used to DISCARD both, and
  // the render's fallback chain (SessionState::displayFormat in session_variable_metadata.cpp: varMeta -> the card's format ->
  // a dtype default) cannot tell "the card does not know" from "the card says
  // %9.0g", so it took the default as an answer.
  //
  // The visible consequence was not subtle and was not confined to `use`: on a
  // dataset carrying %12.0g, %10.0g, %td, %9s and %12s columns, a single
  // `gen zz = 1` reported EVERY one of them as %9.0g afterwards — a date
  // variable silently stopped being a date. Every resident verb re-syncs
  // through here, so gen/replace/egen/drop/keep all did it, and the freeze
  // corpus did not catch it because the corpus was captured with the loss
  // already in place: the pins agreed with the wrong value.
  //
  // The card is a CACHE of what PIET already knew a moment ago, so the fix is
  // to carry the PIET-side half across the re-seed by name rather than to
  // invent a wire field for it. A column that is genuinely new to this frame
  // keeps the VarInfo defaults, which is the honest answer for it.
  std::unordered_map<std::string, const VarInfo*> carried;
  std::vector<VarInfo> dsCard;  // storage for the datastore branch's VarInfos
  if (shadowValid_) {
    carried.reserve(shadowCard_.size());
    for (const auto& v : shadowCard_) carried.emplace(v.name, &v);
  } else if (datastore_ != nullptr) {
    // The FIRST resident verb after a `use` re-seeds with no card yet — but the
    // datastore that `use` just rendered its varlist from is still open, and it
    // is where the formats came from in the first place. Read it only when it
    // already exists (never construct one here: the whole point of the resident
    // plane is not to re-open the head).
    dsCard = datastore_->varlist();
    carried.reserve(dsCard.size());
    for (const auto& v : dsCard) carried.emplace(v.name, &v);
  }
  std::vector<VarInfo> next;
  next.reserve(vars.size());
  for (const auto& fv : vars) {
    VarInfo vi;
    vi.name = fv.name;
    vi.dtype = fv.type;
    vi.nMissing = fv.nMissing;
    vi.nLevels = fv.nLevels;
    auto it = carried.find(fv.name);
    if (it != carried.end()) {
      vi.format = it->second->format;
      vi.formatStated = it->second->formatStated;
      vi.label = it->second->label;
    } else {
      // BUG-365: a column with no carried entry is one PIET is meeting for the
      // first time — the using side of a resident joinby/merge. Its only source
      // for a display format is what the engine just reported; before the engine
      // sent it, such a column silently defaulted and a %10s came back %9.0g.
      //
      // ONLY when the engine actually stated something. "" means "the engine said
      // nothing" — a freshly COMPUTED column (gen/egen) has no source format, and
      // overwriting VarInfo's own default with that emptiness blanked the format on
      // 1,357 columns across 45 fixtures the first time this was written. Absent
      // is not a value.
      if (!fv.format.empty()) {
        vi.format = fv.format;
        vi.formatStated = true;
      }
      if (!fv.label.empty()) vi.label = fv.label;
    }
    next.push_back(std::move(vi));
  }
  shadowCard_ = std::move(next);
  shadowValid_ = true;
  // C4-L2A: rows ride with the card ("seedShadowFromFrameVars pair" rule).
  // Gated on meta.valid — a frame block the parser could not vouch for seeds
  // the card exactly as it always has, but rows stay unknown (-1) so
  // residentRowsIfCurrent() honestly declines rather than reporting a
  // default-constructed 0 as the dataset's _N.
  residentRows_ = meta.valid ? meta.rows : -1;
  // fix/sort-validate-lazy: tag with residentToken_, NOT currentHbin_ — every
  // call site sets residentToken_ to the exact path this frame-op just synced
  // to BEFORE calling this function (residentSyncWorkerToHead/residentRestore/
  // loadFromVirtual/runViaCobraSession's resync branch all do), but that path
  // is not always currentHbin_: a post-estimation re-run (test/margins/lincom)
  // can resync the worker onto lastModel.dataPath while currentHbin_ still
  // names the user's own working dataset. Tagging with residentToken_ makes
  // shadowVarlistIfCurrent() correctly report "not current" in that case
  // instead of mislabeling a foreign frame's card as the live dataset's.
  shadowCardHbin_ = residentToken_;
}

// MUTRES-WIRE (2026-08-22). See the header. Asks the worker for the resident
// varlist so a declined card fold can rebuild from metadata instead of opening
// the frame — which, for a virtual head, means writing the whole thing to disk
// first (measured 128.6 MB per occurrence).
//
// Every failure here is a FALLBACK signal, never an error: no worker, a serve
// plane that is off, a §25-only engine that rejects the op, or a response
// without vars all mean "use the datastore path exactly as before".
const CobraFrameMeta* SessionState::fetchWireVarsFromWorker() {
  if (serveDisabled() || cobraSession_ == nullptr) return nullptr;
  if (cobraSession_->serveUnavailable()) return nullptr;
  if (cobraSession_->framesState() == CobraSession::Frames::No) return nullptr;
  wireVars_ = CobraFrameMeta{};
  CobraRunResult r = cobraSession_->frameVars(&wireVars_);
  if (!r.ok || !wireVars_.valid || wireVars_.vars.empty()) return nullptr;
  return &wireVars_;
}

void SessionState::reseedShadowFrom(Datastore* ds) {
  if (ds == nullptr) {
    shadowValid_ = false;
    return;
  }
  shadowCard_ = ds->varlist();  // BASE list (pre-meta); the render step applies the remap.
  shadowValid_ = true;
  residentRows_ = ds->nRows();   // C4-L2A: rows ride with the card, real-rebuild flavor.
  shadowCardHbin_ = ds->path();  // fix/sort-validate-lazy: ds was just built from currentHbin_
                                 // (mutationResult calls invalidateDatastore()+datastore()
                                 // immediately before this), so ds->path() == *currentHbin_ here.
}

}  // namespace piet
