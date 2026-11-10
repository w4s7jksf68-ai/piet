// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Estimation support.
#include "piet/handlers/vce_types.h"
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

namespace est_detail {

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many lowerM4(...)/trimM4(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string lowerM4(std::string s) { return strutil::lower(std::move(s)); }

std::string trimM4(const std::string& s) { return strutil::trim(s); }

ojson engineErrorBlockM4(const std::string& message, const std::string& code) {
  return eval::engineError(message, code);
}

// BUG-569 (review round): the VCE options' value SHAPE, twin of linear_estimation.cpp
// vceOptionShapeError (the reasons are written there). Each VCE loop in this
// file reads `cluster` only when valued and `robust` only as a flag, so a bare
// `cluster`, an empty `cluster()` or a valued `robust(x)` was skipped and the
// model ran iid at rc 0. Measured on the branch binary 2026-09-29:
// `xtreg y x1, fe cluster`, `logit yb x1, cluster`, `probit yb x1, robust(id)`
// and `poisson cnt x1, cluster` each returned rc 0 with e(vce) iid.
//
// ppmlhdfe and ivreghdfe ignore an EMPTY `cluster()` and fit their default VCE
// at rc 0 (w2l_vce_shape_det.log ppmlhdfe_empty_cluster `0|ppmlhdfe|robust|
// Robust|.|180`, :3508 ivreghdfe_empty_cluster `0|ivreghdfe|||.|180`); the
// official ML commands (logit, probit, ivprobit, ivtobit, ...) keep r(198).
static bool emptyClusterIgnoredM4(const std::string& cmd) {
  return cmd == "ppmlhdfe" || cmd == "ivreghdfe";
}

std::optional<std::string> vceOptionShapeErrorM4(const eval::ParsedCommand& parsed) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster" && !v.isBool && trimM4(v.strValue).empty() &&
        emptyClusterIgnoredM4(parsed.cmd))
      continue;
    if (k == "cluster" && (v.isBool || trimM4(v.strValue).empty())) {
      return std::string("option `cluster` requires a variable: cluster(varname)");
    }
    if (k == "robust" && !v.isBool) {
      return std::string("option `robust` takes no argument");
    }
  }
  return std::nullopt;
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// many call sites didn't need touching. Pure dedup, no behavior change here.
std::string cobraErrorStrM4(const CobraError& ce) { return cobraErrorMessage(ce); }

std::string joinCommaM4(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// kZ975: an inline constexpr in estimation_internal.h (R2 — internal-linkage const cannot cross the TU cut)

double normalPvalue(double z) { return std::erfc(std::abs(z) / std::sqrt(2.0)); }

void fillCoeftableStatsM4(ojson& block);

// BUG-102: a factor-variable coefficient row's engine name is `<level>.<var>`
// (BUG-101, model.cpp:526), or `<level>.<var>#<level>.<var>` for interactions.
// Real Stata substitutes the attached value label for the bare level when
// DISPLAYING such a row (verified live against StataBE 19.5: `regress y
// i.state_num` with `label values state_num statelbl` prints "Arizona"/
// "Idaho" rows, not "2.state_num"/"3.state_num") — but that live table also
// relies on a shared "state_num" group header PIET's flat one-row-per-
// coefficient table has no equivalent for (formatOneTable, console.js), so
// the ".varname" suffix is kept here rather than dropped bare: the level
// portion alone is substituted, segment-by-segment across any "#" interaction
// join, matching the scoping note's "rewrite factor-level row names" framing.
std::string substituteFactorLevelLabelM4(const std::string& name, const SessionState& session) {
  return strutil::substituteFactorLevelLabel(name, session.valueLabels);
}

// Applies substituteFactorLevelLabelM4 to every row["name"] in a coeftable
// array in place. A no-op (session.valueLabels empty, or a coeftable that
// isn't an array) costs one map .empty() check — freeze/pangram-safe for the
// overwhelming majority of runs that never attach a value label.
void applyFactorValueLabelsM4(ojson& coeftable, const SessionState& session) {
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
    std::string sub = substituteFactorLevelLabelM4(name, session);
    if (sub == name) continue;
    row["stub"] = sub;
    // bare label = the text before the first '.' of the LAST '#'-segment that changed
    std::string lastSeg = sub.substr(sub.find_last_of('#') == std::string::npos ? 0 : sub.find_last_of('#') + 1);
    row["label"] = lastSeg.substr(0, lastSeg.find('.'));
  }
}

