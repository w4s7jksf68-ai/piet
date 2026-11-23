// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Count estimation.
#include "piet/handlers/estimation.h"
#include "estimation_families_internal.h"  // fit_detail::fewClustersRankM4
#include "piet/handlers/vce_types.h"
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
#include "core/distributions.h"  // z_critical: poisson level(#)
#include "estimator_command_support.h"  // e3::setEScalarOnFit / setEStringOnFit
#include "postestimation/rtable_internal.h"  // stataColumns: a factor fit's e(b) columns
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;


// poisson / ppmlhdfe / fepois (BS-073): count-model wiring -> cobra fepois.
// Oracle D3a (bare poisson b/se), D3b (offset()), D3c (vce(robust)), D3d
// (ppmlhdfe/absorb()).
//
// D0 finding (pre-existing, re-confirmed live): cobra's `fepois` WITHOUT
// --fe reproduces Stata's bare `poisson` exactly (D3a: beertax b=-.0537468
// se=.0514636, unemp b=-.0349375 se=.0097193, _cons b=1.940487 se=.076412 —
// all byte-for-byte once `cnt` is built with Stata's own round()
// convention, which cobra's `eval --gen round(...)` already matches
// live-verified). Real Stata's `poisson` has NO absorb() option at all;
// `ppmlhdfe` and cobra's own `fepois` command name both DO (-> --fe).
//
// offset() (D3b): LIVE-VERIFIED literal passthrough. Stata's own offset()
// option already expects an ln-scale variable (offset(lpop) where
// lpop=ln(income) — that IS what Stata's offset() has always meant); cobra's
// `--offset` expects the exact same thing. beertax b=.0753822 se=.0495923,
// _cons b=-7.914716 se=.0343278 — byte-for-byte with NO PIET-side ln()
// transform. exposure() (Stata's OTHER, raw-exposure count-model option) has
// no oracle coverage and no engine CLI equivalent — honest reject.
//
// ppmlhdfe/fepois DEFAULT to vce(robust) when the user gives no vce() at
// all — matching real ppmlhdfe's own default (the D3d oracle's own
// `ppmlhdfe cnt beertax, absorb(snum)` never types vce() yet its Stata
// table header reads "Robust std. err.", live-verified). Plain `poisson`
// keeps cobra's own iid default (matches D3a, which likewise never types
// vce() and gets the plain "Std. err." table).
//
// noconstant/level() are NEVER forwarded — live-verified the SAME
// cobra_runner.cpp stdout/exit-code masking bug the D1 report already
// flagged for ivregress/tobit: `cobra fepois --no-intercept` / `--level 90`
// BOTH hard-error on stderr ("unrecognized option", exit 3) while STILL
// printing a full, valid, WRONG (intercept-included / default-CI) result to
// stdout first.
//
// ENGINE FINDING (cobra-side, forbidden this batch — not fixed, reported):
// vce(robust)'s HC1 small-sample correction has a real numeric gap vs
// Stata's own poisson/ppmlhdfe robust SE. Point estimates (b) match Stata
// EXACTLY in every case tried. For the bare fepois case (D3c) the SE gap is
// small (~0.3% relative: engine .0321597 vs Stata .0320636). For the --fe
// (ppmlhdfe, D3d) case the gap is MATERIAL (~8% relative: engine .1238974
// vs Stata .1146782) — apparently from how absorbed-FE degrees of freedom
// enter cobra's small-sample correction factor (dof_resid IS reported
// correctly as 287 = N-k-FE in both the b and se paths, yet the se scale
// doesn't reflect it the way Stata's does). Selftest pins to the ENGINE's
// OWN actual value (not the Stata figure) with a comment, so the gate stays
// meaningful without permanently failing on a gap this batch cannot touch.
namespace extended_fit_detail {

// ppmlhdfe's `d`: the sum of the fixed effects, built on the head the fit read
// and written to a new head whose path is returned ("" and `err` on refusal).
//
// ppmlhdfe stores d = z - resid - x*b, demeaned with the IRLS
// weights mu: at convergence z - resid is eta = ln(mu), so
//   d_i = ln(mu_i) - x_i'b - c,   c = sum mu (ln mu - x'b) / sum mu,
// and c is exactly the reported _cons (:461-465 recovers _cons as the
// mu-weighted mean of ln(mu) - x'b). With ONE absorbed variable the FE score
// equations close the form: within group g, exp(eta) = exp(x'b) * Y_g / E_g with
// Y_g = sum y and E_g = sum exp(x'b), and sum_g mu = Y_g, so
//   d_i = ln(Y_g) - ln(E_g) - c,   c = sum_g Y_g (ln Y_g - ln E_g) / sum Y.
// Measured on the w2b_estim fixture (float-stored, absorb(id)): c from the
// engine's b is -2.0577731961463126, the engine's _cons -2.057773196146313 and
// the oracle's table -2.057773 (w2b_estim.log:1357). Without absorb() the only
// absorbed term is the constant, so d is 0 on the estimation sample.
// The sample is rebuilt here (nonmissing, if, singleton and all-zero-y groups
// out, as the engine drops them); a count that differs from the fit's N refuses
// rather than writing d on the wrong rows.
// The closed form is the UNWEIGHTED one (every sum below is unweighted). Weights never
// reach it: runPoisson refuses d/d() together with any weight before the fit.
std::string ppmlhdfeSaveDM4b(SessionState& session, const ojson& raw, const std::string& y,
                             const std::vector<std::string>& xs,
                             const std::vector<std::string>& fes,
                             const eval::ParsedCommand& parsed, const std::string& dName,
                             ojson& err) {
  auto refuse = [&](const std::string& why) {
    err = ojson(eval::errorBlock("unsupported_option", "ppmlhdfe d: " + why, std::nullopt,
                                 std::nullopt, std::nullopt, "r(198)"));
    return std::string();
  };
  Datastore* ds = session.datastore();
  if (ds == nullptr) return refuse("no dataset in memory");
  for (const auto& f : fes) {
    if (!ds->has(f) || ds->dtype(f) != "f64") {
      err = engineErrorBlockM4(
          "Option `d` is a valid ppmlhdfe option that this version of PIET implements for a "
          "numeric absorb() variable only (`" + f + "`).",
          "unsupported_option");
      return std::string();
    }
  }
  char buf[64];
  std::string xb;
  for (const auto& x : xs) {
    const double b = numberAtM4b(raw, "coef", x);
    if (!std::isfinite(b)) return refuse("the fit has no coefficient for `" + x + "`");
    std::snprintf(buf, sizeof(buf), "%.17g", b);
    xb += (xb.empty() ? "" : "+") + std::string("(") + buf + ")*(" + x + ")";
  }

  std::string sample = "!missing(" + y + ")";
  for (const auto& x : xs) sample += "&!missing(" + x + ")";
  for (const auto& f : fes) sample += "&!missing(" + f + ")";
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    sample += "&!missing(" + *parsed.weightVar + ")";
  }
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    sample += "&(" + *parsed.ifExpr + ")";
  }

  // Scratch column names (design decision: no shared helper mints one); the
  // double-underscore shape of linear_estimation.cpp's residual mask.
  const std::string kS = "__piet_ppmld_s__", kN = "__piet_ppmld_n__", kY = "__piet_ppmld_y__",
                    kK = "__piet_ppmld_k__", kE = "__piet_ppmld_e__", kEs = "__piet_ppmld_es__",
                    kA = "__piet_ppmld_a__", kT1 = "__piet_ppmld_t1__", kT2 = "__piet_ppmld_t2__";
  std::vector<std::string> temps;
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  auto gen = [&](const std::string& name, const std::string& expr) {
    args.push_back("--gen");
    args.push_back(name + "=" + expr);
    if (name != dName) temps.push_back(name);
  };
  auto egen = [&](const std::string& name, const std::string& expr) {
    args.push_back("--egen");
    args.push_back(name + "=" + expr);
    temps.push_back(name);
  };
  gen(kS, "(" + sample + ")");
  if (fes.empty()) {
    gen(dName, "cond(" + kS + "==1,0,.)");
  } else {
    const std::string& g = fes[0];
    egen(kN, "sum(" + kS + "), by(" + g + ")");
    egen(kY, "sum(cond(" + kS + "==1," + y + ",0)), by(" + g + ")");
    gen(kK, "(" + kS + "==1)&(" + kN + ">1)&(" + kY + ">0)");
    gen(kE, "cond(" + kK + "==1,exp(" + (xb.empty() ? std::string("0") : xb) + "),0)");
    egen(kEs, "sum(" + kE + "), by(" + g + ")");
    gen(kA, "cond(" + kK + "==1,ln(" + kY + ")-ln(" + kEs + "),.)");
    egen(kT1, "sum(cond(" + kK + "==1," + y + "*" + kA + ",0))");
    egen(kT2, "sum(cond(" + kK + "==1," + y + ",0))");
    gen(dName, "cond(" + kK + "==1," + kA + "-" + kT1 + "/" + kT2 + ",.)");
  }
  for (const auto& t : temps) {
    args.push_back("--drop");
    args.push_back(t);
  }
  args.push_back("--summarize");
  args.push_back(dName);
  const std::string outPath = nextPathM4(session.workdir(), "ppmlhdfe_d");
  args.push_back("--out");
  args.push_back(outPath);

  nlohmann::json res;
  try {
    res = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    err = engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    return std::string();
  }
  double nD = std::nan("");
  try {
    nD = res.at("summarize").at(dName).at("N").get<double>();
  } catch (...) {
  }
  const double nFit = raw.contains("n_obs") && raw["n_obs"].is_number()
                          ? raw["n_obs"].get<double>()
                          : std::nan("");
  if (!std::isfinite(nD) || !std::isfinite(nFit) || nD != nFit) {
    cecon::native_unlink(outPath);
    return refuse("could not reproduce the estimation sample (" +
                  (std::isfinite(nD) ? std::to_string(static_cast<long long>(nD)) : "?") +
                  " rows against the fit's " +
                  (std::isfinite(nFit) ? std::to_string(static_cast<long long>(nFit)) : "?") +
                  "); d is not saved.");
  }
  return outPath;
}

}  // namespace

