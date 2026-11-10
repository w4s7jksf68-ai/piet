// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: variable display formats and labels (.hbin carriage, format resolution, card overlay, macro-layer projection).
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

// ── BUG-198: the .hbin format/label carriage plane (see session_state.h) ─────

std::vector<cecon::ColMetaPatch> SessionState::hbinMetaPatch(
    const std::vector<VarInfo>& card) const {
  std::vector<cecon::ColMetaPatch> out;
  for (const VarInfo& v : card) {
    auto it = varMeta.find(v.name);
    if (it == varMeta.end()) continue;
    cecon::ColMetaPatch p;
    // Engage a key ONLY when this session actually declared it. A column with a
    // label override and no format override must leave the file's own `format`
    // key alone — engaging it with the card's rendered value would write
    // VarInfo::format's "%9.0g" member-initializer default into the header and
    // turn "unstated" into a fabricated value, which is precisely the
    // absent-vs-empty law this key inherited from P1-A0.
    if (it->second.format.has_value()) p.format = *it->second.format;
    if (it->second.label.has_value()) p.label = *it->second.label;
    if (!p.format.has_value() && !p.label.has_value()) continue;
    p.name = v.name;
    out.push_back(std::move(p));
  }
  return out;
}

bool SessionState::hasHbinMetaOverrides(const std::vector<VarInfo>& card) const {
  if (varMeta.empty()) return false;  // the common case, answered without a walk
  for (const VarInfo& v : card) {
    auto it = varMeta.find(v.name);
    if (it == varMeta.end()) continue;
    if (it->second.format.has_value() || it->second.label.has_value()) return true;
  }
  return false;
}

void SessionState::seedVarMetaFromLoadedFile(Datastore* ds) {
  if (ds == nullptr) return;
  for (const VarInfo& v : ds->varlist()) {
    if (!ds->has(v.name)) continue;
    // The RAW header strings, deliberately — NOT VarInfo::format, which has
    // already had its "%9.0g" member-initializer default substituted for a
    // column the file said nothing about. Adopting that default would
    // manufacture an override out of the file's silence and then write it back
    // into the next save's header, turning "unstated" into a fabricated value
    // for every column of every dataset. "" = the key was absent (P1-A0 /
    // BUG-198 law) and contributes nothing.
    const cecon::Column& c = ds->col(v.name);
    if (!c.format.empty()) varMeta[v.name].format = c.format;
    if (!c.label.empty()) varMeta[v.name].label = c.label;
  }
}

void SessionState::seedVarMetaFromVirtualSave(const std::string& path) {
  auto it = virtualSaveMeta_.find(path);
  if (it == virtualSaveMeta_.end()) return;
  for (const cecon::ColMetaPatch& p : it->second) {
    // "" here means "the override was cleared", exactly as it means "erase the
    // key" on the disk arm — so it must not be adopted as a value.
    if (p.format.has_value() && !p.format->empty()) varMeta[p.name].format = *p.format;
    if (p.label.has_value() && !p.label->empty()) varMeta[p.name].label = *p.label;
  }
}

void SessionState::adoptVirtualSaveFormats(const std::string& path,
                                           const std::vector<std::string>& names) {
  auto it = virtualSaveMeta_.find(path);
  if (it == virtualSaveMeta_.end()) return;
  const std::set<std::string> want(names.begin(), names.end());
  for (const cecon::ColMetaPatch& p : it->second) {
    if (!want.count(p.name) || !p.format.has_value() || p.format->empty()) continue;
    VarMeta& vm = varMeta[p.name];
    if (!vm.format.has_value() || vm.format->empty()) vm.format = *p.format;
  }
}