// -- _cobra_to_estimation (dispatcher.py) — full port including the BUG-006
// margins AME b=null remap that linear_estimation_support.cpp's cobraToEstimation explicitly
// does NOT include (that copy is the M2/regress-only vertical slice). This
// is the M4a-complete version used by every command in this file.
// `session` is optional (default nullptr): BUG-102 value-label substitution
// only runs when a caller has one to offer (regression-family estimators);
// callers with no natural SessionState in scope (corr/tabstat) are unchanged.
ojson cobraToEstimationM4(const ojson& raw, const std::string& cobraCmd,
                           const SessionState* session) {
  ojson block;
  block["type"] = "estimation";
  std::string estimator = raw.value("estimator", cobraCmd);
  block["estimator"] = estimator;
  block["depvar"] = raw.value("depvar", "");
  // BUG-165 (W:P5): a two-sample ttest has no single `n_obs` on the engine's
  // wire — it reports the two group counts — so the plain default published
  // 0 observations for a test that used thousands. Sum the groups when they
  // are what the engine gave us; every other estimator is unchanged.
  if (!raw.contains("n_obs") && raw.contains("n_1") && raw.contains("n_2") &&
      raw["n_1"].is_number() && raw["n_2"].is_number()) {
    block["n_obs"] = raw["n_1"].get<long long>() + raw["n_2"].get<long long>();
  } else {
    block["n_obs"] = raw.value("n_obs", 0);
  }

  // BUG-112(8), consumption half. cobra emits non-fatal model-spec notes as a
  // top-level `warnings` array (contract A; `add_warnings`, absent when empty)
  // — Stata's `note:` line, e.g. "99b.rep78 identifies no observations in the
  // sample" when an `ib#.` base level is absent. Until now NOTHING on this side
  // read the key: the success path discards cobra's stderr and no consumer
  // looked for a new JSON field, so the engine half shipped INERT. Carrying it
  // onto the block is the whole fix — a warning produced and never shown is a
  // warning that does not exist, and doctrine I says PIET mirrors what cobra
  // says, nothing less. Optional and absent-when-empty on this side too, so a
  // clean estimation stays byte-identical (freeze: 0 fixtures carry the key).
  if (raw.contains("warnings") && raw["warnings"].is_array() && !raw["warnings"].empty()) {
    block["warnings"] = raw["warnings"];
  }

  ojson coeftable;
  bool hasCoeftable = raw.contains("coeftable");
  if (hasCoeftable) coeftable = raw["coeftable"];

  // BUG-006: margins AME — coeftable has b=null; remap from coefs[].dydx.
  bool allBNull = hasCoeftable && coeftable.is_array() && !coeftable.empty();
  if (allBNull) {
    for (const auto& row : coeftable) {
      if (!row.contains("b") || !row["b"].is_null()) {
        allBNull = false;
        break;
      }
    }
  } else {
    allBNull = false;
  }
  if (hasCoeftable && estimator == "margins" && allBNull) {
    ojson coefsRaw = raw.value("coefs", ojson::array());
    std::map<std::string, ojson> coefsByName;
    for (const auto& c : coefsRaw) {
      if (c.contains("name")) coefsByName[c["name"].get<std::string>()] = c;
    }
    ojson remapped = ojson::array();
    for (const auto& row : coeftable) {
      std::string name = row.value("name", std::string());
      auto it = coefsByName.find(name);
      ojson c = (it != coefsByName.end()) ? it->second : ojson::object();
      ojson dydx = c.contains("dydx") ? c["dydx"] : ojson(nullptr);
      ojson se = (it != coefsByName.end() && c.contains("se")) ? c["se"] : row.value("se", ojson(nullptr));
      ojson z = (it != coefsByName.end() && c.contains("t")) ? c["t"] : row.value("t", ojson(nullptr));
      if (dydx.is_null()) dydx = row.value("b", ojson(nullptr));

      ojson r;
      r["name"] = name;
      std::optional<double> bF, seF, zF, pF, ciLo, ciHi;
      try {
        if (!dydx.is_null()) bF = dydx.get<double>();
        if (!se.is_null()) seF = se.get<double>();
        if (!z.is_null()) zF = z.get<double>();
        if (bF.has_value() && seF.has_value()) {
          if (!zF.has_value() && *seF != 0.0) zF = *bF / *seF;
          if (zF.has_value()) pF = normalPvalue(*zF);
          ciLo = *bF - kZ975 * *seF;
          ciHi = *bF + kZ975 * *seF;
        }
      } catch (...) {
        bF.reset();
      }
      r["b"] = bF.has_value() ? ojson(*bF) : dydx;
      r["se"] = seF.has_value() ? ojson(*seF) : se;
      r["t"] = zF.has_value() ? ojson(*zF) : z;
      r["p"] = pF.has_value() ? ojson(*pF) : ojson(nullptr);
      r["ci_lo"] = ciLo.has_value() ? ojson(*ciLo) : ojson(nullptr);
      r["ci_hi"] = ciHi.has_value() ? ojson(*ciHi) : ojson(nullptr);
      remapped.push_back(r);
    }
    coeftable = remapped;
  }

  if (hasCoeftable) block["coeftable"] = coeftable;
  if (hasCoeftable && session) applyFactorValueLabelsM4(block["coeftable"], *session);
  // BS-065: cobra's `reg3 --tidy` payload carries its per-equation results
  // under "equations" (an array of {depvar, coefs, coeftable} — NOT the
  // {..., r2_within, dof_resid} shape --y-batch commands like feols/iv use
  // for "outcomes") rather than under the key "outcomes" itself. Rather than
  // inventing a second, parallel multi-model block shape for the frontend
  // to learn, this reuses the EXACT SAME block["outcomes"] output shape
  // outcomes already produces (falling back to null for the two per-eq
  // stats reg3 doesn't emit, r2_within/dof_resid — an honest engine-data
  // gap, not a frontend bug: verified live, cobra's reg3 --tidy simply
  // never computes them per-equation) so any existing "iterate
  // block.outcomes[]" frontend renderer picks up reg3 for free.
  if (raw.contains("outcomes") || raw.contains("equations")) {
    const ojson& srcArr = raw.contains("outcomes") ? raw["outcomes"] : raw["equations"];
    ojson outcomes = ojson::array();
    for (const auto& o : srcArr) {
      ojson oo;
      // P7: mlogit's equations are keyed by OUTCOME, not by a dependent variable
      // (there is only one depvar, split across K−1 equations). Falling through
      // to the empty default rendered three unlabeled blocks with no way to tell
      // which outcome each belonged to — the numbers were right and the table was
      // unreadable. The base outcome carries no coefficients by construction, so
      // it is labelled as such rather than shown as an empty equation.
      if (o.contains("outcome") && !o.contains("depvar")) {
        oo["depvar"] = o["outcome"].is_string() ? o["outcome"].get<std::string>()
                                                : o["outcome"].dump();
        oo["outcome"] = o["outcome"];
        if (o.value("base", false)) oo["base"] = true;
      } else {
        oo["depvar"] = o.value("depvar", "");
      }
      oo["coeftable"] = o.value("coeftable", ojson::array());
      if (session) applyFactorValueLabelsM4(oo["coeftable"], *session);
      oo["r2"] = o.contains("r2_within") ? o["r2_within"] : o.value("r2", ojson(nullptr));
      oo["r2_within"] = o.contains("r2_within") ? o["r2_within"] : ojson(nullptr);
      oo["dof_resid"] = o.contains("dof_resid") ? o["dof_resid"] : ojson(nullptr);
      outcomes.push_back(oo);
    }
    block["outcomes"] = outcomes;
  }

  static const char* kPassthrough[] = {
      "r2", "r2_within", "sigma2", "deviance", "converged", "iters", "n_clusters",
      "cluster_dof_k", "dof_resid", "first_stage_F", "kp_wald_F", "hansen_j", "hansen_j_p",
      "F", "J", "chi2", "df1", "df2", "p_F", "p_chi2", "restrictions",
      "margin", "at", "margins", "df", "mean_1", "mean_2", "by", "coefs",
      // BUG-165 (W:P5): ttest's per-group counts and dispersions. The engine
      // has always emitted n_1/n_2/sd_1/sd_2 — Stata prints all four in the
      // group rows (`Obs`, `Std. dev.`) — but they were never forwarded, so
      // the block reached the frontend with only the pooled diff. Worse, the
      // engine emits no `n_obs` key for ttest at all, so cobraToEstimationM4's
      // `raw.value("n_obs", 0)` default published **n_obs: 0** on the wire: a
      // test over 3,822 observations reporting zero. Carrying these lets the
      // n_obs fallback below compute the real total.
      "n_1", "n_2",
      "r", "variables", "p", "nobs", "method", "pairwise", "sig",
      // BS-026 (BETA Phase C batch C1): cobra's `tabstat --tidy` payload
      // carries the actual per-var per-stat table in "stats" (the
      // requested stat-name list, e.g. ["mean","sd","n"]) and "total" (a
      // {varname: {statname: value}} object) plus "vars" (the ordered var
      // list) — none of the three were forwarded, so every tabstat block
      // reached the frontend as an empty shell ({estimator:"tabstat",
      // n_obs, by:null}, "by" already being the only tabstat-relevant key
      // in the original list above). Verified against a live cobra
      // `tabstat --tidy` run: these are cobra's OWN field names (not a
      // frontend field-name mismatch to translate) — the frontend has no
      // tabstat table renderer at all yet regardless (out of scope,
      // piet/frontend/** forbidden this batch), so this fixes the DATA
      // layer; the block now genuinely carries every stat that was asked
      // for.
      "stats", "total", "vars",
  };
  for (const char* k : kPassthrough) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  static const char* kOls[] = {"adj_r2", "rmse", "mss", "rss", "df_m", "df_r"};
  for (const char* k : kOls) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-156 (wiring half, P2-PREP): poisson/fepois fit statistics. `chi2` is
  // already carried by kPassthrough above; `chi2_df` and `r2_p` (Stata's
  // e(r2_p), the pseudo-R2 spelling poisson uses — distinct from the
  // `r2_pseudo` key logit/probit emit) were dropped on the floor, so even
  // once the engine emits them they could not reach a consumer.
  //
  // The engine does NOT emit these keys yet — that is the parallel half of
  // BUG-156 and is engine COMPUTE work. Until it does, `display e(r2_p)`
  // still answers missing; this is the wiring so it stops being TWO fixes.
  static const char* kGlm[] = {"ll", "ll_0", "r2_pseudo", "df_chi2", "chi2_df", "r2_p"};
  for (const char* k : kGlm) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BUG-156 (wiring half, P2-PREP): feols single-FE variance decomposition —
  // Stata's e(sigma_u)/e(sigma_e)/e(rho)/e(corr). Same story as kGlm above:
  // allowlisted now, emitted by the engine in the parallel workstream.
  static const char* kXtreg[] = {"r2_between", "r2_overall", "sigma_u", "sigma_e", "rho",
                                 "corr_u_xb"};
  for (const char* k : kXtreg) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // BS-066: cobra's `wildboot --tidy` payload has NO "coeftable" at all (a
  // single wild-cluster-bootstrap test result, not a coefficient table) —
  // its own field names (verified live) carry the actual answer.
  static const char* kWildboot[] = {
      "beta", "p_equaltail", "p_symmetric", "null_beta", "seed", "reps", "weights", "t0",
      "webb_fallback",
  };
  for (const char* k : kWildboot) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  if (raw.contains("absorbed") && !raw["absorbed"].empty()) block["absorbed"] = raw["absorbed"];
  if (raw.contains("vcov_type") && raw["vcov_type"].is_string() &&
      !raw["vcov_type"].get<std::string>().empty()) {
    block["vcov_type"] = raw["vcov_type"];
  }
  return block;
}

