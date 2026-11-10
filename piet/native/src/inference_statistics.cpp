// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Inference statistics.
#include "piet/handlers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/stata_format.h"
#include "estimation_internal.h"
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

namespace est_detail {  // R2: reopened — fillCoeftableStatsM4 is declared in TU 1

// nbreg/xtpois/xtlogit's own raw "coeftable" always carries p=ci_lo=ci_hi=
// null (live-verified against all three CLIs — only "b"/"se"/"t" are ever
// populated, t==b/se already). fepois's own coeftable, by contrast, DOES
// compute p/ci itself (also live-verified) so is never routed through this.
// Backfills the null fields from b/se/t the SAME Wald-normal way coefRowM4
// (csdid, above) already does, so the PIET UI doesn't ship a coeftable with
// three permanently-blank columns for these three estimator families.
void fillCoeftableStatsM4(ojson& block) {
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  for (auto& row : block["coeftable"]) {
    if (!row.contains("b") || row["b"].is_null() || !row.contains("se") || row["se"].is_null()) {
      continue;
    }
    double b = row["b"].get<double>();
    double se = row["se"].get<double>();
    double z;
    if (row.contains("t") && row["t"].is_number()) {
      z = row["t"].get<double>();
    } else if (se != 0.0) {
      z = b / se;
      row["t"] = z;
    } else {
      continue;
    }
    if (!row.contains("p") || row["p"].is_null()) row["p"] = normalPvalue(z);
    if (!row.contains("ci_lo") || row["ci_lo"].is_null()) row["ci_lo"] = b - kZ975 * se;
    if (!row.contains("ci_hi") || row["ci_hi"].is_null()) row["ci_hi"] = b + kZ975 * se;
  }
}

}  // namespace est_detail

}  // namespace piet::handlers