// format-display-default-lifecycle: see the declaration (session_state.h).
std::string SessionState::defaultDisplayFormat(const std::optional<std::string>& stataType,
                                               bool isString) {
  if (!isString) return storage::stataDefaultFormat(stataType);
  // str# displays %#s with a minimum width of 9 (str1..str9 %9s, str10 %10s,
  // str20 %20s in the captured describe blocks; str5 %9s,
  // w2p_estim_ev.log:1827); strL and a width nothing recorded display %9s.
  if (stataType.has_value() && stataType->size() > 3 && stataType->compare(0, 3, "str") == 0) {
    const std::string digits = stataType->substr(3);
    if (std::all_of(digits.begin(), digits.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
      const unsigned long w = std::stoul(digits);
      return "%" + std::to_string(std::max<unsigned long>(9, w)) + "s";
    }
  }
  return "%9s";
}

std::string SessionState::displayFormat(const VarInfo& v) const {
  auto it = varMeta.find(v.name);
  if (it != varMeta.end() && it->second.format.has_value() && !it->second.format->empty())
    return *it->second.format;
  if (v.formatStated) return v.format;
  std::optional<std::string> type;
  if (it != varMeta.end() && it->second.stataType.has_value() && !it->second.stataType->empty())
    type = *it->second.stataType;
  return defaultDisplayFormat(type, v.dtype == "factor");
}

std::vector<std::pair<std::string, std::string>> SessionState::displayFormatMap(
    const std::vector<VarInfo>& card) const {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(card.size());
  for (const VarInfo& v : card) out.emplace_back(v.name, displayFormat(v));
  return out;
}

void SessionState::pinDisplayFormat(const VarInfo& v) {
  auto it = varMeta.find(v.name);
  if (it != varMeta.end() && it->second.format.has_value() && !it->second.format->empty()) return;
  varMeta[v.name].format = displayFormat(v);
}

void SessionState::prepareStorageChange(const VarInfo& v) {
  if (v.dtype != "factor") {
    pinDisplayFormat(v);
    return;
  }
  auto it = varMeta.find(v.name);
  if (it == varMeta.end() || !it->second.format.has_value()) return;
  std::optional<std::string> type;
  if (it->second.stataType.has_value() && !it->second.stataType->empty())
    type = *it->second.stataType;
  if (*it->second.format == defaultDisplayFormat(type, true)) it->second.format.reset();
}

std::vector<VarInfo> SessionState::varlistWithMeta(Datastore* ds) const {
  if (ds == nullptr) return {};
  return varlistWithMetaFrom(ds->varlist());
}

// BUG-208. See the doc comment on the declaration for the design question this
// answers (why a projection and not a mirror-on-write, and not a layering
// inversion). Kept deliberately narrow: it refreshes eval.varLabels and touches
// nothing else, so the evalcase harness — which builds STANDALONE EvalSession
// objects that no SessionState owns — is unaffected and its pins keep meaning
// what they meant.
void SessionState::syncEvalVarLabels() {
  // Prefer the resident card: it already carries the resolved label and costs no
  // Datastore construction. nullptr means "not current" and the contract is an
  // honest fallback to the datastore, never trusting a stale card.
  const std::vector<VarInfo>* card = residentReadCard();
  std::vector<VarInfo> resolved;
  if (card != nullptr) {
    resolved = varlistWithMetaFrom(*card);
  } else {
    Datastore* ds = datastore();
    if (ds == nullptr) {
      // No dataset in memory: there are no variable labels to project. Clearing
      // is the honest answer — a label from a previous dataset must not survive
      // into a session that has dropped it.
      eval.varLabels.clear();
      // BUG-216/217: the plane is still AUTHORITATIVE — "no data" means "no
      // variables", so every `: format x` / `: variable label x` here is a
      // missing variable and Stata's r(111) is the right answer. Omitting the
      // sentinel would silently reopen the old lenient "" on exactly the case
      // where the reject is most obviously correct.
      eval.varLabels.set(kEvalVarPlaneKey, "1");
      return;
    }
    resolved = varlistWithMeta(ds);
  }
  // Rebuilt wholesale rather than merged: this is a projection, so a variable
  // that has been dropped or had its label cleared must DISAPPEAR from it. A
  // merge would let a stale label outlive the fact it describes, which is the
  // same class of defect as the one being fixed.
  eval.varLabels.clear();
  // BUG-216/217: mark the plane authoritative BEFORE the rows, so a zero-variable
  // dataset is still "authoritatively empty" rather than "never synced".
  eval.varLabels.set(kEvalVarPlaneKey, "1");
  for (const VarInfo& v : resolved) {
    if (!v.label.empty()) eval.varLabels.set(v.name, v.label);
    // BUG-216/217: the same wholesale rebuild, three more facts per variable.
    //
    // The EXISTENCE marker is the load-bearing one and it is unconditional —
    // that is the whole point. The label row above is still conditional (an
    // empty label is not stored, so `: variable label x` keeps answering ""
    // for a variable that exists and has none, which is Stata-faithful and
    // byte-identical to BUG-208's behaviour). What was missing is the ability
    // to tell that case apart from "no such variable", which Stata answers with
    // r(111). The marker supplies exactly that distinction and nothing else.
    eval.varLabels.set(std::string(kEvalVarExistsPrefix) + v.name, "1");
    // Format: the one resolver (displayFormat). `resolved` came through
    // varlistWithMetaFrom, which already overlays it; reading v.format is that
    // answer, and it is the same one describe, codebook and list report.
    auto fmtIt = varMeta.find(v.name);
    const std::string& resolvedFmt = v.format;
    eval.varLabels.set(std::string(kEvalVarFormatPrefix) + v.name, resolvedFmt);
    // BUG-250: declared storage subtype, describe's own resolution order —
    // recorded declaration first (creation arms + .dta import), then the
    // numeric default "float" (Stata's own `gen` default; also exactly what
    // `describe` answers for the same column). A string column with no
    // recorded width projects NOTHING on purpose — see the header's note.
    if (fmtIt != varMeta.end() && fmtIt->second.stataType.has_value() &&
        !fmtIt->second.stataType->empty()) {
      eval.varLabels.set(std::string(kEvalVarTypePrefix) + v.name, *fmtIt->second.stataType);
    } else if (v.dtype != "factor") {
      eval.varLabels.set(std::string(kEvalVarTypePrefix) + v.name, "float");
    }
    // Value-label SET NAME (`: value label x` answers the name, not the
    // mapping). Absent from varLblname = no value label attached, and Stata
    // answers "" with rc 0 for that — so the key is simply not written, and the
    // reader's default supplies "".
    auto lblIt = varLblname.find(v.name);
    if (lblIt != varLblname.end() && !lblIt->second.empty()) {
      eval.varLabels.set(std::string(kEvalVarLblsetPrefix) + v.name, lblIt->second);
    }
  }
}

std::vector<VarInfo> SessionState::varlistWithMetaFrom(const std::vector<VarInfo>& base) const {
  std::vector<VarInfo> result;
  result.reserve(base.size());
  for (const auto& v : base) {
    VarInfo entry = v;
    auto it = varMeta.find(v.name);
    if (it != varMeta.end() && it->second.label.has_value()) {
      entry.label = *it->second.label;
    }
    // format-display-default-lifecycle: the card states the format every
    // reader reports (displayFormat), not VarInfo's member default.
    entry.format = displayFormat(v);
    auto lblIt = varLblname.find(v.name);
    if (lblIt != varLblname.end()) {
      auto defIt = labelDefs.find(lblIt->second);
      if (defIt != labelDefs.end() && entry.dtype == "factor" && !entry.levels.empty()) {
        // Copy-on-write (A12): entry.levels is a non-owning VIEW into the
        // column's shared level dictionary. Remap a PRIVATE owned copy and
        // install it, so this value-label rewrite never mutates the shared
        // strings that var_detail and other cards read. Byte-identical to the
        // pre-A12 in-place rewrite of the (then-owned) per-entry copy.
        std::vector<std::string> remapped;
        remapped.reserve(entry.levels.size());
        for (std::size_t i = 0; i < entry.levels.size(); ++i) {
          const std::string& lv = entry.levels[i];
          auto mIt = defIt->second.find(lv);
          remapped.push_back(mIt != defIt->second.end() ? mIt->second : lv);
        }
        entry.levels = std::move(remapped);  // CoW install (owned); shared view untouched
      }
    }
    result.push_back(std::move(entry));
  }
  return result;
}

}  // namespace piet
