// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Count estimation.
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
  // double-underscore shape of handlers.cpp's residual mask.
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
  // (BUG-1214: StataBE's `estimates store` writes their _est_NAME).
  static const std::set<std::string> kMl = {"logit", "probit", "poisson", "tobit", "nbreg",
                                            "mlogit", "fepois"};
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
  // The handshake (handlers.cpp esampleHandshakeOk) compares these 1s with the
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
  for (const auto& [k, v] : parsed.opts) {
    if (k == "exposure" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `exposure()` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (no oracle coverage and no engine CLI equivalent — "
          "use offset() with a pre-computed ln(exposure) variable instead).",
          "unsupported_option");
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
    if (k == "level" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `level` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (same stdout/exit-code masking gap as noconstant "
          "above).",
          "unsupported_option");
    }
  }

  // kVocabPpmlD is ppmlhdfe's own slot: the shared count-model set plus `d`.
  const std::unordered_set<std::string>& known =
      (stataCmd == "ppmlhdfe") ? kVocabPpmlD.known : kVocabPoisson.known;
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
  // reghdfe (handlers.cpp).
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
    if (k == "exposure" && !v.isBool) {
      return engineErrorBlockM4(
          "Option `exposure()` is a valid Stata option that this version of PIET does not yet "
          "implement for `nbreg` (no oracle coverage and no engine CLI equivalent — use offset() "
          "with a pre-computed ln(exposure) variable instead).",
          "unsupported_option");
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
  // aweight is Stata's r(101), the tokenizer's kStataLegalWeights row). A pweight
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

ojson runXtPoisson(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtPoissonLogit("xtpois", "xtpoisson", parsed, session);
}

ojson runXtLogitFe(const eval::ParsedCommand& parsed, SessionState& session) {
  return runXtPoissonLogit("xtlogit", "xtlogit", parsed, session);
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