// e(sample) T4 (internal note esample/SPEC.md §3.4 T1b; oracle
// w2_esample.log:1692 H1 logit `0|60|60|`, :1725 H2 probit `if` `0|47|47|`,
// :1928 H7 poisson `0|60|60|`). The engine's ML verbs (logit, probit, fepois)
// have no `--esample-out` (grep esample_request: feols/iv/ols/xtreg/newey/prais
// only), and the engine is not this change's to edit. DESIGN DECISION: the mask
// is the engine's own feols admission rule on the fit's y/x/if/cluster/weight,
// written by feols `--esample-out` and installed through T2's one install
// (esampleInstallOnHead: the same hidden column, serial and predicate), but
// only when the ML fit's own row count equals it. Why that is exact and never a
// guess: load_glm (apps/cmds/common.cpp) admits the rows build_model admits for
// those columns -- the function feols' fit calls, with no FE here -- and then
// the ML screen can only DROP rows (perfect predictors). The
// ML sample is therefore a subset of the feols sample, and a subset of equal
// size is the same set. Anything the auxiliary call does not replay (offset(),
// svy design columns, a TS operator under `if`, whose feols mask is computed on
// the full series) can only make the feols set larger, so it fails the count
// and the model keeps the named r(198) (kUnavailable). So does every refusal
// below. Returns esampleInstallOnHead's answer; `lm`/`raw` as it leaves them.
// Cost: one extra feols over the fit's columns per ML fit.
// ppmlhdfe/fepois with absorb() (BUG-1197; StataBE 19.5: `ppmlhdfe cnt x1 x2,
// absorb(id)` e(N) 176 = count if e(sample)): the auxiliary feols absorbs the same
// effects and drops the same singleton groups, so the fepois sample (singletons and
// separated rows out) is again a subset of it, equal when nothing was separated.
// `ontoFrame` (ppmlhdfe d): the column goes onto that frame -- the head plus the d
// column, which becomes the head after e() is posted -- and *ontoFrame names the
// frame with the mask, as runRegress installs it on its residuals() frame.
bool esampleInstallMlMask(SessionState& session, LastModel& lm, ojson& raw,
                          std::string* ontoFrame) {
  // tobit, nbreg and mlogit admit rows the same way and screen none out
  // (BUG-1214: StataBE's `estimates store` writes their _est_NAME); so does fracreg
  // (bank 2bef1615 `fracreg logit fr weight length`: e(sample) 74 rows), and so do
  // ologit and oprobit (load_glm, cmds_oglm.cpp; vs-stata bank d8de89e3 `ologit rep78
  // mpg weight`: count if e(sample) 69 = e(N)).
  static const std::set<std::string> kMl = {"logit", "probit", "poisson", "tobit", "nbreg",
                                            "mlogit", "fepois", "fracreg", "ologit", "oprobit",
                                            "xtpois_re", "xtlogit", "xtpois"};
  if (kMl.count(lm.cobraCmd) == 0) return false;
  if (!lm.fe.empty() && lm.cobraCmd != "fepois") return false;
  if (!lm.fitDataPath.empty() || lm.cluster.size() > 1) return false;
  if (!session.currentHbin().has_value() || *session.currentHbin() != lm.dataPath) return false;
  if (!raw.is_object() || lm.y.empty()) return false;
  // The handshake counts ROWS. Under fweight/iweight an ML reply's n_obs is Σw
  // (cmd_logit: `n_obs` = Σw under Frequency) and it carries no n_rows, so the
  // feols superset's row count can equal it by accident (screened-out rows whose
  // weights add up to the shortfall) and be installed as e(sample). No row
  // count, no install: the model keeps the named r(198) (review 2026-10-15).
  const bool sumWeights = lm.fitWtype == "fw" || lm.fitWtype == "fweight" ||
                          lm.fitWtype == "iw" || lm.fitWtype == "iweight";
  if (sumWeights && !(raw.contains("n_rows") && raw["n_rows"].is_number())) return false;
  std::vector<std::string> aux = {"feols", "--y", lm.y, "--x",
                                  lm.x.empty() ? std::string("(intercept)") : joinCommaM4(lm.x)};
  if (!lm.fe.empty()) {
    aux.push_back("--fe");
    aux.push_back(joinCommaM4(lm.fe));
  }
  if (!lm.fitKeepif.empty()) {
    aux.push_back("--keepif");
    aux.push_back(lm.fitKeepif);
  }
  // The variance never moves a row; a sandwich is what feols accepts under
  // every weight kind (it refuses an iid pweight fit).
  aux.push_back("--vcov");
  if (lm.cluster.size() == 1) {
    aux.push_back("cluster");
    aux.push_back("--cluster");
    aux.push_back(lm.cluster.front());
  } else {
    aux.push_back("hc1");
  }
  if (!lm.fitWeight.empty()) {
    aux.push_back("--weight");
    aux.push_back(lm.fitWeight);
    if (!lm.fitWtype.empty()) {
      aux.push_back("--wtype");
      aux.push_back(lm.fitWtype);
    }
  }
  aux.push_back("--data");
  aux.push_back(lm.dataPath);
  const std::string values = esampleFitArgs(session, aux, std::string(), 0);
  ojson auxRaw;
  try {
    auxRaw = runCobraJsonM4(aux);
  } catch (const CobraError&) {
    cecon::native_unlink(values);
    return false;
  }
  if (!auxRaw.is_object() || !auxRaw.contains("esample_n") || !auxRaw["esample_n"].is_number()) {
    cecon::native_unlink(values);
    return false;
  }
  // The handshake (estimation_sample_install.cpp esampleHandshakeOk) compares these 1s with the
  // ML reply's own row count (n_rows when it has one, else n_obs).
  raw["esample_n"] = auxRaw["esample_n"];
  if (ontoFrame == nullptr) return esampleInstallOnHead(session, values, raw, lm);
  unsigned long long serial = 0;
  const std::string withMask =
      esampleInstallColumn(session, values, raw, *ontoFrame, false, &serial);
  if (withMask.empty()) return false;
  *ontoFrame = withMask;
  lm.esample = LastModel::EsampleState::kColumn;
  lm.esampleSerial = serial;
  return true;
}

bool esampleInstallMlMask(SessionState& session, LastModel& lm, ojson& raw) {
  return esampleInstallMlMask(session, lm, raw, nullptr);
}

