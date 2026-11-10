// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/estimation.h"
#include "piet/handlers/vce_types.h"
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
#include "engine/engine_call_internal.h"
#include "linear_estimation_internal.h"

namespace {
// W2 estimopts (2026-09-19, BUG-539): reghdfe's `residuals[(newvar)]` and
// `noabsorb`, matched as RAW keys inside runRegress and deliberately NOT
// registered with registerOptVocab. The registry keeps one slot per command
// token and hands back the LAST token's slot, and kVocabIvRegress
// (estimation_options.cpp) is registered on {ivregress, ivreg, ivreghdfe, reghdfe}
// -- so it IS the `reghdfe` slot, and it is the known-set runIvRegress's
// reject net reads (instrumental_variables.cpp) for ivregress/ivreg2/xtivreg2/IV
// reghdfe too. A registration here unioned `resid`/`noabsorb` into that net:
// `ivregress 2sls y (x=z), resid` passed at rc 0 with the option unread
// (review finding, attempt 2). Unregistered, the dispatcher canonicaliser
// leaves these keys exactly as typed (no member of the reghdfe set starts
// with `re` or `noa`), so every accepted spelling is listed here in full.
// reghdfe declares `RESiduals(name) RESiduals2`, floor `res`, so
// `re` is NOT one of them -- w2_smalls.log reghdfe_bare_re is r(198), which
// runRegress's own reject net answers. reghdfe declares `noAbsorb2`,
// floor `noa`, full spelling `noabsorb2`.
const std::unordered_set<std::string> kReghdfeResidSpellings = {
    "res", "resi", "resid", "residu", "residua", "residual", "residuals"};
const std::unordered_set<std::string> kReghdfeNoAbsorbSpellings = {
    "noa", "noab", "noabs", "noabso", "noabsor", "noabsorb", "noabsorb2"};
}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// estimation_results.cpp: the built-in multiway VCE's reply rewrite (e(rank) = the
// clipped V's rank) and its e(kcluster)/e(cluster#) posting.
void applyBuiltinMultiwayReplyM4(ojson& result);
void postBuiltinMultiwayEM4(SessionState& session, const ojson& raw);
// Stata's ordered e() name lists after regress/areg (estimation_results.cpp).
void postENameListsRegressM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed, const std::string& stataCmd,
                             bool builtinMultiway, bool noConstant);

using namespace engine_call_detail;
using linear_estimation_detail::stataRefusalBlock;
using linear_estimation_detail::joinComma;
using linear_estimation_detail::cobraToEstimation;

// ---------------------------------------------------------------------------
// regress / reg / feols (plain OLS/feols vertical slice: iid/hc1/cluster
// vcov, optional --fe via absorb(), noconstant, level(#). IV/xtreg-RE/
// multi-outcome routing is OUT of scope — M3.)

namespace {

// BS-067 (BETA D1): `vce(cluster A B)` / legacy `cluster(A B)` naming TWO
// space-separated variables -> --vcov twoway --cluster A,B (cobra's feols
// --vcov surface accepts "twoway" as a 4th mode alongside iid/hc1/cluster —
// live-verified against the D1i reghdfe 2-way oracle: `reghdfe income
// beertax, absorb(state) vce(cluster state year)` -> beertax se=1867.524
// matches byte-for-byte through this exact shape). One var -> the
// pre-existing single-cluster path, unchanged. Three+ vars -> --vcov
// multiway --cluster A,B,C for reghdfe only (allowMultiway,
// engine-multiway-cluster3, BUG-604); every other command gets a named
// refusal (unsupported_option, rc 198). Mirrors estimation_support.cpp's
// own splitClusterVarsM4 — kept as a separate TU-local copy (same
// established pattern as this file's lower()/cobraErrorStr() thin wrappers)
// rather than introducing a new shared header for one small helper.
struct ClusterSplitResult {
  std::vector<std::string> vcovArgv;  // {"--vcov", TYPE, "--cluster", VARS}
  bool errored = false;
  std::string errorMsg;
};

ClusterSplitResult splitClusterVars(const std::string& raw, bool allowMultiway = false) {
  ClusterSplitResult r;
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
    r.vcovArgv = {"--vcov", "cluster", "--cluster", toks[0]};
  } else if (toks.size() == 2) {
    r.vcovArgv = {"--vcov", "twoway", "--cluster", toks[0] + "," + toks[1]};
  } else if (allowMultiway) {
    std::string joined = toks[0];
    for (size_t i = 1; i < toks.size(); ++i) joined += "," + toks[i];
    r.vcovArgv = {"--vcov", "multiway", "--cluster", joined};
  } else {
    r.errored = true;
    r.errorMsg =
        "three or more cluster variables are not supported for this command in this "
        "version of PIET; multiway clustering with three or more cluster variables is "
        "implemented for reghdfe only";
  }
  return r;
}

// FU-117: a token after a non-cluster vce type (`vce(robust x1)`) was dropped
// and the fit ran HC1 at rc 0. w2l_vce_shape.log:407 (regress) and :764 (areg)
// are r(198) `option vce() incorrectly specified`. reghdfe/feols keep their
// accepting behaviour (w2l_vce_shape.log:4034, community command).
std::optional<ojson> vceTrailingTokenRefusal(const std::string& stataCmd, const std::string& lv,
                                             const std::string& vceRest) {
  if (vceRest.empty() || (lv != "robust" && lv != "hc1" && lv != "iid")) return std::nullopt;
  if (stataCmd == "reghdfe" || stataCmd == "feols") return std::nullopt;
  return stataRefusalBlock("option vce() incorrectly specified: `" + vceRest +
                               "` cannot follow vce(" + lv + ")",
                           198);
}

// FU-147 / FU-160: validate a cluster list before any engine call. `legacy`
// is the cluster(...) option form, `vce(cluster ...)` is the other.
//  - FU-160: legacy cluster() takes ONE variable on regress (r(198), the two
//    lines of w2l_vce_shape.log:268-275) and areg (r(103), :624-630); the
//    vce(cluster a b) forms are multiway. reghdfe/feols legacy lists stay
//    accepted (:3838). It comes after the name lookup (StataBE 19.5,
//    2026-11-03: cluster(kgr nosuch) on regress is the lookup's r(198)).
//  - FU-147: a name that is not a variable of the dataset is r(111)
//    `variable X not found` (class oracle bug483_484_control_oracle.log:121-125),
//    not the engine's rc 1.
std::optional<ojson> clusterSpecRefusal(const std::string& raw, bool legacy,
                                        const std::string& stataCmd, SessionState& session,
                                        const std::string* dataOverride) {
  std::vector<std::string> toks;
  std::istringstream iss(raw);
  std::string t;
  while (iss >> t) toks.push_back(t);
  if (toks.empty()) return std::nullopt;
  std::optional<Datastore> own;
  Datastore* ds = nullptr;
  try {
    if (dataOverride != nullptr) {
      own.emplace(*dataOverride);
      ds = &*own;
    } else {
      ds = session.datastore();
    }
  } catch (const std::exception&) {
    ds = nullptr;
  }
  // A name is looked up before the legacy count (StataBE 19.5, 2026-11-03:
  // `regress y x, cluster(kgr nosuch)` is "variable nosuch not found" r(198)).
  // The dispatcher has already put a unique abbreviation's full name in; a name
  // that begins several variables is "NAME ambiguous abbreviation". Under
  // regress's cluster() both carry "option cluster() incorrectly specified" at
  // r(198), under areg's "(error in option cluster())" at r(111); vce(cluster)
  // and reghdfe's cluster() are r(111) with no tail.
  if (ds != nullptr) {
    for (const auto& n : toks) {
      if (ds->has(n)) continue;
      int hits = 0;
      for (const auto& v : ds->names())
        if (v.size() > n.size() && v.compare(0, n.size(), n) == 0) ++hits;
      const std::string what = hits > 1 ? n + " ambiguous abbreviation" : "variable " + n + " not found";
      if (legacy && stataCmd == "regress")
        return stataRefusalBlock(what + "\noption cluster() incorrectly specified", 198);
      if (legacy && stataCmd == "areg")
        return stataRefusalBlock(what + "\n(error in option cluster())", 111);
      return stataRefusalBlock(what, 111);
    }
  }
  if (legacy && toks.size() >= 2) {
    if (stataCmd == "regress") {
      return stataRefusalBlock(
          "too many variables specified\noption cluster() incorrectly specified", 198);
    }
    if (stataCmd == "areg") {
      return stataRefusalBlock("cluster():  too many variables specified", 103);
    }
  }
  return std::nullopt;
}

// BUG-569 (review round): the VCE options' value SHAPE. `cluster` takes a
// variable and `robust` takes none. The VCE loop in runRegress reads a bare
// `cluster` (or an empty `cluster()`) as "no cluster variable", which pushes no
// VCE, and it skips a valued `robust(x)`. The unknown-option net passes both
// names, so either one fitted the iid model at rc 0: a silent wrong answer.
// Measured on the branch binary 2026-09-29: `regress y x1, cluster`,
// `areg y x1, a(id) cl` and `areg y x1, a(id) rob(id)` each returned rc 0 with
// e(vce) iid. Stata refuses a malformed option with r(198). The wording below is
// PIET's own; no golden pins Stata's message for these two shapes. Twin:
// estimation_support.cpp vceOptionShapeErrorM4 (piet/handlers.h, where one shared
// copy belongs, is outside this change).
//
// An EMPTY `cluster()` is not the bare option: regress, areg and reghdfe ignore
// it and fit their default VCE at rc 0 (w2l_vce_shape_det.log:338
// regress_empty_cluster `0|regress|ols||.|180`, :695 areg, :3938 reghdfe), so
// for those words it is passed over here and the VCE loop, which skips an
// empty value, fits the default. A bare `cluster` stays r(198) everywhere.
std::optional<std::string> vceOptionShapeError(const eval::ParsedCommand& parsed,
                                               const std::string& stataCmd) {
  const bool emptyClusterIgnored =
      stataCmd == "regress" || stataCmd == "areg" || stataCmd == "reghdfe";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster" && !v.isBool && strutil::trim(v.strValue).empty() && emptyClusterIgnored)
      continue;
    if (k == "cluster" && (v.isBool || strutil::trim(v.strValue).empty())) {
      return std::string("option `cluster` requires a variable: cluster(varname)");
    }
    if (k == "robust" && !v.isBool) {
      return std::string("option `robust` takes no argument");
    }
  }
  return std::nullopt;
}

