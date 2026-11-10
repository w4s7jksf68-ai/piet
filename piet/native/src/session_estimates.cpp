// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Named-estimate storage, restoration and estadd bookkeeping. Declarations,
// state layout and observable ordering remain in SessionState; these methods
// keep the existing bodies and ownership/lifetime rules out of its shared header.
#include "piet/session_state.h"

#include "piet/strutil.h"  // globMatch for stored-estimate namelists

namespace piet {

std::string SessionState::eststo(const std::optional<std::string>& name, nlohmann::ordered_json snapshot,
                   const std::string& autoPrefix, bool viaEststo) {
  const bool named = name.has_value() && !name->empty();
  if (estimates.empty()) eststoSeq = 0;
  if (named) {
    for (auto& [k, v] : estimates) {
      if (k == *name) {
        v = std::move(snapshot);
        return k;
      }
    }
  }
  // An unnamed `estimates store` is not Stata at all; it keeps minting a name
  // here, as it always has, so it has to take a number to mint it from.
  if (viaEststo || !named) ++eststoSeq;
  const std::string key = named ? *name : autoPrefix + std::to_string(eststoSeq);
  for (auto& [k, v] : estimates) {
    if (k == key) {
      v = std::move(snapshot);
      return key;
    }
  }
  estimates.emplace_back(key, std::move(snapshot));
  return key;
}

std::vector<std::string> SessionState::estimatesNames() const {
  std::vector<std::string> out;
  out.reserve(estimates.size());
  for (const auto& [k, v] : estimates) out.push_back(k);
  return out;
}

const nlohmann::ordered_json* SessionState::estimatesGet(const std::string& name) const {
  for (const auto& [k, v] : estimates) {
    if (k == name) return &v;
  }
  return nullptr;
}

nlohmann::ordered_json* SessionState::estimatesGetMutable(const std::string& name) {
  for (auto& [k, v] : estimates) {
    if (k == name) return &v;
  }
  return nullptr;
}

std::string SessionState::estAddFingerprint(const nlohmann::ordered_json& estBlock) {
  std::string dep = estBlock.value("depvar", std::string());
  std::string est = estBlock.value("estimator", std::string());
  std::string n = "-";
  if (estBlock.contains("n_obs") && estBlock["n_obs"].is_number()) {
    n = std::to_string(estBlock["n_obs"].get<long long>());
  }
  std::size_t k = 0;
  if (estBlock.contains("coeftable") && estBlock["coeftable"].is_array()) {
    k = estBlock["coeftable"].size();
  }
  return dep + "|" + est + "|" + n + "|" + std::to_string(k);
}

void SessionState::dropEstAddedMirrors() {
  for (const char* kind : {"scalars", "locals"}) {
    if (!estAdded.contains(kind) || !estAdded[kind].is_object()) continue;
    for (const auto& kv : estAdded[kind].items()) {
      eval.eScalars.erase(kv.key());
      eval.eStrings.erase(kv.key());
    }
  }
  if (estAdded.contains("matrices") && estAdded["matrices"].is_object()) {
    for (const auto& kv : estAdded["matrices"].items()) {
      const std::string n = kv.key();
      eval.eMatrices.erase(std::remove_if(eval.eMatrices.begin(), eval.eMatrices.end(),
                                          [&](const auto& p) { return p.first == n; }),
                           eval.eMatrices.end());
    }
  }
}

void SessionState::estAddedClear() {
  estAdded = nlohmann::ordered_json{{"scalars", nlohmann::ordered_json::object()},
                                    {"locals", nlohmann::ordered_json::object()},
                                    {"matrices", nlohmann::ordered_json::object()}};
  estAddedFp.clear();
}

bool SessionState::estimatesDrop(const std::string& name) {
  for (size_t i = 0; i < estimates.size(); ++i) {
    if (estimates[i].first == name) {
      estimates.erase(estimates.begin() + static_cast<long>(i));
      estimatesModelErase(name);
      // estimates.ado Drop: dropping the name the active e() carries clears
      // e(_estimates_name) (ClearFields); the results themselves stay active.
      if (activeEstimatesName == name) activeEstimatesName.clear();
      return true;
    }
  }
  return false;
}

void SessionState::estimatesClear() {
  estimates.clear();
  for (const auto& kv : estimatesModels) {
    if (kv.second) pinStoredModel(*kv.second, -1);
  }
  estimatesModels.clear();
  activeEstimatesName.clear();
  eststoSeq = 0;
}

void SessionState::pinStoredModel(const StoredModel& sm, int delta) {
  if (!sm.lastModel.has_value()) return;
  for (const std::string* p : {&sm.lastModel->dataPath, &sm.lastModel->fitDataPath}) {
    if (p->empty()) continue;
    int& refs = storedModelPinRefs[*p];
    refs += delta;
    if (refs > 0) {
      scratchPins.insert(*p);
    } else {
      storedModelPinRefs.erase(*p);
      scratchPins.erase(*p);
    }
  }
}

void SessionState::estimatesModelSet(const std::string& name, std::shared_ptr<const StoredModel> sm) {
  // W2D-T01 round 3: the snapshot of the ACTIVE e() (eststo_esttab.cpp, the
  // only caller) also carries the names that sit on top of the fit.
  if (sm && sm->lastModel.has_value() && !eOnFitNames.empty()) {
    auto withNames = std::make_shared<StoredModel>(*sm);
    withNames->eOnFitNames = eOnFitNames;
    sm = std::move(withNames);
  }
  estimatesModelErase(name);
  if (sm) pinStoredModel(*sm, +1);
  estimatesModels[name] = std::move(sm);
}

void SessionState::estimatesModelErase(const std::string& name) {
  const auto it = estimatesModels.find(name);
  if (it == estimatesModels.end()) return;
  if (it->second) pinStoredModel(*it->second, -1);
  estimatesModels.erase(it);
}

std::string SessionState::activeStoredName() const {
  if (activeEstimatesName.empty() || estimatesGet(activeEstimatesName) == nullptr) return {};
  const auto it = estimatesModels.find(activeEstimatesName);
  if (it == estimatesModels.end() || !it->second) return {};
  return it->second->estBlock == lastEstBlock ? activeEstimatesName : std::string();
}

bool SessionState::estimatesRestore(const std::string& name) {
  const auto it = estimatesModels.find(name);
  if (it == estimatesModels.end() || !it->second) return false;
  const std::shared_ptr<const StoredModel> sm = it->second;
  if (!sm->lastModel.has_value()) {
    if (activeStoredName() == name) return true;
    // eb-order T-P3 (SPEC C, `estimates restore` of an estpost store): a
    // store with an estimation block but no fitted model behind it (an
    // estpost posting; runEststo stashes its e() stores) is installed as the
    // posted e() it was -- no LastModel, its block, its stores -- exactly as
    // postPseudoEstimate installed it, so the next fit supersedes it.
    if (!sm->estBlock.has_value() && !sm->postedE) return false;
    lastModel_reset();
    ePostedLive = true;
    ePostedEsampleSerial = sm->postedEsampleSerial;
  } else {
    LastModel lm = *sm->lastModel;
    lm.estHandle.clear();
    // e(sample) T2: the stored LastModel may say kColumn, but the column on the
    // data now holds whichever fit ran LAST, not this one's sample. The default
    // is the named refusal, never another model's rows; runEstimatesRestore
    // replaces it with the mask it installs from `_est_NAME` (T3).
    lm.esample = LastModel::EsampleState::kUnavailable;
    setLastModel(std::move(lm));
  }
  lastEstBlock = sm->estBlock;
  eval.lastModelResult = sm->lastModelResult;
  eval.eScalars = sm->eScalars;
  eval.eStrings = sm->eStrings;
  eval.eMatrices = sm->eMatrices;
  eOnFitNames = sm->eOnFitNames;  // round 3: setLastModel above emptied the live record
  eNameContract = sm->eNameContract;
  estAddedClear();
  const nlohmann::ordered_json* snap = estimatesGet(name);
  if (snap != nullptr && snap->contains("added") && (*snap)["added"].is_object()) {
    const nlohmann::ordered_json& bag = (*snap)["added"];
    if (lastEstBlock.has_value()) {
      estAdded = bag;
      estAddedFp = estAddFingerprint(*lastEstBlock);
    }
    republishAddedBag(bag);
  }
  activeEstimatesName = name;
  return true;
}

void SessionState::republishAddedBag(const nlohmann::ordered_json& bag) {
  if (bag.contains("scalars") && bag["scalars"].is_object()) {
    for (const auto& [n, v] : bag["scalars"].items()) {
      if (!v.is_number()) continue;
      eval.setEScalar(n, v.get<double>());
      if (eval.lastModelResult.has_value() && eval.lastModelResult->is_object() &&
          !eval.lastModelResult->contains(n)) {
        (*eval.lastModelResult)[n] = v;
      }
    }
  }
  if (bag.contains("locals") && bag["locals"].is_object()) {
    for (const auto& [n, v] : bag["locals"].items()) {
      if (v.is_string()) eval.setEString(n, v.get<std::string>());
    }
  }
  if (bag.contains("matrices") && bag["matrices"].is_object()) {
    for (const auto& [n, m] : bag["matrices"].items()) {
      eval::EvalMatrix em;
      if (m.contains("rownames") && m["rownames"].is_array())
        em.rownames = m["rownames"].get<std::vector<std::string>>();
      if (m.contains("colnames") && m["colnames"].is_array())
        em.colnames = m["colnames"].get<std::vector<std::string>>();
      if (m.contains("data") && m["data"].is_array()) {
        for (const auto& row : m["data"]) {
          std::vector<double> r;
          if (row.is_array()) {
            for (const auto& cell : row) {
              r.push_back(cell.is_number() ? cell.get<double>()
                                           : std::numeric_limits<double>::quiet_NaN());
            }
          }
          em.data.push_back(std::move(r));
        }
      }
      bool replaced = false;
      for (auto& kv : eval.eMatrices) {
        if (kv.first == n) { kv.second = em; replaced = true; break; }
      }
      if (!replaced) eval.eMatrices.emplace_back(n, std::move(em));
    }
  }
}

SessionState::EstNamelist SessionState::expandEstimatesNamelist(const std::vector<std::string>& tokens) const {
  EstNamelist out;
  auto fail = [&out](int rc, std::string msg) {
    out.names.clear();
    out.rc = rc;
    out.message = std::move(msg);
  };
  try {
    for (const std::string& tok : tokens) {
      if (tok == "_all") {
        // Empty store: nothing appended, not an error.
        for (const auto& kv : estimates) out.names.push_back(kv.first);
        continue;
      }
      if (tok == ".") {
        const bool haveActive = lastModel_.has_value() || lastEstBlock.has_value();
        if (!haveActive) {
          fail(301, "last estimates (.) not found");
          return out;
        }
        std::string stored = activeStoredName();
        out.names.push_back(stored.empty() ? std::string(".") : std::move(stored));
        continue;
      }
      bool hit = false;
      for (const auto& kv : estimates) {
        if (piet::strutil::globMatch(tok, kv.first)) {
          out.names.push_back(kv.first);
          hit = true;
        }
      }
      if (!hit) {
        fail(111, "estimation result " + tok + " not found");
        return out;
      }
    }
  } catch (...) {
    fail(198, "invalid namelist");
  }
  return out;
}

}  // namespace piet
