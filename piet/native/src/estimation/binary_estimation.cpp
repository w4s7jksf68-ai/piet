// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Binary estimation.
#include "piet/handlers/estimation.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

ojson runLogitProbit(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // FU-585 / BUG-1286: logit, logistic and probit let every weight through the
  // registry gate. Stata takes fweights, iweights and pweights for them
  // (logit.sthlp, probit.sthlp), so an aweight is its own r(101). The engine fits
  // fw and iw under the default and robust VCEs, and logit's fw under
  // vce(cluster) too (StataBE 19.5 `logit d x [fw=wi]` _se[x] .091571881918893761,
  // e(N) 30; probit .056114942772508274); an iweight under vce(cluster), and
  // probit's fweight there, are valid Stata the engine does not fit here.
  if (parsed.weightType.has_value()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt == "aw" || wt == "aweight")
      return ojson(eval::errorBlock("engine_error", "aweights not allowed", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(101)")));
    bool clusterVce = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "cluster" && !v.isBool) clusterVce = true;
      if (k == "vce" && !v.isBool) {
        const std::string t = lowerM4(trimM4(v.strValue));
        clusterVce = clusterVce || t.rfind("cl", 0) == 0;
      }
    }
    const bool iw = wt == "iw" || wt == "iweight";
    const bool fw = wt == "fw" || wt == "fweight";
    if (clusterVce && (iw || (fw && cobraCmd != "logit")))
      return ojson(eval::errorBlock(
          "unsupported_option",
          "`" + cobraCmd + "` [" + *parsed.weightType + "=] with vce(cluster ...) is not supported "
          "in this version of PIET (it fits fweights and iweights under the default and robust "
          "VCEs" + std::string(cobraCmd == "logit" ? ", and fweights under vce(cluster ...)" : "") +
              "); nothing was fitted.",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }
  EstimationArgvResult built = buildEstimationArgvM4(cobraCmd, parsed, session);
  if (built.errored) return built.errorBlock;
  std::vector<std::string> cobraArgs = built.argv;
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  // ME07: the fit writes its own e(sample) (the engine's rows after listwise,
  // weight, cluster AND perfect-prediction-screen losses; cmd_logit/cmd_probit
  // --esample-out), installed by cacheLastModelM4 below -- so `count if
  // e(sample)` is e(N) after a screened fit (w2u_binary_ml_edges.log:199
  // bm_l_dz0_fit `esample=190`) and margins averages over those rows
  // (w2v_margins_siblings.log:3574 F_pp_g `N=234`).
  std::string esampleValues;
  try {
    // eb-order T-P1: the fit's full e(V) (the logit/probit options_table rows
    // carry --emit-v, apps/cmds/cmds_mle.cpp add_e_v). Only the fit call gets
    // the flag; the args the LastModel records (cacheLastModelM4) do not.
    std::vector<std::string> fitArgs = cobraArgs;
    fitArgs.push_back("--emit-v");
    esampleValues = esampleFitArgs(session, fitArgs, std::string(), 0);
    raw = runCobraJsonM4(fitArgs);
  } catch (const CobraError& ce) {
    if (!esampleValues.empty()) cecon::native_unlink(esampleValues);
    // FU-547: the engine's `no_obs` (a string variable in the varlist, or no
    // row left) is Stata's "no observations" r(2000) (w2u_binary_ml_edges.log:
    // 3686-3692 `probit ystr x1`), never the rc 1 of a bare engine_error. One
    // row is r(2001) "insufficient observations" (BUG-1269, StataBE 19.5
    // `logit d x if x == 1`).
    if (ce.code == "no_obs" && std::string(ce.what()) == "insufficient observations")
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    if (ce.code == "no_obs")
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  // FU-471: the reply's `df_m` is e(df_m), read from the cached reply (macros.cpp
  // eScalarKey). The block never carried it for logit/probit, and the shared
  // kOls allowlist would copy it onto every logit/probit block (25 freeze
  // fixtures), so it stays on the reply only -- the e(k) pattern below.
  block.erase("df_m");
  // W:P8: cobra's logit/probit raw JSON carries no "depvar" field at all
  // (live-verified: poisson/tobit/feols all do, these two are the only
  // estimators PIET drives that omit it), so the block published an EMPTY
  // depvar. Two user-visible consequences, both rc 0: `estimates dir` listed
  // the model as "p8_logit  ()", and `estimates table` — whose equation
  // grouping is keyed on exactly this name in Stata (logit's e(b) columns are
  // `ybin:x1`, regress's are bare `x1`) — had nothing to label the group with.
  // The name is the user's own `logit YVAR ...` token echoed back, not an
  // engine-computed quantity, so filling it here is the same honest move the
  // tobit/heckman paths in this file already make for the same engine gap.
  if (block.value("depvar", std::string()).empty() && !parsed.varlist.empty()) {
    block["depvar"] = parsed.varlist[0];
  }
  fillCoeftableStatsM4(block);
  // e(k), the number of parameters (= columns of e(b)): w2b_estim.log:706
  // probit_r_nocons e(k) 2. The engine's logit/probit reply carries no `k`, and
  // an e() name with no key map entry resolves from the cached reply's key of
  // the same name (macros.cpp resolveEScalar), so it is posted on that reply
  // only -- the e(df_a) pattern (linear_estimation.cpp absorbedDofA).
  if (!raw.contains("k") && raw.contains("coefs") && raw["coefs"].is_array()) {
    raw["k"] = raw["coefs"].size();
  }
  if (!raw.is_object() || !raw.contains("esample_n")) {
    cecon::native_unlink(esampleValues);
    esampleValues.clear();
  }
  // BUG-1192: e(cmd) after `logistic` is `logistic` (StataBE 19.5 `logistic yb x1 x2`,
  // whose e() otherwise matches logit's but for e(predict) logistic_p), so it is the
  // e(cmd) word LastModel records; the replays keep keying on cobraCmd "logit".
  cacheLastModelM4(session, cobraCmd, cobraArgs, block, raw, esampleValues,
                   lowerM4(parsed.cmd) == "logistic" ? std::string("logistic") : std::string());
  // FU-30: e(vce) after a default-VCE logit/probit is `oim` -- both are ML
  // estimators, and Stata's ML default VCE is the observed information matrix
  // (tests/eval/golden/aer_w2/w2c_ml_getmata.log:321 ml_pw_vceoim and :356
  // ml_opts_nospace `oim`, the same probit likelihood fitted through `ml`; the
  // engine's default probit SE is the observed-information one already, BUG-140).
  // setLastModel() published lm.vcov's engine spelling `iid`, which the replay
  // machinery keys on, so only the macro store is corrected -- the regress `ols`
  // / xtreg,fe `conventional` pattern. vce(robust)/vce(cluster) keep `robust`/
  // `cluster` with e(vcetype) `Robust` (w2b_estim.log:619 logit_vce_robust).
  if (session.lastModel().has_value() && session.lastModel()->vcov == "iid") {
    session.eval.setEString("vce", "oim");
  }
  // FU-471 / FU-466: e(chi2type) and e(rules). The engine reply carries
  // `chi2type` (LR, or Wald after robust/cluster/noconstant) and `rules`, one
  // row (op, rhs, outcome, #dropped) per perfect predictor the screen dropped,
  // J(1,4,0) when none (logit's ado). e(N_cds)/e(N_cdf)/e(df_m) resolve
  // from the cached reply by name and e(rank) from its `k_vars` key. Both stores
  // are recorded with noteEOnFit so the next model install drops them.
  if (raw.contains("chi2type") && raw["chi2type"].is_string()) {
    session.eval.setEString("chi2type", raw["chi2type"].get<std::string>());
    session.noteEOnFit("chi2type");
  }
  if (raw.contains("rules") && raw["rules"].is_array() && !raw["rules"].empty()) {
    eval::EvalMatrix m;
    const ojson names = raw.value("rules_names", ojson::array());
    size_t r = 0;
    for (const auto& row : raw["rules"]) {
      std::vector<double> cells;
      if (row.is_array())
        for (const auto& c : row) cells.push_back(c.is_number() ? c.get<double>() : 0.0);
      cells.resize(4, 0.0);
      m.data.push_back(std::move(cells));
      m.rownames.push_back(r < names.size() && names[r].is_string()
                               ? names[r].get<std::string>()
                               : "r" + std::to_string(r + 1));
      ++r;
    }
    m.colnames = {"c1", "c2", "c3", "c4"};
    bool replaced = false;
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == "rules") { kv.second = m; replaced = true; }
    if (!replaced) session.eval.eMatrices.emplace_back("rules", std::move(m));
    session.noteEOnFit("rules");
  }
  return block;
}

// ===========================================================================
// csdid / drdid + estat simple/event/calendar/group/pretrend


}  // namespace piet::handlers