// -- shared vcov-option parsing (translate.py's _vcov_from_opts) -----------
// Returns the --vcov[/--cluster] argv tail; throws a std::string error
// message (caller wraps into unsupported_option) on a malformed vce().
// struct VcovResult: defined in estimation_internal.h (R2 split)

// BS-067 (BETA D1): `vce(cluster A B)` / legacy `cluster(A B)` naming TWO
// space-separated variables -> `--vcov twoway --cluster A,B` (cobra's
// feols/iv/fepois --vcov surface accepts "twoway" as a 4th mode alongside
// iid/hc1/cluster — live-verified against the D1i reghdfe 2-way oracle:
// `reghdfe income beertax, absorb(state) vce(cluster state year)` ->
// beertax se=1867.524 matches byte-for-byte through this exact
// --vcov twoway --cluster state,year shape). One var -> the pre-existing
// single-cluster path (unchanged). Three+ vars -> `--vcov multiway --cluster
// A,B,C` where `allowMultiway` (the callers whose k-way fit is measured, see
// vcovFromOpts), else a named refusal.
struct ClusterSplitM4 {
  std::vector<std::string> argv;  // {"--vcov", TYPE, "--cluster", VARS}
  bool errored = false;
  std::string errorMsg;
};

ClusterSplitM4 splitClusterVarsM4(const std::string& raw, bool allowMultiway = false) {
  ClusterSplitM4 r;
  std::vector<std::string> toks;
  std::istringstream iss(raw);
  std::string t;
  while (iss >> t) toks.push_back(t);
  if (toks.empty()) {
    r.errored = true;
    r.errorMsg = "cluster variable required";
    return r;
  }
  if (toks.size() == 1) {
    r.argv = {"--vcov", "cluster", "--cluster", toks[0]};
  } else if (toks.size() == 2) {
    r.argv = {"--vcov", "twoway", "--cluster", toks[0] + "," + toks[1]};
  } else if (allowMultiway) {
    std::string joined = toks[0];
    for (size_t i = 1; i < toks.size(); ++i) joined += "," + toks[i];
    r.argv = {"--vcov", "multiway", "--cluster", joined};
  } else {
    r.errored = true;
    r.errorMsg =
        "three or more cluster variables are not supported for this command in this "
        "version of PIET; multiway clustering with three or more cluster variables is "
        "implemented for reghdfe, xtreg, fe and ppmlhdfe only";
  }
  return r;
}

// The M4 commands whose k-way (k >= 3) cluster VCE is measured against StataBE 19.5:
// xtreg, fe under the built-in multiway convention (BUG-1290: vce(cluster cg1 cg2
// cg3) _se[x1] .019191881847891872, e(df_r) 2, e(N_clust) .) and ppmlhdfe under
// reghdfe's (BUG-1189: vce(cluster id id2 id3) e(N_clust) 5, _se[x1]
// .018284790750361792). The engine's two-way and multiway modes are one
// (apps/cmds/common.cpp Vcov::Multiway), over every listed variable.
static bool multiwayClusterMeasuredM4(const std::string& cmd) {
  return cmd == "xtreg" || cmd == "ppmlhdfe";
}

static bool vceTrailingTokenAcceptedM4(const std::string& cmd) {
  return cmd == "reghdfe" || cmd == "ppmlhdfe" || cmd == "fepois" || cmd == "feols" ||
         cmd == "csdid" || cmd == "ivreg2" || cmd == "xtivreg2";
}