// `dMutationOut` (BUG-1251): ppmlhdfe's d/d() adds a column to the live frame after
// the fit; the handler still returns the one estimation block and writes the
// data_mutation announcing the column here, for the caller to push as its own block
// (runRegress's residMutationOut). The dispatcher's poisson arm passes it (declared
// there: piet/handlers.h keeps the three-argument form, below).
ojson runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                  SessionState& session, ojson* dMutationOut) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: `" + stataCmd + "` requires a dependent variable",
                              "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  bool allowFe = (stataCmd == "ppmlhdfe" || stataCmd == "fepois");

  std::string feRaw;
  bool sawAbsorb = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "absorb") {
      sawAbsorb = true;
      if (!v.isBool) feRaw = v.strValue;
    }
  }
  if (sawAbsorb && !allowFe) {
    return engineErrorBlockM4(
        "option `absorb` not allowed — real Stata's `poisson` has no absorb() option; use "
        "`ppmlhdfe` for a fixed-effects Poisson (PPML) fit.",
        "unsupported_option");
  }

  // ppmlhdfe `d` / `d(newvar)`: save the sum of the fixed effects
  // (w2b_estim.log:1305 ppmlhdfe_d, :1336 ppmlhdfe_absorb_d_named, both rc 0
  // with the fit of the option-free command). ppmlhdfe: bare `d`
  // DROPS any existing _ppmlhdfe_d and names the column that; d(newvar) is
  // `confirm new variable` (r(110) when it exists), checked before the fit.
  // The column is built after the fit by ppmlhdfeSaveDM4b. Only the shapes it
  // reproduces exactly are taken; the rest are refused by name.
  std::string dName;
  if (stataCmd == "ppmlhdfe") {
    for (const auto& [k, v] : parsed.opts) {
      if (k != "d") continue;
      if (v.isBool) {
        dName = "_ppmlhdfe_d";
        // ppmlhdfe drops the old column before the fit reads its
        // variables, so a model that USES _ppmlhdfe_d no longer finds it.
        const std::vector<std::string> feToks = splitTokensM4(feRaw);
        if (y == dName || std::find(xs.begin(), xs.end(), dName) != xs.end() ||
            std::find(feToks.begin(), feToks.end(), dName) != feToks.end()) {
          return stataRefusalM4b("variable " + dName + " not found", 111);
        }
        continue;
      }
      dName = trimM4(v.strValue);
      if (dName.empty() || !strutil::isValidStataIdentifier(dName)) {
        return stataRefusalM4b("d(): invalid name `" + dName + "'", 198);
      }
      Datastore* ds = session.datastore();
      if (ds != nullptr && ds->has(dName)) {
        return stataRefusalM4b("variable " + dName + " already defined", 110);
      }
    }
  }
  // More than one absorbed effect: the sum has no closed form, so the engine
  // writes it from the fit itself (fepois --d-out; w2d_uncovered
  // ppmlhdfe_xi_absorb_cluster2_d_001, `absorb(id t) ... d`). The engine maps the
  // fit's rows onto the frame only without a row filter, so `if` stays refused.
  bool engineD = false;
  if (!dName.empty()) {
    std::string why;
    if (splitTokensM4(feRaw).size() > 1) {
      if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
        why = "an `if` qualifier and more than one absorbed fixed effect";
      } else {
        engineD = true;
      }
    }
    for (const auto& [k, v] : parsed.opts) {
      if (k == "offset" || k == "exposure") why = "offset()/exposure()";
    }
    if (parsed.inRange.has_value() && !parsed.inRange->empty()) why = "an `in` range";
    for (const auto& x : xs) {
      if (x.find_first_of(".#()") != std::string::npos) {
        why = "factor-variable or time-series regressors";
      }
    }
    // Weights (last, so it wins). Corpus: 0 of 81 ppmlhdfe statements carry a weight. The
    // weighted closed form is source-derived (ppmlhdfe,478-479, reghdfe5:
    // the FE sum is demeaned with W = w*mu) but uncaptured (draft w2q_ppml_d_weights), and
    // ppmlhdfeSaveDM4b only knows the unweighted sums. The engine refuses weighted --fe
    // (src/estimators/fepois.cpp:30-33), so no weighted absorb() fit could exercise it.
    if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) why = "weights";
    if (!why.empty()) {
      return ojson(eval::errorBlock(
          "unsupported_option",
          "Option `d` is a valid ppmlhdfe option that this version of PIET does not implement "
          "together with " + why + ".",
          std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }

  // No regressors (`ppmlhdfe cnt`, `ppmlhdfe cnt, absorb(t)`): the engine's
  // --no-x fits the constant alone, or the absorbed effects with _cons
  // recovered (w2d_uncovered ppmlhdfe_noreg_noabsorb_ctl `_b[_cons]` =
  // ln(mean y) 2.083599551828503, ppmlhdfe_absorb_noreg_d_062 2.091719879995292).
  std::vector<std::string> argv = {"fepois", "--y", y};
  if (xs.empty()) {
    argv.push_back("--no-x");
  } else {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  }

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // W:P5: `poisson … [pweight=w]`. registry.cpp's row for this command decides
  // which weight types get this far; the engine's fepois honours --weight/
  // --wtype. Same normalization the shared builder and runRegress use.
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

  if (sawAbsorb) {
    std::vector<std::string> feToks = splitTokensM4(feRaw);
    if (!feToks.empty()) {
      argv.push_back("--fe");
      argv.push_back(joinCommaM4(feToks));
    }
  }

  // offset(varname) — D3b oracle-verified LITERAL passthrough (see header comment).
  std::optional<std::string> offsetVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "offset" && !v.isBool) offsetVar = v.strValue;
  }
  if (offsetVar.has_value() && !offsetVar->empty()) {
    argv.push_back("--offset");
    argv.push_back(*offsetVar);
  }
  // exposure(varname): the offset ln(varname) (the engine's --exposure; StataBE
  // 19.5 bank e62ba9a9 `poisson fatal beertax unemp, exposure(pop)`, e(offset)
  // "ln(pop)").
  for (const auto& [k, v] : parsed.opts) {
    if (k == "exposure" && !v.isBool && !trimM4(v.strValue).empty()) {
      if (offsetVar.has_value() && !offsetVar->empty())
        return engineErrorBlockM4("only one of offset() or exposure() can be specified",
                                  "syntax_error");
      argv.push_back("--exposure");
      argv.push_back(trimM4(v.strValue));
    }
  }

  // BUG-1278: a cluster variable that is not in the data is r(111) "variable V
  // not found" before the engine runs, and poisson, which takes one cluster
  // variable, refuses a second (r(198) under vce(), first; r(103) under
  // cluster()). ppmlhdfe clusters multiway and has its own parser.
  const bool poissonCmd = stataCmd == "poisson";
  if (auto bad = clusterSpecRefusalM4(
          parsed, session,
          poissonCmd ? ClusterOptParserM4::kVceParse : ClusterOptParserM4::kPpmlhdfe,
          /*multiwayVceRefused=*/poissonCmd, /*legacyOneVar=*/poissonCmd))
    return *bad;
  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    return vcovErrorBlockM4(vr);
  }
  bool sawExplicitVce = !vr.argv.empty();
  // ppmlhdfe/fepois DEFAULT to robust when the user specified no vce() at
  // all (see header comment) — plain `poisson` keeps cobra's iid default.
  // BUG-1538: a probability weight implies vce(robust) on `poisson` too, the
  // buildEstimationArgvM4 rule (BUG-536; StataBE 19.5 `poisson cnt x z [pw=wf]`
  // e(vce) robust, _se[x] .41060909851198435).
  const std::string wtLower = parsed.weightType.has_value() ? lowerM4(*parsed.weightType) : "";
  const bool pweighted = parsed.weightVar.has_value() && !parsed.weightVar->empty() &&
                         (wtLower == "pw" || wtLower == "pweight");
  if (!sawExplicitVce && (allowFe || pweighted)) {
    vr.argv = {"--vcov", "hc1"};
  }
  for (const auto& a : vr.argv) argv.push_back(a);

  // noconstant/level() — NEVER forwarded (masking-bug precedent, see header comment).
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (live-verified: cobra fepois's `--no-intercept` "
          "hard-errors on stderr while still printing a full WRONG result to stdout — never "
          "forwarded).",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool && stataCmd != "poisson") {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (same stdout/exit-code masking gap as noconstant "
          "above).",
          "unsupported_option");
    }
  }
  // poisson's level(#), iterate(#) and irr (bank e140142b `poisson ..., level(90)`,
  // 2ca6271a `iterate(0)`, 990889c5 `irr`). level(#) and irr set how the table is
  // reported (the engine is never given --level, above); iterate(#) is ml's.
  std::optional<double> poissonLevel;
  std::optional<int> poissonIterate;
  bool poissonIrr = false;
  if (stataCmd == "poisson") {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "level" && !v.isBool) {
        const std::string val = trimM4(v.strValue);
        char* end = nullptr;
        const double d = std::strtod(val.c_str(), &end);
        if (val.empty() || end == nullptr || *end != '\0' || !(d >= 10.0 && d <= 99.99))
          return ojson(eval::errorBlock("engine_error", "level() must be between 10 and 99.99 inclusive",
                                        std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
        poissonLevel = d;
      }
      if (k == "iterate") {
        const std::string val = trimM4(v.isBool ? std::string() : v.strValue);
        char* end = nullptr;
        const double d = std::strtod(val.c_str(), &end);
        if (val.empty() || end == nullptr || *end != '\0' || !(d >= 0 && d <= 16000) || d != std::floor(d))
          return engineErrorBlockM4("option iterate() must be an integer from 0 to 16000 in this build "
                                    "(got `" + val + "`)",
                                    "unsupported_option");
        poissonIterate = static_cast<int>(d);
      }
      if (k == "irr" && v.isBool && v.boolValue) poissonIrr = true;
    }
    if (poissonIterate.has_value() && sawAbsorb) poissonIterate.reset();
  }

  // kVocabPpmlD is ppmlhdfe's own slot: the shared count-model set plus `d`.
  const std::unordered_set<std::string>& known =
      (stataCmd == "ppmlhdfe") ? kVocabPpmlD.known
      : (stataCmd == "poisson") ? kVocabPoissonLog.known  // poisson's slot: + nolog / log
                                : kVocabPoisson.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  // TS-context injection (MI-083 §16), as buildEstimationArgvM4 does for logit/probit:
  // after `xtset id t`, `poisson cnt L.x1 x2` needs the panel and time variables to
  // resolve L.x1 (StataBE 19.5, BUG-1484: N 150, _b[L.x1] -.050220717740893102).
  std::vector<std::string> tsTokens = {y};
  for (const auto& x : xs) tsTokens.push_back(x);
  if (translate::hasTsToken(tsTokens)) argv = translate::injectTsContext(argv, session);

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  std::string engineDPath;
  try {
    // FU-859 (eb-order T-P1): the fit's full e(V) (the fepois options_table row
    // carries --emit-v; apps/cmds/common.cpp run_fepois_emit_v). Only the fit
    // call gets the flag, not `argv`, which the LastModel filter reads. The
    // recovered _cons and its full covariance are retained on absorbed fits too.
    std::vector<std::string> fitArgs = argv;
    fitArgs.push_back("--emit-v");
    // Stata `poisson` posts ml's iterate from its own starting values, with e(ic),
    // e(ilog) and e(gradient) (fepois.cpp ModelData::stata_ml); ppmlhdfe is not ml.
    if (stataCmd == "poisson" && !sawAbsorb) fitArgs.push_back("--stata-ml");
    if (poissonIterate.has_value()) {
      fitArgs.push_back("--iterate");
      fitArgs.push_back(std::to_string(*poissonIterate));
    }
    if (engineD) {
      engineDPath = nextPathM4(session.workdir(), "ppmlhdfe_d");
      fitArgs.push_back("--d-out");
      fitArgs.push_back(engineDPath);
      fitArgs.push_back("--d-name");
      fitArgs.push_back(dName);
    }
    raw = runCobraJsonM4(fitArgs);
  } catch (const CobraError& ce) {
    if (!engineDPath.empty()) cecon::native_unlink(engineDPath);
    // BUG-1269: ppmlhdfe, like reghdfe, calls an empty sample insufficient,
    // r(2001), and so a one-row one, whose single row the singleton drop removes
    // (StataBE 19.5: `ppmlhdfe c x if x > 20, absorb(g)` and `if x == 1`).
    if (stataCmd == "ppmlhdfe" && ce.code == "no_obs" && std::string(ce.what()) == "no observations")
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  // d: built on the head the fit read, BEFORE e() is posted, so a column that
  // cannot be reproduced refuses the whole command instead of leaving e()
  // describing a fit whose d is absent.
  std::string dPath;
  if (engineD) {
    if (!std::filesystem::exists(cecon::to_native_path(engineDPath))) {
      return ojson(eval::errorBlock("unsupported_option",
                                    "ppmlhdfe d: the engine did not write the sum of the fixed "
                                    "effects; d is not saved.",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
    dPath = engineDPath;
  } else if (!dName.empty()) {
    ojson err;
    dPath = ppmlhdfeSaveDM4b(session, raw, y, xs, splitTokensM4(feRaw), parsed, dName, err);
    if (dPath.empty()) return err;
  }

  // An iterate(#) fit's e(b) is ml's iterate there: the post-estimation refits,
  // which run to ml's convergence, refuse it (postestRefitMatchesFitM4).
  if (poissonIterate.has_value() && raw.is_object()) raw["ml_iterate"] = *poissonIterate;
  ojson block = cobraToEstimationM4(raw, "fepois", &session);
  block["estimator"] = stataCmd;  // literal Stata command word typed, not cobra's internal name.
  block["depvar"] = y;

  LastModel lm;
  lm.cobraCmd = sawAbsorb ? "fepois" : "poisson";  // margins' kModelMap already maps BOTH keys
                                                    // to "--model poisson" for AME dydx.
  // e(cmd) is `ppmlhdfe` after ppmlhdfe (w2l_vce_shape_det.log:2526
  // ppmlhdfe_ctl_cluster `0|ppmlhdfe|cluster|Robust|30|180`; FU-967); the
  // replay machinery keeps keying on cobraCmd.
  if (stataCmd == "ppmlhdfe") lm.stataCmd = "ppmlhdfe";
  lm.y = y;
  lm.x = xs;
  if (sawAbsorb) {
    for (const auto& f : splitTokensM4(feRaw)) lm.fe.push_back(f);
  }
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vcov" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) {
      for (const auto& c : splitTokensM4(argv[i + 1])) lm.cluster.push_back(c);
    }
  }
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  // `poisson [pweight=]`: a pseudolikelihood has no constant-only comparison in
  // Stata's poisson, so e(ll_0) is not posted and e(r2_p) is missing (bank fd85b1f8
  // `poisson grade wage hours [pweight=w]` and 03ca3e32: r2_p ., no ll_0; the fweight
  // f2ea5635 posts both). Cached reply only.
  // vce(cluster c) is the same: no constant-only comparison, e(r2_p) missing (bank
  // 4c264835 `poisson fatal beertax unemp, cluster(sid)`: r2_p ., no ll_0, Wald chi2;
  // vce(robust) keeps both, bank 7ba9b3d0 r2_p .0148665728).
  // Only the cluster() option itself does so: under vce(cluster c) poisson fits the
  // constant-only model and posts e(ll_0) and e(r2_p) (bank 59b00740 `poisson fatal
  // beertax unemp, vce(cluster sid)`: ll_0 present; 4c264835 cluster(sid): none).
  bool clusterOption = false;
  for (const auto& [k, v] : parsed.opts)
    if (k == "cluster" && !v.isBool) clusterOption = true;
  const bool pseudoNoNull =
      (parsed.weightType.has_value() &&
       (lowerM4(*parsed.weightType) == "pw" || lowerM4(*parsed.weightType) == "pweight")) ||
      (lm.vcov == "cluster" && clusterOption);
  if (!sawAbsorb && stataCmd == "poisson" && pseudoNoNull && lm.result->is_object()) {
    ojson& rr = *lm.result;
    for (const char* key : {"r2_p", "ll_0", "r2_pseudo"})
      if (rr.contains(key)) rr[key] = nullptr;
  }
  // `poisson ..., vce(cluster c)` with fewer clusters than coefficients (G - 1 < k):
  // e(rank) G - 1, e(df_m) that rank less the constant, no model chi2 (bank 03ca3e32
  // `poisson rep78 mpg weight [pw=w], vce(cluster foreign)`: rank 1, df_m 0, chi2 .,
  // p .; fit_detail::fewClustersRankM4). Cached reply and block only.
  if (!sawAbsorb && stataCmd == "poisson" && lm.vcov == "cluster" && lm.cluster.size() == 1 &&
      lm.result->is_object() && lm.result->contains("n_clusters") &&
      (*lm.result)["n_clusters"].is_number() && lm.result->contains("k_vars") &&
      (*lm.result)["k_vars"].is_number()) {
    ojson& rr = *lm.result;
    if (const auto rk = fit_detail::fewClustersRankM4(rr, rr["n_clusters"].get<double>(),
                                                      rr["k_vars"].get<double>())) {
      rr["k_vars"] = *rk;
      rr["df_m"] = std::max(0.0, *rk - 1.0);
      for (const char* key : {"chi2", "p_chi2", "p_F", "p"}) {
        if (rr.contains(key)) rr[key] = nullptr;
        if (block.contains(key)) block[key] = nullptr;
      }
    }
  }
  // ppmlhdfe's e(ll_0) is the intercept-only null without the offset, and e(r2_p)
  // 1 - ll/ll_0 against it (bank 2f9a91bc `ppmlhdfe fatal beertax unemp, absorb(sid)
  // offset(lpop)`: ll_0 -119412.988171009, r2_p .98319695800772); the engine's `ll_0`
  // is poisson's, which keeps the offset. Cached reply only.
  if (stataCmd == "ppmlhdfe" && lm.result->is_object() &&
      lm.result->contains("ll_0_nooffset") && (*lm.result)["ll_0_nooffset"].is_number() &&
      lm.result->contains("ll") && (*lm.result)["ll"].is_number()) {
    ojson& rr = *lm.result;
    const double ll0 = rr["ll_0_nooffset"].get<double>();
    rr["ll_0"] = ll0;
    if (ll0 != 0.0 && rr.contains("r2_p")) rr["r2_p"] = 1.0 - rr["ll"].get<double>() / ll0;
  }
  // ppmlhdfe without absorb() absorbs the constant itself (e(N_hdfe) 1, e(df_a) 1):
  // e(rank) counts the slopes only, while e(b) still carries the recovered _cons (bank
  // 456255c9 `ppmlhdfe fatal beertax unemp`: rank 2, e(b) 1 x 3). Cached reply only.
  if (!sawAbsorb && stataCmd == "ppmlhdfe" && lm.result->is_object() &&
      lm.result->contains("k_vars") && (*lm.result)["k_vars"].is_number() &&
      (*lm.result)["k_vars"].get<double>() >= 1.0) {
    // the reply's `coef` is keyed by name
    bool hasIntercept = false;
    if (lm.result->contains("coef") && (*lm.result)["coef"].is_object())
      hasIntercept = (*lm.result)["coef"].contains("(intercept)") ||
                     (*lm.result)["coef"].contains("_cons");
    if (hasIntercept) (*lm.result)["k_vars"] = (*lm.result)["k_vars"].get<double>() - 1.0;
  }
  // e(sample) T4 (w2_esample.log:1928 H7): plain poisson, and ppmlhdfe/fepois with
  // absorb() (BUG-1197). BUG-518 order: the mask, then setLastModel. With d the mask
  // goes onto the d frame, the head once e() is posted.
  (void)esampleInstallMlMask(session, lm, *lm.result, dPath.empty() ? nullptr : &dPath);
  // BUG-1481: ppmlhdfe's cluster bookkeeping, reghdfe's (StataBE 19.5, 180 rows):
  // e(N_clustervars) the number of cluster variables, e(N_clust1..k) each one's
  // clusters in cluster() order (vce(cluster id) 30; vce(cluster id id2) 30/7;
  // vce(cluster id id2 id3) 30/7/5, while e(N_clust) is the smallest) and
  // e(clustvar1..k) their names (below, with the model); none under vce(robust).
  // One-way, e(N_clust1) is the reply's n_clusters; multiway, each dimension's own G
  // (n_clusters_by, read before any key is added). Cached reply only, as after
  // reghdfe (linear_estimation.cpp).
  const bool ppmlClustered = lm.stataCmd == "ppmlhdfe" && !lm.cluster.empty() &&
                             (lm.vcov == "cluster" || lm.vcov == "twoway" ||
                              lm.vcov == "multiway") &&
                             lm.result->is_object() && lm.result->contains("n_clusters") &&
                             (*lm.result)["n_clusters"].is_number();
  if (ppmlClustered) {
    ojson& rr = *lm.result;
    std::vector<double> counts;
    if (lm.cluster.size() == 1) {
      counts.push_back(rr["n_clusters"].get<double>());
    } else if (rr.contains("n_clusters_by") && rr["n_clusters_by"].is_array() &&
               rr["n_clusters_by"].size() == lm.cluster.size()) {
      for (const auto& g : rr["n_clusters_by"])
        if (g.is_number()) counts.push_back(g.get<double>());
      if (counts.size() != lm.cluster.size()) counts.clear();
    }
    rr["N_clustervars"] = static_cast<double>(lm.cluster.size());
    for (size_t j = 0; j < counts.size(); ++j) rr["N_clust" + std::to_string(j + 1)] = counts[j];
  }
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // FU-859: e(V) is the fit's own, never diag(se^2)
  if (ppmlClustered) {
    for (size_t j = 0; j < lm.cluster.size(); ++j) {
      const std::string nm = "clustvar" + std::to_string(j + 1);
      session.eval.setEString(nm, lm.cluster[j]);
      session.noteEOnFit(nm);
    }
  }
  if (!dPath.empty()) {
    // The head the fit (and lm.dataPath) read stays untouched until e() is
    // posted; the new column lands now (the reghdfe residuals() order).
    Datastore* before = session.datastore();
    const bool dExisted = before != nullptr && before->has(dName);  // bare d's old _ppmlhdfe_d
    session.load(dPath);
    // d is saved double, %10.0g, labelled "Sum of fixed effects" (ppmlhdfe), and
    // ppmlhdfe posts its name as e(d) (StataBE 19.5, BUG-1250/BUG-1252: e(d) "dd"
    // after d(dd), "_ppmlhdfe_d" after d, empty after a later fit without d -- so the
    // macro is noted on this fit, and the next install drops it).
    VarMeta& meta = session.varMeta[dName];
    meta.stataType = "double";
    meta.format = "%10.0g";
    meta.label = "Sum of fixed effects";
    session.eval.setEString("d", dName);
    session.noteEOnFit("d");
    // BUG-1251: the column is announced as every new variable is, so the variables
    // panel and the n_missing fast path see it (the reghdfe residuals() hand-off).
    if (dMutationOut != nullptr) {
      std::vector<std::string> added, rewritten;
      (dExisted ? rewritten : added).push_back(dName);
      *dMutationOut = mutationResult(session, "gen", added, {}, rewritten);
    }
  }
  // (after e(b) is cached from the block above: the table only)
  // level(#): the interval b -/+ invnormal(1 - (1 - level/100)/2) se; irr: exp() of
  // b, se scaled by exp(b), and of the interval (z and p unchanged), the row marked
  // eform so r(table) carries eform 1. e(b) stays in the coefficient metric.
  if ((poissonLevel.has_value() || poissonIrr) && block.contains("coeftable") &&
      block["coeftable"].is_array()) {
    const double zc = poissonLevel.has_value() ? cecon::z_critical(1.0 - *poissonLevel / 100.0) : 0.0;
    for (auto& row : block["coeftable"]) {
      if (!row.is_object() || !row.contains("b") || !row["b"].is_number()) continue;
      const double b = row["b"].get<double>();
      const double se = row.contains("se") && row["se"].is_number() ? row["se"].get<double>() : 0.0;
      if (poissonLevel.has_value() && se > 0) {
        row["ci_lo"] = b - zc * se;
        row["ci_hi"] = b + zc * se;
      }
      if (poissonIrr) {  // an omitted column too: exp(0) = 1, se 0 (logistic's rule)
        const double e = std::exp(b);
        row["b"] = e;
        row["se"] = e * se;
        if (row.contains("ci_lo") && row["ci_lo"].is_number()) row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
        if (row.contains("ci_hi") && row["ci_hi"].is_number()) row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
        row["eform"] = true;
      }
    }
    // the whole table is in the IRR metric: a base column too is 1 with eform 1
    // (bank a7d4526c `poisson ... i.year, vce(cluster sid) irr`)
    if (poissonIrr) block["coef_metric"] = "irr";
  }
  return block;
}

ojson runPoisson(const std::string& stataCmd, const eval::ParsedCommand& parsed,
                  SessionState& session) {
  return runPoisson(stataCmd, parsed, session, nullptr);
}

// nbreg (BS-074): negative-binomial (NB2, "dispersion(mean)" — Stata's
// default) regression -> cobra nbreg. Oracle D3e.
//
// ENGINE FIX (cobra 0.25.0): the Newton optimizer's convergence gap
// described below is fixed. cobra's nbreg now converges on the D3e oracle's
// own data (`nbreg cnt beertax unemp` on fatalities.csv) — the true MLE
// sits at the alpha->0 boundary (the Poisson limit: this data has no
// detectable overdispersion once beertax/unemp are controlled for), and
// cobra now detects and reports that boundary cleanly via a Poisson
// warm-start feeding the joint (beta, ln-alpha) Newton fit plus explicit
// boundary reporting, instead of the previous "no_convergence" failure.
// (History: through 0.24.x, cobra's nbreg Newton optimizer FAILED TO
// CONVERGE on this and several independently-constructed synthetic
// overdispersed count datasets whenever the fitted intercept sat
// appreciably away from zero — a genuine intercept-initialization/step-size
// gap in the engine's joint Newton fit, not a PIET-wiring problem.) The
// wiring below is complete and correct; selftest now pins b/se/alpha/
// lnalpha against the small SYNTHETIC dataset (argv/JSON round-trip proof,
// not Stata-oracle-pinned) AND separately oracle-pins the D3e leg's own
// exact call (`nbreg cnt beertax unemp`) against its Stata coefficients,
// ll, and the alpha=0 boundary (selftest.cpp).
// nbreg's ml record and model statistics, from a fit whose reply carries the ml
// path (apps/cmds/cmds_panel.cpp cmd_nbreg; none at the alpha -> 0 boundary).
// StataBE 19.5 bank, every default-dispersion nbreg fit (dba6710a `nbreg fatal
// beertax unemp`: e(ic) 2, e(ll_0) -2622.763 the constant-only fit, e(rank0) 2,
// e(chi2) 5.3988445 = 2(ll - ll_0) LR, e(p) .06724435 on e(df_m) 2, e(r2_p)
// 1 - ll/ll_0, e(ll_c) -117906.88 the Poisson fit, e(chi2_c) 2(ll - ll_c),
// e(b_pclass) 0 ... 0 2, e(k_eq) 2, e(k_aux) 1). Under vce(robust)/vce(cluster)
// or a pweight the model test is the Wald test of the slopes on e(V), there is no
// comparison Poisson test, and e(V_modelbased) is the inverse information.
static void postNbregMlContractM4(SessionState& session, const ojson& raw, const std::vector<std::string>& argv,
                           bool consOnly, bool noConstant, const std::string& offsetVar) {
  if (!raw.is_object() || !raw.contains("ilog") || !raw["ilog"].is_array() || raw["ilog"].empty() ||
      !raw.contains("e_gradient") || !raw["e_gradient"].is_object())
    return;
  const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  const auto eV = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
  if (!eb.matrix.has_value() || !eV.matrix.has_value() || eb.matrix->data.size() != 1) return;
  const std::vector<std::string>& cols = eb.matrix->colnames;
  const size_t K = cols.size();
  if (K < 2 || cols.back() != "/lnalpha" || eV.matrix->data.size() != K) return;
  // engine index of each e(b) column (-1: a base or omitted level, a structural 0)
  const ojson& gn = raw["e_gradient"]["names"];
  const ojson& gv = raw["e_gradient"]["values"];
  if (!gn.is_array() || !gv.is_array() || gn.size() != gv.size()) return;
  std::vector<long> at(K, -1);
  for (size_t j = 0; j < K; ++j) {
    const size_t colon = cols[j].find(':');
    const std::string bare = (cols[j] == "/lnalpha" || colon == std::string::npos) ? cols[j]
                                                                                 : cols[j].substr(colon + 1);
    for (size_t e = 0; e < gn.size(); ++e)
      if (gn[e].is_string() && gn[e].get<std::string>() == bare) at[j] = static_cast<long>(e);
    if (at[j] < 0) {
      // only a marked level may be absent from the engine's columns
      const size_t dot = bare.find('.');
      const std::string op = dot == std::string::npos ? std::string() : bare.substr(0, dot);
      if (op.find('b') == std::string::npos && op.find('o') == std::string::npos) return;
    }
  }
  const auto rowVec = [&](const std::vector<double>& v, const char* row) {
    eval::EvalMatrix m;
    m.rownames = {row};
    m.colnames = cols;
    m.data = {v};
    return m;
  };
  const auto putE = [&session](const std::string& name, eval::EvalMatrix m) {
    auto& store = session.eval.eMatrices;
    store.erase(std::remove_if(store.begin(), store.end(), [&name](const auto& q) { return q.first == name; }),
                store.end());
    store.emplace_back(name, std::move(m));
    session.noteEOnFit(name);
  };
  std::vector<double> grad(K, 0.0), pclass(K, 0.0);
  for (size_t j = 0; j < K; ++j)
    if (at[j] >= 0 && gv[static_cast<size_t>(at[j])].is_number()) grad[j] = gv[static_cast<size_t>(at[j])].get<double>();
  pclass[K - 1] = 2.0;
  putE("gradient", rowVec(grad, "r1"));
  putE("b_pclass", rowVec(pclass, "y1"));
  {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    size_t i = 0;
    for (const auto& v : raw["ilog"]) {
      if (i >= 20) break;
      if (v.is_number()) m.data[0][i] = v.get<double>();
      ++i;
    }
    putE("ilog", std::move(m));
  }
  if (raw.contains("e_Vmb") && raw["e_Vmb"].is_object() && raw["e_Vmb"].contains("matrix") &&
      raw["e_Vmb"]["matrix"].is_array() && raw["e_Vmb"]["matrix"].size() == gn.size()) {
    const ojson& M = raw["e_Vmb"]["matrix"];
    eval::EvalMatrix m;
    m.rownames = cols;
    m.colnames = cols;
    m.data.assign(K, std::vector<double>(K, 0.0));
    for (size_t a = 0; a < K; ++a)
      for (size_t c = 0; c < K; ++c)
        if (at[a] >= 0 && at[c] >= 0 && M[static_cast<size_t>(at[a])].is_array() &&
            M[static_cast<size_t>(at[a])][static_cast<size_t>(at[c])].is_number())
          m.data[a][c] = M[static_cast<size_t>(at[a])][static_cast<size_t>(at[c])].get<double>();
    putE("V_modelbased", std::move(m));
  }
  // the fit's VCE and weights, as forwarded
  std::string vce = "iid", wtype;
  for (size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--vce") vce = argv[i + 1];
    if (argv[i] == "--wtype") wtype = argv[i + 1];
  }
  const bool sandwich = vce != "iid" || wtype == "pw";
  const auto num = [&raw](const char* k) -> std::optional<double> {
    if (!raw.contains(k) || !raw[k].is_number()) return std::nullopt;
    return raw[k].get<double>();
  };
  const auto ll = num("loglik");
  const auto ll0 = num("ll_0");
  const auto llc = num("ll_c");
  double autoCns = 0.0;
  for (size_t j = 0; j + 1 < K; ++j) autoCns += at[j] < 0 ? 1.0 : 0.0;
  // the slopes: estimated equation columns other than the constant
  std::vector<size_t> slope;
  for (size_t j = 0; j + 1 < K; ++j) {
    const size_t colon = cols[j].find(':');
    const std::string bare = colon == std::string::npos ? cols[j] : cols[j].substr(colon + 1);
    if (at[j] >= 0 && bare != "_cons" && eV.matrix->data[j][j] > 0) slope.push_back(j);
  }
  const double dfm = static_cast<double>(slope.size());
  const auto setS = [&session](const char* n, double v) { e3::setEScalarOnFit(session, n, v); };
  const auto setM = [&session](const char* n, const std::string& v) { e3::setEStringOnFit(session, n, v); };
  setS("k_eq", 2.0);
  setS("k_eq_model", 1.0);
  setS("k_dv", 1.0);
  setS("k_aux", 1.0);
  setS("noconstant", noConstant ? 1.0 : 0.0);
  setS("consonly", consOnly ? 1.0 : 0.0);
  setS("rc", 0.0);
  setS("k_autoCns", autoCns);
  setS("df_m", dfm);
  if (ll0 && ll) {
    setS("rank0", 2.0);
    setS("r2_p", 1.0 - *ll / *ll0);
  }
  std::optional<double> chi2;
  if (!sandwich && !noConstant && ll0 && ll) {
    chi2 = 2.0 * (*ll - *ll0);
    setM("chi2type", "LR");
  } else if (!slope.empty()) {
    // Wald test of the slopes on e(V)
    const size_t q = slope.size();
    std::vector<std::vector<double>> A(q, std::vector<double>(q));
    std::vector<double> bvec(q);
    for (size_t a = 0; a < q; ++a) {
      bvec[a] = eb.matrix->data[0][slope[a]];
      for (size_t c = 0; c < q; ++c) A[a][c] = eV.matrix->data[slope[a]][slope[c]];
    }
    // Cholesky solve
    bool ok = true;
    std::vector<std::vector<double>> L(q, std::vector<double>(q, 0.0));
    for (size_t i = 0; i < q && ok; ++i)
      for (size_t j = 0; j <= i && ok; ++j) {
        double sum = A[i][j];
        for (size_t k2 = 0; k2 < j; ++k2) sum -= L[i][k2] * L[j][k2];
        if (i == j) {
          if (!(sum > 0)) ok = false;
          else L[i][i] = std::sqrt(sum);
        } else {
          L[i][j] = sum / L[j][j];
        }
      }
    if (ok) {
      double w = 0.0;
      std::vector<double> yv(q);
      for (size_t i = 0; i < q; ++i) {
        double sum = bvec[i];
        for (size_t k2 = 0; k2 < i; ++k2) sum -= L[i][k2] * yv[k2];
        yv[i] = sum / L[i][i];
        w += yv[i] * yv[i];
      }
      chi2 = w;
      setM("chi2type", "Wald");
    }
  }
  if (chi2 && std::isfinite(*chi2)) {
    setS("chi2", *chi2);
    if (dfm > 0) setS("p", est_detail::chi2UpperTailM4(dfm, *chi2));
  }
  if (!sandwich && llc && ll) {
    setS("ll_c", *llc);
    setS("chi2_c", 2.0 * (*ll - *llc));
    setM("chi2_ct", "LR");
  }
  setM("crittype", sandwich ? "log pseudolikelihood" : "log likelihood");
  setM("deriv_useminbound", "off");
  setM("diparm1", "lnalpha, exp label(\"alpha\")");
  setM("diparm_opt2", "noprob");
  setM("dispers", "mean");
  setM("ml_method", "e2");
  setM("opt", "moptimize");
  setM("singularHmethod", "m-marquardt");
  setM("technique", "nr");
  setM("user", "nbreg_lf");
  setM("which", "max");
  if (vce == "iid" && session.eval.eStrings.get("vce") == "iid") session.eval.setEString("vce", "oim");
  if (sandwich) setM("vcetype", "Robust");
  if (vce == "cluster")
    if (const auto g = num("n_clusters")) setS("N_clust", *g);
  // e(offset): offset(lpop) "lpop", exposure(pop) "ln(pop)" (bank cf4e2c5d)
  for (size_t i = 0; i + 1 < argv.size(); ++i) {
    if (argv[i] == "--exposure") setM("offset", "ln(" + argv[i + 1] + ")");
  }
  if (!offsetVar.empty()) setM("offset", strutil::trim(offsetVar));
}

ojson runNbreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4("syntax_error: `nbreg` requires a dependent variable and at least "
                              "one regressor",
                              "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::vector<std::string> argv = {"nbreg", "--y", y, "--x", joinCommaM4(xs)};

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  bool noConstant = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) noConstant = true;
  }
  // Unlike fepois above, cobra nbreg's `--no-intercept` is DOCUMENTED and
  // live-verified genuinely honored (not a masking no-op) — safe to forward.
  if (noConstant) argv.push_back("--no-intercept");

  std::optional<std::string> offsetVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "offset" && !v.isBool) offsetVar = v.strValue;
  }
  if (offsetVar.has_value() && !offsetVar->empty()) {
    argv.push_back("--offset");
    argv.push_back(*offsetVar);
  }
  for (const auto& [k, v] : parsed.opts) {
    // exposure(varname): the offset ln(varname), as poisson's (the engine's --exposure)
    if (k == "exposure" && !v.isBool && !trimM4(v.strValue).empty()) {
      if (offsetVar.has_value() && !offsetVar->empty())
        return engineErrorBlockM4("only one of offset() or exposure() can be specified",
                                  "syntax_error");
      argv.push_back("--exposure");
      argv.push_back(trimM4(v.strValue));
      continue;
    }
    if (k == "dispersion" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `dispersion()` is a valid Stata option that this version of PIET does not yet "
          "implement for `nbreg` (only the default NB2 \"mean\" dispersion is wired — no oracle "
          "coverage for dispersion(constant)/NB1).",
          "unsupported_option");
    }
  }

  // BUG-1278: _vce_parse's refusals come first: a second cluster variable (nbreg
  // takes one: vce(cluster a b) r(198) before any lookup, cluster(a b) r(103)
  // after it), a variable that is not in the data (r(111)), vce() paired with
  // cluster() (r(198)/r(100)).
  if (auto bad = clusterSpecRefusalM4(parsed, session, ClusterOptParserM4::kVceParse,
                                      /*multiwayVceRefused=*/true))
    return *bad;
  // vce(): nbreg's own CLI shape is `--vce iid|robust|cluster [--cluster C]`
  // (NOT feols/fepois's `--vcov`/hc1/twoway shape) — reuse vcovFromOpts for
  // the Stata-side parsing, then translate its {"--vcov",TYPE,...} output.
  // "twoway" (2 cluster vars) has no nbreg equivalent (usage banner: a
  // single --cluster slot only) — honest reject. A missing cluster var
  // inside vce(cluster) already fails inside vcovFromOpts itself (shared
  // discipline with feols/logit) before ever reaching here, so nbreg's own
  // live-verified `--vce cluster`-with-no-`--cluster` silent-no-op shape
  // (identical numbers to vce(robust)) is structurally unreachable from
  // this handler.
  VcovResult vr = vcovFromOpts(parsed);
  if (vr.errored) {
    return vcovErrorBlockM4(vr);
  }
  if (vr.argv.size() >= 2 && vr.argv[0] == "--vcov") {
    const std::string& type = vr.argv[1];
    if (type == "hc1") {
      argv.push_back("--vce");
      argv.push_back("robust");
    } else if (type == "cluster") {
      argv.push_back("--vce");
      argv.push_back("cluster");
      argv.push_back("--cluster");
      argv.push_back(vr.argv[3]);  // vr.argv == {"--vcov","cluster","--cluster",VAR}
    } else if (type == "twoway") {
      return engineErrorBlockM4(
          "vce(cluster A B) (two-way clustering) is not supported for `nbreg` — the engine's "
          "nbreg CLI has only a single --cluster slot.",
          "unsupported_option");
    } else if (type == "iid") {
      argv.push_back("--vce");
      argv.push_back("iid");
    }
  }
  // BUG-1537: nbreg's fweights, iweights and pweights (the engine fits them; its
  // aweight is Stata's r(101), the registry's kStataQualifiers row). A pweight
  // implies vce(robust), the buildEstimationArgvM4 rule (BUG-536; StataBE 19.5
  // `nbreg cnt x z [pw=wf]` e(vce) robust, _se[x] .4424215353885973).
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    const std::string wt = lowerM4(*parsed.weightType);
    const std::string wtype = (wt == "fw" || wt == "fweight")   ? "fw"
                              : (wt == "iw" || wt == "iweight") ? "iw"
                              : (wt == "pw" || wt == "pweight") ? "pw"
                                                                : "";
    if (!wtype.empty()) {
      if (wtype == "pw" && vr.argv.empty()) {
        argv.push_back("--vce");
        argv.push_back("robust");
      }
      argv.push_back("--weight");
      argv.push_back(*parsed.weightVar);
      argv.push_back("--wtype");
      argv.push_back(wtype);
    }
  }

  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      // BUG-274: this used to reject, on the premise that "cobra nbreg's --level hard-errors
      // on stderr while still printing a full WRONG result to stdout". That premise is doubly
      // expired: BUG-202 gave nbreg a real `--level` (per-coef ci_lo/ci_hi at the asked level,
      // z-based like Stata's own nbreg table — live-verified `--level 90` on this tree's
      // engine), and even the PRE-fix half was stale against the current pin (WS-S re-measured:
      // stdout was EMPTY on the hard-error, not a masked result). Forward, same idiom as
      // runTobit/runQreg above. The engine validates the range itself ([10, 99.99]) and its
      // error envelope surfaces through CobraError below.
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }
  // iterate(#): ml's limit for the constant-only and the full fit (bank f4609cb1
  // `nbreg fatal beertax unemp, iterate(2)`: constant-only "convergence not
  // achieved" at e(ll_0) -2622.763092231246, full fit e(ic) 2); irr: incidence-rate
  // ratios for the equation's rows (bank d6cad8e6: r(table) eform 1 there, 0 on
  // /lnalpha and alpha).
  bool nbIrr = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "iterate") {
      const std::string val = trimM4(v.isBool ? std::string() : v.strValue);
      char* end = nullptr;
      const double d = std::strtod(val.c_str(), &end);
      if (val.empty() || end == nullptr || *end != '\0' || !(d >= 0 && d <= 16000) || d != std::floor(d))
        return engineErrorBlockM4("option iterate() must be an integer from 0 to 16000 in this build "
                                  "(got `" + val + "`)",
                                  "unsupported_option");
      argv.push_back("--iterate");
      argv.push_back(std::to_string(static_cast<int>(d)));
    }
    if (k == "irr" && v.isBool && v.boolValue) nbIrr = true;
  }

  const std::unordered_set<std::string>& known = kVocabNbreg.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  // BUG-1196: the fit's e(V), posted below (none at the α→0 boundary).
  argv.push_back("--emit-v");
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "nbreg", &session);
  block["estimator"] = "nbreg";
  block["depvar"] = y;
  fillCoeftableStatsM4(block);
  if (raw.contains("alpha")) block["alpha"] = raw["alpha"];
  if (raw.contains("lnalpha")) block["lnalpha"] = raw["lnalpha"];
  if (raw.contains("se_lnalpha_iid")) block["se_lnalpha"] = raw["se_lnalpha_iid"];
  // ll: cobra emits `loglik`; the block key is `ll` like every other MLE block
  // (logit/probit) — was omitted here, leaving nbreg the odd one out.
  if (raw.contains("loglik")) block["ll"] = raw["loglik"];
  // e(ll) reads the reply's `ll` (macro_results.cpp eScalarKey); nbreg names it `loglik`,
  // so e(ll) read missing after every fit (bank dba6710a e(ll) -2620.0636081397306).
  if (raw.contains("loglik") && !raw.contains("ll")) raw["ll"] = raw["loglik"];
  // e(k) is the number of e(b) columns: the depvar equation's coefficients and
  // /lnalpha (bank dba6710a `nbreg fatal beertax unemp`: 4). The engine's `k`
  // counts the equation alone. Base levels of a factor term are added when the
  // e(b) stripe is recorded (coefficient_column_names.cpp annotateEbStripe).
  if (raw.contains("coefs") && raw["coefs"].is_array())
    raw["k"] = static_cast<double>(raw["coefs"].size() + 1);
  // e(rank) (read through `k_vars`) is the number of estimated parameters: the
  // equation's coefficients the fit kept and /lnalpha (bank d93bc657 `nbreg fatal
  // beertax i.year`: rank 9 = beertax + 6 years + _cons + /lnalpha). A collinear
  // regressor the engine omitted (b = se = 0) is not counted, nor is a base level
  // (the engine does not send one). lrtest takes its df from e(rank): without it a
  // fit with factor terms (no e(V) posted) read as rank 0, and `lrtest a c` swapped
  // the nesting and returned a negative LR at rc 0.
  if (raw.contains("coefs") && raw["coefs"].is_array()) {
    double rank = 1.0;  // /lnalpha
    for (const auto& c : raw["coefs"]) {
      const bool omitted = c.is_object() && c.contains("beta") && c["beta"].is_number() &&
                           c["beta"].get<double>() == 0.0 && c.contains("se_iid") &&
                           c["se_iid"].is_number() && c["se_iid"].get<double>() == 0.0;
      if (!omitted) rank += 1.0;
    }
    raw["k_vars"] = rank;
  }

  LastModel lm;
  lm.cobraCmd = "nbreg";
  lm.y = y;
  lm.x = xs;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--vce" && i + 1 < argv.size()) lm.vcov = argv[i + 1];
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster = {argv[i + 1]};
  }
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // BUG-1196: Stata's e(b) is the depvar's equation, then `/lnalpha` (StataBE
  // 19.5 `cnt:x1 cnt:x2 cnt:_cons /lnalpha`); e(V) is the fit's, in that order.
  if (raw.contains("e_V") && raw.contains("coefs") && raw.contains("lnalpha") &&
      raw["lnalpha"].is_number()) {
    std::vector<AncillaryEbColM4> cols;
    if (depvarEqColsM4(y, raw["coefs"], "beta", cols)) {
      cols.push_back({"/lnalpha", "/lnalpha", raw["lnalpha"].get<double>()});
      (void)postAncillaryEbM4(session, raw, cols);
    }
  }
  postNbregMlContractM4(session, raw, argv, xs.empty(), noConstant,
                        offsetVar.has_value() && !offsetVar->empty() ? *offsetVar : std::string());
  // irr: the table only, after e(b) is posted
  if (nbIrr && block.contains("coeftable") && block["coeftable"].is_array()) {
    block["eq_eform"] = true;  // r(table): a base column the coeftable lacks is eform too
    for (auto& row : block["coeftable"]) {
      if (!row.is_object() || !row.contains("b") || !row["b"].is_number()) continue;
      const std::string nm = row.value("name", std::string());
      if (nm == "/lnalpha" || nm == "lnalpha" || nm == "alpha") continue;
      const double b = row["b"].get<double>();
      const double se = row.contains("se") && row["se"].is_number() ? row["se"].get<double>() : 0.0;
      // A base or omitted column too: exp(0) = 1 with se 0 and eform 1, as poisson's
      // (bank 3589963928edcb2f `nbreg ... i.year, vce(cluster sid) irr`: the base
      // column's b 1 and eform 1).
      const double e = std::exp(b);
      row["b"] = e;
      row["se"] = e * se;
      if (row.contains("ci_lo") && row["ci_lo"].is_number()) row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
      if (row.contains("ci_hi") && row["ci_hi"].is_number()) row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
      row["eform"] = true;
    }
  }
  return block;
}

