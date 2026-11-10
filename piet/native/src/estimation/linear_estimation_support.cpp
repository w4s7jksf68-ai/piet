// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
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

#include "estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
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
#include "linear_estimation_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// BUG-102: TU-local copy of estimation_support.cpp's substituteFactorLevelLabelM4/
// applyFactorValueLabelsM4 (cobraToEstimation, defined below, is the separate
// M2/regress-only slice — no shared header links the two
// anonymous namespaces, and strutil.h is deliberately dependency-light, so
// this follows the same duplicate-and-keep-in-sync pattern strutil.h's own
// doc comment describes for the TUs it hasn't swept yet). A factor-variable
// coefficient row's engine name is `<level>.<var>` (BUG-101, model.cpp:526),
// or `<level>.<var>#<level>.<var>` for interactions; real Stata substitutes
// the attached value label for the bare level when displaying such a row
// (verified live, StataBE 19.5). PIET's flat one-row-per-coefficient table
// keeps the ".varname" suffix (no shared group-header the way Stata's ASCII
// table has one) — only the level segment is substituted.
std::string substituteFactorLevelLabel(const std::string& name, const SessionState& session) {
  return strutil::substituteFactorLevelLabel(name, session.valueLabels);
}

void applyFactorValueLabels(ojson& coeftable, const SessionState& session) {
  // BUG-397 (2026-09-02, the g12 oracle-first leg's FIX:BUG102-COEFTABLE-NAME-PARITY): the row's
  // `name` is Stata's e(b) colname and is INVARIANT under value labels (`1983.yearpl` stays
  // `1983.yearpl`; `_b[1983.yearpl]` must keep resolving). The label is DISPLAY: it rides on two
  // additive fields — `stub` = the BUG-102 display shape with the level segment substituted
  // (`Odd83.yearpl`), which the frontend prints in place of the name; `label` = the bare label
  // text of the (last) substituted level (`Odd83`), Stata's own stub. Rows without a label get
  // neither field, so every unlabeled fixture is byte-identical.
  if (session.valueLabels.empty() || !coeftable.is_array()) return;
  for (auto& row : coeftable) {
    if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) continue;
    const std::string name = row["name"].get<std::string>();
    std::string sub = substituteFactorLevelLabel(name, session);
    if (sub == name) continue;
    row["stub"] = sub;
    // bare label = the text before the first '.' of the LAST '#'-segment that changed
    std::string lastSeg = sub.substr(sub.find_last_of('#') == std::string::npos ? 0 : sub.find_last_of('#') + 1);
    row["label"] = lastSeg.substr(0, lastSeg.find('.'));
  }
}

}  // namespace

namespace linear_estimation_detail {

// Stata's own refusals, carried with Stata's rc (coarseRc reads the `r(NNN)`
// reason first, so the rc is never the generic 1).
ojson stataRefusalBlock(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

std::string joinComma(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// _cobra_to_estimation (dispatcher.py) — normalise cobra JSON -> EstimationResult.
// M2 slice: does NOT port the margins AME b=null remap (margins isn't in the
// M2 handler set) — pass-through only. Field list and truthy-check semantics
// (`if raw.get("absorbed")` / `if vcov:` — Python empty-dict/empty-string are
// falsy, so an empty `{}`/`""` is DROPPED, not passed through) mirror
// dispatcher.py's "Optional fields — standard pass-through" block exactly.
// `session` optional (default nullptr) mirrors estimation_support.cpp's cobraToEstimationM4 (the M4a copy).
ojson cobraToEstimation(const ojson& raw, const std::string& cobraCmd,
                         const SessionState* session) {
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = raw.value("estimator", cobraCmd);
  block["depvar"] = raw.value("depvar", "");
  block["n_obs"] = raw.value("n_obs", 0);
  if (raw.contains("coeftable")) block["coeftable"] = raw["coeftable"];
  if (raw.contains("coeftable") && session) applyFactorValueLabels(block["coeftable"], *session);
  if (raw.contains("outcomes")) block["outcomes"] = raw["outcomes"];

  // `if k in raw` — presence-only pass-through (no truthiness check).
  static const char* kPassthrough[] = {
      "r2", "r2_within", "sigma2", "deviance", "converged", "iters", "n_clusters",
      "cluster_dof_k", "dof_resid", "first_stage_F", "kp_wald_F", "hansen_j", "hansen_j_p",
      "F", "J", "chi2", "df1", "df2", "p_F", "p_chi2", "restrictions",
      "margin", "at", "margins", "df", "mean_1", "mean_2", "by", "coefs",
      "r", "variables", "p", "nobs", "method", "pairwise", "sig",
      "adj_r2", "rmse", "mss", "rss", "df_m", "df_r", "ll", "ll_0", "r2_pseudo", "df_chi2",
      "r2_between", "r2_overall",
      // BUG-156 (wiring half, P2-PREP): poisson/fepois `chi2_df`/`r2_p` and
      // feols single-FE `sigma_u`/`sigma_e`/`rho`/`corr_u_xb`. `chi2`/`ll`/
      // `ll_0` are already above. This is the TWIN of estimation_support.cpp's
      // kGlm/kXtreg lists — the same allowlist exists in both files and a
      // one-sided edit makes the M2 and M4a paths publish different blocks
      // for the same fit (BUG-158/BUG-161 class). Keep them in step.
      // The engine does not emit these yet; that half of BUG-156 is compute
      // work in a parallel workstream.
      "chi2_df", "r2_p", "sigma_u", "sigma_e", "rho", "corr_u_xb",
  };
  for (const char* k : kPassthrough) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  // BUG-112(8), consumption half — and this is the M2 twin of the identical
  // carry in estimation_support.cpp::cobraToEstimationM4. `regress` routes HERE, not
  // there, so a one-sided landing would have surfaced the engine's note for
  // some estimators and silently dropped it for the most common one (measured:
  // the M4a copy alone left `regress ... ib99.g` still silent). cobra emits
  // non-fatal model-spec notes as a top-level `warnings` array (contract A,
  // absent when empty) — Stata's `note:` line. Absent-when-empty on this side
  // too, so a clean estimation block stays byte-identical.
  if (raw.contains("warnings") && raw["warnings"].is_array() && !raw["warnings"].empty()) {
    block["warnings"] = raw["warnings"];
  }

  // `if raw.get("absorbed"):` — truthy check: an empty {} is dropped.
  if (raw.contains("absorbed") && !raw["absorbed"].empty()) {
    block["absorbed"] = raw["absorbed"];
  }
  // `if vcov:` — truthy check: an empty/absent string is dropped.
  if (raw.contains("vcov_type") && raw["vcov_type"].is_string() &&
      !raw["vcov_type"].get<std::string>().empty()) {
    block["vcov_type"] = raw["vcov_type"];
  }
  return block;
}

}  // namespace linear_estimation_detail

}  // namespace piet::handlers