// Stata's own refusals, carried with Stata's rc (coarseRc reads the `r(NNN)`
// reason first, so the rc is never the generic 1).
static ojson stataRefusalBlockM4(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// FU-147 / FU-160, twin of linear_estimation.cpp clusterSpecRefusal for the callers of
// buildEstimationArgvM4 (xtreg, logit, probit, ...) and, BUG-1278, of the m4b
// fits that read vce() themselves (ivregress, poisson/ppmlhdfe, nbreg): what
// the fit's own option parser refuses about its cluster variables, before any
// engine call, in that parser's order. Both options are read, whichever one
// vcovFromOpts would pick (BUG-1278: `poisson y x, vce(cluster k) cluster(nosuch)`
// fitted at rc 0). StataBE 19.5, 2026-11-03, over logit/probit/xtreg/poisson/
// nbreg/ivregress/ppmlhdfe with vce(cluster), cluster() and both:
//  - kVceParse (_vce_parse): with `multiwayVceRefused`, vce(cluster a b) is
//    "multiway cluster VCE not allowed" r(198) before anything is looked up;
//    then cluster()'s names, "variable V not found" + "(error in option
//    cluster())" r(111); with `legacyOneVar`, a second cluster() name "cluster():
//    too many variables specified" r(103); then vce(cluster)'s names, r(111)
//    without the tail; then the pairing: vce(robust) with cluster() "only one of
//    options cluster() or vce(robust) is allowed" r(198), vce(cluster a) with
//    cluster(b) "options cluster(clustvar) and vce(cluster clustvar) are in
//    conflict" r(100) (the same variable in both is accepted).
//  - kIvregress: cluster()'s names and r(103) as above, then any vce() with
//    cluster() "cannot specify options robust or cluster() with option vce()"
//    r(198), then vce(cluster)'s names, which its vce() parser turns into
//    "option vce() incorrectly specified" r(198).
//  - kPpmlhdfe: vce() with cluster() "only one of cluster() and vce() can be
//    specified" r(198) first; a name is r(111) with no cluster() tail.
std::optional<ojson> clusterSpecRefusalM4(const eval::ParsedCommand& parsed, SessionState& session,
                                          ClusterOptParserM4 parser, bool multiwayVceRefused,
                                          bool legacyOneVar) {
  std::optional<std::string> vceVal;
  std::optional<std::string> clusterOpt;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "cluster" && !v.isBool) {
      clusterOpt = v.strValue;
    }
  }
  auto tokens = [](const std::string& raw) {
    std::vector<std::string> toks;
    std::istringstream iss(raw);
    std::string t;
    while (iss >> t) toks.push_back(t);
    return toks;
  };
  std::string vceType;
  std::string vceRest;
  if (vceVal.has_value() && !vceVal->empty()) vceType = canonicalVceType(*vceVal, &vceRest);
  const bool both = !vceType.empty() && clusterOpt.has_value();
  const std::vector<std::string> legacyToks = tokens(clusterOpt.value_or(""));
  // vce(cluster) with no variable of its own takes cluster()'s (vcovFromOpts).
  const std::vector<std::string> vceToks =
      vceType == "cluster" && !vceRest.empty() ? tokens(vceRest) : std::vector<std::string>();
  if (parser == ClusterOptParserM4::kPpmlhdfe && both)
    return stataRefusalBlockM4("only one of cluster() and vce() can be specified", 198);
  if (multiwayVceRefused && vceToks.size() >= 2)
    return stataRefusalBlockM4("multiway cluster VCE not allowed", 198);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return std::nullopt;
  // BUG-1437: the dispatcher has already put a unique abbreviation's full name
  // in; a name that begins several variables is "NAME ambiguous abbreviation"
  // where one that begins none is "variable NAME not found", with the same rc and
  // tail (StataBE 19.5, 2026-11-03, over the seven fits above).
  auto notFound = [ds](const std::string& n) {
    int hits = 0;
    for (const auto& v : ds->names())
      if (v.size() > n.size() && v.compare(0, n.size(), n) == 0) ++hits;
    return hits > 1 ? n + " ambiguous abbreviation" : "variable " + n + " not found";
  };
  for (const auto& n : legacyToks) {
    if (ds->has(n)) continue;
    return stataRefusalBlockM4(
        notFound(n) +
            (parser == ClusterOptParserM4::kPpmlhdfe ? "" : "\n(error in option cluster())"),
        111);
  }
  if (legacyOneVar && legacyToks.size() >= 2)
    return stataRefusalBlockM4("cluster():  too many variables specified", 103);
  if (parser == ClusterOptParserM4::kIvregress && both)
    return stataRefusalBlockM4("cannot specify options robust or cluster() with option vce()", 198);
  for (const auto& n : vceToks) {
    if (ds->has(n)) continue;
    if (parser == ClusterOptParserM4::kIvregress)
      return stataRefusalBlockM4(notFound(n) + "\noption vce() incorrectly specified", 198);
    return stataRefusalBlockM4(notFound(n), 111);
  }
  if (parser == ClusterOptParserM4::kVceParse && both) {
    if (vceType == "robust")
      return stataRefusalBlockM4("only one of options cluster() or vce(robust) is allowed", 198);
    if (vceType == "cluster" && !vceToks.empty() && vceToks != legacyToks)
      return stataRefusalBlockM4(
          "options cluster(clustvar) and vce(cluster clustvar) are in conflict", 100);
  }
  return std::nullopt;
}

ojson vcovErrorBlockM4(const VcovResult& vr) {
  if (vr.rc != 0) return stataRefusalBlockM4(vr.errorMsg, vr.rc);
  return engineErrorBlockM4(vr.errorMsg, "unsupported_option");
}