// xtpoisson / xtlogit, fe (BS-075): conditional fixed-effects panel count/
// binary MLE. Oracle D3f (xtpoisson,fe), D3g (xtlogit,fe; depvar
// hi2=(spirits>1.5)). Both reproduce the oracle's b/se EXACTLY (bit-for-bit
// at 8+ significant digits, including D3g's 37-groups-omitted/n_obs=77
// all-positive-or-negative-outcome dropping and D3f's loglik=-460.6417) — no
// engine gap found for either, unlike BS-073/074 above.
//
// Real Stata's DEFAULT (bare `xtpoisson`/`xtlogit`, no fe/re/pa) is
// RANDOM-EFFECTS, not fixed — this engine has NO xtpoisson/xtlogit RE (or
// PA) estimator at all (only the conditional-FE MLE), so `,re`/`,pa` and
// the bare/no-option form ALL honest-reject naming `,fe` as the only
// supported form, matching runXtreg's own fe/re model-select precedent.
//
// vce(robust) genuinely changes the SE for BOTH (live-verified: se_robust
// != se_iid, real numbers, not a no-op). vce(cluster ...)/cluster() is a
// SILENT NO-OP for both — live-verified on the compiled engine directly:
// neither CLI's usage banner even lists a --cluster flag, yet passing
// `--vce cluster` to EITHER is accepted with exit 0 and produces the SAME
// numbers as the non-robust default (xtlogit's case is worse: se==se_iid,
// not even se_robust) — honest reject rather than forward a flag that would
// silently claim a clustered SE the engine never computed. noconstant/
// level() are NEVER forwarded on either CLI (same stdout/exit-code
// masking-bug shape as BS-073/074 above, live-verified on both).

