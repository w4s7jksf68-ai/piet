// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// session lifecycle. Method bodies retain their original ordering.
#include "piet/session_state.h"

namespace piet {

SessionState::SessionState(std::string workdir) : workdir_(std::move(workdir)) {
    resetProcessCaches();
    // BUG-512: the macro layer's `_N` (eval/session.h, EvalSession::obsCount).
    // Capturing `this` is sound because a SessionState can be neither copied
    // nor moved (its std::mutex members delete both), and nothing in the tree
    // ever reassigns `eval` wholesale.
    eval.obsCount = [this]() -> long long { return macroObsCount(); };
    // The macro layer's runiform()/runiformint() seed (EvalSession::rngSeed),
    // same capture rationale as obsCount above.
    eval.rngSeed = [this]() -> eval::EvalSession::RngSeedReading {
      return eval::EvalSession::RngSeedReading{seed.get(), seed.epoch()};
    };
    eval.rngMt64 = [this]() -> cecon::Mt64* { return &seed.stream(); };
    // e(sample) T2: the macro layer's observation-1 value (esampleObs1 below),
    // same capture rationale as obsCount above.
    eval.esampleObs1 = [this](std::string* why) -> std::optional<int> {
      return esampleObs1(why);
    };
    // BUG-718: the macro layer's dataset subscript `VAR[i]` (macroVarObs below),
    // same capture rationale as obsCount above.
    eval.varObs = [this](const std::string& name,
                         long long idx) -> std::optional<eval::EvalSession::VarObsReading> {
      return macroVarObs(name, idx);
    };
    // BUG-719: the inline `: type VAR' refreshes the plane it reads
    // (EvalSession::syncVarPlane), same capture rationale as obsCount above.
    eval.syncVarPlane = [this]() { syncEvalVarLabels(); };
    // `: label lblname #` reads the value-label sets (EvalSession::valueLabelSet),
    // same capture rationale as obsCount above.
    eval.valueLabelSet =
        [this](const std::string& lblname) -> const std::map<std::string, std::string>* {
      auto it = labelDefs.find(lblname);
      return it == labelDefs.end() ? nullptr : &it->second;
    };
    // BUG-1006: the macro layer's `` `c(name)' `` (EvalSession::cValue,
    // macroCValue below), same capture rationale as obsCount above.
    eval.cValue = [this](const std::string& name) -> std::optional<std::string> {
      return macroCValue(name);
    };
    // EM13 / LBL-CHAR: `: char NAME[CHAR]` (EvalSession::characteristic), same
    // capture rationale as obsCount above.
    eval.characteristic = [this](const std::string& target,
                                 const std::string& name) -> std::optional<std::string> {
      for (const auto& c : characteristics)
        if (c.varname == target && c.charname == name) return c.value;
      return std::nullopt;
    };
    // LBL-DATALABEL: `: data label` (EvalSession::dataLabel), same capture rationale.
    eval.dataLabel = [this]() -> std::string { return dataLabel; };
    // `: sortedby` (EvalSession::sortedBy): the head's engine sort claim.
    eval.sortedBy = [this]() -> std::string {
      if (!hasData()) return std::string();
      try {
        const Datastore* d = datastore();
        return d != nullptr ? d->sortedBy() : std::string();
      } catch (...) {
        return std::string();
      }
    };
    // eb-order T-P3: the one coefficient source (EvalSession::estBlock) and the
    // e()-is-live signal (EvalSession::modelLive), same capture rationale as
    // obsCount above. An `ereturn post` e() writes no block; its coeftable
    // mirror in eval.lastModelResult (handlers_ereturn.cpp) is that e()'s own,
    // and it is read only while the posting is live.
    eval.estBlock = [this]() -> const nlohmann::ordered_json* {
      if (lastEstBlock.has_value()) return blockPostsEb(*lastEstBlock) ? &*lastEstBlock : nullptr;
      if (ePostedLive && !lastModel_.has_value() && eval.lastModelResult.has_value())
        return &*eval.lastModelResult;
      return nullptr;
    };
    eval.modelLive = [this]() -> bool {
      return lastModel_.has_value() || lastEstBlock.has_value() || ePostedLive;
    };
    eval.eNameListsComplete = [this](const std::string& kind) -> bool {
      if (ePostedLive || eval.eStrings.get("cmd") == "estpost" ||
          (!lastModel_.has_value() && !lastEstBlock.has_value())) return true;
      // PCA publishes its entire matrix contract, while its scalar/macro
      // lists still have separate implicit metadata and remain unclaimed.
      if (kind != "matrices" || eval.eStrings.get("cmd") != "pca") return false;
      for (const char* name : {"means", "components", "C", "Psi", "Ev", "L"})
        if (!eval.findClassMatrix('e',name)) return false;
      return eval.eStrings.get("Ctype") != "correlation" || eval.findClassMatrix('e',"sds");
    };
    eval.eNameOrder = [this](const std::string& kind) { return eNameOrderFor(kind); };
    registerLiveSession();
  }

}