VcovResult vcovFromOpts(const eval::ParsedCommand& parsed, bool* sawRobustFlag) {
  VcovResult out;
  // ppmlhdfe's own option parser (ppmlhdfe.ado's parse_vce through
  // _assert_abort) refuses a bare `cluster` with r(3498) "Invalid options:
  // cluster" (w2l_vce_shape_det.log:2819 ppmlhdfe_bare_cluster).
  if (parsed.cmd == "ppmlhdfe") {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "cluster" && v.isBool) {
        out.errored = true;
        out.errorMsg = "Invalid options: cluster";
        out.rc = 3498;
        return out;
      }
    }
  }
  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    out.errored = true;
    out.errorMsg = *shapeErr;
    return out;
  }
  std::optional<std::string> vceVal;
  std::optional<std::string> clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "robust" && v.isBool && v.boolValue) {
      robustFlag = true;
    } else if (k == "cluster" && !v.isBool) {
      clusterOpt = trimM4(v.strValue);
    }
  }
  if (sawRobustFlag) *sawRobustFlag = robustFlag;
  if (vceVal.has_value() && !vceVal->empty()) {
    // W1b: `vce()`'s argument abbreviates -- ONE canonicaliser for all three
    // sites (piet/handlers.h `canonicalVceType`), measured floors.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    // FU-117: the token after a non-cluster vce type was dropped (HC1 at rc 0).
    // Official commands refuse it as Stata's _vce_parse does
    // (w2l_vce_shape.log:407 regress, :764 areg, :3580 ivreghdfe: r(198)); the
    // community commands that reach this helper accept it (:2972 ppmlhdfe,
    // :4034 reghdfe, csdid :2471), so they are exempt by command word.
    if (!vceRest.empty() && (lv == "robust" || lv == "hc1" || lv == "iid") &&
        !vceTrailingTokenAcceptedM4(parsed.cmd)) {
      out.errored = true;
      out.errorMsg = "option vce() incorrectly specified: `" + vceRest + "` cannot follow vce(" +
                     lv + ")";
      return out;
    }
    if (lv == "robust" || lv == "hc1") {
      out.argv = {"--vcov", "hc1"};
    } else if (lv == "cluster") {
      std::string cvar = clusterOpt.value_or("");
      if (!vceRest.empty()) cvar = vceRest;
      if (cvar.empty() && parsed.cmd == "ppmlhdfe") {
        // reghdfe's parse_vce, which ppmlhdfe shares: r(9)
        // (w2l_vce_shape_det.log:2893 ppmlhdfe_vce_cluster_novar).
        out.errored = true;
        out.errorMsg = "Can't specify cluster without clustervars (and viceversa)";
        out.rc = 9;
        return out;
      }
      if (cvar.empty()) {
        out.errored = true;
        out.errorMsg = "Option `vce(cluster)` is not supported in this version of PIET. "
                        "Cluster variable required: vce(cluster varname)";
        return out;
      }
      ClusterSplitM4 cs = splitClusterVarsM4(cvar, multiwayClusterMeasuredM4(parsed.cmd));
      if (cs.errored) {
        out.errored = true;
        out.errorMsg = cs.errorMsg;
        return out;
      }
      out.argv = cs.argv;
    } else if (lv == "iid") {
      out.argv = {"--vcov", "iid"};
    } else if (!unimplementedVceType(*vceVal).empty()) {
      out.errored = true;
      out.errorMsg = "Option `vce(" + unimplementedVceType(*vceVal) +
                     ")` is a valid Stata option that this version of PIET does not implement";
      return out;
    } else {
      out.errored = true;
      out.errorMsg = "Unknown vce type: " + *vceVal;
      return out;
    }
  } else if (clusterOpt.has_value() && !clusterOpt->empty()) {
    // BUG-055 / P2: Stata's legacy `cluster(varname)` — with neither vce()
    // nor robust given — is a synonym for `vce(cluster varname)`: it
    // implies robust-cluster VCE, not iid. Before this fix, `clusterOpt`
    // was parsed (above) but only ever consulted from inside the
    // `vce(cluster ...)` branch, so a bare `cluster(v)` fell through both
    // `if`s and silently ran iid (v wholly ignored) for every caller of
    // this shared helper (xtreg fe/re, logit, probit). Oracle
    // P2_legacycluster (mirrors linear_estimation.cpp::runRegress's identical fix).
    // BUG-537 (W2 estimopts): tested BEFORE the bare-`robust` branch, which
    // used to win and drop the cluster (HC1, N_clust 0) whenever both were
    // typed -- `xtreg ..., robust fe cluster(firmID)` x12 in one corpus
    // file. Stata's cluster wins: w2_pweight.log R5_pw_robust_AND_cluster_
    // equals_R4 is byte-identical to R4. Twin ordering in runRegress.
    ClusterSplitM4 cs = splitClusterVarsM4(*clusterOpt, multiwayClusterMeasuredM4(parsed.cmd));
    if (cs.errored) {
      out.errored = true;
      out.errorMsg = cs.errorMsg;
      return out;
    }
    out.argv = cs.argv;
  } else if (robustFlag) {
    out.argv = {"--vcov", "hc1"};
  }
  return out;
}

// build_estimation_argv (translate.py) — shared skeleton for xtreg/logit/
// probit's argv construction (fe/absorb/vcov/noconstant/level/TS-injection).
// `extraFe` mirrors the xtreg,fe router's `extra={"fe": panelvar}` override.
// struct EstimationArgvResult: defined in estimation_internal.h (R2 split)

// W:P5 fit-sample fidelity (BUG-163). The fit argv is ground truth for what
// restriction/weights the engine actually estimated under — scan it once at
// LastModel-caching time so every post-estimation refit can replay the SAME
// sample instead of silently answering from the full dataset. Called at every
// `lm.dataPath = ...` site in this file (linear_estimation.cpp's runRegress sets
// lm.fitDataPath directly instead — it materializes its if/in filter into a
// scratch file rather than passing --keepif).
void captureFitFilterM4(LastModel& lm, const std::vector<std::string>& argv) {
  for (size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--keepif") lm.fitKeepif = argv[i + 1];
    if (argv[i] == "--weight") lm.fitWeight = argv[i + 1];
    if (argv[i] == "--wtype") lm.fitWtype = argv[i + 1];
  }
}

