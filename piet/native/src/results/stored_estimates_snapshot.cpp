// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

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

namespace stored_estimates_detail {

ojson engineErrorBlockEt(const std::string& message, const std::string& code) {
  return eval::engineError(message, code);
}

// W2 esttabnames (BUG-552): a failed namelist expansion, under Stata's exact
// rc (111 no match, 301 no active `.`, 198 count). The
// `reason` field is the channel that carries an exact rc (BUG-187).
ojson estNamelistError(const SessionState::EstNamelist& ex) {
  return ojson(eval::errorBlock("engine_error", ex.message, std::nullopt, std::nullopt,
                                std::nullopt, "r(" + std::to_string(ex.rc) + ")"));
}

// esttab.py's make_snapshot: extract the essential fields from an
// EstimationResult block. Renames "(intercept)" -> "_cons".
ojson makeSnapshot(const ojson& estBlock) {
  ojson r2 = estBlock.contains("r2_within") ? estBlock["r2_within"] : ojson(nullptr);
  if (r2.is_null() && estBlock.contains("r2")) r2 = estBlock["r2"];

  ojson coeftableClean = ojson::array();
  if (estBlock.contains("coeftable") && estBlock["coeftable"].is_array()) {
    for (const auto& row : estBlock["coeftable"]) {
      ojson r = row;
      if (r.contains("name") && r["name"].is_string() && r["name"].get<std::string>() == "(intercept)") {
        r["name"] = "_cons";
      }
      coeftableClean.push_back(r);
    }
  }

  ojson snap;
  snap["coeftable"] = coeftableClean;
  snap["n_obs"] = estBlock.contains("n_obs") ? estBlock["n_obs"] : ojson(0);
  snap["r2"] = r2;
  snap["depvar"] = estBlock.value("depvar", std::string());
  snap["estimator"] = estBlock.value("estimator", std::string());
  snap["fe"] = estBlock.contains("absorbed") ? estBlock["absorbed"] : ojson(nullptr);
  // W2AK: a multi-outcome fit (mlogit) carries its coefficients per outcome and
  // no top-level coeftable; esttab/estout render one equation per outcome
  // (estout keys rows by equation), so the outcomes travel with the snapshot.
  if (estBlock.contains("outcomes") && estBlock["outcomes"].is_array()) {
    snap["outcomes"] = estBlock["outcomes"];
  }
  return snap;
}

// AER-SCOPE1 bundle E (TABLES): freeze e()'s SCALARS into the snapshot at
// `eststo` time. `esttab ..., stats(N r2_a)` reads these; without them the
// footer could only ever print the two hard-coded rows below, so every
// `stats()` a paper actually writes would render blank.
//
// The name->key map is `eval::eScalarKey()`, the same one `display e(r2_a)`
// resolves through -- deliberately NOT a copy. A key ABSENT from the estimate
// is omitted rather than stored as null, so "the engine does not emit this yet"
// and "this model has no such statistic" stay the same, honest, blank cell.
ojson escalarsFrom(const ojson& estBlock) {
  ojson out = ojson::object();
  for (const auto& [stataName, cobraKey] : eval::eScalarKey()) {
    if (!estBlock.contains(cobraKey)) continue;
    const ojson& v = estBlock[cobraKey];
    // n_clusters 0 is an unclustered fit, which has no e(N_clust) (BUG-1466)
    if (stataName == "N_clust" && v.is_number() && v.get<double>() == 0.0) continue;
    if (v.is_number()) out[stataName] = v;
  }
  return out;
}

ojson emptyAddedBag() {
  return ojson{{"scalars", ojson::object()},
               {"locals", ojson::object()},
               {"matrices", ojson::object()}};
}

}  // namespace stored_estimates_detail

}  // namespace piet::handlers