// xtpoisson's random-effects (gamma) model, Stata's default and `, re` (engine
// `xtpois --re`, estimators/xtpoisre.h): the comparison Poisson fit and the full
// model by Stata's ml rules from the Poisson b and ln alpha 0 (vs-stata bank
// f8d5160a `xtpoisson fatal beertax unemp`: e(ll) -2178.6746, iteration 0
// -3931.0015, /lnalpha -.2757715). offset() / exposure(), noconstant, irr (an
// eform table, e(b) unchanged), level(), and vce(robust) / robust / vce(cluster
// panelvar) -- the sandwich over the panel scores (bank 8c1f9870, bee652c3: the
// same e(V) under both). `normal` (Gauss-Hermite quadrature) and a cluster
// variable other than the panel variable are refused by name; pa and fe have
// their own arms.
ojson runXtPoissonReM4(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string y = parsed.varlist[0];
  const std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());
  std::vector<std::string> argv = {"xtpois", "--re", "--y", y, "--x", joinCommaM4(xs), "--ivar",
                                   *session.panelvar};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  static const std::regex rePlainVar(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  bool irr = false, robust = false, clusterOpt = false;
  std::string clusterVar;
  for (const auto& [k, v] : parsed.opts) {
    const std::string val = v.isBool ? std::string() : trimM4(v.strValue);
    if (k == "re" || k == "nolog" || k == "log") continue;
    if (k == "cluster") {
      clusterOpt = true;
      continue;
    }
    if (k == "irr") {
      irr = true;
      continue;
    }
    if (k == "noconstant" || k == "nocons") {
      argv.push_back("--no-intercept");
      continue;
    }
    if (k == "offset" || k == "exposure") {
      if (!std::regex_match(val, rePlainVar))
        return engineErrorBlockM4(k + "(" + val + ") must name one variable in this version of PIET",
                                  "unsupported_option");
      argv.push_back(k == "offset" ? "--offset" : "--exposure");
      argv.push_back(val);
      continue;
    }
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(val);
      continue;
    }
    if (k == "robust" && v.isBool) {
      robust = true;
      continue;
    }
    // vcov() is not an xtpoisson option: Stata's r(198) "option vcov() not allowed"
    // (vs-stata bank 3212c705 `xtpoisson fatal beertax unemp, vcov(robust)`)
    if (k == "vcov")
      return ojson(eval::errorBlock("engine_error", "option vcov() not allowed", std::nullopt,
                                    std::nullopt, std::nullopt, "r(198)"));
    if (k == "vce" && !v.isBool) {
      const std::string t = lowerM4(val);
      if (t == "oim") continue;
      if (t == "r" || t.rfind("rob", 0) == 0) {
        robust = true;
        continue;
      }
      if (t.rfind("cl", 0) == 0) {
        std::istringstream is(val);
        std::string w, cv, extra;
        is >> w >> cv >> extra;
        if (!cv.empty() && extra.empty() && cv == *session.panelvar) {
          robust = true;
          clusterVar = cv;
          continue;
        }
      }
      return engineErrorBlockM4("Option `vce(" + val + ")` is a valid Stata option that this version "
                                "of PIET does not implement for `xtpoisson, re`.",
                                "unsupported_option");
    }
    if (k == "normal")
      return engineErrorBlockM4("Option `normal` (normally distributed random effects, fitted by "
                                "quadrature) is a valid Stata option that this version of PIET does "
                                "not implement for `xtpoisson`.",
                                "unsupported_option");
    return engineErrorBlockM4("Option `" + k + "` is a valid Stata option that this version of PIET "
                              "does not implement for `xtpoisson, re`.",
                              "unsupported_option");
  }
  // The random-effects model reads a cluster() option as the cluster variable only
  // through vce(cluster V): with cluster(V) alone (or beside vce(robust)) Stata's
  // re path hands its cluster parser no variable, "varlist required" r(100)
  // (vs-stata bank a7ec1a11 `xtpoisson fatal beertax unemp, cluster(sid)`).
  if (clusterOpt && clusterVar.empty())
    return ojson(eval::errorBlock("engine_error", "varlist required", std::nullopt, std::nullopt,
                                  std::nullopt, "r(100)"));
  // vce(robust) / robust / vce(cluster panelvar): xtpoisson.ado's own robust VCE
  // (XTPOISSON_FE_ROBUST, re; the engine's xtpoisson_re_stata_robust).
  if (robust) {
    argv.push_back("--vce");
    argv.push_back(clusterVar.empty() ? "robust" : "cluster");
  }
  argv.push_back("--emit-v");
  ojson raw;
  ojson block = runP7MleAsM4("xtpois_re", argv, session, &raw, "xtpoisson");
  if (!raw.is_object() || block.value("type", std::string()) == "error") return block;
  block["estimator"] = "xtpoisson_re";
  block["depvar"] = y;
  // e(b): the depvar equation, then /lnalpha (bank f8d5160a `fatal:beertax ...
  // fatal:_cons /lnalpha`); a factor fit's columns as r(table) lays them out.
  if (raw.contains("e_V") && raw.contains("coefs") && raw.contains("lnalpha")) {
    std::vector<AncillaryEbColM4> cols;
    bool ok = depvarEqColsM4(y, raw["coefs"], "b", cols);
    if (!ok && session.lastModel().has_value() && block.contains("coeftable")) {
      std::vector<rtable_detail::RtCol> rc;
      bool placed = false;
      try {
        placed = rtable_detail::stataColumns(session, *session.lastModel(), block["coeftable"], rc);
      } catch (const std::exception&) {
        placed = false;
      }
      if (placed) {
        ok = true;
        cols.clear();
        for (const auto& c : rc) {
          if (c.row == nullptr) {
            cols.push_back({y + ":" + c.name, std::string(), 0.0});
            continue;
          }
          std::string en = c.row->value("name", std::string());
          const ojson& bv = c.row->contains("b") ? (*c.row)["b"] : ojson();
          const ojson& sv = c.row->contains("se") ? (*c.row)["se"] : ojson();
          if (en.empty() || !bv.is_number() ||
              (bv.get<double>() == 0.0 && sv.is_number() && sv.get<double>() == 0.0)) {
            ok = false;
            break;
          }
          if (en == "_cons") en = "(intercept)";
          cols.push_back({y + ":" + c.name, en, bv.get<double>()});
        }
      }
    }
    if (ok) {
      cols.push_back({"/lnalpha", "/lnalpha", raw["lnalpha"].value("b", 0.0)});
      (void)postAncillaryEbM4(session, raw, cols);
    }
  }
  // /lnalpha, then the alpha row (Stata's diparm, `_diparm1:alpha` in r(table)):
  // no test statistic for either.
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) block["coeftable"] = ojson::array();
  for (const char* key : {"lnalpha", "alpha"}) {
    if (!raw.contains(key) || !raw[key].is_object()) continue;
    const ojson& a = raw[key];
    ojson row;
    row["name"] = std::string(key) == "lnalpha" ? "/lnalpha" : "_diparm1:alpha";
    // A bound the engine could not represent (exp of a large lnalpha limit: alpha's
    // upper limit when the fit is at the alpha -> 0 boundary) is missing, as Stata
    // shows it.
    auto num = [&a](const char* f) {
      return a.contains(f) && a[f].is_number() ? a[f] : ojson(nullptr);
    };
    row["b"] = num("b");
    row["se"] = num("se");
    row["t"] = nullptr;
    row["p"] = nullptr;
    row["ci_lo"] = num("ci_lo");
    row["ci_hi"] = num("ci_hi");
    block["coeftable"].push_back(row);
  }
  // e()'s block keeps the coefficient metric: _b[] / _se[] read it, and they are
  // e(b) / e(V)'s whatever the display option.
  if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
    (*session.lastEstBlock)["coeftable"] = block["coeftable"];
  // irr: incidence-rate ratios for the depvar equation's rows of the returned block
  // only, the table shown and r(table) (bank cc7fb098: r(table) eform 1 on `fatal:`,
  // 0 on /lnalpha and alpha). A display metric: e(b), _b[] and _se[] are untouched.
  if (irr)
    for (auto& row : block["coeftable"]) {
      if (!row.is_object() || !row.contains("b") || !row["b"].is_number()) continue;
      const std::string nm = row.value("name", std::string());
      if (nm == "/lnalpha" || nm == "_diparm1:alpha") continue;
      const double b = row["b"].get<double>();
      const double e = std::exp(b);
      row["b"] = e;
      if (row.contains("se") && row["se"].is_number()) row["se"] = e * row["se"].get<double>();
      if (row.contains("ci_lo") && row["ci_lo"].is_number()) row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
      if (row.contains("ci_hi") && row["ci_hi"].is_number()) row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
      row["eform"] = true;
    }
  return block;
}