EstimationArgvResult buildEstimationArgvM4(const std::string& cobraCmd,
                                            const eval::ParsedCommand& parsed, SessionState& session,
                                            const std::optional<std::string>& extraFe) {
  EstimationArgvResult res;
  if (parsed.varlist.empty()) {
    res.errored = true;
    res.errorBlock = engineErrorBlockM4("syntax_error: no variables specified", "syntax_error");
    return res;
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {cobraCmd, "--y", y};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  } else {
    argv.push_back("--x");
    argv.push_back("(intercept)");
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // --fe: extraFe (xtreg,fe router) OR absorb() option.
  std::string fe = extraFe.value_or("");
  if (fe.empty()) {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "absorb" && !v.isBool) fe = v.strValue;
    }
  }
  if (!fe.empty()) {
    std::string joined;
    std::string cur;
    for (char c : fe) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) {
          if (!joined.empty()) joined += ",";
          joined += cur;
          cur.clear();
        }
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) {
      if (!joined.empty()) joined += ",";
      joined += cur;
    }
    if (!joined.empty()) {
      argv.push_back("--fe");
      argv.push_back(joined);
    }
  }

  // xtreg's _vce_parse takes vce(cluster a b) on to its own panel checks.
  if (auto bad = clusterSpecRefusalM4(parsed, session, ClusterOptParserM4::kVceParse,
                                      /*multiwayVceRefused=*/parsed.cmd != "xtreg")) {
    res.errored = true;
    res.errorBlock = *bad;
    return res;
  }
  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    res.errored = true;
    res.errorBlock = vcovErrorBlockM4(vr);
    return res;
  }
  // P3 / BUG-056: cobra's logit/probit estimator does not implement
  // cluster-robust VCE. Reject HONESTLY here — whether the user asked via
  // `vce(cluster ...)` or the legacy `cluster()` synonym (BUG-055, above,
  // now maps to the exact same "--vcov cluster" shape in vr.argv for every
  // caller of this shared builder) — instead of forwarding a flag cobra
  // would otherwise have to silently drop or bounce as an unknown-flag
  // error (engine E6 net). vr.argv is always exactly {"--vcov", TYPE, ...}
  // (see every branch of vcovFromOpts above), so this check is precise.
  // BS-067: also catches "twoway" (a 2-var cluster now that
  // splitClusterVarsM4 exists) — logit/probit never gained cluster-robust
  // VCE of any shape, so a `vce(cluster A B)` must reject the SAME way a
  // single-var `vce(cluster A)` always has, not slip past this guard just
  // because vr.argv[1] reads "twoway" instead of "cluster".
  // W:P5: single-variable vce(cluster) is now IMPLEMENTED for logit/probit
  // (weighted/unweighted score sandwich in the engine, src/estimators/logit),
  // so the blanket reject above narrows to the shape that is still missing:
  // two-way clustering, which no MLE path in this engine computes. Keeping
  // the reject for "twoway" is the honest half — a silent fallback to
  // one-way would answer a different question.
  // cloglog (BUG-1169/1170) is the third binary ML verb and shares their CLI.
  const bool binaryMl = cobraCmd == "logit" || cobraCmd == "probit" || cobraCmd == "cloglog";
  if (binaryMl && vr.argv.size() >= 2 &&
      vr.argv[0] == "--vcov" && vr.argv[1] == "twoway") {
    res.errored = true;
    res.errorBlock = engineErrorBlockM4(
        "vce(cluster A B) — two-way clustering is not supported for logit/probit in this engine; "
        "one-way vce(cluster A) is",
        "unsupported_option");
    return res;
  }
  // BUG-536 (W2 estimopts): a probability weight IMPLIES a robust VCE in
  // Stata -- w2_pweight.log R2_pw is byte-identical to R3 (`vce(robust)`),
  // and X4_pw_fe to X5 (the xtreg,fe rewrite below turns this hc1 into the
  // panel cluster). Twin of runRegress's rule: only when no vce()/robust/
  // cluster() chose one. logit's engine refused a bare pweight outright
  // ("requires vce(robust)", rc 1) where Stata runs it robust at rc 0.
  if (vr.argv.empty() && parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    if (wt == "pw" || wt == "pweight") vr.argv = {"--vcov", "hc1"};
  }
  for (const auto& a : vr.argv) argv.push_back(a);

  // W:P5: forward the weight, normalized to cobra's 2-letter --wtype
  // spelling — the same shape linear_estimation.cpp::runRegress has used since P3.
  // The tokenizer's registry gate (registry.cpp WeightPolicy) already
  // decided WHICH types are legal for this command, so any weight reaching
  // here is one the command declares support for; an engine that cannot
  // honour it surfaces its own error through the ordinary CobraError catch.
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    std::string wtype;
    if (wt == "aw" || wt == "aweight") wtype = "aw";
    else if (wt == "fw" || wt == "fweight") wtype = "fw";
    else if (wt == "iw" || wt == "iweight") wtype = "iw";
    else if (wt == "pw" || wt == "pweight") wtype = "pw";
    if (!wtype.empty()) {
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
    }
  }

  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      argv.push_back("--no-intercept");
    }
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      try {
        int lv = std::stoi(v.strValue);
        if (lv < 1 || lv > 99) {
          res.errored = true;
          res.errorBlock = engineErrorBlockM4("level(#) must be an integer between 1 and 99.",
                                              "unsupported_option");
          return res;
        }
        argv.push_back("--level");
        argv.push_back(std::to_string(lv));
      } catch (...) {
        res.errored = true;
        res.errorBlock = engineErrorBlockM4("level(#) requires an integer argument, e.g. level(90).",
                                            "unsupported_option");
        return res;
      }
    }
  }
  // BS-048 (BETA A4) — NOT extended here: verified that cobra's own
  // xtreg/logit/probit CLIs never read --level at all (grepped every
  // apps/cmds/*.cpp `opt(a, "--level", ...)` site — only qreg/sdtest/ttest/
  // mediate do, and only ttest is wired into PIET's dispatch surface today;
  // runTtest carries the session.level fallback instead). Sending --level
  // to a command that never consumes it trips cobra's E6 unconsumed-flag
  // net (apps/cmds/common.cpp::check_unconsumed_flags) and turns an
  // otherwise-successful estimation into a hard error — so a session.level
  // fallback HERE would make an ordinary `set level 90` + subsequent
  // xtreg/logit/probit call newly error where it worked before (a
  // regression this fix must not introduce). The pre-existing explicit
  // `level(#)` handling above has the SAME latent gap (untested by any
  // freeze fixture — no `level(...)` usage anywhere in debug_all.do) but is
  // OUT of this phase's 4-mechanism scope to fix; flagged for the operator.

  // P4 / BUG-060: unknown-option rejection net for xtreg(fe/re)/logit/
  // probit. Known-option set is the UNION of what THIS shared builder
  // consumes (absorb/vce/vcov/robust/cluster/noconstant/nocons/level,
  // above) and what runXtreg's caller-side checks already consume BEFORE
  // invoking this builder (fe/re model-select; be/gls/mle/ec1/pa reject) —
  // one shared, deliberately-permissive net rather than a per-caller list,
  // so an option one caller legitimately handles is never misflagged as
  // leftover for another caller of this same builder (conservative by
  // design: false-accept is safe here, false-reject is not — see the
  // BUG-060 report for the full over-rejection sweep list). No curated
  // "valid-Stata-but-gap" bucket here (unlike regress's `beta`): no
  // evidenced case yet, so every leftover reads as the r(198)-parity
  // "not allowed" message.
  static const std::set<std::string> kEstimationKnownOpts = {
      "absorb", "vce", "vcov", "robust", "cluster", "noconstant", "nocons",
      "level",  "fe",  "re",   "be",     "gls",     "mle",        "ec1",  "pa",
  };
  // BUG-333: `or` (odds-ratio display) is logit-ONLY. It is gated on cobraCmd
  // rather than added to the shared set above, because this builder also serves
  // probit/xtreg/etc. — widening the shared set would manufacture an
  // over-acceptance on the other commands. `or` is consumed in the dispatcher
  // (display metric, never reaches the engine).
  const bool logitOnlyOk = (cobraCmd == "logit");
  // FU-425: Stata's maximize options, on logit AND probit: both pass them
  // to the shared ML-options parser, whose syntax line
  // is ITERate(int) TOLerance(real) LTOLerance(real) NRTOLerance(real)
  // DIFficult; the capitalised stem is the minimum abbreviation. BUG-333 had
  // gated `iterate` on logit alone, so `probit ..., iterate(300)` was r(198)
  // where Stata runs it (w2e_probes.log:412 lp_probit_tight_ctl, rc 0). Gated on
  // the two commands, not the shared set, for the reason `or` is: xtreg and the
  // other callers of this builder must not start accepting them. Forwarded below
  // under the engine's spellings (cmds_mle.cpp parse_ml_max_options).
  const bool mlMaxOk = binaryMl;
  struct MlMaxOpt { const char* name; size_t minLen; bool valued; };
  static const MlMaxOpt kMlMaxOpts[] = {{"iterate", 4, true},     {"tolerance", 3, true},
                                        {"ltolerance", 4, true},  {"nrtolerance", 5, true},
                                        {"difficult", 3, false}};
  // The canonical maximize option `k` spells (nullptr when none), and only in
  // its own shape: a valued one with a value, `difficult` bare.
  auto mlMaxOptOf = [&](const std::string& k, const eval::OptValue& v) -> const MlMaxOpt* {
    if (!mlMaxOk) return nullptr;
    for (const MlMaxOpt& o : kMlMaxOpts) {
      const std::string name = o.name;
      if (k.size() >= o.minLen && k.size() <= name.size() && name.compare(0, k.size(), k) == 0)
        return (o.valued ? !v.isBool : (v.isBool && v.boolValue)) ? &o : nullptr;
    }
    return nullptr;
  };
  // BUG-331: `offset()` is a logit/probit option in Stata (StataBE 19.5 oracle,
  // 2026-08-17) and the engine implements it as of cobra 0.42.0. Gated on the two
  // commands that have it rather than widened into the shared set, for the same
  // reason `or`/`iterate` are: this builder also serves xtreg and friends.
  const bool offsetOk = (cobraCmd == "logit" || cobraCmd == "probit");
  // FU-468: `asis` keeps perfect predictors and their rows (logit's help,
  // probit's help); a flag, forwarded as the engine's --asis below.
  const bool asisOk = offsetOk || cobraCmd == "cloglog";  // cobra cloglog has no --offset
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (kEstimationKnownOpts.count(k)) continue;
    if (logitOnlyOk && k == "or") continue;
    if (mlMaxOptOf(k, v) != nullptr) continue;
    if (offsetOk && k == "offset") continue;
    if (asisOk && k == "asis" && v.isBool) continue;
    res.errored = true;
    res.errorBlock = engineErrorBlockM4("option `" + k + "` not allowed", "unsupported_option");
    return res;
  }

  // GLM-style: cobra's logit/probit CLI spells its VCE selector `--vce`, not
  // the `--vcov` every linear estimator uses, and names the robust shape
  // "robust" rather than "hc1". W:P5 widens this remap from robust-only to
  // cluster as well — the engine now implements clustered MLE VCE, and
  // leaving `--vcov cluster` unmapped made a supported combination fail as an
  // unrecognized flag. `--cluster VAR` rides along unchanged (same spelling
  // on both CLIs).
  if (binaryMl) {
    for (size_t i = 0; i < argv.size(); ++i) {
      if (argv[i] != "--vcov" || i + 1 >= argv.size()) continue;
      if (argv[i + 1] == "hc1") {
        argv[i] = "--vce";
        argv[i + 1] = "robust";
      } else if (argv[i + 1] == "cluster") {
        argv[i] = "--vce";
      }
    }
  }

  // TS-context injection (MI-083 §16) — same helper regress uses.
  std::vector<std::string> tsTokens = {y};
  for (const auto& x : xs) tsTokens.push_back(x);
  if (translate::hasTsToken(tsTokens)) {
    argv = translate::injectTsContext(argv, session);
  }

  // BUG-333 / FU-425: forward the maximize options (logit and probit) as
  // --iterate/--tolerance/--ltolerance/--nrtolerance/--difficult. This MUST push
  // onto the local `argv`, not onto res.argv — the assignment below overwrites
  // res.argv wholesale, which silently swallowed the first version of this
  // forward (measured: `logit ..., iterate(2)` returned the un-truncated default
  // fit). The first spelling of each option wins, as the --iterate forward did.
  // The engine echoes them (`ml_max_argv`) and every post-estimation refit
  // carries them (postestimation/postestimation_context.cpp pushFitAsisM4), so lincom/test/margins
  // answer from the posted iterate (FU-426).
  // A value the engine's parse_ml_max_options would refuse (exit 2, which this
  // layer reports as rc 1) is refused here by name at r(198) instead: a
  // tolerance that is not a nonnegative number, an iterate() outside the
  // engine's [1, 16000]. No captured record covers these values (Stata's
  // mlopts takes iterate(0) and any real tolerance), so this is a refusal of a
  // shape this build does not run, not a claim about Stata's answer.
  // BUG-872: the value is a decimal literal parsed whole (no hex, inf or nan),
  // and iterate() is forwarded as the integer it parsed to, never as typed: the
  // engine reads --iterate as a plain integer, so `iterate(1e3)` forwarded
  // verbatim used to run ONE iteration at rc 0 (e(ic) 1, e(converged) 0).
  if (mlMaxOk) {
    std::set<std::string> sent;
    for (const auto& [k, v] : parsed.opts) {
      const MlMaxOpt* o = mlMaxOptOf(k, v);
      if (o == nullptr || !sent.insert(o->name).second) continue;
      if (o->valued) {
        const std::string val = strutil::trim(v.strValue);
        char* end = nullptr;
        const double d = std::strtod(val.c_str(), &end);
        const bool decimal = val.find_first_not_of("0123456789.eE+-") == std::string::npos;
        const bool parsed_all =
            !val.empty() && decimal && end != nullptr && *end == '\0' && std::isfinite(d);
        const bool isIterate = std::string(o->name) == "iterate";
        // iterate(0) reports the starting values (BUG-1244: StataBE e(converged) 0,
        // e(ic) 0, the constant-only start; the engine takes --iterate 0).
        const bool ok = parsed_all && (isIterate ? (d == std::floor(d) && d >= 0 && d <= 16000) : d >= 0);
        if (!ok) {
          res.errored = true;
          res.errorBlock = engineErrorBlockM4(
              std::string("option ") + o->name + "() " +
                  (isIterate ? "must be an integer from 0 to 16000 in this build"
                             : "must be a nonnegative number in this build") +
                  " (got `" + v.strValue + "`)",
              "unsupported_option");
          return res;
        }
      }
      argv.push_back(std::string("--") + o->name);
      if (o->valued) {
        const std::string val = strutil::trim(v.strValue);
        argv.push_back(std::string(o->name) == "iterate"
                           ? std::to_string(static_cast<int>(std::strtod(val.c_str(), nullptr)))
                           : val);
      }
    }
  }

  // BUG-331: forward `offset(VAR)`. Same local-`argv` rule as --iterate above —
  // res.argv is overwritten wholesale below, so pushing onto res.argv here would
  // silently swallow the flag (the defect BUG-333's comment records).
  if (offsetOk) {
    for (const auto& [k, v] : parsed.opts)
      if (k == "offset" && !v.isBool) {
        argv.push_back("--offset");
        argv.push_back(v.strValue);
        break;
      }
  }
  if (asisOk) {
    for (const auto& [k, v] : parsed.opts)
      if (k == "asis" && v.isBool && v.boolValue) {
        argv.push_back("--asis");
        break;
      }
  }

  res.argv = argv;
  return res;
}