// reghdfe's own spelling of the conventional VCE: `vce(unadjusted)`.
// g11_fe_header_scalars.log and reghdfe_absorb_vce.log run `reghdfe y x1 x2,
// absorb(id) vce(unadjusted)` at rc 0 with the non-robust SEs (x1 .0394148);
// runRegress answered "Unknown vce type: unadjusted" r(198). EXACT token
// (case-insensitive) only, and reghdfe only: no captured arm measures an
// abbreviation, nor the spelling under regress/areg, which are left as before.
bool isReghdfeUnadjustedVce(const std::string& raw) {
  const size_t a = raw.find_first_not_of(" \t");
  if (a == std::string::npos) return false;
  const size_t z = raw.find_last_not_of(" \t");
  std::string tok = raw.substr(a, z - a + 1);
  for (auto& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return tok == "unadjusted";
}

}  // namespace

// e(df_a) -- the absorbed degrees of freedom -- for the two absorbing words
// runRegress serves, derived from the engine's feols reply without new engine
// fields: `absorbed` ({fe name: levels}), `k_absorbed` (dof.k_fe: the rank of
// the FE dummies, redundancy removed; 1 FE = its levels, 2 FE = K1 + K2 -
// connected components), `fe_exact` (false = 3+ FE, k_fe only an upper
// bound) and `nested_fe` (the FEs nested in a cluster variable). nullopt =
// not derivable, and nothing is posted (e(df_a) stays missing). The engine's
// iv reply carries the same fields (no fe_exact), so ivreghdfe reads its
// e(df_a) here as "reghdfe" (estimation_internal.h; runIvRegress).
//
//   areg:    df_a = k_absorbed - 1. areg absorbs exactly one variable and
//            keeps _cons in e(b), so one level is the constant's: w2b_estim.log
//            PREC areg_a(id)/areg_ab(id)/areg_abs(id)/areg_absorb(id): 10
//            categories -> df_a 9, df_r 48 = 60 - 2 slopes - 1 - 9. areg charges
//            every level whatever the VCE (BUG-568 keeps nested FEs in its
//            cluster dof), so nested_fe does not enter.
//   reghdfe: df_a = k_absorbed - (levels of every FE nested in the cluster),
//            reghdfe's "Categories - Redundant = Num. Coefs" column summed.
//            Redundant = the connected-components term already inside
//            k_absorbed: g11_fe_header_scalars.log absorb(id) df_a 100 (100
//            categories, 0 redundant); absorb(id t) 100 + (8 - 1) = 107;
//            aer_b4/predict-e-xbd.log:348 absorb(id f2) df_a 49 = 43 + (7 - 1).
//            A nested FE is ALL redundant: w2b_estim.log reghdfe_cluster_ctl
//            (absorb(id) vce(cluster id)): 10 categories, 10 redundant, df_a 0.
//            3+ FEs (fe_exact false): the engine charges K1 + sum(K_g - 1),
//            reghdfe's pairwise count K1 + K2 - G(1,2) + sum_g (K_g -
//            max_h G(h,g)) with G the connected components of a pair; the two
//            agree when every pair is connected, and the engine's e(df_r)
//            already rests on the same count (StataBE 19.5, BUG-1267:
//            absorb(id id2 id3) -> 30 + 7 + 5 - 2 = 40, df_r 138).
//            Not posted (no oracle arm fixes it): a nested FE alongside
//            another FE (how reghdfe then re-assigns the connected-components
//            redundancy is not measured).
std::optional<double> absorbedDofA(const ojson& raw, const std::string& stataCmd) {
  if (!raw.is_object() || !raw.contains("k_absorbed") || !raw["k_absorbed"].is_number() ||
      !raw.contains("absorbed") || !raw["absorbed"].is_object() || raw["absorbed"].empty()) {
    return std::nullopt;
  }
  const double kFe = raw["k_absorbed"].get<double>();
  const ojson& absorbed = raw["absorbed"];
  if (stataCmd == "areg") {
    if (absorbed.size() != 1 || kFe < 1.0) return std::nullopt;
    return kFe - 1.0;
  }
  if (stataCmd != "reghdfe") return std::nullopt;
  double nestedLevels = 0.0;
  size_t nNested = 0;
  if (raw.contains("nested_fe") && raw["nested_fe"].is_array()) {
    for (const auto& name : raw["nested_fe"]) {
      if (!name.is_string()) return std::nullopt;
      const std::string n = name.get<std::string>();
      if (!absorbed.contains(n) || !absorbed[n].is_number()) return std::nullopt;
      nestedLevels += absorbed[n].get<double>();
      ++nNested;
    }
  }
  // Two absorbed variables with the FIRST one nested in a cluster variable:
  // reghdfe counts all of its categories redundant and charges the second its
  // categories less the one the connected components take, which is k_absorbed
  // less the nested levels (w2s_multiway_class.log mw_hdfe_abst_c2: absorb(id t)
  // with vce(cluster id id2) -> e(df_a) 5 = 30 + 6 - 1 - 30). The other orders
  // stay unposted (no record pins them).
  if (nNested > 0 && absorbed.size() != 1) {
    if (!(nNested == 1 && absorbed.size() == 2 && raw["nested_fe"][0].is_string() &&
          absorbed.begin().key() == raw["nested_fe"][0].get<std::string>()))
      return std::nullopt;
  }
  return kFe - nestedLevels;
}

// The InRangeFilterResult twin that keeps the row-position column on the copy
// (inrange_filter.cpp; its header is not this task's to change).
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState& session,
                                                   const std::string& basePath,
                                                   const std::optional<std::string>& ifExpr,
                                                   const std::optional<std::string>& inRange,
                                                   std::string* indexVar);

// FU-475 (w2_esample.log:751-775 C6_tsop_if_boundary `0|40|40|`, :782 C6b
// `10|`): Stata applies an `if` AFTER the time-series operators have read the
// full series, so `regress y L.x1 x2 if t>=3` keeps the t==3 rows (their L.x1
// is the t==2 value). A fit that reads a physically filtered copy (runRegress's
// applyIfAndInFilter route, BS-090) evaluates L.x1 on the copy instead, where
// the t==2 row is gone: e(N) 30 at rc 0. The engine's own `--keepif` keeps the
// FULL frame for a TS-bearing regressor list and applies the restriction as a
// row mask after its ts_prepass (apps/cmds/common.cpp plan_keepif, the E1
// path), so such a fit must hand the engine the working frame plus `--keepif`.
// DESIGN DECISION (one rule for every wired fit route; the iv, xtreg, newey
// and prais routes already send the full frame plus --keepif):
//  * tsFitKeepsFullFrame: regressors carry a TS operator and the restriction is
//    an `if` alone -> the engine route.
//  * tsFitRestrictionRefusal: the restriction is one the engine's --keepif
//    cannot evaluate on the full frame -- an `in` range (`_n` needs a bysort
//    context there, BS-090), an `if` naming `_n`/`_N`, or an `if` carrying a TS
//    operator of its own (the mask is evaluated without the tsset metadata) --
//    a named r(198), never the filtered copy's silently smaller sample.
// The depvar counts as much as a regressor: `regress D.y x1 x2 if t>=3` reads
// y's t==2 value for the t==3 rows (StataBE 19.5 e(N) 48, BUG-1277).
namespace {
bool regressorsCarryTs(const eval::ParsedCommand& parsed) {
  return !parsed.varlist.empty() && translate::hasTsToken(parsed.varlist);
}
}  // namespace

bool tsFitKeepsFullFrame(const eval::ParsedCommand& parsed) {
  return regressorsCarryTs(parsed) && parsed.ifExpr.has_value() && !parsed.ifExpr->empty() &&
         !(parsed.inRange.has_value() && !parsed.inRange->empty());
}