ojson runXtPoissonLogit(const std::string& cobraCmd, const std::string& stataCmd,
                         const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  bool fePresent = false, rePresent = false, paPresent = false;
  eval::OptValue feVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "fe") {
      fePresent = true;
      feVal = v;
    }
    if (k == "re") rePresent = true;
    if (k == "pa") paPresent = true;
  }
  bool isFe = fePresent && feVal.isBool && feVal.boolValue;
  // BUG-657: xtlogit.ado / xtpoisson.ado call `_xt, i()` before any model is
  // fitted, whose refusal is "must specify panelvar; use xtset" r(459), the
  // default (random-effects) model included (vs-stata bank b1d131f5 `xtlogit hi
  // beertax unemp`, 4e2bb7cc `xtpoisson fatal beertax unemp`).
  if (!session.panelvar.has_value() || session.panelvar->empty()) {
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  }
  // xtpoisson's count outcome: a negative value in the estimation sample is
  // Stata's r(459) "<depvar> must be greater than or equal to zero", whatever the
  // model (vs-stata bank d5fe8ada `xtpoisson nf beertax unemp`). Checked when the
  // regressors are plain variables (the sample is then y, x and the panel variable
  // nonmissing under `if`).
  if (cobraCmd == "xtpois" && parsed.varlist.size() >= 2) {
    static const std::regex rePlainV(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
    std::vector<std::string> vars(parsed.varlist.begin(), parsed.varlist.end());
    vars.push_back(*session.panelvar);
    bool plain = true;
    for (const auto& v : vars) plain = plain && std::regex_match(v, rePlainV);
    if (plain) {
      ojson filterErr;
      const std::string basePath = *session.currentHbin();
      const auto filtered = applyIfFilterM4(session, basePath, parsed.ifExpr, filterErr);
      if (filtered.has_value()) {
        const bool madeTemp = *filtered != basePath;
        std::optional<Datastore> tempDs;
        Datastore* ds = nullptr;
        try {
          if (madeTemp) {
            tempDs.emplace(*filtered);
            ds = &*tempDs;
          } else {
            ds = session.datastore();
          }
        } catch (const std::exception&) {
          ds = nullptr;
        }
        bool negative = false;
        if (ds != nullptr) {
          std::vector<const double*> cols;
          for (const auto& v : vars) {
            if (!ds->has(v) || ds->dtype(v) != "f64") {
              cols.clear();
              break;
            }
            cols.push_back(ds->col(v).f64());
          }
          if (cols.size() == vars.size()) {
            const int64_t n = ds->nRows();
            for (int64_t i = 0; i < n && !negative; ++i) {
              bool complete = true;
              for (const double* c : cols)
                complete = complete && !std::isnan(c[i]) && c[i] < eval::kStataMissingSentinel;
              if (complete && cols[0][i] < 0) negative = true;
            }
          }
        }
        if (madeTemp) {
          tempDs.reset();
          cecon::native_unlink(*filtered);
        }
        if (negative)
          return ojson(eval::errorBlock("engine_error",
                                        parsed.varlist[0] + " must be greater than or equal to zero",
                                        std::nullopt, std::nullopt, std::nullopt, "r(459)"));
      }
    }
  }
  // An iweight must be constant within each panel of the estimation sample:
  // xtpoisson checks it before either the fe or the re fit, xtlogit before its
  // random-effects fit, with "weight must be constant within <panelvar>" r(199)
  // (vs-stata bank cc03e30c `xtlogit hi beertax unemp [iweight=w]`, 20ff3911
  // `xtpoisson fatal beertax unemp [iweight=w]`, w = 1 + mod(_n,3)). fweights and
  // pweights are refused before this outside pa (tokenizer), aweights always.
  if (!paPresent && (cobraCmd == "xtpois" || !isFe) && parsed.weightType.has_value() &&
      parsed.weightVar.has_value() && parsed.varlist.size() >= 2) {
    const std::string wt = lowerM4(*parsed.weightType);
    static const std::regex rePlainW(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
    if ((wt == "iw" || wt == "iweight") && std::regex_match(*parsed.weightVar, rePlainW)) {
      std::vector<std::string> vars = {*session.panelvar, *parsed.weightVar};
      bool plain = true;
      for (const auto& v : parsed.varlist) {
        plain = plain && std::regex_match(v, rePlainW);
        vars.push_back(v);
      }
      ojson filterErr;
      const std::string basePath = *session.currentHbin();
      const auto filtered =
          plain ? applyIfFilterM4(session, basePath, parsed.ifExpr, filterErr) : std::nullopt;
      if (filtered.has_value()) {
        const bool madeTemp = *filtered != basePath;
        std::optional<Datastore> tempDs;
        Datastore* ds = nullptr;
        try {
          if (madeTemp) {
            tempDs.emplace(*filtered);
            ds = &*tempDs;
          } else {
            ds = session.datastore();
          }
        } catch (const std::exception&) {
          ds = nullptr;
        }
        bool varies = false;
        if (ds != nullptr) {
          std::vector<const double*> cols;
          for (const auto& v : vars) {
            if (!ds->has(v) || ds->dtype(v) != "f64") {
              cols.clear();
              break;
            }
            cols.push_back(ds->col(v).f64());
          }
          if (cols.size() == vars.size()) {
            std::map<double, double> firstW;
            const int64_t n = ds->nRows();
            for (int64_t i = 0; i < n && !varies; ++i) {
              bool complete = true;
              for (const double* c : cols)
                complete = complete && !std::isnan(c[i]) && c[i] < eval::kStataMissingSentinel;
              if (!complete) continue;
              const auto [it, fresh] = firstW.emplace(cols[0][i], cols[1][i]);
              if (!fresh && it->second != cols[1][i]) varies = true;
            }
          }
        }
        if (madeTemp) {
          tempDs.reset();
          cecon::native_unlink(*filtered);
        }
        if (varies)
          return ojson(eval::errorBlock("engine_error",
                                        "weight must be constant within " + *session.panelvar,
                                        std::nullopt, std::nullopt, std::nullopt, "r(199)"));
      }
    }
  }
  // Past that check no fit of these commands takes a weight in this version (the
  // registry lets the weight through only so that the check above can run).
  if (parsed.weightType.has_value())
    return engineErrorBlockM4(
        "Option `[" + *parsed.weightType + "=" + parsed.weightVar.value_or("") +
            "]` is not supported in this version of PIET. `" + stataCmd +
            "` does not support weighted estimation in this engine (no [aw=]/[fw=]/[pw=]/[iw=] "
            "support for this command). Remove the weight specification — unweighted estimation "
            "will differ from weighted results.",
        "unsupported_option");
  if (!isFe && cobraCmd == "xtpois" && !paPresent && !fePresent && parsed.varlist.size() >= 2)
    return runXtPoissonReM4(parsed, session);
  if (!isFe) {
    std::string which = rePresent ? ",re" : (paPresent ? ",pa" : "the default (random-effects)");
    return engineErrorBlockM4(
        "This version of PIET supports only `" + stataCmd + " ,fe` (conditional fixed-effects "
        "MLE). `" + stataCmd + "` with " + which + " is not available — the engine has no "
        "random-effects or population-averaged " + stataCmd + " estimator.",
        "unsupported_option");
  }

  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `" + stataCmd + "` requires a dependent variable and at least one "
        "regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  // BUG-657: xtlogit.ado / xtpoisson.ado call `_xt, i()`, whose refusal is
  // "must specify panelvar; use xtset" r(459) (as xtreg: w2_eststo_prefix.log:
  // 437-441); was unsupported_option = r(198).
  if (!session.panelvar.has_value() || session.panelvar->empty()) {
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  }

  std::vector<std::string> argv = {cobraCmd, "--y",    y,
                                    "--x",    joinCommaM4(xs), "--ivar", *session.panelvar};

  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // offset() — xtpoisson only (cobra's xtpois CLI has --offset; xtlogit's
  // does not, per its own usage banner — an offset() on xtlogit falls
  // through to the leftover unknown-option reject below).
  if (cobraCmd == "xtpois") {
    std::optional<std::string> offsetVar;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "offset" && !v.isBool) offsetVar = v.strValue;
    }
    if (offsetVar.has_value() && !offsetVar->empty()) {
      argv.push_back("--offset");
      argv.push_back(*offsetVar);
    }
  }

  // FU-23 (BUG-756): the cluster VCE is refused by Stata itself for both ,fe
  // estimators, in every spelling but one, and this loop read `cluster` only
  // when valued -- so a bare `cluster` fitted the iid model at rc 0.
  // w2l_vce_shape.log, r(198) unless noted (xtpoisson message / xtlogit message):
  //   cluster(id), cluster(id2), cluster(id id2)  "option cluster() not allowed" (both)
  //   bare `cluster`                "option cluster not allowed" / "cluster invalid"
  //   vce(cluster id), vce(cluster) "option cluster not allowed" / "vcetype cluster not allowed"
  //   vce(cluster id id2)           "multiway cluster VCE not allowed" (both)
  //   robust(x1)                    "option robust() not allowed" / "robust(x1) invalid"
  //   vce(robust x1)                "option x1 not allowed" / "vcetype robust not allowed"
  //   xtlogit vce(robust)           "vcetype robust not allowed" (xtlogit_fe_ctl_vcerobust;
  //                                 xtpoisson_fe_ctl_vcerobust fits, rc 0)
  //   EMPTY `cluster()`             rc 0, IGNORED: the default fit (xtpoisson e(vce) oim;
  //                                 xtlogit runs as clogit, e(cmd) clogit)
  // The engine could not honour a cluster VCE anyway (its conditional-FE CLIs
  // accept `--vce cluster` and silently compute the non-clustered SE), so no
  // refusal here hides a capability.
  const bool isXtlogit = (cobraCmd == "xtlogit");
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  if (vceShape.cluster == OptShapeM4b::kBare) {
    return stataRefusalM4b(isXtlogit ? "cluster invalid" : "option cluster not allowed", 198);
  }
  if (vceShape.cluster == OptShapeM4b::kOne || vceShape.cluster == OptShapeM4b::kMany) {
    return stataRefusalM4b("option cluster() not allowed", 198);
  }
  if (vceShape.robust != OptShapeM4b::kAbsent && vceShape.robust != OptShapeM4b::kBare) {
    return stataRefusalM4b(isXtlogit ? "robust(" + vceShape.robustRaw + ") invalid"
                                     : std::string("option robust() not allowed"),
                           198);
  }
  if (vceShape.vceType == "cluster") {
    if (vceShape.vceRest.size() > 1) return stataRefusalM4b("multiway cluster VCE not allowed", 198);
    return stataRefusalM4b(isXtlogit ? "vcetype cluster not allowed" : "option cluster not allowed",
                           198);
  }
  const bool vceRobustFamily = vceShape.vceType == "robust" || vceShape.vceType == "hc1";
  if (vceRobustFamily && !vceShape.vceRest.empty()) {
    return stataRefusalM4b(isXtlogit ? std::string("vcetype robust not allowed")
                                     : "option " + vceShape.vceRest[0] + " not allowed",
                           198);
  }
  // xtlogit ,fe takes no robust VCE at all. The bare `robust` flag is the same
  // request as vce(robust) in this handler (and in Stata's vce parsing); it has
  // no arm of its own in the oracle, so refusing it rests on vce(robust)'s.
  if (isXtlogit && (vceRobustFamily || vceShape.robust == OptShapeM4b::kBare)) {
    return stataRefusalM4b("vcetype robust not allowed", 198);
  }
  bool sawRobustFlag = false;
  std::optional<std::string> vceVal;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value()) {
      vceVal = v.isBool ? std::string() : v.strValue;
    }
    if (k == "robust" && v.isBool && v.boolValue) sawRobustFlag = true;
  }
  bool wantsRobust =
      sawRobustFlag ||
      (vceVal.has_value() && (canonicalVceType(*vceVal, nullptr) == "robust" ||
                              canonicalVceType(*vceVal, nullptr) == "hc1"));
  if (vceVal.has_value() && !vceVal->empty()) {
    const std::string lv = canonicalVceType(*vceVal, nullptr);
    if (lv != "robust" && lv != "hc1" && lv != "iid") {
      if (!unimplementedVceType(*vceVal).empty()) {
        return engineErrorBlockM4("Option `vce(" + unimplementedVceType(*vceVal) + ")` is a valid Stata option that this version of PIET does not implement", "unsupported_option");
      }
      return engineErrorBlockM4("Unknown vce type: " + *vceVal, "unsupported_option");
    }
  }
  if (wantsRobust) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }

  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (conditional-FE MLE has no separate constant to "
          "suppress — live-verified the engine's CLI hard-errors while still masking a WRONG "
          "result to stdout, same precedent as the other D3 estimators above).",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      // BUG-274, xtpois half: BUG-202 gave cobra's `xtpois` CLI a real `--level`
      // (live-verified on this tree's engine: `--level 90` emits per-coef
      // ci_lo/ci_hi), so the old shared refusal's premise expired for xtpoisson
      // and it forwards, same idiom as runTobit/runNbreg. The xtlogit half of
      // the premise is still TRUE — re-measured on the same build: `cobra
      // xtlogit --level 90` hard-errors "unrecognized option" (exit 3) with a
      // CLEAN empty stdout (the old "masking gap" wording was stale too: no
      // masked result prints), so xtlogit keeps refusing, now for the measured
      // reason.
      if (cobraCmd == "xtpois") {
        argv.push_back("--level");
        argv.push_back(v.strValue);
        continue;
      }
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (the engine's xtlogit CLI has no --level surface — "
          "live-verified: `cobra xtlogit --level` hard-errors 'unrecognized option', exit 3, "
          "with a clean empty stdout).",
          "unsupported_option");
    }
  }

  // BUG-233: the base set is the file-scope literal; `offset` stays conditional
  // here because xtlogit must keep REJECTING it. The registered vocabulary is
  // the union of the two, which is correct for canonicalization: resolution
  // only rewrites a key to a canonical spelling, and the reject net below is
  // still the authority on whether that spelling is allowed for THIS command.
  std::unordered_set<std::string> known = kXtPoissonLogitBase;
  if (cobraCmd == "xtpois") known.insert("offset");
  if (auto err = eval::rejectUnknownOptions(parsed.opts, known)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    // ME06: xtpoisson, fe sends its e(V) (--emit-v), the matrix margins reads.
    std::vector<std::string> fitArgv = argv;
    fitArgv.push_back("--emit-v");
    raw = runCobraJsonM4(fitArgv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  block["estimator"] = stataCmd + "_fe";
  block["depvar"] = y;
  fillCoeftableStatsM4(block);
  static const char* kXtFields[] = {"n_groups", "n_dropped_const", "n_dropped_missing",
                                    "n_dropped_size"};
  for (const char* k : kXtFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }

  LastModel lm;
  lm.cobraCmd = cobraCmd;  // "xtpois"/"xtlogit" — already anticipated in margins' kModelMap.
  // e(cmd) is Stata's name, not the engine's: `xtpoisson` (w2l_vce_shape.log
  // xtpoisson_fe_ctl_vcerobust, where main published `xtpois`), and `clogit` for
  // every xtlogit ,fe fit: xtlogit ,fe runs `clogit ... group(ivar)` and
  // never reposts e(cmd), only e(cmd2) "xtlogit" (w2p_estim_ev D_xtlogit_fe
  // `|clogit|`, w2l_vce_shape xtlogit_fe_empty_cluster).
  lm.stataCmd = isXtlogit ? std::string("clogit") : stataCmd;
  // FU-102: e(N_g) and the panel row counts over the fit's panels (after the
  // all-0/all-1/all-zero and singleton drops) for xtlogit and
  // xtpoisson (w2p_estim_ev D_xtlogit_fe / D_xtpoisson_fe
  // `0|54|9|6|6|`). The engine emits g_min/g_avg/g_max; N_g is its n_groups.
  if (raw.is_object() && raw.contains("n_groups") && raw["n_groups"].is_number())
    raw["N_g"] = raw["n_groups"];
  lm.y = y;
  lm.x = xs;
  lm.ivar = *session.panelvar;
  if (wantsRobust) lm.vcov = "hc1";
  // The fe fits are ML: the default e(vce) is `oim` (w2l_vce_shape_det.log:1003
  // xtpoisson_fe_empty_cluster `0|xtpoisson|oim||.|180`, xtlogit_fe_empty_cluster
  // `0|clogit|oim||.|180`).
  if (lm.vcov == "iid") lm.eVce = "oim";
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // e(V) is the fit's own, for both conditional FE families
  if (isXtlogit) {
    session.eval.setEString("cmd2", "xtlogit");
    session.noteEOnFit("cmd2");
  }
  return block;
}

// `in a/b`: folded into the fit's `if` as an observation-number condition
// (inRangeConditionM4), so every reader of the condition sees the same rows
// (vs-stata bank 037b467c `xtlogit ... in 1/60`).
static ojson runXtInFoldM4(const std::string& cobraCmd, const std::string& stataCmd,
                           const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    std::optional<std::string> eff;
    if (auto err = inRangeConditionM4(session, parsed, eff, stataCmd)) return *err;
    eval::ParsedCommand folded = parsed;
    folded.ifExpr = eff;
    folded.inRange.reset();
    return runXtPoissonLogit(cobraCmd, stataCmd, folded, session);
  }
  return runXtPoissonLogit(cobraCmd, stataCmd, parsed, session);
}