// P6: the ONE choke point every cobra invocation built in this file passes
// through, so the persisted `provenance_hash` setting (settings.h/.cpp)
// applies uniformly across every M4a estimator and post-estimation command
// without a per-call-site decision. Interactive default is OFF
// (Settings::provenanceHash = false) -> append `--no-provenance` (VERIFIED
// E6-net-safe: cobra consumes it, never rejects it, and simply omits its
// `provenance{}` block from the result JSON). The user opting IN
// (provenanceHash = true) appends nothing, so cobra's own CLI default
// (provenance ON) is unchanged — this is a PIET-native default flip only,
// never the engine's.
nlohmann::json runCobraJsonM4(std::vector<std::string> args) {
  if (!piet::settings::load().provenanceHash) {
    args.push_back("--no-provenance");
  }
  return runCobraJson(args);
}

std::string nextPathM4(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BUG-042: shared if-prefilter for tabstat/correlate/ttest — none of the
// three has a --keepif of its own on cobra's CLI (checked against each
// subcommand's own usage banner), so this mirrors descriptive/summarize.cpp's
// runSummarize if-prefilter (the SAME row-filter infra: `cobra eval
// --keepif COND --out tmp.hbin`, then point the real call at the filtered
// copy instead of the live hbin). No ifExpr/inRange -> returns hbinPath
// unchanged, zero behavior change on the unqualified path (freeze-compatible).
// On a cobra error, returns nullopt and leaves the CommandErrorResult block
// in `errBlockOut` for the caller to return directly.
// `inRange` (BS-090, BETA Phase G, default nullopt): an optional `in a/b`
// row-range, composed with `ifExpr` via AND through
// piet::handlers::applyIfAndInFilter — only ttest/correlate/pwcorr pass a
// real value here (registry.cpp's allowInRange); tabstat/tobit's existing
// call sites are untouched (still pass no 5th argument), so their behavior
// is byte-identical to before this phase.
std::optional<std::string> applyIfFilterM4(SessionState& session, const std::string& hbinPath,
                                           const std::optional<std::string>& ifExpr,
                                           ojson& errBlockOut,
                                           const std::optional<std::string>& inRange) {
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (!haveIf && !haveIn) return hbinPath;
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, hbinPath, ifExpr, inRange);
  } catch (const CobraError& ce) {
    errBlockOut = engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    return std::nullopt;
  }
  if (!fr.ok) {
    errBlockOut = inRangeRejectError(fr, *inRange, "this command");
    return std::nullopt;
  }
  return fr.dataPath;
}

