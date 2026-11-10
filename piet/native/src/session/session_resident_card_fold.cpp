// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: variable card after a resident mutation, fold side (plan capture, application, completeness guard, fold trace).
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

namespace piet {

// PHASE-0 DIAGNOSTIC (BUG-360 follow-up, 2026-08-20). The card fold declines on
// roughly half of a 460-rename corpus and the census cannot say which guard
// rejects — every rejection looks identical from outside (a full rebuild). This
// names the guard. Env-gated to a file, report-only, no behaviour change.
namespace {
const char* foldtrace_path() {
  static const char* p = [] {
    const char* e = std::getenv("PIET_FOLDTRACE");
    return (e && *e) ? e : nullptr;
  }();
  return p;
}
}  // namespace

void SessionState::foldTrace(const char* why, long long a, long long b) {
  if (const char* fp = foldtrace_path()) {
    if (std::FILE* f = std::fopen(fp, "a")) {
      std::fprintf(f, "%s\t%lld\t%lld\n", why, a, b);
      std::fclose(f);
    }
  }
}

// fix/verb-overhead-sweep S1b. Recognize — with certainty, never by guess —
// the two resident mutations whose post-mutation card is an incremental fold
// of the pre-mutation card rather than a fresh read of the frame:
//   • `order`  — a column PERMUTATION. Every surviving column keeps its name,
//     type, missing-count and level count; only the ENTRY ORDER moves.
//   • `drop`/`keep` — a column REMOVAL (both compile to cobra `eval --drop`;
//     runKeep passes the complement). Every surviving column is byte-
//     identical; the card just loses entries.
// Neither is expressible through mutationResult's (newVars, droppedVars,
// rewrittenVars) hints — `order` passes all three empty — and the engine's run
// response carries only counts for them, so the argv the intercept is sending
// is the one place the change is stated. ANY argv shape not matched here
// leaves Kind::None and the card is rebuilt exactly as before.
void SessionState::capturePendingCardPlan(const std::vector<std::string>& argv) {
  pendingCardPlan_ = CardFoldPlan{};
  if (argv.empty()) return;

  if (argv[0] == "order") {
    // `order --data D --vars a,b[,…] [--first|--last|--before V|--after V] --out O`
    // (datamgmt/data_order.cpp runOrder).
    CardFoldPlan p;
    p.kind = CardFoldPlan::Kind::Order;
    bool last = false;
    std::string before, after, varspec;
    bool haveVars = false;
    for (size_t i = 1; i < argv.size();) {
      const std::string& f = argv[i];
      if (f == "--first") { i += 1; continue; }  // the default; recorded via the precedence below
      if (f == "--last") { last = true; i += 1; continue; }
      if (i + 1 >= argv.size()) return;  // dangling value-taking flag: shape not understood
      if (f == "--data" || f == "--out") { i += 2; continue; }
      if (f == "--vars") { varspec = argv[i + 1]; haveVars = true; i += 2; continue; }
      if (f == "--before") { before = argv[i + 1]; i += 2; continue; }
      if (f == "--after") { after = argv[i + 1]; i += 2; continue; }
      return;  // ANY other flag (--keepif, a future mode) — not a pure permutation.
    }
    if (!haveVars) return;
    // cmd_order's own precedence, mirrored exactly (apps/cmds/cmds_data.cpp
    // cmd_order): last > before > after > first, default first. NOT an
    // else-if over what runOrder happens to emit today — this must agree with
    // the engine even for an argv runOrder would never build.
    if (last) {
      p.mode = "last";
    } else if (!before.empty()) {
      p.mode = "before";
      p.anchor = before;
    } else if (!after.empty()) {
      p.mode = "after";
      p.anchor = after;
    } else {
      p.mode = "first";
    }
    // split_csv (apps/cmds/common.cpp), mirrored exactly: trim each element,
    // drop empties. A divergence here would silently permute by a name the
    // engine never saw, so it is copied rather than approximated.
    size_t start = 0;
    while (start <= varspec.size()) {
      size_t comma = varspec.find(',', start);
      std::string cur = varspec.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start);
      size_t a = cur.find_first_not_of(" \t");
      if (a != std::string::npos) {
        size_t b = cur.find_last_not_of(" \t");
        p.vars.push_back(cur.substr(a, b - a + 1));
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (p.vars.empty()) return;
    pendingCardPlan_ = std::move(p);
    return;
  }

  if (argv[0] == "rename") {
    // P2-PERF ARC-B/B2. `rename --data D --out O --pairs old=new[,old=new…]`
    // (datamgmt/variable_copy_rename.cpp runRename). A rename is the purest fold there
    // is: the column keeps its position, its values, its dtype, its factor
    // dictionary and every count — only the name in slot i changes. Without a
    // plan here the card is rebuilt from the datastore, which for a resident
    // rename would mean materialising the very head the residency exists to
    // keep virtual.
    CardFoldPlan p;
    p.kind = CardFoldPlan::Kind::Rename;
    for (size_t i = 1; i < argv.size();) {
      const std::string& f = argv[i];
      if (i + 1 >= argv.size()) return;  // dangling / value-less flag: shape not understood
      if (f == "--data" || f == "--out") { i += 2; continue; }
      if (f == "--pairs") {
        // Same split as cmd_rename's own (comma-separated `old=new`), copied
        // rather than approximated: a divergence would rename a name the engine
        // never saw.
        const std::string& spec = argv[i + 1];
        size_t start = 0;
        while (start <= spec.size()) {
          size_t comma = spec.find(',', start);
          std::string cur =
              spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          size_t eq = cur.find('=');
          if (eq == std::string::npos) return;  // not a pair: shape not understood
          p.vars.push_back(cur.substr(0, eq));      // old
          p.renameTo.push_back(cur.substr(eq + 1));  // new
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
        i += 2;
        continue;
      }
      return;  // ANY other flag — not a pure rename.
    }
    if (p.vars.empty() || p.vars.size() != p.renameTo.size()) return;
    pendingCardPlan_ = std::move(p);
    return;
  }

  if (argv[0] == "eval") {
    // `eval --data D --out O --drop v [--drop v …]` (datamgmt/observation_selection.cpp runDrop/
    // runKeep). Every OTHER eval flag (--gen/--replace/--rename/--winsor/
    // --xtile/--ipolate/--keepif/--summarize) changes values, rows, or names,
    // so ONE of them anywhere in the argv refuses the whole plan: a fold that
    // guessed at a mixed rename+drop would ship a silently wrong card, which
    // is strictly worse than rebuilding it.
    CardFoldPlan p;
    p.kind = CardFoldPlan::Kind::Drop;
    for (size_t i = 1; i < argv.size();) {
      const std::string& f = argv[i];
      if (i + 1 >= argv.size()) return;  // dangling / value-less flag: shape not understood
      if (f == "--data" || f == "--out") { i += 2; continue; }
      if (f == "--drop") { p.vars.push_back(argv[i + 1]); i += 2; continue; }
      return;
    }
    if (p.vars.empty()) return;  // a no-op eval: nothing to fold, rebuild as before.
    pendingCardPlan_ = std::move(p);
  }
}

// Apply a captured plan to shadowCard_. Every guard below is an A12 fall-back:
// it answers "can I PROVE the post-mutation card equals this transformation of
// the pre-mutation card?", and any answer short of yes returns false with the
// card untouched, so mutationResult rebuilds it from the datastore exactly as
// it does today. The engine's own delta counts (dropped / cols / changed /
// added) are used as a CHECKSUM on the argv-derived plan: they were computed
// by diffing the real pre- and post-mutation frames, so a plan that disagrees
// with them describes a mutation that did not happen.
bool SessionState::applyPendingCardPlan(const CardFoldPlan& plan, const CobraFrameMeta& delta) {
  if (plan.kind == CardFoldPlan::Kind::None) return false;
  // A permutation and a removal both leave every SURVIVING column byte-
  // identical, so the engine must have reported no column changed, none added,
  // and an empty vars_delta. Anything else (a value rewrite, a new column, a
  // row-count change that re-materializes every buffer) is not this plan.
  //
  // Rename is exempt from the vars_delta clause and states its own conditions
  // below: the engine reports a renamed column THROUGH vars_delta (under its
  // new name), so an empty delta is exactly what a rename does NOT produce.
  // The other two clauses still bind it — a rename adds no column and changes
  // no value.
  if (plan.kind != CardFoldPlan::Kind::Rename) {
    if (delta.changed != 0 || delta.added != 0 || !delta.varsDelta.empty()) return false;
  }
  if (delta.cols <= 0) return false;  // an empty/columnless result — rebuild rather than reason about it.
  // The card must name every column exactly once, or "move name N" and "remove
  // name N" are ambiguous.
  std::unordered_map<std::string, size_t> idx;
  idx.reserve(shadowCard_.size());
  for (size_t i = 0; i < shadowCard_.size(); ++i) {
    if (!idx.emplace(shadowCard_[i].name, i).second) return false;
  }

  if (plan.kind == CardFoldPlan::Kind::Rename) {
    // Column COUNT unchanged, nothing dropped, and every old name present
    // exactly once while every new name is absent — the same question
    // runRename's own entry guard asks, re-asked here against the card the
    // fold is about to rewrite rather than the one it saw before the call.
    // MEASURED, not assumed: the engine reports a rename of k columns as
    // dropped=k, added=k, changed=0, cols unchanged, with vars_delta carrying
    // the k NEW names (`[rentrace] drop=1 add=1 chg=0 vd=1` on a one-pair
    // rename). It is a drop-and-add at the delta layer and a pure relabel at
    // the card layer; those two readings only agree if the counts line up
    // exactly with the pairs this plan claims, so require that.
    const int64_t k = static_cast<int64_t>(plan.vars.size());
    // PHASE-0: log every axis at once, before any short-circuit, so ONE run
    // says which guards would fire rather than only the first.
    foldTrace("rn-shape/k-drop-add", k * 1000000 + delta.dropped * 1000 + delta.added,
              delta.changed);
    foldTrace("rn-shape/vd-cols-card", static_cast<long long>(delta.varsDelta.size()) * 1000 + delta.cols,
              static_cast<long long>(shadowCard_.size()));
    if (delta.dropped != k || delta.added != k) {
      foldTrace("rn-counts", delta.dropped * 1000 + delta.added, delta.changed);
      return false;
    }
    // COMPLETE-DELTA rename. The `changed == 0` this branch used to require was
    // recorded as measured — and it is measured only of the frame it was taken
    // on. On the acceptance corpus HALF of a 322-rename run reports
    // `dropped=1, added=1, changed=11` with a varsDelta carrying ALL 12 columns
    // (PIET_FOLDTRACE, 2026-08-20). Those 154 declines fell into
    // mutationResult's full rebuild, and rename that way forced 220 of the
    // run's 476 head materializations — 46% of them, more than any other verb.
    //
    // A whole-frame report is not a reason to distrust the engine; it is MORE
    // information, not less. So when the delta covers every column, the card is
    // REBUILT FROM IT rather than folded — the identical discipline the row
    // filter branch below already uses, and for the identical reason: nothing
    // is assumed invariant, every metadata field comes from the engine, and the
    // result is committed only once whole.
    //
    // The fold path (changed == 0, varsDelta == just the k new names) is left
    // exactly as it was; this is an additional branch, not a relaxation of that
    // one. Order is carried from the card because a rename cannot permute
    // columns — the same reasoning, and the same limit, the row-filter branch
    // states about reading order off a delta that does not contract it.
    const bool completeDelta =
        delta.changed != 0 && static_cast<int64_t>(delta.varsDelta.size()) == delta.cols &&
        static_cast<int64_t>(shadowCard_.size()) == delta.cols;
    if (completeDelta) {
      std::unordered_map<std::string, std::string> newFor;  // old -> new
      for (size_t i = 0; i < plan.vars.size(); ++i) {
        if (idx.find(plan.vars[i]) == idx.end()) {
          foldTrace("rn-cd-oldmissing", 0, 0);
          return false;  // old name not in the card
        }
        if (!newFor.emplace(plan.vars[i], plan.renameTo[i]).second) {
          foldTrace("rn-cd-dupold", 0, 0);
          return false;  // same old named twice
        }
      }
      std::unordered_map<std::string, const CobraFrameVar*> byName;
      byName.reserve(delta.varsDelta.size());
      for (const auto& fv : delta.varsDelta) {
        if (!byName.emplace(fv.name, &fv).second) {
          foldTrace("rn-cd-dupdelta", 0, 0);
          return false;  // the engine named one column twice — refuse to guess
        }
      }
      std::vector<VarInfo> next;
      next.reserve(shadowCard_.size());
      for (const auto& existing : shadowCard_) {
        auto rn = newFor.find(existing.name);
        const std::string post = rn == newFor.end() ? existing.name : rn->second;
        auto it = byName.find(post);
        if (it == byName.end()) break;  // the engine did not report this column
        VarInfo vi = existing;          // keeps PIET-side display metadata (format/label)
        vi.name = post;
        vi.dtype = it->second->type;
        vi.nMissing = it->second->nMissing;
        vi.nLevels = it->second->nLevels;
        // Same rule as the row filter: trust the carried dictionary only while
        // it still agrees with the count the engine just reported.
        if (!vi.nLevels.has_value() || static_cast<size_t>(*vi.nLevels) != vi.levels.size()) {
          vi.levels.clear();
        }
        next.push_back(std::move(vi));
      }
      if (next.size() != shadowCard_.size()) {
        foldTrace("rn-cd-incomplete", static_cast<long long>(next.size()),
                  static_cast<long long>(shadowCard_.size()));
        return false;  // never ship a partial card
      }
      shadowCard_ = std::move(next);
      for (const auto& n : plan.vars) residentComputedColInvalidate(n);
      foldTrace("rn-cd-ok", k, delta.changed);
      return true;
    }
    if (delta.changed != 0) {
      foldTrace("rn-counts", delta.dropped * 1000 + delta.added, delta.changed);
      return false;
    }
    if (static_cast<int64_t>(delta.varsDelta.size()) != static_cast<size_t>(k)) {
      foldTrace("rn-vdsize", static_cast<long long>(delta.varsDelta.size()), k);
      return false;
    }
    if (static_cast<int64_t>(shadowCard_.size()) != delta.cols) {
      foldTrace("rn-cols", static_cast<long long>(shadowCard_.size()), delta.cols);
      return false;
    }
    std::vector<size_t> slots;
    slots.reserve(plan.vars.size());
    std::unordered_set<std::string> touched;
    std::unordered_set<std::string> newNames;
    for (size_t k = 0; k < plan.vars.size(); ++k) {
      auto it = idx.find(plan.vars[k]);
      if (it == idx.end()) return false;                  // old name not in the card
      if (!touched.insert(plan.vars[k]).second) return false;  // named twice
      if (idx.find(plan.renameTo[k]) != idx.end()) return false;  // new name already taken
      if (!newNames.insert(plan.renameTo[k]).second) return false;  // two olds -> one new
      slots.push_back(it->second);
    }
    // Every column the engine reported must be one this plan claims to have
    // created — never a third thing it renamed behind our back.
    for (const auto& fv : delta.varsDelta) {
      if (newNames.count(fv.name) == 0) return false;
    }
    for (size_t k = 0; k < slots.size(); ++k) shadowCard_[slots[k]].name = plan.renameTo[k];
    // Adopt whatever metadata the engine reported for the renamed columns —
    // it recomputed them against the real post-rename frame, so it outranks
    // the carried card. (A rename should not move any of these; taking them
    // from the engine means the card does not have to assume that.)
    for (const auto& fv : delta.varsDelta) {
      for (auto& existing : shadowCard_) {
        if (existing.name != fv.name) continue;
        existing.dtype = fv.type;
        existing.nMissing = fv.nMissing;
        existing.nLevels = fv.nLevels;
        break;
      }
    }
    // C4-L4: the cached computed column is keyed by NAME, so a rename of it
    // would leave the cache answering under a name that no longer exists.
    for (const auto& n : plan.vars) residentComputedColInvalidate(n);
    return true;
  }

  if (plan.kind == CardFoldPlan::Kind::Drop) {
    if (delta.dropped != static_cast<int64_t>(plan.vars.size())) return false;
    std::unordered_set<std::string> removed;
    for (const auto& n : plan.vars) {
      if (idx.find(n) == idx.end()) return false;   // not in the card (stale card, or engine no-op'd it)
      if (!removed.insert(n).second) return false;  // named twice — the engine's 2nd drop was a no-op
    }
    if (static_cast<int64_t>(shadowCard_.size() - removed.size()) != delta.cols) return false;
    std::vector<VarInfo> next;
    next.reserve(shadowCard_.size() - removed.size());
    for (auto& v : shadowCard_) {
      if (removed.find(v.name) == removed.end()) next.push_back(std::move(v));
    }
    shadowCard_ = std::move(next);  // erase preserves the survivors' relative order, as Frame::drop does.
    return true;
  }

  // Kind::Order — a pure permutation: the column COUNT must be unchanged.
  if (delta.dropped != 0) return false;
  if (static_cast<int64_t>(shadowCard_.size()) != delta.cols) return false;
  std::unordered_set<std::string> inList;
  for (const auto& n : plan.vars) {
    if (idx.find(n) == idx.end()) return false;
    // A duplicated --vars entry makes the engine name a column TWICE (see
    // Frame::order's rvalue overload, which falls back to the copy path for
    // exactly this): the result has more columns than the card. Refuse.
    if (!inList.insert(n).second) return false;
  }
  // Frame::order_names (src/data/frame.cpp), mirrored: `remaining` keeps the
  // pre-mutation order of everything not in the reorder list, and the list is
  // spliced in at one of four positions.
  std::vector<std::string> remaining;
  remaining.reserve(shadowCard_.size() - inList.size());
  for (const auto& v : shadowCard_) {
    if (inList.find(v.name) == inList.end()) remaining.push_back(v.name);
  }
  std::vector<std::string> newOrder;
  newOrder.reserve(shadowCard_.size());
  if (plan.mode == "first") {
    newOrder = plan.vars;
    newOrder.insert(newOrder.end(), remaining.begin(), remaining.end());
  } else if (plan.mode == "last") {
    newOrder = remaining;
    newOrder.insert(newOrder.end(), plan.vars.begin(), plan.vars.end());
  } else if (plan.mode == "before" || plan.mode == "after") {
    auto it = std::find(remaining.begin(), remaining.end(), plan.anchor);
    if (it == remaining.end()) return false;  // anchor absent or inside the list (the engine throws).
    std::ptrdiff_t cut = (it - remaining.begin()) + (plan.mode == "after" ? 1 : 0);
    newOrder.assign(remaining.begin(), remaining.begin() + cut);
    newOrder.insert(newOrder.end(), plan.vars.begin(), plan.vars.end());
    newOrder.insert(newOrder.end(), remaining.begin() + cut, remaining.end());
  } else {
    return false;  // an unrecognized mode string — never silently treat it as "first".
  }
  if (newOrder.size() != shadowCard_.size()) return false;
  std::vector<VarInfo> next;
  next.reserve(newOrder.size());
  for (const auto& n : newOrder) next.push_back(shadowCard_[idx[n]]);
  shadowCard_ = std::move(next);
  return true;
}

// fix/verb-overhead-sweep S2. Answers ONE question, and it is the load-bearing
// safety check of the row-filter fast path below: does `delta.varsDelta`
// describe every SURVIVING column of the post-mutation frame, so that a card
// can be rebuilt from it with nothing left to infer?
//
// WHY THIS IS A GUARD AND NOT AN ASSUMPTION. `vars_delta` is a DELTA, not a
// varlist, and its completeness for a row filter is INCIDENTAL rather than
// contracted — CoBrA Op verified the mechanism against the shipped 0.33.0
// engine and deliberately declined to contract it (contracting "complete
// vars_delta whenever card_stable is false" would oblige every structural verb
// to a full per-column n_missing rescan forever, which is precisely the card
// tax §30 exists to shed). What actually happens today: `changed` is computed
// by data-pointer identity, and Frame::keep_if (src/data/frame.cpp) allocates a
// fresh vector per column and moves it in, so every pointer moves and the delta
// HAPPENS to cover every column. A later lazy/CoW row selection — a rewrite
// this codebase already does elsewhere (§27 column-CoW, zero-copy views) —
// would preserve untouched pointers and this same field would go PARTIAL while
// the rows still change. That is a live risk, not a theoretical one.
//
// So: what may be relied on is `card_stable` as the validity signal and the
// correctness of the entries that ARE present. What may NOT be relied on is
// that all of them are present. Hence the count is compared against
// `delta.cols` — the POST-mutation column count — and never against the
// pre-mutation card's size alone: `collapse` already returns vars_delta=2 with
// dropped=13 TODAY, so a consumer reading vars_delta as "the whole varlist" is
// wrong on the shipped engine, not merely after some future change.
//
// The `added == 0 && dropped == 0` + exact name-set-equality requirements are
// stricter than bare completeness on purpose: together they prove the column
// SET is untouched, which is what lets the caller take the surviving column
// ORDER from the pre-mutation card instead of trusting the delta's own
// ordering (a delta has no contracted order either). Every case short of a
// provably complete, set-preserving delta returns false, and the caller then
// falls back to the full datastore rebuild — never to a partial card.
bool SessionState::residentCardDeltaComplete(const CobraFrameMeta& delta,
                                             const std::vector<VarInfo>& card) {
  if (delta.card_stable) return false;  // the card is still valid — not this branch's business.
  if (delta.added != 0) return false;  // column set moved (collapse/reshape).
  // INSPECT-2608: a bare `dropped != 0` also refused the case where the ONLY
  // columns dropped are ones this card never held — which is exactly what a
  // string-predicate row filter does now that it pushes a scratch mask column
  // and has the engine delete it in the same call (BUG-351). The card never
  // learned the mask, so nothing the card DESCRIBES moved, and rebuilding the
  // whole frame to discover that was 7 full materializations on a 7-filter
  // fixture. Refuse only when a dropped name is one the card actually holds;
  // when the engine reported a bare count with no names, keep the old
  // conservative refusal.
  if (delta.dropped != 0) {
    if (delta.droppedNames.size() != static_cast<size_t>(delta.dropped)) return false;
    for (const auto& gone : delta.droppedNames)
      for (const auto& have : card)
        if (have.name == gone) return false;
  }
  if (delta.cols <= 0) return false;  // an empty/columnless result — rebuild rather than reason about it.
  if (static_cast<int64_t>(delta.varsDelta.size()) != delta.cols) return false;  // PARTIAL delta.
  if (static_cast<int64_t>(card.size()) != delta.cols) return false;
  std::unordered_set<std::string> names;
  names.reserve(card.size());
  for (const auto& v : card) {
    if (!names.insert(v.name).second) return false;  // a card naming a column twice is unusable.
  }
  for (const auto& fv : delta.varsDelta) {
    if (names.erase(fv.name) != 1) return false;  // unknown name, or named twice by the delta.
  }
  return names.empty();  // every card entry was covered exactly once.
}

std::optional<nlohmann::ordered_json> SessionState::tryResidentMutationCard(
    const std::string& action, const std::vector<std::string>& newVars,
    const std::vector<std::string>& droppedVars, const std::vector<std::string>& rewrittenVars) {
  (void)rewrittenVars;  // the shadow fold keys on vars_delta, not the caller's hint lists.
  if (!pendingDeltaValid_) { foldTrace("no-pending-delta", 0, 0); return std::nullopt; }
  CobraFrameMeta delta = pendingDelta_;  // this delta belongs to exactly the just-completed mutation.
  CardFoldPlan plan = pendingCardPlan_;  // …and so does this plan (written at the same site).
  pendingDeltaValid_ = false;
  pendingCardPlan_ = CardFoldPlan{};
  if (!shadowValid_) {
    foldTrace("no-shadow-base", static_cast<long long>(plan.kind), 0);
    return std::nullopt;  // no base card to fold into.
  }

  if (plan.kind != CardFoldPlan::Kind::None) {
    // S1b: order/drop/keep. Checked BEFORE the vars_delta branch below, not
    // after: `order`'s vars_delta is EMPTY, so if a future engine ever reports
    // card_stable=true for it (Frame::order's structure_dirty_ is a write-path
    // fact — "a column permutation can't be patched onto the source" — not a
    // card fact, so that flag could legitimately move), the vars_delta branch
    // would "fold" it by changing nothing and ship an UNPERMUTED card. A plan
    // that declines therefore falls straight through to the rebuild rather
    // than into that branch.
    if (!applyPendingCardPlan(plan, delta)) {
      foldTrace("plan-declined", static_cast<long long>(plan.kind), delta.card_stable ? 1 : 0);
      if (!delta.card_stable) shadowValid_ = false;
      return std::nullopt;
    }
    foldTrace("plan-ok", static_cast<long long>(plan.kind), 0);
    // C4-L4: order/drop/keep restructures the column set/order — the cached
    // computed column may have been dropped or the frame reshaped; retire it
    // wholesale (conservative, and this branch is never the setcol hot path).
    residentComputedColClear();
  } else if (residentCardDeltaComplete(delta, shadowCard_)) {
    // fix/verb-overhead-sweep S2: a resident ROW FILTER (`keep if`/`drop if`).
    // card_stable is FALSE here and that is honest — a row filter changes
    // n_missing and n_levels per column, so the pre-filter card genuinely IS
    // invalid. The card is therefore REBUILT from the engine's freshly
    // RECOMPUTED vars_delta, never folded from what the card used to say.
    //
    // This distinction is the whole point of the branch. The S1b fold above
    // works because a permutation/removal provably cannot change any surviving
    // column's metadata; extending that argv-derived reasoning to a row filter
    // would ship stale missing-counts and factor levels — numbers that are
    // wrong in the variable panel and that no value pin would catch, since the
    // pinned card would simply keep agreeing with itself. Nothing here assumes
    // invariance: every metadata field below comes from the engine.
    //
    // Only the column ORDER is carried over, and only because
    // residentCardDeltaComplete proved added==0, dropped==0 and exact name-set
    // equality — i.e. the column set is untouched, and a row filter cannot
    // permute columns. Reading the order off the delta instead would trust an
    // ordering the delta does not contract.
    //
    // Note also that a false card_stable does NOT mean rows were dropped:
    // keep_if sets structure_dirty_ unconditionally, so a filter matching every
    // row lands here too. n_rows_after is taken from delta.rows below, so the
    // "did it shrink" question is never asked or assumed.
    std::unordered_map<std::string, const CobraFrameVar*> byName;
    byName.reserve(delta.varsDelta.size());
    for (const auto& fv : delta.varsDelta) byName.emplace(fv.name, &fv);
    // Built into a SEPARATE vector and committed only once whole: a mid-loop
    // bail must not be able to leave shadowCard_ half-engine/half-stale, which
    // is the partial-card outcome this branch exists to make impossible.
    std::vector<VarInfo> next;
    next.reserve(shadowCard_.size());
    for (const auto& existing : shadowCard_) {
      auto it = byName.find(existing.name);
      if (it == byName.end()) break;  // unreachable given the guard; refuse anyway.
      VarInfo vi = existing;  // keeps PIET-side display metadata (format/label).
      vi.dtype = it->second->type;
      vi.nMissing = it->second->nMissing;
      vi.nLevels = it->second->nLevels;
      // The level DICTIONARY is retained across a filter by the engine (a level
      // whose last row was filtered out keeps its slot), so the carried strings
      // stay right — but only trust them while they still agree with the count
      // the engine just reported. They are not emitted in varlist_after (only
      // n_levels is, datastore.cpp VarInfo::toJson); this keeps the in-memory
      // card from carrying a dictionary that contradicts its own count.
      if (!vi.nLevels.has_value() || static_cast<size_t>(*vi.nLevels) != vi.levels.size()) {
        vi.levels.clear();
      }
      next.push_back(std::move(vi));
    }
    if (next.size() != shadowCard_.size()) {
      shadowValid_ = false;
      return std::nullopt;  // fall back to the rebuild; never ship a partial card.
    }
    shadowCard_ = std::move(next);
  } else if (!delta.card_stable || delta.dropped > 0) {
    // This gate answers exactly one question: is the shadow metadata CARD
    // still valid (card_stable)? A row-count/column-set change makes it
    // invalid, same as no shadow base, same as a drop we cannot name from
    // vars_delta -> fall back to the real datastore rebuild (rare; never the
    // hot gen/replace/egen add-or-replace path). S2: an INVALID card whose
    // vars_delta is complete is handled by the branch above; reaching here with
    // card_stable false now means the delta could NOT rebuild it — a partial
    // delta, a moved column set, a mismatched name set — and this rebuild is
    // exactly the intended landing spot for all of them. `structure` deliberately
    // answers a different, write-path question (can a later `save`
    // byte-patch the file) and is no longer consulted here — see
    // CobraFrameMeta in cobra_session.h. Using `structure` here was the bug:
    // `sort` reports structure=true (no source path) but card_stable=true
    // (the card is still valid), so it was falling back on every call.
    // GA sweep 2026-08-22: this branch had NO foldTrace, so the census could see
    // that mutationResult forced 640 head materializations (82.28 GB — 71% of all
    // materialized bytes, the single largest coordinate left) but could not see
    // WHY, because the refusals that land here are invisible. Traced now with the
    // two sub-causes separated, since they want different fixes: an unstable card
    // is a rebuild we may be able to avoid, an unnameable drop is a delta the
    // engine could report more precisely.
    foldTrace(!delta.card_stable ? "cd-card-unstable" : "cd-unnamed-drop",
              static_cast<long long>(delta.dropped),
              static_cast<long long>(delta.varsDelta.size()));
    if (!delta.card_stable) shadowValid_ = false;
    return std::nullopt;
  } else {
    // Fold the changed/added columns into the shadow card, preserving column
    // order (append unknown names; update known names in place). PIET-side
    // label/levels metadata on an updated column is preserved.
    for (const auto& fv : delta.varsDelta) {
      // C4-L4: any fold touching the cached computed column's name supersedes
      // its values. (A setcol's OWN fold also lands here first — the handler
      // re-puts the fresh values right after mutationResult returns.)
      residentComputedColInvalidate(fv.name);
      bool found = false;
      for (auto& existing : shadowCard_) {
        if (existing.name == fv.name) {
          existing.dtype = fv.type;
          existing.nMissing = fv.nMissing;
          existing.nLevels = fv.nLevels;
          found = true;
          break;
        }
      }
      if (!found) {
        VarInfo vi;
        vi.name = fv.name;
        vi.dtype = fv.type;
        vi.nMissing = fv.nMissing;
        vi.nLevels = fv.nLevels;
        // BUG-365, and only when stated — see the sibling site above for why an
        // empty engine format must never overwrite the VarInfo default.
        if (!fv.format.empty()) {
          vi.format = fv.format;
          vi.formatStated = true;
        }
        if (!fv.label.empty()) vi.label = fv.label;
        shadowCard_.push_back(std::move(vi));
      }
    }
  }
  ++cardFoldCount_;  // selftest instrumentation: this card came from a fold, not a rebuild.
  // fix/sort-validate-lazy: this fold just brought shadowCard_ current for
  // the frame residentToken_ now names — the handler's session.load(out)
  // (called before mutationResult/this function) already advanced
  // currentHbin_ to that SAME path along the virtual-chain-continuation
  // branch (load() only leaves residentToken_ untouched when path ==
  // residentToken_), so tagging with residentToken_ here is equivalent to
  // tagging with currentHbin_ but stays consistent with
  // seedShadowFromFrameVars' own convention (see its doc comment).
  shadowCardHbin_ = residentToken_;
  // C4-L2A: rows ride with the card — the SAME delta.rows this function has
  // always trusted as n_rows_after (below), now also the card's row count.
  residentRows_ = delta.rows;

  nlohmann::ordered_json out;
  out["type"] = "data_mutation";
  out["action"] = action;
  auto vlist = varlistWithMetaFrom(shadowCard_);
  nlohmann::ordered_json varlistAfter = nlohmann::ordered_json::array();
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  updateNMissCache(vlist);  // keep the C2-F13a cache coherent for a later datastore-path verb.
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = delta.rows;
  if (!newVars.empty()) out["new_vars"] = newVars;
  if (!droppedVars.empty()) out["dropped_vars"] = droppedVars;
  return out;
}

}  // namespace piet
