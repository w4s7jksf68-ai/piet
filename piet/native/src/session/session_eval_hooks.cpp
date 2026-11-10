// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SessionState: answers to evaluator callbacks (VAR[i], c(name), `ereturn list` name order).
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

std::optional<eval::EvalSession::VarObsReading> SessionState::macroVarObs(
    const std::string& name, long long idx) {
  std::optional<VarObservation> o = readVarObservation(name, idx, *this);
  if (!o) return std::nullopt;
  return eval::EvalSession::VarObsReading{o->text, o->isString, o->inRange};
}

std::optional<std::string> SessionState::macroCValue(const std::string& name) {
  std::optional<CValue> v = resolveCValue(name, *this);
  if (!v) return std::nullopt;
  return v->text;
}

// SessionState::eNameOrderFor (declared with ENameContract in session_state.h).
std::optional<std::vector<std::string>> SessionState::eNameOrderFor(const std::string& kind) const {
  if (!eNameContract.has_value() || ePostedLive || !lastModel_.has_value() ||
      lastModel_->fitSerial != eNameContract->fitSerial ||
      eval.eStrings.get("cmd") != eNameContract->cmd)
    return std::nullopt;
  const ENameContract& c = *eNameContract;
  const std::optional<std::vector<std::string>>* list =
      kind == "scalars" ? &c.scalars : kind == "macros" ? &c.macros
                                    : kind == "matrices" ? &c.matrices : nullptr;
  if (list == nullptr || !list->has_value()) return std::nullopt;
  const std::vector<std::string>& names = **list;
  const auto listed = [&names](const std::string& n) {
    return std::find(names.begin(), names.end(), n) != names.end();
  };
  if (kind == "scalars") {
    for (const auto& item : eval.eScalars.items())
      if (!eval::isReservedStoreKey(item.first) && !listed(item.first)) return std::nullopt;
    const nlohmann::ordered_json* result =
        eval.lastModelResult.has_value() && eval.lastModelResult->is_object()
            ? &*eval.lastModelResult
            : nullptr;
    for (const auto& n : names) {
      if (std::find(c.missingScalars.begin(), c.missingScalars.end(), n) != c.missingScalars.end())
        continue;
      if (eval.eScalars.find(n) != nullptr) continue;
      std::string key = n;
      for (const auto& [k, v] : eval::eScalarKey())
        if (k == n) {
          key = v;
          break;
        }
      if (result == nullptr || !result->contains(key) || !result->at(key).is_number())
        return std::nullopt;
    }
  } else if (kind == "macros") {
    for (const auto& item : eval.eStrings.items())
      if (!eval::isReservedStoreKey(item.first) && !item.second.empty() && !listed(item.first))
        return std::nullopt;
    for (const auto& n : names) {
      const std::string* v = eval.eStrings.find(n);
      if (v == nullptr || v->empty()) return std::nullopt;
    }
  } else {
    for (const auto& item : eval.eMatrices)
      if (!listed(item.first)) return std::nullopt;
    for (const auto& n : names) {
      if (eval.findClassMatrix('e', n) != nullptr) continue;
      // e(b)/e(V) and the block-derived e(beta)/e(V_modelbased) resolve lazily.
      if (eval.resolveEMatrixFor(n, eval::EvalSession::EReader::kCopy).matrix.has_value())
        continue;
      return std::nullopt;
    }
  }
  return names;
}

}  // namespace piet