// BS-069 (BETA D2): Stata's tobit accepts BARE `ll`/`ul` (no numeric value)
// meaning "censor at the depvar's own observed min/max" — cobra's tobit
// CLI has no equivalent (live-verified: `--ll` with no following value fails
// inside cobra's own arg parser with "stod: no conversion", exit 1). This
// computes the min/max of `yName` over the SAME sample cobra will fit on
// (the `if`-filtered copy, reusing applyIfFilterM4 above) so PIET can pass
// an explicit numeric value either way. Returns false + fills errBlockOut on
// any failure (missing data, non-numeric column, filter error, no rows).
bool computeYMinMaxM4(SessionState& session, const std::string& yName,
                       const std::optional<std::string>& ifExpr, double& outMin, double& outMax,
                       ojson& errBlockOut) {
  std::string basePath = *session.currentHbin();
  ojson filterErr;
  auto filteredPath = applyIfFilterM4(session, basePath, ifExpr, filterErr);
  if (!filteredPath.has_value()) {
    errBlockOut = filterErr;
    return false;
  }
  bool madeTemp = (*filteredPath != basePath);
  std::optional<Datastore> tempDs;
  Datastore* ds = nullptr;
  if (madeTemp) {
    tempDs.emplace(*filteredPath);
    ds = &*tempDs;
  } else {
    ds = session.datastore();
  }
  bool ok = false;
  if (ds != nullptr && ds->has(yName) && ds->dtype(yName) == "f64") {
    const cecon::Column& c = ds->col(yName);
    const double* d = c.f64();
    int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
    double mn = std::numeric_limits<double>::infinity();
    double mx = -std::numeric_limits<double>::infinity();
    bool any = false;
    for (int64_t i = 0; i < count; ++i) {
      double v = d[i];
      if (std::isnan(v)) continue;
      any = true;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    if (any) {
      outMin = mn;
      outMax = mx;
      ok = true;
    }
  }
  if (madeTemp) cecon::native_unlink(*filteredPath);
  if (!ok) {
    errBlockOut = engineErrorBlockM4(
        "tobit: could not compute the observed min/max of `" + yName + "` for a bare ll/ul",
        "engine_error");
    return false;
  }
  return true;
}

std::optional<std::vector<int64_t>> esampleRowsM4(SessionState& session, const LastModel& lm) {
  if (lm.esample != LastModel::EsampleState::kColumn || lm.esampleSerial == 0) return std::nullopt;
  try {
    Datastore* ds = session.datastore();
    if (ds == nullptr || !ds->has(kEsampleColumn) || ds->dtype(kEsampleColumn) != "f64")
      return std::nullopt;
    const double* mk = ds->col(kEsampleColumn).f64();
    const double serial = static_cast<double>(lm.esampleSerial);
    std::vector<int64_t> rows;
    for (int64_t i = 0; i < ds->nRows(); ++i)
      if (mk[i] == serial) rows.push_back(i);
    return rows;
  } catch (const std::exception&) {
    return std::nullopt;  // an unreadable frame: the caller posts nothing that needs the rows
  }
}

const double* f64ColumnM4(SessionState& session, const std::string& name) {
  try {
    Datastore* ds = session.datastore();
    if (ds == nullptr || name.empty() || !ds->has(name) || ds->dtype(name) != "f64") return nullptr;
    return ds->col(name).f64();
  } catch (const std::exception&) {
    return nullptr;
  }
}

}  // namespace est_detail

}  // namespace piet::handlers
