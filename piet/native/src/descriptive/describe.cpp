// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/descriptive.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "estimation/estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "piet/strmutate.h"      // BUG-1326: assert's string-condition count
#include "core/exact_sum.h"  // cecon::ExactSum: summarize's accumulation (BUG-1013)
#include "estimators/posthest.h"  // fitted design atoms for named absorbed effects
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_unescape: .dta label text (BUG-1372)
#include "piet/strutil.h"
#include "piet/translate.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// describe

namespace {

// C4-L2B (D1 card plane): the per-variable describe entry, shared by the card
// plane and the datastore plane so the two renders cannot drift — every field
// except n_unique's f64 branch is metadata the resident card already carries
// (dtype vocabulary, n_missing) or session-side meta (stataType/format/label/
// val_label), and the field ORDER here is the frozen wire order.
// format-display-default-lifecycle: `fmt` is the caller's
// SessionState::displayFormat(card entry) answer, the one resolver every reader
// shares (session > the file's own > the declared type's default). describe
// used to keep its own fallback here (`isFactor ? "%9s" : "%9.0g"`), which
// answered %9.0g for a byte and %9s for a str12 (w2p_estim_ev.log:1725/:1827).
ojson describeEntry(const SessionState& session, const std::string& name, bool isFactor,
                    int64_t nMissing, int64_t nUnique, const std::string& fmt) {
  auto metaIt = session.varMeta.find(name);
  std::string stataType;
  if (metaIt != session.varMeta.end() && metaIt->second.stataType.has_value()) {
    stataType = *metaIt->second.stataType;
  } else {
    stataType = isFactor ? "str#" : "float";
  }
  std::string label;
  if (metaIt != session.varMeta.end() && metaIt->second.label.has_value()) {
    label = *metaIt->second.label;
  }

  ojson entry;
  entry["name"] = name;
  entry["dtype"] = stataType;
  entry["format"] = fmt;
  entry["label"] = label;
  entry["n_missing"] = nMissing;

  auto lblIt = session.varLblname.find(name);
  if (lblIt != session.varLblname.end()) entry["val_label"] = lblIt->second;

  entry["n_unique"] = nUnique;
  return entry;
}

}  // namespace

ojson runDescribe(const std::optional<std::vector<std::string>>& varlist, SessionState& session) {
  // C4-L2B (D1 card plane, brief §1 describe row): describe's CARD SHARE.
  // Every field the entry needs is card/session metadata EXCEPT n_unique for
  // an f64 column, which is a capped distinct-count over the column VALUES —
  // no card can answer that, so a described set containing any f64 (or a
  // factor whose level count the card's writer did not capture) falls back
  // WHOLESALE to the datastore plane below, exactly the pre-D1 path (one
  // construct, same bytes). A factor-only set answers entirely off the CURRENT
  // card: presence (a current card is authoritative for absence too — the
  // honest-currency rule), dtype, n_missing, and n_unique == n_levels (the
  // datastore plane's factor n_unique IS levels.size(), Datastore::
  // buildVarInfo, which is the same number the card carries as nLevels).
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::vector<std::string> names;
    if (varlist.has_value()) {
      names = *varlist;
    } else {
      names.reserve(card->size());
      for (const auto& v : *card) names.push_back(v.name);
    }
    bool cardCanAnswer = true;
    ojson variables = ojson::array();
    for (const auto& name : names) {
      const VarInfo* vi = nullptr;
      for (const auto& v : *card) {
        if (v.name == name) {
          vi = &v;
          break;
        }
      }
      if (vi == nullptr) continue;  // authoritative absence — matches !ds->has(name)
      if (vi->dtype != "factor" || !vi->nLevels.has_value()) {
        cardCanAnswer = false;  // f64 n_unique needs column values — fall back
        break;
      }
      variables.push_back(describeEntry(session, name, /*isFactor=*/true, vi->nMissing,
                                        *vi->nLevels, session.displayFormat(*vi)));
    }
    if (cardCanAnswer) {
      ojson out;
      out["type"] = "describe";
      out["variables"] = variables;
      return out;
    }
  }

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  std::vector<std::string> names = varlist.has_value() ? *varlist : ds->names();

  ojson variables = ojson::array();
  for (const auto& name : names) {
    if (!ds->has(name)) continue;
    VarInfo vi = ds->varinfo(name);

    int64_t nUnique;
    if (vi.dtype == "factor") {
      nUnique = static_cast<int64_t>(vi.levels.size());
    } else {
      // Unique-value count over non-missing f64 values, capped at 65536
      // (describe.py parity). O(n log n) via sort — acceptable for the
      // vertical-slice corpus's dataset sizes.
      const cecon::Column& c = ds->col(name);
      const double* d = c.f64();
      int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
      std::vector<double> valid;
      valid.reserve(count);
      for (int64_t i = 0; i < count; ++i) {
        if (!std::isnan(d[i])) valid.push_back(d[i]);
      }
      std::sort(valid.begin(), valid.end());
      valid.erase(std::unique(valid.begin(), valid.end()), valid.end());
      nUnique = static_cast<int64_t>(std::min<size_t>(valid.size(), 65536));
    }
    variables.push_back(describeEntry(session, name, vi.dtype == "factor", ds->nMissing(name),
                                       nUnique, session.displayFormat(vi)));
  }

  ojson out;
  out["type"] = "describe";
  out["variables"] = variables;
  return out;
}

}  // namespace piet::handlers