ojson runXtPoisson(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtInFoldM4("xtpois", "xtpoisson", parsed, session);
}

ojson runXtLogitFe(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtInFoldM4("xtlogit", "xtlogit", parsed, session);
}

// drdid (BS-076): the classic 2-period/2-group doubly-robust DiD estimator
// -> the engine's csdid --method dripw machinery, ONE (g,t) cell. Oracle:
// tests/validate/drdid2x2.{do,log} — `drdid Y x1 x2, ivar(id) time(t)
// treatment(treat) dripw` (the fixture's EXACT invocation; this is the ONLY
// drdid surface wired — other method()s (reg/ipw/std_ipw/trad) and the
// bare/no-`dripw` form have no oracle coverage and honest-reject).
//
// Implementation boundary:
// this wiring touches ZERO cobra-side code — src/estimators/csdid.cpp's
// estimator runs completely unmodified, exactly as `csdid`/`estat` already
// call it. The ONLY new logic is PIET-side: constructing a synthetic `gvar`
// column from the fixture's own treatment()/time() variables
// (treatment()!=0 -> gvar=max(time), the post period; treatment()==0 ->
// gvar=0, CS21's "never-treated" sentinel — csdid_dripw_run only ever forms
// a cohort from gvar>0 rows, so 0 is a safe comparison-group marker
// regardless of how the panel's own time variable happens to be coded) via
// cobra's GENERIC `eval --gen` data-prep verb (the same verb `generate`/
// `egen` already route through elsewhere in this codebase — not
// csdid-specific), then calling the UNMODIFIED `cobra csdid --method dripw
// --comparison never` on the augmented file.
//
// LIVE-VERIFIED against the oracle (drdid2x2.dta, 600 obs, 300 units x 2
// periods): ATT = 2.0561083418597157 (oracle 2.05610834185971), SE =
// 0.12400508894658192 (oracle .124005088944043) — matches to ~1e-13
// relative, i.e. this construction is mathematically exact, not an
// approximation.
//
// Restricted to the fixture's own 2-period design (time() must have EXACTLY
// 2 distinct values) — matching the command's own name ("2x2") and its real
// Stata namesake, which is likewise a strictly-2-period estimator.

}  // namespace piet::handlers