std::optional<ojson> tsFitRestrictionRefusal(const eval::ParsedCommand& parsed,
                                             const std::string& stataCmd) {
  if (!regressorsCarryTs(parsed)) return std::nullopt;
  std::string what;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    what = "an `in` range";
  } else if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    static const std::regex kObsNo(R"((^|[^A-Za-z0-9_])_[nN]([^A-Za-z0-9_]|$))");
    if (std::regex_search(*parsed.ifExpr, kObsNo)) {
      what = "an `if` condition naming _n or _N";
    } else if (translate::exprHasTsToken(*parsed.ifExpr)) {
      what = "an `if` condition with its own time-series operator";
    }
  }
  if (what.empty()) return std::nullopt;
  return ojson(eval::errorBlock(
      "unsupported_option",
      stataCmd + " with time-series operators in the regressors and " + what +
          " is not supported in this version of PIET (the restriction would be applied before "
          "the operators read the full series, which changes the estimation sample).",
      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
}

ojson runRegress(const eval::ParsedCommand& parsed, SessionState& session,
                  const std::string& stataCmd, const std::string* dataOverride,
                  ojson* residMutationOut) {
  if (!session.currentHbin().has_value()) {
    return eval::noDataLoaded();
  }
  if (parsed.varlist.empty()) {
    return engineErrorBlock("syntax_error: no variables specified", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"feols"};
  argv.push_back("--y");
  argv.push_back(y);
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinComma(xs));
  } else {
    argv.push_back("--x");
    argv.push_back("(intercept)");
  }
  // BS-090 (BETA Phase G): `if`/`in` used to be forwarded here as a bare
  // `--keepif` flag straight onto cobra's `feols` CLI. That still worked
  // for if-only (feols's own --keepif runs the SAME non-bysort
  // Frame::keep_if() cobra's `eval` verb uses), but a `_n`-based `in`
  // predicate can NOT go through it (op-cobra's evaluator throws "_n
  // requires a bysort context" outside --bygen) — so the row-range
  // pre-filter below (piet::handlers::applyIfAndInFilter) now runs BEFORE
  // the cobra call and points `--data` at the already-filtered copy
  // instead, and `--keepif` is never pushed to `argv` at all (removed here,
  // see the `--data` construction near the bottom of this function).

  // vce/vcov/robust/cluster (translate.py's _vcov_from_opts, iid/hc1/cluster
  // — BS-067 (BETA D1) added twoway, see splitClusterVars below).
  std::optional<std::string> vceVal;
  bool sawRobustFlag = false;
  std::optional<std::string> clusterOpt;
  bool noConstant = false;
  std::optional<int> level;
  bool keepSingletons = false;
  // BUG-539: reghdfe's `residuals[(newvar)]`. Bare -> `_reghdfe_resid`
  // (w2_smalls.log reghdfe_bare_resid/residuals/res/residual/resi, all rc 0
  // with the variable made); `NAME(newvar)` -> that name (reghdfe_named_*).
  // Only reghdfe has the option -- the reject net below still refuses these
  // keys for regress/feols/areg, which is Stata's own r(198) there.
  bool residWanted = false;
  std::string residName;
  std::string savedFeName, savedFeVar, savedFeError;
  if (auto shapeErr = vceOptionShapeError(parsed, stataCmd)) {
    return engineErrorBlock(*shapeErr, "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && vceVal.has_value() && k == "vce") {
      // Stata: "option vce() specified multiple times" r(198)
      // (w2s_multiway_class.log:1510 mw_reg_vce2x).
      return stataRefusalBlock("option vce() specified multiple times", 198);
    }
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    } else if (k == "robust" && v.isBool && v.boolValue) {
      sawRobustFlag = true;
    } else if (k == "cluster") {
      clusterOpt = v.isBool ? std::string() : v.strValue;
    } else if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      noConstant = true;
    } else if (stataCmd == "reghdfe" && kReghdfeResidSpellings.count(k)) {
      residWanted = true;
      residName = v.isBool ? std::string("_reghdfe_resid") : strutil::trim(v.strValue);
    } else if (stataCmd == "reghdfe" && kReghdfeNoAbsorbSpellings.count(k)) {
      // w2_smalls.log reghdfe_noabsorb: rc 0, _b[x] = 1.90685050483118 = the
      // constant-only OLS fit. reghdfe parses the option as
      // `noAbsorb2` and never reads it: `has_standard_fe` (:212) is decided
      // by absorb() ALONE, so the flag is consumed here and absorb() still
      // wins when both are typed. Nothing to push -- no absorb(), no --fe.
    } else if (k == "level" && !v.isBool) {
      try {
        level = std::stoi(v.strValue);
      } catch (...) {
      }
    } else if (k == "keepsingletons" && v.isBool && v.boolValue) {
      // BS-068: reghdfe's own option name — real Stata has no
      // `keepsingletons` on plain regress/feols at all (that gate is below,
      // outside this loop, where stataCmd is compared).
      keepSingletons = true;
    } else if (k == "absorb") {
      if (!v.isBool) {
        std::string fe = v.strValue;
        // space/comma -> comma-joined tokens (translate.py's --fe rule)
        std::string joined;
        std::string cur;
        auto flush = [&]() {
          if (!cur.empty()) {
            const size_t eq = cur.find('=');
            if (eq != std::string::npos) {
              const std::string name = cur.substr(0, eq), var = cur.substr(eq + 1);
              if (stataCmd != "reghdfe" || !savedFeName.empty() ||
                  !strutil::isValidStataIdentifier(name) ||
                  !strutil::isValidStataIdentifier(var)) {
                savedFeError = "absorb(): named effects require one new variable and one plain fixed effect";
              } else {
                savedFeName = name;
                savedFeVar = var;
              }
              cur = var;
            }
            if (!joined.empty()) joined += ",";
            joined += cur;
            cur.clear();
          }
        };
        for (char c : fe) {
          if (c == ' ' || c == ',') {
            flush();
          } else {
            cur += c;
          }
        }
        flush();
        if (!joined.empty()) {
          argv.push_back("--fe");
          argv.push_back(joined);
        }
      }
    }
  }

  if (!savedFeError.empty()) return engineErrorBlock(savedFeError, "unsupported_option");
  if (!savedFeName.empty()) {
    const auto it = std::find(argv.begin(), argv.end(), "--fe");
    if (dataOverride != nullptr || residMutationOut == nullptr ||
        it == argv.end() || it + 1 == argv.end() || *(it + 1) != savedFeVar ||
        translate::hasTsToken(parsed.varlist))
      return engineErrorBlock("absorb(newvar=fe) currently requires one plain fixed effect, "
                              "without a by: prefix or time-series regressors", "unsupported_option");
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(savedFeName))
      return ojson(eval::engineError("variable " + savedFeName + " already defined"));
    if (residWanted && savedFeName == residName)
      return ojson(eval::engineError("variable " + savedFeName + " already defined"));
  }

  // P4 / BUG-060: unknown-option rejection net. After the loop above has
  // consumed every option `regress`/`reg`/`feols`/`reghdfe` implements, any
  // leftover token is either a genuine typo/invalid option (r(198)-parity:
  // Stata's own "option X not allowed") or a real Stata option this
  // version of PIET hasn't implemented yet (honest "not yet supported" —
  // currently only `beta`, the oracle P4 example: standardized-coefficient
  // display column). Conservative by construction: this only fires on
  // option NAMES the loop above never reads at all; it never revisits a
  // value it already accepted, so it cannot false-reject anything that
  // worked before this fix.
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "vce" || k == "vcov" || k == "robust" || k == "cluster" || k == "noconstant" ||
        k == "nocons" || k == "level") {
      continue;
    }
    // BUG-539: residuals()/noabsorb are reghdfe's alone (the two spelling
    // sets at the top of this file). Gated on the Stata word typed, like
    // keepsingletons below: `regress ..., resid` stays "option `resid` not
    // allowed".
    if (stataCmd == "reghdfe" &&
        (kReghdfeResidSpellings.count(k) || kReghdfeNoAbsorbSpellings.count(k))) {
      continue;
    }
    if (k == "absorb") {
      // BUG-473 (User ruling 2026-09-09: match Stata): plain `regress`/`reg`
      // has NO `absorb()` option in any Stata edition on this machine —
      // measured StataBE r(198), message "option absorb() not allowed"
      // (tests/eval/golden/aer_b4/b5_absorb_class.log:32-36,
      // b5_misc_pins.log:29-51 incl. the cluster()/vce()/if combinations,
      // and the `ab()` abbreviation at
      // tests/eval/golden/aer_b4/pins_oracle_p.log:103-107, same
      // message and rc). `feols`/`reghdfe`/`areg` share this handler and DO
      // have it (b5_absorb_class.log:80-99 areg rc 0, :125-151 reghdfe rc 0;
      // freeze_extra.do:41 `feols y x1 x2, absorb(id)`) — gate on the Stata
      // word actually typed, mirroring the `keepsingletons` gate below.
      if (stataCmd == "regress") {
        return engineErrorBlock("option absorb() not allowed", "unsupported_option");
      }
      continue;
    }
    if (k == "keepsingletons") {
      // BS-068: real Stata has `keepsingletons` on reghdfe only (regress/
      // feols have no such option at all) — gate on the ACTUAL Stata word
      // the user typed, not on whether this file happens to consume it.
      if (stataCmd != "reghdfe") {
        return engineErrorBlock("option `keepsingletons` not allowed", "unsupported_option");
      }
      continue;
    }
    if (k == "beta") {
      return engineErrorBlock("Option `" + k + "` is a valid Stata option that this version of "
                              "PIET does not yet implement.",
                              "unsupported_option");
    }
    return engineErrorBlock("option `" + k + "` not allowed", "unsupported_option");
  }
  // BS-068: keepsingletons parsed above regardless of stataCmd (so the
  // reject-net check just above can compare stataCmd itself), but only
  // ACTUALLY forwarded to cobra for reghdfe — matches real Stata (regress/
  // feols reaching this point with keepSingletons=true is unreachable: the
  // net above already returned for any stataCmd != "reghdfe").
  if (keepSingletons && stataCmd == "reghdfe") {
    argv.push_back("--keep-singletons");
  }

  if (vceVal.has_value()) {
    // W1b: `vce()`'s argument abbreviates -- ONE canonicaliser for all three
    // sites (piet/handlers.h `canonicalVceType`), measured floors: a prefix of
    // `robust` at least 1 long, of `cluster` at least 2.
    std::string vceRest;
    const std::string lv = canonicalVceType(*vceVal, &vceRest);
    if (auto trailing = vceTrailingTokenRefusal(stataCmd, lv, vceRest)) return *trailing;
    if (lv == "robust" || lv == "hc1") {
      argv.push_back("--vcov");
      argv.push_back("hc1");
    } else if (lv == "cluster") {
      std::string cvar = clusterOpt.value_or("");
      if (!vceRest.empty()) cvar = vceRest;
      // reghdfe's own refusal (reghdfe.ado's parse_vce: "Can't specify cluster
      // without clustervars (and viceversa)", r(9); w2l_vce_shape_det.log:3959
      // reghdfe_vce_cluster_novar).
      if (cvar.empty() && stataCmd == "reghdfe")
        return stataRefusalBlock("Can't specify cluster without clustervars (and viceversa)", 9);
      if (cvar.empty()) {
        return engineErrorBlock(
            "Option `vce(cluster)` is not supported in this version of PIET. "
            "Cluster variable required: vce(cluster varname)",
            "unsupported_option");
      }
      // BS-067: one var -> --vcov cluster (unchanged); two -> --vcov
      // twoway --cluster A,B; three+ -> --vcov multiway for reghdfe, else a
      // named refusal.
      if (auto bad = clusterSpecRefusal(cvar, /*legacy=*/false, stataCmd, session, dataOverride)) {
        return *bad;
      }
      ClusterSplitResult cs = splitClusterVars(
          cvar, stataCmd == "reghdfe" || stataCmd == "regress" || stataCmd == "areg");
      if (cs.errored) {
        return engineErrorBlock(cs.errorMsg, "unsupported_option");
      }
      for (const auto& a : cs.vcovArgv) argv.push_back(a);
    } else if (lv == "iid" || (stataCmd == "reghdfe" && isReghdfeUnadjustedVce(*vceVal))) {
      argv.push_back("--vcov");
      argv.push_back("iid");
    } else if (!unimplementedVceType(*vceVal).empty()) {
      return engineErrorBlock("Option `vce(" + unimplementedVceType(*vceVal) + ")` is a valid Stata option that this version of PIET does not implement", "unsupported_option");
    } else {
      return engineErrorBlock("Unknown vce type: " + *vceVal, "unsupported_option");
    }
  } else if (clusterOpt.has_value() && !clusterOpt->empty()) {
    // BUG-055 / P2: Stata's legacy `cluster(varname)` — with neither vce()
    // nor robust given — is a synonym for `vce(cluster varname)`: it
    // implies robust-cluster VCE, not iid. Before this fix, `clusterOpt`
    // was parsed (above) but only ever consulted from inside the
    // `vce(cluster ...)` branch, so a bare `cluster(v)` fell through to
    // neither `if` nor here and silently ran iid (v wholly ignored).
    // Oracle P2_legacycluster: `regress price mpg, cluster(rep78)` must
    // come back cluster-robust (n_obs=69, n_clusters=5, se_mpg~=82.63), not
    // iid (n_obs=74, se_mpg~=53.08, rep78 ignored).
    // BS-067: same one/two/three+ split as the vce(cluster ...) branch
    // above applies to the legacy bare cluster() synonym too.
    // BUG-537 (W2 estimopts): this branch sits BEFORE the bare-`robust` one.
    // `robust cluster(id)` typed together is cluster-robust in Stata --
    // w2_pweight.log R5_pw_robust_AND_cluster_equals_R4 is byte-identical to
    // R4 (`vce(cluster id)`: N_clust 15, se_x1 +1.7f38be33c76eeX-003). With
    // robust tested first, the cluster() was parsed and then IGNORED (HC1,
    // N_clust 0): a silent wrong answer on ~29 corpus lines. Twin ordering in
    // estimation_support.cpp's vcovFromOpts.
    if (auto bad = clusterSpecRefusal(*clusterOpt, /*legacy=*/true, stataCmd, session, dataOverride)) {
      return *bad;
    }
    ClusterSplitResult cs = splitClusterVars(
        *clusterOpt, stataCmd == "reghdfe" || stataCmd == "regress" || stataCmd == "areg");
    if (cs.errored) {
      return engineErrorBlock(cs.errorMsg, "unsupported_option");
    }
    for (const auto& a : cs.vcovArgv) argv.push_back(a);
  } else if (sawRobustFlag) {
    argv.push_back("--vcov");
    argv.push_back("hc1");
  }

  // BUG-568: areg's clustered SE charges every absorbed parameter, including an FE
  // nested in the cluster; reghdfe/xtreg,fe/feols (the engine default) exempt it.
  // w2b_estim.log areg_a_cl se[x1] .0722686736191015 (N_clust 10, df_r 9) against
  // reghdfe_cluster_ctl / xtreg_fe_cluster_ctl .0663182832 on the same fit; the
  // weighted twins w2_pweight.log A2 (0x1.cfb6746651353p-3) vs H1
  // (0x1.b315e83b84cedp-3). Only the areg word sets it, and only with a cluster
  // VCE and an absorbed FE (the engine refuses the flag anywhere else).
  if (stataCmd == "areg" && std::find(argv.begin(), argv.end(), "--fe") != argv.end()) {
    // e(sample) T2: areg has no singleton rule -- a one-observation group is in
    // the sample with its own absorbed mean (w2_esample.log:925 D4 e(N) 59 and
    // :1957 H8 58 keep id 11, where the engine's reghdfe default dropped it: 58
    // and 57). The refits forward it (LastModel::keepSingletons).
    argv.push_back("--keep-singletons");
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov" && (argv[i + 1] == "cluster" || argv[i + 1] == "twoway" ||
                                  argv[i + 1] == "multiway")) {
        argv.push_back("--keep-nested-fe-dof");
        break;
      }
    }
  }

  // BUG-1067: of a collinear set, reghdfe and areg keep the EARLIER term and omit
  // the later one (StataBE scratch-r150/c1067/m1.do, z = 2*x: `x o.z`, _b[x]
  // .527368857557537 for reghdfe with and without absorb() and for areg), where
  // regress keeps the engine's default norm-pivoted basis (`o.x z`). The refits
  // forward it from LastModel::collinearForward.
  if (stataCmd == "reghdfe" || stataCmd == "areg") argv.push_back("--collinear-forward");

  // FU-365: regress and areg run Stata's built-in multiway cluster
  // (Stata's multiway-cluster step -> Stata's robust-VCE step), not reghdfe's: every cluster
  // combination carries its own small-sample factor and the negative eigenvalues
  // of the sum are set to 0. w2l_vce_shape_det.log regress_vce_c2_se
  // .0643525900770342 and areg_vce_c2_se .0778721308909151, where the engine
  // default (reghdfe's Gmin factor, reghdfe_vce_c2_se .0711126484294995) gave
  // .0638929607 and .0777683042. reghdfe/feols keep the default.
  const bool builtinMultiway = stataCmd == "regress" || stataCmd == "reg" || stataCmd == "areg";
  if (builtinMultiway) {
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov" && (argv[i + 1] == "twoway" || argv[i + 1] == "multiway")) {
        argv.push_back("--multiway-subset-factors");
        break;
      }
    }
  }

  // reghdfe's `noconstant` with absorbed effects: Stata fits as usual and only
  // leaves `_cons` out of e(b)/r(table) (StataBE scratch-r150/rtab/nc.do: _b[x]
  // and _se[x] identical to the fit with a constant, e(b) colnames `x`; 007's
  // tables run `reghdfe treatment $balance, absorb(...) nocons`). The engine
  // refuses --no-intercept with --fe, so the flag is not sent and `_cons` is
  // dropped from the result below.
  const bool feNoCons = noConstant && stataCmd == "reghdfe" &&
                        std::find(argv.begin(), argv.end(), "--fe") != argv.end();
  if (noConstant && !feNoCons) argv.push_back("--no-intercept");
  // BS-048 (BETA A4) — session.level fallback NOT added here: verified
  // cobra's own feols CLI (cmd_feols, apps/cmds/cmds_linear.cpp) never
  // reads `opt(a, "--level", ...)` at all — the ONLY apps/cmds/*.cpp sites
  // that do are qreg/sdtest/ttest/mediate, and only ttest is wired into
  // PIET's dispatch surface today (runTtest carries the session.level
  // fallback instead). Forwarding --level to a command that never consumes
  // it trips cobra's E6 unconsumed-flag net (common.cpp::
  // check_unconsumed_flags) and turns an otherwise-successful regression
  // into a hard error — a session.level fallback here would make an
  // ordinary `set level 90` + subsequent plain `regress` newly error where
  // it worked before. The pre-existing explicit `level(#)` handling below
  // has the SAME latent gap (untested by any freeze fixture — no
  // `level(...)` usage anywhere in debug_all.do) but is OUT of this phase's
  // 4-mechanism scope to fix; flagged for the operator.
  if (level.has_value()) {
    if (*level < 1 || *level > 99) {
      return engineErrorBlock("level(#) must be an integer between 1 and 99.",
                              "unsupported_option");
    }
    argv.push_back("--level");
    argv.push_back(std::to_string(*level));
  }

  // P3 weights wave: registry.cpp's AllWeights row for regress/reghdfe/feols
  // means the tokenizer already let aw/fw/iw/pw all the way through here —
  // forward whichever one the user gave, normalized to cobra's 2-letter
  // --wtype spelling. `parsed.weightVar` only survives the tokenizer's guard
  // when `parsed.weightType` is one of the four legal spellings, so no
  // separate legality check is needed here. A weight combined with --fe
  // (already pushed to argv above, if `absorb()` was given) is fitted by the
  // engine's weighted absorption (feols_fit demeans with the weights); the
  // combinations Stata's absorbing commands refuse (iweight, pweight with an
  // iid VCE) are refused inside cobra itself and surface through the ordinary
  // CobraError catch below -- not checks this handler needs to duplicate.
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    std::string wt = lower(*parsed.weightType);
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
      // BUG-536 (W2 estimopts): a probability weight IMPLIES a robust VCE.
      // w2_pweight.log R2_pw (`[pweight=w]`, no vce): e(vce)=robust,
      // e(vcetype)=Robust, se_x1 +1.a3421f4182c9fX-003 -- byte-identical to
      // R3 (`[aweight=w], vce(robust)`) and NOT to R1 (`[aweight=w]` iid,
      // +1.691bde09fd0feX-003). This handler forwarded the weight and let the
      // engine default to iid, which is exactly the R1 number under a pweight
      // header: 13 corpus lines in paper 034 ran with the wrong SE at rc 0.
      // Only when nothing chose a VCE above -- vce()/robust/cluster() all
      // pushed `--vcov` already, and R4/R5 keep their cluster.
      if (wtype == "pw" && std::find(argv.begin(), argv.end(), "--vcov") == argv.end()) {
        argv.push_back("--vcov");
        argv.push_back("hc1");
      }
    }
  }

  // Stata's builtin `regress` treats a fit with e(r2) > 1 - 1e-14 as exact:
  // e(rss) 0, e(V) 0, and t/p/CI missing (w2e_float_default prop_predict_untyped
  // and _double: _se[p] 0 where the computed RSS left 1.4e-8). reghdfe does not
  // (StataBE `reghdfe yx x, noabsorb` keeps e(rss) 8.5e-28 and _se 1.7e-16), so
  // only the regress word asks the engine for the rule, under the classic VCE.
  // BUG-1055: areg (the builtin's absorb) and an aweighted, fweighted or
  // iweighted regress have it too (StataNow 19.5 BE: `regress y2 x [aw=w]`,
  // `[fw=gp]` and `areg y2 x, absorb(gp)` post e(rss) 0 and _se[x] 0 on an exact
  // y2 = 3 + 2*x); a pweight implies the robust VCE. The refits forward it from
  // LastModel (fitConventionFlagsM4).
  if (stataCmd == "regress" || stataCmd == "reg" || stataCmd == "areg") {
    bool iid = true;
    std::string wtype;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--vcov" && argv[i + 1] != "iid") iid = false;
      if (argv[i] == "--wtype") wtype = argv[i + 1];
    }
    if (iid && wtype != "pw" && std::find(argv.begin(), argv.end(), "--svy") == argv.end() &&
        (stataCmd == "areg" || std::find(argv.begin(), argv.end(), "--fe") == argv.end()))
      argv.push_back("--regress-exact-fit");
  }

  // TS-context injection (MI-083, CONTRACT-engine §16, M3a): when any y/x
  // token is a time-series operator expression (L./F./D./S.), inject
  // --xtset/--tsset from session.panelvar/timevar (translate.py's
  // _has_ts_token + _inject_ts_context, applied to the RAW pre-translation
  // varlist — matches build_estimation_argv's `ts_tokens = [y_raw] + xs_raw`
  // ordering; parsed.varlist IS y_raw/xs_raw here since dispatcher.cpp's
  // TODO(M3) TS-token reject was removed in favor of this real injection).
  if (translate::hasTsToken(parsed.varlist)) {
    argv = translate::injectTsContext(argv, session);
  }

  // BS-090 (BETA Phase G): materialize the `if`/`in`-restricted sample
  // FIRST (piet::handlers::applyIfAndInFilter — same mechanism summarize/
  // count/tabulate/corr/ttest now all share) and point `--data` at it
  // instead of forwarding a raw `--keepif` to cobra's feols CLI (which
  // cannot evaluate an `in`-derived `_n` predicate — see the comment above
  // where `--keepif` used to be pushed onto `argv`).
  // dataOverride (by-group driver): the caller already materialized this group's
  // slice WITH the sub-command's own `if`/`in` ANDed in, so re-running the filter
  // here would re-apply `in` against the slice's own row numbering and silently
  // fit a sub-range of the group.
  //
  // e(sample) T2: the filtered copy keeps each row's position in the working
  // frame (applyIfAndInFilterKeepingIndex), so the engine can scatter the
  // admitted rows back onto it. Under a by: prefix (dataOverride) the slice is
  // the caller's and no mask is asked for: e(sample) after `by g: regress`
  // stays a named r(198) (kUnavailable), never one group's rows.
  std::string regressDataPath;
  std::string esampleIndexVar;
  int64_t esampleHeadRows = 0;
  // FU-475: a TS-bearing regressor list under `if` reads the full working
  // frame and the engine applies the `if` after its operators (see
  // tsFitKeepsFullFrame). Under a by: prefix the caller hands such a fit the
  // group's whole slice, and an `if` naming the pre-evaluated mask column that
  // slice carries (BUG-1166), so the same --keepif goes onto the slice.
  const bool tsEngineKeepif = tsFitKeepsFullFrame(parsed);
  if (dataOverride == nullptr) {
    if (auto refused = tsFitRestrictionRefusal(parsed, stataCmd)) return *refused;
  }
  if (dataOverride != nullptr) {
    regressDataPath = *dataOverride;
    if (tsEngineKeepif) {
      argv.push_back("--keepif");
      argv.push_back(*parsed.ifExpr);
    }
  } else if (tsEngineKeepif) {
    regressDataPath = *session.currentHbin();
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  } else {
    InRangeFilterResult regressFilter;
    try {
      // e(sample) T2: the filtered copy is the fit's input only, so on an
      // unmoved install frame it is cut from that install's on-disk base (same
      // rows and values, no hidden column) instead of writing the head first.
      // An `if` that names the mask (`if e(sample)`) needs the head itself.
      std::string filterSource = *session.currentHbin();
      const bool namesMask =
          (parsed.ifExpr.has_value() && namesHiddenColumn(*parsed.ifExpr));
      const bool restricted = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                              (parsed.inRange.has_value() && !parsed.inRange->empty());
      if (restricted && !namesMask &&
          session.esampleHeadIsInstallOf(session.esampleLastInstallBase) &&
          cecon::native_exists(session.esampleLastInstallBase))
        filterSource = session.esampleLastInstallBase;
      regressFilter = applyIfAndInFilterKeepingIndex(session, filterSource, parsed.ifExpr,
                                                     parsed.inRange, &esampleIndexVar);
      // The install frame's row count is the install's (macroObsCount would
      // open the head as a Datastore, writing it to disk).
      if (!esampleIndexVar.empty())
        esampleHeadRows = filterSource != *session.currentHbin() && session.esampleLastInstallRows > 0
                              ? session.esampleLastInstallRows
                              : session.macroObsCount();
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!regressFilter.ok) return inRangeRejectError(regressFilter, *parsed.inRange, stataCmd);
    regressDataPath = regressFilter.dataPath;
  }

  // BUG-539: everything residuals() cannot honour is refused BEFORE the fit,
  // by name, so e() is never left pointing at a model whose residual column
  // was silently not made. The column is produced by the engine's `predict
  // --resid` refit (the same call runPredict makes), which has no --weight /
  // --no-intercept / --keep-singletons / --tsset flags: forwarding those fits
  // through it would write a DIFFERENT model's residuals under the user's
  // name. An if/in restriction is NOT refused (review finding, attempt 2 --
  // paper 082 types `reghdfe ... if is_teacher >= 3, ... residuals`): see the
  // sample-masked depvar below the fit.
  bool residExisted = false;
  if (residWanted) {
    if (dataOverride != nullptr || residMutationOut == nullptr) {
      return engineErrorBlock(
          "reghdfe residuals() is not supported under a by: prefix in this version of PIET.",
          "unsupported_option");
    }
    if (residName.empty()) {
      return engineErrorBlock("residuals() requires a new variable name, e.g. residuals(res).",
                              "syntax_error");
    }
    if (!strutil::isValidStataIdentifier(residName)) {
      return engineErrorBlock("reghdfe residuals(): invalid variable name `" + residName + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
    if (std::find(argv.begin(), argv.end(), "--weight") != argv.end() || noConstant ||
        keepSingletons || translate::hasTsToken(parsed.varlist)) {
      return engineErrorBlock(
          "reghdfe residuals() is not supported together with weights, noconstant, "
          "keepsingletons or time-series operators in this version of PIET (the residual refit "
          "cannot carry them).",
          "unsupported_option");
    }
    Datastore* ds = session.datastore();
    if (ds != nullptr) {
      const auto& nm = ds->names();
      residExisted = std::find(nm.begin(), nm.end(), residName) != nm.end();
    }
    // w2_smalls.log reghdfe_named_exists: `residuals(myr)` with myr present is
    // r(110) "variable myr already defined" (coarseRc's "already defined"
    // rule). The BARE form is the exception -- reghdfe `cap drop
    // _reghdfe_resid` first, so an old `_reghdfe_resid` is replaced.
    if (residExisted && residName != "_reghdfe_resid") {
      return ojson(eval::engineError("variable " + residName + " already defined"));
    }
  }

  std::vector<std::string> cobraArgs = argv;
  cobraArgs.push_back("--data");
  cobraArgs.push_back(regressDataPath);
  cobraArgs.push_back("--tidy");
  // eb-order T-P1: the fit's full e(V) (the feols options_table row carries
  // --emit-v; apps/cmds/common.cpp run_feols add_e_v). Only the fit call gets
  // the flag, not `argv`, which LastModel records for refits. The engine
  // refuses it with --svy, so an svy fit's e(V) stays the named refusal.
  if (std::find(cobraArgs.begin(), cobraArgs.end(), "--svy") == cobraArgs.end())
    cobraArgs.push_back("--emit-v");
  // e(sample) T2: the engine writes the fit's admitted rows (installed below).
  std::string esampleValues;
  if (dataOverride == nullptr) {
    esampleValues = esampleFitArgs(session, cobraArgs, esampleIndexVar, esampleHeadRows);
  }

  ojson raw;
  try {
    // e(sample) T2: a fit on a filtered scratch copy, with the head still an
    // install frame (dirty, worker-only), reads the copy statelessly: routed
    // resident it would evict the head, writing it to disk first, once per fit.
    std::optional<SessionState::StatelessRead> keepHeadResident;
    if (dataOverride == nullptr && regressDataPath != *session.currentHbin() &&
        session.esampleHeadIsInstallOf(session.esampleLastInstallBase))
      keepHeadResident.emplace(session);
    raw = runCobraJson(cobraArgs);
  } catch (const CobraError& ce) {
    // dispatcher.py "4b. Cobra estimator": `except CobraError as ce:
    // blocks.append(make_engine_error(str(ce), code=ce.code))` — str(ce) is
    // the bracket-prefixed "[code] message". This is the exact site the 2
    // Eval M2-ratification TS carve-out cells exercise (`feols y L.x1`
    // without tsset -> cobra's native `ts_no_tsset` code, re-wrapped here as
    // "[ts_no_tsset] ..." with code coerced to "engine_error" by errorBlock's
    // allow-list, matching the frozen oracle cell byte-for-byte).
    // BUG-1269: reghdfe counts an empty sample as too small for its absorbed
    // effects, r(2001) "insufficient observations", where regress and areg say
    // r(2000) "no observations" (StataBE 19.5: `reghdfe y x if x > 20, absorb(g)`).
    if (stataCmd == "reghdfe" && ce.code == "no_obs" && std::string(ce.what()) == "no observations")
      return eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                              std::nullopt, std::nullopt, std::string("r(2001)"));
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // BUG-539: reghdfe's residual column. This is runPredict's engine call
  // (`predict --resid NAME`, within-residuals under --fe) pointed at the very
  // head the fit just read, so the output is that head plus one column -- no
  // column made before the fit is lost (predict's own stale-dataPath defect
  // is another lane's). It runs BEFORE e() is posted: a refit that fails, or
  // whose estimation sample is not the fit's own (a cluster variable missing
  // on rows the refit keeps, say), refuses the whole command instead of
  // leaving e() describing a model whose residual column is wrong or absent.
  // The estimation block stays the one object this handler returns; the
  // data_mutation travels through `residMutationOut` and the dispatcher
  // pushes it as its own block (ARRAY-vs-BLOCK: never hand back an array).
  //
  // Under if/in the fit read a filtered COPY (regressDataPath), which cannot
  // take the column: it has fewer rows than the live head. reghdfe fills
  // residuals() on e(sample) only, and `predict --fit-data` would evaluate
  // y - xb on EVERY row with data (and declines --fe). So the refit runs on
  // the live head with the depvar MASKED to the if/in rows: one `eval` pass
  // writes `kResidMaskY = cond((if & in), y, .)` (the in-range through a
  // materialized `_n`, the applyIfAndInFilter recipe, inrange_filter.cpp);
  // predict's listwise deletion then drops exactly the rows the filter
  // dropped, and writes NaN on every row outside md.keep; a last `eval`
  // drops the mask column. The n_obs equality check below is what proves the
  // masked refit saw the fit's own sample.
  std::string residOutPath;
  if (residWanted) {
    const bool restricted = regressDataPath != *session.currentHbin();
    std::string predictData = regressDataPath;
    std::string predictY = y;
    // Scratch column names (design decision: no existing helper exports one;
    // inrange_filter.cpp's kIdxVar is file-local). Double-underscore, like
    // kIdxVar, so a Stata user variable cannot collide in practice.
    const std::string kResidMaskY = "__piet_resid_y__";
    const std::string kResidMaskIdx = "__piet_resid_idx__";
    if (restricted) {
      const bool haveIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
      const bool haveIn = parsed.inRange.has_value() && !parsed.inRange->empty();
      std::string cond;
      std::vector<std::string> margs = {"eval", "--data", *session.currentHbin()};
      if (haveIn) {
        Datastore* hds = session.datastore();
        InRangeBounds rr = parseInRangeBounds(*parsed.inRange, hds != nullptr ? hds->nRows() : 0);
        if (!rr.ok) {
          InRangeFilterResult bad;
          bad.ok = false;
          bad.negative = rr.negative;
          return inRangeRejectError(bad, *parsed.inRange, stataCmd);
        }
        margs.push_back("--bygen");
        margs.push_back(kResidMaskIdx + "=_n");
        cond = kResidMaskIdx + ">=" + std::to_string(rr.a) + " & " + kResidMaskIdx +
               "<=" + std::to_string(rr.b);
      }
      if (haveIf) cond = haveIn ? "(" + *parsed.ifExpr + ") & (" + cond + ")" : *parsed.ifExpr;
      margs.push_back("--gen");
      margs.push_back(kResidMaskY + "=cond((" + cond + "),(" + y + "),.)");
      if (haveIn) {
        margs.push_back("--drop");
        margs.push_back(kResidMaskIdx);
      }
      predictData = nextPath(session.workdir(), "reghdfe_resid_mask");
      margs.push_back("--out");
      margs.push_back(predictData);
      try {
        runCobraJson(translate::injectTsContextForExpr(margs, cond, session));
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      predictY = kResidMaskY;
    }
    std::vector<std::string> pargs = {"predict", "--data", predictData, "--y", predictY,
                                      "--x", xs.empty() ? std::string("(intercept)") : joinComma(xs)};
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
      if (argv[i] == "--fe") {
        pargs.push_back("--fe");
        pargs.push_back(argv[i + 1]);
      }
    }
    // The residuals of the fitted sample: a fit that kept its singletons
    // (`keepsingletons`) has residuals on those rows too.
    if (std::find(argv.begin(), argv.end(), "--keep-singletons") != argv.end())
      pargs.push_back("--keep-singletons");
    // BUG-1067: the same collinear omission as the fit (the residual itself does
    // not depend on which member is dropped; the refit's coefficients do).
    if (std::find(argv.begin(), argv.end(), "--collinear-forward") != argv.end())
      pargs.push_back("--collinear-forward");
    pargs.push_back("--resid");
    pargs.push_back(residName);
    residOutPath = nextPath(session.workdir(), "reghdfe_resid");
    pargs.push_back("--out");
    pargs.push_back(residOutPath);
    ojson praw;
    try {
      praw = runCobraJson(pargs);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    const bool sameSample = praw.is_object() && raw.is_object() && praw.contains("n_obs") &&
                            raw.contains("n_obs") && praw["n_obs"].is_number() &&
                            raw["n_obs"].is_number() &&
                            praw["n_obs"].get<double>() == raw["n_obs"].get<double>();
    if (!sameSample) {
      return engineErrorBlock(
          "reghdfe residuals(): the residual refit did not reproduce the estimation sample "
          "(e.g. a cluster variable missing on some rows) -- not supported in this version of "
          "PIET.",
          "unsupported_option");
    }
    if (restricted) {
      const std::string unmasked = nextPath(session.workdir(), "reghdfe_resid");
      try {
        runCobraJson({"eval", "--data", residOutPath, "--drop", kResidMaskY, "--out", unmasked});
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      residOutPath = unmasked;
    }
  }

  if (!savedFeName.empty()) {
    // A single absorbed effect is identified by its weighted mean-zero
    // normalization. Recover it from THIS fit's slopes and admission mask,
    // rather than fitting a second model or assigning effects off e(sample).
    try {
      Datastore* ds = session.datastore();
      if (ds == nullptr || !ds->has(savedFeVar) || ds->dtype(savedFeVar) != "f64")
        return engineErrorBlock("saving absorbed effects currently requires a numeric fixed effect",
                                "unsupported_option");
      const int64_t n = ds->nRows();
      std::vector<int> sample(static_cast<size_t>(n), 0);
      std::ifstream mask(cecon::to_native_path(esampleValues));
      int64_t admitted = 0;
      for (int64_t i = 0; i < n; ++i) {
        if (!(mask >> sample[static_cast<size_t>(i)]) ||
            (sample[static_cast<size_t>(i)] != 0 && sample[static_cast<size_t>(i)] != 1))
          return engineErrorBlock("saving absorbed effects: the fitted sample is unavailable");
        admitted += sample[static_cast<size_t>(i)];
      }
      if (!raw.contains("esample_n") || !raw["esample_n"].is_number() ||
          raw["esample_n"].get<double>() != static_cast<double>(admitted))
        return engineErrorBlock("saving absorbed effects: the fitted sample does not match e(N)");
      std::string weight;
      for (size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == "--weight") weight = argv[i + 1];
      const double* w = weight.empty() ? nullptr : ds->col(weight).f64();
      const double* yv = ds->col(y).f64();
      const double* fv = ds->col(savedFeVar).f64();
      std::vector<double> remainder(yv, yv + n);
      for (const auto& row : raw["coeftable"]) {
        const double b = row.value("b", 0.0);
        if (b == 0.0) continue;
        bool ok = true;
        const auto atoms = cecon::margins_parse_column(row.value("name", std::string()), &ok);
        if (!ok) return engineErrorBlock("saving absorbed effects: the fitted design cannot be rebuilt",
                                         "unsupported_option");
        std::vector<const double*> cols;
        for (const auto& a : atoms) {
          if (!ds->has(a.var) || ds->dtype(a.var) != "f64")
            return engineErrorBlock("saving absorbed effects: the fitted design cannot be read");
          cols.push_back(ds->col(a.var).f64());
        }
        for (int64_t i = 0; i < n; ++i) {
          if (!sample[static_cast<size_t>(i)]) continue;
          double x = 1.0;
          for (size_t j = 0; j < atoms.size(); ++j)
            x *= atoms[j].factor ? (cols[j][i] == atoms[j].level ? 1.0 : 0.0) : cols[j][i];
          remainder[static_cast<size_t>(i)] -= b * x;
        }
      }
      std::map<double, std::pair<double, double>> groups;
      double total = 0.0, mass = 0.0;
      for (int64_t i = 0; i < n; ++i) {
        if (!sample[static_cast<size_t>(i)]) continue;
        const double wi = w == nullptr ? 1.0 : w[i];
        auto& g = groups[fv[i]];
        g.first += wi * remainder[static_cast<size_t>(i)];
        g.second += wi;
        total += wi * remainder[static_cast<size_t>(i)];
        mass += wi;
      }
      if (!(mass > 0.0)) return engineErrorBlock("saving absorbed effects: no fitted observations");
      const std::string values = nextPath(session.workdir(), "reghdfe_saved_fe_values");
      std::ofstream out(cecon::to_native_path(values));
      out.precision(17);
      for (int64_t i = 0; i < n; ++i) {
        if (!sample[static_cast<size_t>(i)]) out << ".\n";
        else {
          const auto& g = groups.at(fv[i]);
          out << g.first / g.second - total / mass << '\n';
        }
      }
      out.flush();
      if (!out) return engineErrorBlock("saving absorbed effects: could not write effect values");
      out.close();
      const std::string savedOut = nextPath(session.workdir(), "reghdfe_saved_fe");
      const std::string base = residWanted ? residOutPath : *session.currentHbin();
      const CobraRunResult wr = runCobra({"setcol", "--data", base, "--var", savedFeName,
                                         "--values", values, "--dtype", "f64", "--new",
                                         "--out", savedOut});
      cecon::native_unlink(values);
      if (!wr.ok || !cecon::native_exists(savedOut))
        return engineErrorBlock("saving absorbed effects: " + wr.stderr_text);
      residOutPath = savedOut;
    } catch (const std::exception& e) {
      return engineErrorBlock(std::string("saving absorbed effects: ") + e.what());
    }
  }

  if (feNoCons && raw.is_object() && raw.contains("e_V") && raw["e_V"].is_object()) {
    ojson& ev = raw["e_V"];
    if (ev.contains("names") && ev["names"].is_array() && ev.contains("matrix") && ev["matrix"].is_array()) {
      std::vector<size_t> keepIdx;
      ojson names = ojson::array();
      for (size_t i = 0; i < ev["names"].size(); ++i) {
        const std::string nm = ev["names"][i].is_string() ? ev["names"][i].get<std::string>() : std::string();
        if (nm == "_cons" || nm == "(intercept)") continue;
        keepIdx.push_back(i);
        names.push_back(ev["names"][i]);
      }
      ojson mat = ojson::array();
      for (size_t r : keepIdx) {
        ojson row = ojson::array();
        for (size_t c : keepIdx) row.push_back(ev["matrix"][r][c]);
        mat.push_back(row);
      }
      ev["names"] = names;
      ev["matrix"] = mat;
    }
  }
  ojson block = cobraToEstimation(raw, "feols", &session);
  if (feNoCons && block.contains("coeftable") && block["coeftable"].is_array()) {
    ojson kept = ojson::array();
    for (const auto& row : block["coeftable"]) {
      const std::string nm = row.value("name", std::string());
      if (nm == "_cons" || nm == "(intercept)") continue;
      kept.push_back(row);
    }
    block["coeftable"] = kept;
  }
  // Label-only override: reghdfe IS feols --fe under the hood (cobra has no
  // separate reghdfe estimator), so the cobra invocation and LastModel replay
  // command (below) stay "feols" — only the displayed "estimator" field is
  // renamed to match the Stata command word the user actually typed, so
  // output parity holds (Stata shows "reghdfe", not "feols").
  if (stataCmd == "reghdfe") {
    block["estimator"] = "reghdfe";
  }

  // e(sample) T2: the mask install (esampleInstallOnHead) comes after every
  // refusal above, so a failed command never leaves a new mask under the
  // previous model, and BEFORE setLastModel (BUG-518 order). `fitFiltered` and
  // the fit frame are decided on the head the fit read, before the install
  // moves it. With residuals() the column goes onto the residual frame instead,
  // which becomes the head after e() is posted below.
  const bool fitFiltered = regressDataPath != *session.currentHbin();
  LastModel lm;
  lm.dataPath = *session.currentHbin();
  // FU-475: the engine applied the `if` itself; refits replay it as --keepif
  // (lastModelBaseArgv, runMargins, predict, boottest read lm.fitKeepif). Set
  // before the install, which keeps the fit's frame when this names the mask.
  if (tsEngineKeepif) lm.fitKeepif = *parsed.ifExpr;
  if (residWanted || !savedFeName.empty()) {
    unsigned long long serial = 0;
    const std::string withMask =
        esampleValues.empty()
            ? std::string()
            : esampleInstallColumn(session, esampleValues, raw, residOutPath, false, &serial);
    if (!withMask.empty()) {
      residOutPath = withMask;
      lm.esample = LastModel::EsampleState::kColumn;
      lm.esampleSerial = serial;
    }
  } else if (!esampleValues.empty()) {
    (void)esampleInstallOnHead(session, esampleValues, raw, lm);
  }

  lm.cobraCmd = "feols";
  lm.stataCmd = stataCmd;  // BUG-458: e(cmd) reads "regress"/"reghdfe", not the engine's "feols"
  lm.y = y;
  lm.x = xs;
  // --fe
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--fe" && i + 1 < argv.size()) {
      std::string joined = argv[i + 1];
      std::string cur;
      for (char c : joined) {
        if (c == ',') {
          if (!cur.empty()) lm.fe.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) lm.fe.push_back(cur);
    }
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster.push_back(argv[i + 1]);
    if (argv[i] == "--weight" && i + 1 < argv.size()) lm.fitWeight = argv[i + 1];
    if (argv[i] == "--wtype" && i + 1 < argv.size()) lm.fitWtype = argv[i + 1];
    if (argv[i] == "--keep-singletons") lm.keepSingletons = true;
    if (argv[i] == "--collinear-forward") lm.collinearForward = true;  // BUG-1067
  }
  lm.depvar = block.value("depvar", "");
  // W:P5 fit-sample fidelity (BUG-163): this handler materializes its if/in
  // restriction into a scratch file and fits THAT — record it so
  // test/lincom/margins/predict/boottest refit the same sample. Left empty
  // when no filter applied (regressDataPath == the session head), keeping
  // every unfiltered refit byte-identical to before.
  if (fitFiltered) lm.fitDataPath = regressDataPath;  // e(sample) T2: decided pre-install
  lm.result = raw;
  // BUG-570: e(N_clust) exists only after a clustered fit. The engine reports
  // n_clusters 0 for every other VCE and e(N_clust) resolves from this cached
  // reply, so `display e(N_clust)` printed 0 where Stata prints `.`
  // (w2_pweight.log R2_pw PSTR `120|117|.|robust|...`; R0/R1 and the areg/reghdfe
  // arms without a cluster alike). The key is dropped from the cached reply ONLY:
  // the emitted block keeps the engine's field, whose shape the freeze fixtures pin.
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("n_clusters") &&
      (*lm.result)["n_clusters"].is_number() &&
      (*lm.result)["n_clusters"].get<double>() == 0.0) {
    lm.result->erase("n_clusters");
  }
  // FU-365: after a multiway regress/areg, Stata posts e(kcluster)/e(cluster#) and
  // no e(N_clust) (Stata's multiway-cluster step): w2l_vce_shape_det.log
  // regress_vce_c2 and areg_vce_c2 read `.`. The engine's n_clusters (the
  // smallest G, which still drives df_r = min - 1) is dropped from the cached reply
  // only, like BUG-570 above. reghdfe keeps it (reghdfe_vce_c2 :3820 reads 7).
  const bool postMultiway = builtinMultiway && (lm.vcov == "twoway" || lm.vcov == "multiway") &&
                            lm.result.has_value() && lm.result->is_object();
  if (postMultiway) {
    lm.result->erase("n_clusters");
    // e(rank) is the rank of the (eigenvalue-clipped) V (w2s_multiway_class.log
    // :116 mw_reg_c2_names `...|6|2|180|`), the engine's vcov_rank.
    applyBuiltinMultiwayReplyM4(*lm.result);
  }
  // BUG-1041: areg's MAP path and reghdfe post e(mss) = r(ssr), the model sum of
  // squares about the overall mean (Stata's absorb step, e(r2) =
  // r(ssr)/r(sst)), and e(tss) = r(sst): StataBE areg and reghdfe
  // e(mss) 9177.98 = e(tss) 9544.62 - e(rss) 366.64 under every VCE, where the
  // engine's mss is the within one (areg's hidden e(mss_w)). reghdfe also posts
  // e(tss_within), read here under the reply's own key. Cached reply only: the
  // emitted block keeps the engine's fields, whose shape the freeze fixtures pin.
  if ((stataCmd == "areg" || stataCmd == "reghdfe") && lm.result.has_value() &&
      lm.result->is_object() && lm.result->contains("tss_total") &&
      (*lm.result)["tss_total"].is_number() && lm.result->contains("rss") &&
      (*lm.result)["rss"].is_number()) {
    const double tss = (*lm.result)["tss_total"].get<double>();
    (*lm.result)["mss"] = tss - (*lm.result)["rss"].get<double>();
    (*lm.result)["tss"] = tss;
  }
  // BUG-1268: a constant-only `regress y` posts e(F) 0 (StataBE 19.5 `F(0, 39) =
  // 0.00`); the engine's reply carries no F when df_m is 0. Cached reply only,
  // like the keys above.
  if (stataCmd == "regress" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("df_m") && (*lm.result)["df_m"].is_number() &&
      (*lm.result)["df_m"].get<double>() == 0.0 &&
      !(lm.result->contains("F") && (*lm.result)["F"].is_number()))
    (*lm.result)["F"] = 0.0;
  // reghdfe's e(N_clustervars) and e(N_clust1..k) (w2b_estim.log:1410
  // reghdfe_cluster3 reads e(N_clustervars) 3). Cached reply only, like df_a
  // below: the resolver's key == name fall-through reads them. N_clustervars
  // is the number of forwarded cluster names; N_clust# are the engine's
  // per-dimension G (n_clusters_by, a JSON ARRAY read element by element --
  // never pushed as a block). Only the multiway fit is pinned by an arm; the
  // one-way and two-way values follow reghdfe's documented e() list.
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object() &&
      (lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway") &&
      !lm.cluster.empty()) {
    size_t nVars = 1;
    for (char c : lm.cluster.back()) {
      if (c == ',') ++nVars;
    }
    (*lm.result)["N_clustervars"] = static_cast<double>(nVars);
    if (lm.result->contains("n_clusters_by") && (*lm.result)["n_clusters_by"].is_array()) {
      size_t j = 0;
      for (const auto& g : (*lm.result)["n_clusters_by"]) {
        ++j;
        if (g.is_number()) (*lm.result)["N_clust" + std::to_string(j)] = g.get<double>();
      }
    } else if (nVars == 1 && lm.result->contains("n_clusters") &&
               (*lm.result)["n_clusters"].is_number()) {
      (*lm.result)["N_clust1"] = (*lm.result)["n_clusters"].get<double>();
    }
  }
  // reghdfe's e(N_full): e(N) plus the singleton rows the fit dropped
  // (reghdfe `st_numscalar("e(N_full)", st_numscalar("e(N)") +
  // num_singletons)`; w2v_margins_corpus F3_at_atmeans_enopost N_full=240).
  // Cached reply only, read by the resolver's key == name fall-through; set
  // only when the engine reports both counts.
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("n_obs") && (*lm.result)["n_obs"].is_number() &&
      lm.result->contains("n_dropped_singletons") &&
      (*lm.result)["n_dropped_singletons"].is_number()) {
    (*lm.result)["N_full"] = (*lm.result)["n_obs"].get<double>() +
                             (*lm.result)["n_dropped_singletons"].get<double>();
  }
  // e(df_a) after areg / reghdfe (derivation and oracle arms: absorbedDofA).
  // Like N_g/df_a on the xtreg,fe route, it goes into the cached reply only
  // (the e() resolver falls through key == name); the emitted block, whose
  // shape the freeze fixtures pin, is unchanged.
  if (lm.result.has_value()) {
    if (const auto dfA = absorbedDofA(*lm.result, stataCmd)) (*lm.result)["df_a"] = *dfA;
  }
  // FU-479: reghdfe's e(N_hdfe), the number of absorbed FE dimensions
  // (w2_esample.log:818-819 D1_reghdfe_singleton_out absorb(id) -> 1;
  // predict-e-xbd.log:348 absorb(id f2) -> 2). The reply carries no such key;
  // its `absorbed` object holds one entry per absorbed term the engine fitted
  // (apps/cmds/common.cpp run_feols, md.fe), so its size is the count. Cached
  // reply only (key == name fall-through), like df_a above.
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("absorbed") && (*lm.result)["absorbed"].is_object() &&
      !(*lm.result)["absorbed"].empty()) {
    (*lm.result)["N_hdfe"] = static_cast<double>((*lm.result)["absorbed"].size());
  }
  // BUG-1510: reghdfe's degrees-of-freedom bookkeeping (StataBE 19.5, 180 rows, id 30
  // categories, id2 7, id3 5; reghdfe.mata's estimate_dof, method identified, not
  // ported). Each absorbed variable has K categories, M of them redundant:
  //  * one that is a cluster variable or nested in one (the reply's nested_fe) is all
  //    redundant, M = K (Nested? 1);
  //  * of the others, in absorb() order, each but the first has at least one redundant
  //    category, the first too when a nested one exists;
  //  * each later one takes the most connected components (mobility groups) it forms
  //    with any earlier one ("pairwise"); the second one's count is exact, those after
  //    it are flagged Inexact? 1.
  // e(df_a_initial) = sum K, e(df_a_redundant) = sum M, e(df_a) = their difference and
  // e(df_a_nested) the nested variables' K: absorb(id) 30/0/30, absorb(id id2) 37/1/36,
  // absorb(id id2 id3) 42/2/40 with e(dof_table) (30 0 30 0 0 \ 7 1 6 0 0 \ 5 1 4 1 0),
  // absorb(id id2) vce(cluster id) and absorb(id2 id) vce(cluster id) 37/31/6 (nested 30),
  // absorb(id id2 id4) with id4 nested in id 40/4/36 (id4 forms 3 groups with id). For
  // two variables the components are the engine's (k_absorbed = K1 + K2 - components);
  // for three or more they are counted here over e(sample). noabsorb absorbs `_cons`
  // (1/0/1, e(N_hdfe) 1). e(df_a) takes the same count, so it also covers the cases
  // absorbedDofA leaves missing or miscounts (StataBE: absorb(id id4) vce(cluster id) 2,
  // absorb(id2 id5 id3) with id5 nested in id2 11; the engine's e(df_r) keeps its own
  // count for three or more FEs). Nothing is posted (e(df_a) stays absorbedDofA's) for
  // an absorbed term that is not a plain numeric variable, or a fit whose e(sample) is
  // not on the working frame when the count needs it. Cached reply only, like df_a
  // above; the strings and e(dof_table) are posted with the model below.
  struct HdfeDof {
    std::vector<std::string> names;  // e(extended_absvars), the dof_table rows
    std::vector<double> K, M, inexact, nested;
  };
  std::optional<HdfeDof> hdfeDof;
  if (stataCmd == "reghdfe" && lm.result.has_value() && lm.result->is_object()) {
    const ojson& rr = *lm.result;
    HdfeDof d;
    bool ok = true;
    if (lm.fe.empty()) {
      d = HdfeDof{{"_cons"}, {1.0}, {0.0}, {0.0}, {0.0}};
    } else if (rr.contains("absorbed") && rr["absorbed"].is_object() &&
               rr["absorbed"].size() == lm.fe.size()) {
      std::set<std::string> nestedSet;
      if (rr.contains("nested_fe") && rr["nested_fe"].is_array()) {
        for (const auto& n : rr["nested_fe"]) {
          if (n.is_string()) nestedSet.insert(n.get<std::string>());
          else ok = false;
        }
      }
      // in absorb() order (lm.fe), which the reply's `absorbed` object does not keep
      for (const std::string& name : lm.fe) {
        ok = cecon::ident::isValidName(name) && rr["absorbed"].contains(name) &&
             rr["absorbed"][name].is_number();
        if (!ok) break;
        const bool nst = nestedSet.count(name) > 0;
        d.names.push_back(name);
        d.K.push_back(rr["absorbed"][name].get<double>());
        d.M.push_back(nst ? d.K.back() : 0.0);
        d.inexact.push_back(0.0);
        d.nested.push_back(nst ? 1.0 : 0.0);
      }
      std::vector<size_t> rest;  // the variables not nested in a cluster, in order
      for (size_t g = 0; ok && g < d.names.size(); ++g)
        if (d.nested[g] == 0.0) rest.push_back(g);
      const bool anyNested = rest.size() < d.names.size();
      for (size_t r = 0; ok && r < rest.size(); ++r) {
        if (r > 0 || anyNested) d.M[rest[r]] = 1.0;
        if (r > 1) d.inexact[rest[r]] = 1.0;
      }
      if (ok && rest.size() >= 2) {
        // comp[a][b], a < b: the connected components of rest[a] and rest[b]
        std::vector<std::vector<double>> comp(rest.size(), std::vector<double>(rest.size(), 0.0));
        if (d.names.size() == 2 && rr.contains("k_absorbed") && rr["k_absorbed"].is_number()) {
          comp[0][1] = d.K[0] + d.K[1] - rr["k_absorbed"].get<double>();
        } else {
          const auto rows = est_detail::esampleRowsM4(session, lm);
          const char* rowsKey = rr.contains("n_rows") ? "n_rows" : "n_obs";
          ok = rows.has_value() && rr.contains(rowsKey) && rr[rowsKey].is_number() &&
               static_cast<double>(rows->size()) == rr[rowsKey].get<double>();
          // each variable's categories coded 0..K-1 over e(sample)'s rows
          std::vector<std::vector<size_t>> codes(rest.size());
          std::vector<size_t> levels(rest.size(), 0);
          for (size_t r = 0; ok && r < rest.size(); ++r) {
            const double* v = est_detail::f64ColumnM4(session, d.names[rest[r]]);
            ok = v != nullptr;
            if (!ok) break;
            std::map<double, size_t> lev;
            codes[r].reserve(rows->size());
            for (const int64_t i : *rows)
              codes[r].push_back(lev.emplace(v[i], lev.size()).first->second);
            levels[r] = lev.size();
            ok = static_cast<double>(levels[r]) == d.K[rest[r]];  // the engine's categories
          }
          // a pair's components: union-find over its categories, joined by every row
          for (size_t a = 0; ok && a < rest.size(); ++a) {
            for (size_t b = a + 1; b < rest.size(); ++b) {
              std::vector<size_t> parent(levels[a] + levels[b]);
              std::iota(parent.begin(), parent.end(), size_t{0});
              const auto find = [&parent](size_t x) {
                while (parent[x] != x) x = parent[x] = parent[parent[x]];  // path halving
                return x;
              };
              size_t groups = parent.size();
              for (size_t i = 0; i < codes[a].size(); ++i) {
                const size_t p = find(codes[a][i]), q = find(levels[a] + codes[b][i]);
                if (p != q) {
                  parent[p] = q;
                  --groups;
                }
              }
              comp[a][b] = static_cast<double>(groups);
            }
          }
        }
        for (size_t b = 1; ok && b < rest.size(); ++b)
          for (size_t a = 0; a < b; ++a) d.M[rest[b]] = std::max(d.M[rest[b]], comp[a][b]);
      }
    } else {
      ok = false;
    }
    if (ok) hdfeDof = d;
  }
  if (hdfeDof.has_value()) {
    ojson& rr = *lm.result;
    double initial = 0.0, redundant = 0.0, nested = 0.0;
    for (size_t g = 0; g < hdfeDof->names.size(); ++g) {
      initial += hdfeDof->K[g];
      redundant += hdfeDof->M[g];
      if (hdfeDof->nested[g] != 0.0) nested += hdfeDof->K[g];
    }
    rr["df_a"] = initial - redundant;
    rr["df_a_initial"] = initial;
    rr["df_a_redundant"] = redundant;
    rr["df_a_nested"] = nested;
    rr["N_hdfe"] = static_cast<double>(hdfeDof->names.size());
    rr["N_hdfe_extended"] = static_cast<double>(hdfeDof->names.size());
    const bool singletons = rr.contains("n_dropped_singletons") &&
                            rr["n_dropped_singletons"].is_number();
    rr["num_singletons"] = singletons ? rr["n_dropped_singletons"].get<double>() : 0.0;
    // e(converged): the absorption converged (the engine's demeaning; 1 with nothing absorbed)
    bool converged = true;
    if (rr.contains("diagnostics") && rr["diagnostics"].is_object() &&
        rr["diagnostics"].contains("demean_converged") &&
        rr["diagnostics"]["demean_converged"].is_boolean())
      converged = rr["diagnostics"]["demean_converged"].get<bool>();
    rr["converged"] = converged ? 1.0 : 0.0;
  }
  // e(vce) after a default-VCE `regress` is `ols` (w2_pweight.log R0_unweighted
  // PSTR `120|117|.|ols|||||`, R1_aw_control `...|ols|||aweight|= w|`;
  // w2h_postest_refit.log R_ctl/R_aw/R_fw `ols`), and so it is after areg and
  // reghdfe (w2p_estim_ev.log:1154 F_areg_aw `...|60|ols|`; w2l_vce_shape_det.log
  // :695 areg, :3938 reghdfe). lm.vcov keeps the engine spelling `iid`, which the
  // replay machinery keys on; LastModel::eVce carries Stata's, so a later
  // setLastModel() on the same model (setEstHandleM4) republishes `ols` too.
  if ((stataCmd == "regress" || stataCmd == "areg" || stataCmd == "reghdfe") && lm.vcov == "iid")
    lm.eVce = "ols";
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // eb-order T-P1: e(V) is the fit's own (w2g_ebmat.log:365)
  // BUG-1189: reghdfe's e(clustvar1..k), the cluster variables in cluster() order
  // beside e(N_clust1..k) above (StataBE 19.5: vce(cluster id id2 id3) e(clustvar1)
  // id, e(clustvar2) id2, e(clustvar3) id3; vce(cluster id) e(clustvar1) id; none
  // under vce(robust) or the default VCE). Recorded with noteEOnFit, so the next
  // install drops them.
  if (stataCmd == "reghdfe" &&
      (lm.vcov == "cluster" || lm.vcov == "twoway" || lm.vcov == "multiway") &&
      !lm.cluster.empty()) {
    size_t j = 0;
    std::string cur;
    for (const char c : lm.cluster.back() + ",") {
      if (c != ',') {
        cur += c;
        continue;
      }
      if (cur.empty()) continue;
      const std::string nm = "clustvar" + std::to_string(++j);
      session.eval.setEString(nm, cur);
      session.noteEOnFit(nm);
      cur.clear();
    }
  }
  // BUG-1510 (above): reghdfe's absorbed-variable macros and e(dof_table), one row per
  // absorbed variable named `1.var` (`_cons` with noabsorb), columns Categories,
  // Redundant, Num Coefs, Inexact?, Nested? (StataBE 19.5 `matrix list e(dof_table)`;
  // e(title2) "Absorbing 3 HDFE groups", e(dofmethod) "pairwise clusters continuous").
  if (hdfeDof.has_value()) {
    std::string absvars;
    for (const auto& n : hdfeDof->names) absvars += (absvars.empty() ? "" : " ") + n;
    const size_t g = hdfeDof->names.size();
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"absvars", absvars},
             {"extended_absvars", absvars},
             {"title2", "Absorbing " + std::to_string(g) + " HDFE group" + (g == 1 ? "" : "s")},
             {"dofmethod", "pairwise clusters continuous"}}) {
      session.eval.setEString(name, value);
      session.noteEOnFit(name);
    }
    eval::EvalMatrix t;
    t.colnames = {"Categories", "Redundant", "Num Coefs", "Inexact?", "Nested?"};
    for (size_t k = 0; k < g; ++k) {
      t.rownames.push_back(lm.fe.empty() ? hdfeDof->names[k] : "1." + hdfeDof->names[k]);
      t.data.push_back({hdfeDof->K[k], hdfeDof->M[k], hdfeDof->K[k] - hdfeDof->M[k],
                        hdfeDof->inexact[k], hdfeDof->nested[k]});
    }
    auto& store = session.eval.eMatrices;
    store.erase(std::remove_if(store.begin(), store.end(),
                               [](const auto& p) { return p.first == "dof_table"; }),
                store.end());
    store.emplace_back("dof_table", std::move(t));
    session.noteEOnFit("dof_table");
  }
  // e(kcluster) and e(cluster1..) after a built-in multiway regress/areg
  // (Stata's multiway-cluster step; w2s_multiway_class.log:133 mw_reg_c2_kclust
  // `30|7|180|id|id2|id#id2|`), as xtreg's multiway fit posts them.
  if (postMultiway) postBuiltinMultiwayEM4(session, raw);
  // eb-order ROUND 3: a table without `_cons` is Stata's e(b) only for a
  // noconstant fit (w2g_ebmat.log:1241 e9_colnames_eb `x1 x2`); the FE fits
  // drop `_cons` from the table for other reasons (resolveEMatrixFor's
  // kNeedCons). Session block only, like e_V: the console block is unchanged.
  if (noConstant && session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
    (*session.lastEstBlock)["noconstant"] = true;
  // Stata's ordered `: e(scalars)'/`: e(macros)'/`: e(matrices)' for the measured
  // regress/areg paths (w2s_multiway_class mw_reg_*/mw_areg_*; w2v_estpost_ematrix
  // ep15d_ematrices `b V beta`).
  postENameListsRegressM4(session, raw, parsed, stataCmd, builtinMultiway, noConstant);

  // BUG-539: e(resid) names the column made by THIS fit (w2_smalls.log
  // reghdfe_e_resid); setLastModel() just erased the previous fit's. The
  // column itself lands on the live head only now, after e() is posted, so
  // the head the fit (and lm.dataPath) read is untouched until this point.
  if (residWanted || !savedFeName.empty()) {
    session.load(residOutPath);
    // A replaced `_reghdfe_resid` is a rewrite (mutation_result.h's third
    // slot), a fresh name is a pure add; the value is the same either way.
    std::vector<std::string> added, rewritten;
    if (residWanted) (residExisted ? rewritten : added).push_back(residName);
    if (!savedFeName.empty()) added.push_back(savedFeName);
    *residMutationOut = mutationResult(session, "gen", added, {}, rewritten);
    if (residWanted) session.eval.setEString("resid", residName);
    // BUG-1260: reghdfe stores its residuals as double (StataBE 19.5
    // scratch-laneA/agents/N1/pex.do: `: type _reghdfe_resid` double); the engine
    // wrote them as doubles, and without the record `: type` read float.
    if (residWanted) session.varMeta[residName].stataType = "double";
    if (!savedFeName.empty()) session.varMeta[savedFeName].stataType = "double";
  }

  return block;
}

}  // namespace piet::handlers
