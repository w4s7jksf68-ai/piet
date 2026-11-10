// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Panel estimation.
#include "piet/handlers.h"
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
#include "lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

namespace fit_detail {

// dispatcher.py's "Cache last_model for test/margins/predict/lincom/
// coefplot/eststo" block — extracts --y/--x/--fe/--vcov/--cluster from the
// argv actually sent to cobra (post TS-injection) so post-estimation
// commands reconstruct the SAME call. Mirrors handlers.cpp::runRegress's
// identical inline logic (M2), duplicated here per this file's
// established one-copy-per-milestone-file convention.
//
// e(sample) T2: `esampleValues` is the fit's `--esample-out` file (esampleFitArgs,
// NOT in `argv`) for a route wired for the mask; empty (every other caller)
// leaves the model's e(sample) a named r(198) and the call byte-identical.
//
// `stataCmd`: the Stata command word e(cmd) publishes (LastModel::stataCmd), or
// empty to publish cobraCmd as before.
void cacheLastModelM4(SessionState& session, const std::string& cobraCmd,
                       const std::vector<std::string>& argv, const ojson& block, const ojson& raw,
                       const std::string& esampleValues,
                       const std::string& stataCmd) {
  LastModel lm;
  lm.cobraCmd = cobraCmd;
  lm.stataCmd = stataCmd;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--y" && i + 1 < argv.size()) lm.y = argv[i + 1];
    if (argv[i] == "--x" && i + 1 < argv.size()) {
      std::string joined = argv[i + 1];
      std::string cur;
      for (char c : joined) {
        if (c == ',') {
          if (!cur.empty()) lm.x.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) lm.x.push_back(cur);
    }
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
    if (argv[i] == "--keep-singletons") lm.keepSingletons = true;  // e(sample) T2
    if (argv[i] == "--collinear-forward") lm.collinearForward = true;  // BUG-1067
    // W:P5: logit/probit spell the same selector `--vce` (buildEstimationArgvM4
    // remaps `--vcov hc1|cluster` onto it), so scanning only `--vcov` left
    // lm.vcov at its "iid" default for exactly the models that DID have a
    // robust/cluster VCE — and every post-estimation command then replayed the
    // fit as iid. Normalize back to the linear spelling so this one field
    // means one thing no matter which CLI produced it.
    if (argv[i] == "--vce" && i + 1 < argv.size()) {
      lm.vcov = (argv[i + 1] == "robust") ? std::string("hc1") : argv[i + 1];
    }
    if (argv[i] == "--cluster" && i + 1 < argv.size()) lm.cluster.push_back(argv[i + 1]);
    // runSvyPrefix: a `svy:` fit's e(vce) is linearized (StataBE 19.5, BUG-1513);
    // lm.vcov keeps the engine spelling the replays key on.
    if (argv[i] == "--svy") lm.eVce = "linearized";
  }
  lm.depvar = block.value("depvar", "");
  captureFitFilterM4(lm, argv);
  lm.dataPath = session.currentHbin().value_or("");
  lm.result = raw;
  // BUG-570, the twin of runRegress's rule: e(N_clust) exists only after a
  // clustered fit, but the engine's feols reports n_clusters 0 for every other
  // VCE, so `xtreg ..., fe` (routed here as "feols") printed e(N_clust) 0 where
  // Stata prints `.` (w2_pweight.log X0_fe PSTR `120|103|.|conventional|||||`,
  // X3_aw_fe alike). logit/probit and xtreg,re were already `.` -- their engines
  // omit the key when unclustered (measured on main 2026-09-28) -- so this is a
  // no-op for them. Dropped from the cached reply only; the emitted block is
  // unchanged.
  if (lm.result.has_value() && lm.result->is_object() && lm.result->contains("n_clusters") &&
      (*lm.result)["n_clusters"].is_number() &&
      (*lm.result)["n_clusters"].get<double>() == 0.0) {
    lm.result->erase("n_clusters");
  }
  // BUG-1041: xtreg, fe posts e(tss), the total sum of squares about the overall
  // mean (StataBE 9544.62 where its within e(mss) is 8389.45). Cached reply only.
  if (lm.stataCmd == "xtreg" && lm.result.has_value() && lm.result->is_object() &&
      lm.result->contains("tss_total") && (*lm.result)["tss_total"].is_number())
    (*lm.result)["tss"] = (*lm.result)["tss_total"];
  // BUG-518 order: the mask column first, then setLastModel.
  if (!esampleValues.empty()) (void)esampleInstallOnHead(session, esampleValues, *lm.result, lm);
  session.setLastModel(lm);
  session.lastEstBlock = block;
  session.attachFitCovariance(raw);  // eb-order T-P1: e(V) when the fit sent one
  // A noconstant fit (`--no-intercept`) records it, as runRegress does, so the
  // e(b) readers know the missing `_cons` is Stata's too (w2p_estim_ev.log:1585
  // G_pnc_fit `probit d x1 x2, noconstant`: colsof(e(b)) = 2).
  if (std::find(argv.begin(), argv.end(), "--no-intercept") != argv.end() &&
      session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
    (*session.lastEstBlock)["noconstant"] = true;
}

// FU-29: xtreg,fe's weight rule. After `mark touse [weight] if in` and
// `markout touse ivar tvar varlist` it runs `by ivar touse: assert w == w[_N]
// if touse` and stops with "weight must be constant within <ivar>" r(199)
// (w2_pweight.log X10_pw_varies_within_id; aer_b4 predict-e-xbd.log
// G_w_varies, aweight). The engine fits a varying weight without complaint, so
// the check has to happen here, before the fit.
//
// The rows checked are a SUPERSET of Stata's sample: the `if` filter, a
// non-missing panel id, a non-missing and nonzero weight (`mark` drops zero
// weights), and a non-missing depvar and plain/c./i. regressor. Terms behind
// any other operator (L./D./ib2./...) do not narrow the rows. A superset can
// only refuse where Stata would fit (an honest r(199) on a weight that varies
// only on rows Stata drops); it can never fit where Stata refuses.
// Returns nullopt when the weight passes.
std::optional<ojson> xtregWeightWithinPanelM4(const eval::ParsedCommand& parsed,
                                              SessionState& session, const std::string& ivar) {
  const std::string& wvar = *parsed.weightVar;
  const std::string basePath = *session.currentHbin();
  ojson filterErr;
  auto filteredPath = applyIfFilterM4(session, basePath, parsed.ifExpr, filterErr);
  if (!filteredPath.has_value()) return filterErr;
  const bool madeTemp = (*filteredPath != basePath);
  std::optional<ojson> verdict;
  try {
    std::optional<Datastore> tempDs;
    Datastore* ds = nullptr;
    if (madeTemp) {
      tempDs.emplace(*filteredPath);
      ds = &*tempDs;
    } else {
      ds = session.datastore();
    }
    // A weight that is not a column of the data (an expression such as
    // `[aw=1/x]`) is refused by the engine itself ("column not found"), and a
    // panel variable is always a column, so only the type needs a verdict here.
    if (ds != nullptr && ds->has(wvar) && ds->has(ivar)) {
      if (ds->dtype(wvar) != "f64" || ds->dtype(ivar) != "f64") {
        verdict = engineErrorBlockM4("xtreg: could not verify that the weight is constant within " +
                                     ivar + " (weight and panel variable must be numeric)");
      } else {
        std::vector<const double*> need;
        int64_t n = ds->nRows();
        auto add = [&](const std::string& name) {
          if (!ds->has(name) || ds->dtype(name) != "f64") return;
          const cecon::Column& c = ds->col(name);
          const int64_t rows = c.nbytes / static_cast<int64_t>(sizeof(double));
          if (rows < n) n = rows;
          need.push_back(c.f64());
        };
        add(wvar);
        add(ivar);
        const double* w = need[0];  // both are f64 columns (checked just above)
        const double* g = need[1];
        for (const auto& term : parsed.varlist) {
          size_t start = 0;
          while (start <= term.size()) {
            const size_t hash = term.find('#', start);
            const std::string part =
                term.substr(start, hash == std::string::npos ? std::string::npos : hash - start);
            start = (hash == std::string::npos) ? term.size() + 1 : hash + 1;
            if (part.empty()) continue;
            const size_t dot = part.rfind('.');
            if (dot == std::string::npos) {
              add(part);
            } else {
              const std::string op = lowerM4(part.substr(0, dot));
              if (op == "c" || op == "i") add(part.substr(dot + 1));
            }
          }
        }
        std::map<double, double> first;  // panel id -> its first weight in the sample
        for (int64_t i = 0; i < n && !verdict.has_value(); ++i) {
          bool in = !(w[i] == 0.0);
          for (const double* col : need) {
            if (std::isnan(col[i])) in = false;
          }
          if (!in) continue;
          auto it = first.emplace(g[i], w[i]).first;
          if (it->second != w[i]) {
            verdict = ojson(eval::errorBlock("engine_error", "weight must be constant within " + ivar,
                                             std::nullopt, std::nullopt, std::nullopt, "r(199)"));
          }
        }
      }
    }
  } catch (const std::exception& e) {
    verdict = engineErrorBlockM4(std::string("xtreg: could not read the data to check the weight: ") +
                                 e.what());
  }
  if (madeTemp) cecon::native_unlink(*filteredPath);
  return verdict;
}

// FU-76: the e() spellings of the panel R-squared pair. The engine reports them
// as r2_between/r2_overall (feols single-FE, apps/cmds/common.cpp
// add_panel_scalars; xtreg,re, cmds_panel.cpp), and the e() resolver only knew
// `r2_w`, so e(r2_b)/e(r2_o) read `.` beside a number the reply already held
// (w2_pweight.log X0_fe PXT +1.f03574901785fX-001 / +1.5afec63f08acbX-001; the
// engine's 0.9691578317540571 / 0.6777250244088803). Copied into the cached
// reply only, as N_g/df_a are: the emitted block's shape is freeze-pinned. A
// reply without the source key (weighted fits, see runXtreg) stays `.`.
void aliasPanelR2M4(ojson& raw) {
  if (!raw.is_object()) return;
  if (raw.contains("r2_between") && raw["r2_between"].is_number()) raw["r2_b"] = raw["r2_between"];
  if (raw.contains("r2_overall") && raw["r2_overall"].is_number()) raw["r2_o"] = raw["r2_overall"];
}

// BUG-1482: the rank of a one-way clustered V (e_V, slopes and _cons), Stata's
// r(rankc) (Stata's robust-VCE step: the pivoted QR of the clustered V, tolerance 1e-9):
// the eigenvalues of the symmetric V above 1e-9 of the largest. Under xtreg, fe
// the clusters nest the panels, so the _cons scores sum to zero in every cluster
// and the rank is min(slopes, G - 1) on ordinary data (StataBE 19.5, 2 clusters
// and 2 slopes: e(rank) 1). nullopt when e_V is absent or not square.
std::optional<double> clusteredVRankM4(const ojson& raw) {
  if (!raw.contains("e_V") || !raw["e_V"].is_object() || !raw["e_V"].contains("matrix") ||
      !raw["e_V"]["matrix"].is_array())
    return std::nullopt;
  const ojson& m = raw["e_V"]["matrix"];
  const size_t k = m.size();
  if (k == 0) return std::nullopt;
  std::vector<double> a(k * k);
  for (size_t r = 0; r < k; ++r) {
    if (!m[r].is_array() || m[r].size() != k) return std::nullopt;
    for (size_t c = 0; c < k; ++c) {
      if (!m[r][c].is_number()) return std::nullopt;
      a[r + c * k] = m[r][c].get<double>();
    }
  }
  std::vector<double> w, z;
  if (piet::lapack::dsyevrVAL(static_cast<int>(k), std::move(a), 0.0, w, z) != 0 || w.empty())
    return std::nullopt;
  const double top = *std::max_element(w.begin(), w.end());
  double rank = 0.0;
  for (double v : w)
    if (v > 1e-9 * top) rank += 1.0;
  return rank;
}

}  // namespace

// ===========================================================================
// xtreg — fe routes to feols with --fe=panelvar; re routes to cobra xtreg
// with --ivar=panelvar (no vcov support).

ojson runXtreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  bool isBe = false;
  for (const auto& [k, v] : parsed.opts) {
    static const std::set<std::string> kBad = {"gls", "mle", "ec1", "pa"};
    if (kBad.count(k) && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "This version of PIET supports only `xtreg ,fe` (-> feols within), "
          "`xtreg ,re` (Swamy-Arora GLS) and `xtreg ,be` (between). `xtreg ," + k +
              "` is not available.",
          "unsupported_option");
    }
    if (k == "be" && v.isBool && v.boolValue) isBe = true;
  }
  // BUG-1515: between effects, xtreg,be: the OLS of the panel means of y on the
  // panel means of the regressors and a constant, one row per panel (cobra xtreg
  // --be). StataBE 19.5 (i6.do, 20 panels of 6): _b[x] .7062577229568211, _se[x]
  // .12404345599768729, e(df_r) 18, e(r2_b) .64298067688888827, e(model) "be".
  // Its syntax has no weight and no vce other than conventional (m2.do, StataBE
  // 19.5): [aw=] r(101) "weights not allowed", vce(robust) / vce(cluster id) r(198)
  // "vcetype robust|cluster not allowed", and robust, cluster() and noconstant are
  // r(198) "option ... not allowed". `wls` (weights by panel size) is not served.
  if (isBe) {
    for (const auto& [k, v] : parsed.opts) {
      if ((k == "fe" || k == "re") && v.isBool && v.boolValue)  // xtreg.ado opts_exclusive
        return ojson(eval::errorBlock("engine_error",
                                      "only one of re, fe, be, pa, or cre is allowed",
                                      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
    if (!session.panelvar.has_value() || session.panelvar->empty()) {
      return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset",
                                    std::nullopt, std::nullopt, std::nullopt, "r(459)"));
    }
    if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
        !parsed.weightVar->empty()) {
      return ojson(eval::errorBlock("engine_error", "weights not allowed", std::nullopt,
                                    std::nullopt, std::nullopt, "r(101)"));
    }
    for (const auto& [k, v] : parsed.opts) {
      if (v.isBool && !v.boolValue) continue;
      std::string refusal;
      if (k == "vce" || k == "vcov") {
        // The first word as typed: vce(r) is "vcetype r not allowed", and even
        // vce(conv) is refused (StataBE 19.5 m3b.do); vce(bootstrap) and
        // vce(jackknife) are prefix runs this build does not have.
        const std::string vt = trimM4(v.isBool ? std::string() : v.strValue);
        const std::string word = vt.substr(0, vt.find_first_of(" \t,"));
        const std::string lw = lowerM4(word);
        if (lw.size() >= 4 && (std::string("bootstrap").rfind(lw, 0) == 0 ||
                               std::string("jackknife").rfind(lw, 0) == 0))
          return engineErrorBlockM4("`xtreg ,be vce(" + word + ")` is not available in this version "
                                    "of PIET.",
                                    "unsupported_option");
        if (!word.empty() && word != "conventional") refusal = "vcetype " + word + " not allowed";
      } else if (k == "robust" || k == "noconstant" || k == "nocons") {
        refusal = "option " + k + " not allowed";
      } else if (k == "cluster") {
        refusal = "option cluster() not allowed";
      } else if (k == "absorb") {
        refusal = "option absorb() not allowed";
      } else if (k == "wls") {
        return engineErrorBlockM4("`xtreg ,be wls` (weights by panel size) is not available in this "
                                  "version of PIET.",
                                  "unsupported_option");
      }
      if (!refusal.empty())
        return ojson(eval::errorBlock("engine_error", refusal, std::nullopt, std::nullopt,
                                      std::nullopt, "r(198)"));
    }
    if (auto refused = tsFitRestrictionRefusal(parsed, "xtreg")) return *refused;
    // What is left of vce() is vce(conventional), the default (rc 0 in Stata),
    // which the shared builder does not spell.
    eval::ParsedCommand beParsed = parsed;
    beParsed.opts.erase(std::remove_if(beParsed.opts.begin(), beParsed.opts.end(),
                                       [](const auto& o) { return o.first == "vce" || o.first == "vcov"; }),
                        beParsed.opts.end());
    EstimationArgvResult built = buildEstimationArgvM4("xtreg", beParsed, session);
    if (built.errored) return built.errorBlock;
    std::vector<std::string> cobraArgs = built.argv;
    cobraArgs.push_back("--ivar");
    cobraArgs.push_back(*session.panelvar);
    cobraArgs.push_back("--be");
    cobraArgs.push_back("--emit-v");
    cobraArgs.push_back("--data");
    cobraArgs.push_back(*session.currentHbin());
    cobraArgs.push_back("--tidy");
    ojson raw;
    try {
      raw = runCobraJsonM4(cobraArgs);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    ojson block = cobraToEstimationM4(raw, "xtreg", &session);
    aliasPanelR2M4(raw);
    // e(N_g), e(Tbar) (the mean panel size), e(Tcon), e(rank) (cached reply only).
    if (raw.is_object()) {
      if (raw.contains("n_groups") && raw["n_groups"].is_number()) raw["N_g"] = raw["n_groups"];
      if (raw.contains("g_avg") && raw["g_avg"].is_number()) raw["Tbar"] = raw["g_avg"];
      if (raw.contains("g_min") && raw["g_min"].is_number() && raw.contains("g_max") &&
          raw["g_max"].is_number())
        raw["Tcon"] = raw["g_min"].get<double>() == raw["g_max"].get<double>() ? 1.0 : 0.0;
      if (raw.contains("e_V") && raw["e_V"].is_object() && raw["e_V"].contains("names") &&
          raw["e_V"]["names"].is_array())
        raw["k_vars"] = raw["e_V"]["names"].size();
    }
    cacheLastModelM4(session, "xtreg_be", cobraArgs, block, raw, std::string(), "xtreg");
    if (session.lastModel().has_value()) {
      putEStringNL(session, "model", "be");
      session.eval.setEString("vce", "conventional");
    }
    return block;
  }
  // model = str(opts.get("fe", opts.get("re", ""))).lower() — reads fe's
  // value if the key is present (bare flag -> "true"), else falls back to
  // re's value, else "". isFe = (model=="fe") or (opts.get("fe") is True).
  bool fePresent = false, rePresent = false;
  eval::OptValue feVal, reVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "fe") {
      fePresent = true;
      feVal = v;
    }
    if (k == "re") {
      rePresent = true;
      reVal = v;
    }
  }
  bool isFe = false;
  if (fePresent) {
    isFe = feVal.isBool && feVal.boolValue;  // opts.get("fe") is True (bare flag)
  } else if (rePresent) {
    isFe = false;
  } else {
    isFe = false;  // model == "" -> not "fe" -> RE branch (oracle errors there on missing panelvar)
  }

  if (isFe) {
    // BUG-657: Stata's own refusal (xtreg.ado -> `_xt, i()`): "must specify
    // panelvar; use xtset" r(459) -- w2_eststo_prefix.log:437-441 and
    // w2d_uncovered.log:3468-3473. It was unsupported_option = r(198), so a
    // do-file branching on `_rc == 459` took the wrong arm.
    if (!session.panelvar.has_value() || session.panelvar->empty()) {
      return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset",
                                    std::nullopt, std::nullopt, std::nullopt, "r(459)"));
    }
    // FU-29: xtreg,fe's `syntax ... [aw fw pw]` -- an iweight is Stata's
    // r(101) "iweights not allowed" (w2_pweight.log X11_iw_fe). The registry
    // row is AllWeights because the RE arm below has a different rule.
    const bool weighted =
        parsed.weightType.has_value() && parsed.weightVar.has_value() && !parsed.weightVar->empty();
    if (weighted) {
      const std::string wt = lowerM4(*parsed.weightType);
      if (wt == "iw" || wt == "iweight") {
        return ojson(eval::errorBlock("engine_error", "iweights not allowed", std::nullopt,
                                      std::nullopt, std::nullopt, "r(101)"));
      }
    }
    // FU-475: the engine applies --keepif after its time-series operators only
    // for a restriction it can evaluate on the full frame.
    if (auto refused = tsFitRestrictionRefusal(parsed, "xtreg")) return *refused;
    EstimationArgvResult built =
        buildEstimationArgvM4("feols", parsed, session, *session.panelvar);
    if (built.errored) return built.errorBlock;
    if (weighted) {
      if (auto refused = xtregWeightWithinPanelM4(parsed, session, *session.panelvar)) {
        return *refused;
      }
    }
    std::vector<std::string> cobraArgs = built.argv;
    // FU-386: xtreg,fe's multiway cluster VCE is Stata's built-in convention
    // (xtreg,fe -> Stata's absorb step -> Stata's robust-VCE step), minus = rank + dfax:
    // w2s_multiway_class.log:696 vce(cluster cg1 cg2) se(x1) .0532737214421747 where
    // reghdfe's Gmin factor (the engine default) gave .0509313674247111.
    // FU-401/FU-407: every cluster variable must nest the panels, else r(498)
    // (Stata's absorb step -> Stata's xtreg cluster check; log:640
    // vce(cluster id id2), vce(cluster id2)): the engine checks it on the
    // estimation sample (--fe-nested-in-clusters) and the refusal is mapped below.
    bool clusterVce = false;
    bool multiwayVce = false;
    for (size_t i = 0; i + 1 < cobraArgs.size(); ++i) {
      if (cobraArgs[i] != "--vcov") continue;
      clusterVce = cobraArgs[i + 1] == "cluster" || cobraArgs[i + 1] == "twoway" ||
                   cobraArgs[i + 1] == "multiway";
      multiwayVce = cobraArgs[i + 1] == "twoway" || cobraArgs[i + 1] == "multiway";
    }
    if (multiwayVce) cobraArgs.push_back("--multiway-subset-factors");
    // BUG-538 (W2 estimopts): under `xtreg, fe` Stata's vce(robust) / bare
    // `robust` IS vce(cluster panelvar) -- w2_pweight.log X1_fe_vcerobust
    // is byte-identical to X2_fe_clusterid (e(vce)=cluster, e(clustvar)=id,
    // N_clust 15, df_r 14, se_x1 +1.0ba4e5218cb87X-002), where this route
    // forwarded HC1 (n_clusters 0, df_r 103) at rc 0. The shared builder
    // cannot know the panel variable, so the rewrite lives here, after it.
    for (size_t i = 0; i + 1 < cobraArgs.size(); ++i) {
      if (cobraArgs[i] == "--vcov" && cobraArgs[i + 1] == "hc1") {
        cobraArgs[i + 1] = "cluster";
        cobraArgs.insert(cobraArgs.begin() + static_cast<std::ptrdiff_t>(i + 2),
                         {"--cluster", *session.panelvar});
        break;
      }
    }
    // e(sample) T2: xtreg, fe keeps a one-observation panel in the sample and in
    // N_g (w2_esample.log:897 D3 `0|59|59|11|`; the engine's reghdfe default
    // dropped id 11: 58 and 10). Read back into LastModel::keepSingletons by
    // cacheLastModelM4, so the refits keep it too.
    cobraArgs.push_back("--keep-singletons");
    // BUG-1067: xtreg, fe keeps the earlier term of a collinear set, as reghdfe and
    // areg do (StataBE scratch-r150/c1067/m1.do A_xtreg `x o.z`), not the engine's
    // default pivoted basis (regress's `o.x z`). Read back into
    // LastModel::collinearForward by cacheLastModelM4, so the refits keep it too.
    cobraArgs.push_back("--collinear-forward");
    cobraArgs.push_back("--data");
    cobraArgs.push_back(*session.currentHbin());
    cobraArgs.push_back("--tidy");
    // e(sample) T2: the engine writes the fit's admitted rows over the --data
    // rows (its own --keepif included); cacheLastModelM4 installs them. Kept
    // out of cobraArgs, which cacheLastModelM4 reads as the fit's flags.
    std::vector<std::string> fitArgv = cobraArgs;
    const std::string esampleValues = esampleFitArgs(session, fitArgv, std::string(), 0);
    fitArgv.push_back("--emit-v");  // eb-order T-P1: the fit's full e(V) (feols row)
    // FU-401: the nesting check is the fit's alone (not in cobraArgs, which the
    // refits replay: a model that was fitted already passed it). vce(robust) was
    // rewritten to cluster(panelvar) above, which nests trivially.
    if (clusterVce || std::find(cobraArgs.begin(), cobraArgs.end(), "--cluster") != cobraArgs.end())
      fitArgv.push_back("--fe-nested-in-clusters");
    ojson raw;
    try {
      raw = runCobraJsonM4(fitArgv);
    } catch (const CobraError& ce) {
      cecon::native_unlink(esampleValues);
      // Stata's r(498), with its message (one-way: the first line only); no e() is
      // posted, so a following `_se[x1]` is r(111) (log:645-648).
      if (ce.detail == "panels_not_nested")
        return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                      std::nullopt, std::string("r(498)")));
      // BUG-1269: `xtreg, fe` on an empty sample is r(2001) "insufficient
      // observations", not regress's r(2000) (StataBE 19.5, `xtreg y x if x > 20, fe`).
      if (ce.code == "no_obs" && std::string(ce.what()) == "no observations")
        return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                      std::nullopt, std::nullopt, std::string("r(2001)")));
      return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
    }
    // FU-29: e(sigma_e) of a weighted fit. The engine's panel-scalar producer
    // (apps/cmds/common.cpp capture_panel_means) stands down under weights, so
    // the weighted reply has no sigma_e -- but it has rmse, and for the within
    // estimator both are sqrt(rss / (N - N_g - k)) (the unweighted reply
    // carries the two keys bit-identical: 2.4986263477117516). Filled only when
    // the reply's own residual dof IS N - N_g - k. Oracle: w2_pweight.log X3-X9
    // PVAL sigma_e +1.4013601975c1dX+001 = the engine's weighted rmse
    // 2.500591289931903; X14 (fweight) header sigma_e 2.3734216 = rmse with dof
    // 343. sigma_u/rho/corr/r2_b/r2_o have no such identity and stay `.`,
    // named in a note on the block (see below).
    if (raw.is_object() && !raw.contains("sigma_e") && raw.contains("rmse") &&
        raw["rmse"].is_number() && raw.contains("dof_resid") && raw["dof_resid"].is_number() &&
        raw.contains("n_obs") && raw["n_obs"].is_number() && raw.contains("k_vars") &&
        raw["k_vars"].is_number() && raw.contains("absorbed") && raw["absorbed"].is_object() &&
        raw["absorbed"].size() == 1 && raw["absorbed"].contains(*session.panelvar) &&
        raw["absorbed"][*session.panelvar].is_number()) {
      const double dofWithin = raw["n_obs"].get<double>() -
                               raw["absorbed"][*session.panelvar].get<double>() -
                               raw["k_vars"].get<double>();
      if (raw["dof_resid"].get<double>() == dofWithin) raw["sigma_e"] = raw["rmse"];
    }
    ojson block = cobraToEstimationM4(raw, "feols", &session);
    // BUG-538, e() half: [XT] xtreg stores e(N_g) (panels in the fit) and
    // e(df_a) = N_g - 1 (w2_pweight.log X0_fe PXT: 15 and 14; X7 under
    // `if t>2`: still 15 and 14). The engine reports the panel count as the
    // absorbed-levels entry for the panel variable; it is copied into the
    // cached reply under the xtreg names so e(N_g)/e(df_a) resolve (the
    // resolver falls through key==name), and NOT into the emitted block,
    // whose shape the freeze fixtures pin.
    if (raw.is_object() && raw.contains("absorbed") && raw["absorbed"].is_object() &&
        raw["absorbed"].contains(*session.panelvar) &&
        raw["absorbed"][*session.panelvar].is_number()) {
      const double ng = raw["absorbed"][*session.panelvar].get<double>();
      raw["N_g"] = ng;
      raw["df_a"] = ng - 1.0;
    }
    // FU-102: e(g_min)/e(g_avg)/e(g_max), the panel row counts over the
    // estimation sample (xtreg,fe; w2p_estim_ev D_xtreg_fe_ctl
    // `0|60|10|6|6|xtreg|`). Copied from the engine's fe_group_sizes into the
    // cached reply only, like N_g above.
    if (raw.is_object() && raw.contains("fe_group_sizes") && raw["fe_group_sizes"].is_object()) {
      const ojson& gs = raw["fe_group_sizes"];
      if (gs.contains("min") && gs.contains("avg") && gs.contains("max")) {
        raw["g_min"] = gs["min"];
        raw["g_avg"] = gs["avg"];
        raw["g_max"] = gs["max"];
        // BUG-1516: e(Tbar) is the mean group size and e(Tcon) whether every panel
        // has the same size (xtreg,fe); e(k_absorb) is e(N_g), the
        // absorbed panel count (also under a multiway VCE).
        // StataBE 19.5 (30 panels of 6): e(Tbar) 6, e(Tcon) 1, e(k_absorb) 30.
        raw["Tbar"] = gs["avg"];
        if (gs["min"].is_number() && gs["max"].is_number())
          raw["Tcon"] = gs["min"].get<double>() == gs["max"].get<double>() ? 1.0 : 0.0;
        if (raw.contains("N_g")) raw["k_absorb"] = raw["N_g"];
      }
    }
    // BUG-383 #1 (off-pin sweep vs live Stata, 2026-09-01): the xtreg,fe route
    // published the underlying feols block untouched, and two of its members
    // are feols conventions, not xtreg ones. [XT] xtreg stores e(r2) as the
    // WITHIN R-squared (captured: 0.07589076642404147 — our raw r2_within
    // matches it to 1e-15), where feols's r2 is the FULL-model LSDV fit
    // (0.9905). And e(df_m) counts the absorbed group dummies alongside the
    // slopes (captured: 48 = 1 slope + 47 dummies), where feols reports the
    // slope count alone. Both raw ingredients are already in the engine reply
    // (r2_within, k_vars, k_absorbed); this route just has to pick the xtreg
    // spellings. The r2_w/r2_b/r2_o trio and the rendered header are untouched.
    // The block's df_m now mirrors e(df_m), with the header's own numerator beside
    // it (BUG-1291, below the model-based scalars).
    if (raw.contains("r2_within")) block["r2"] = raw["r2_within"];
    // FU-76: the same two members, e() side. BUG-383 wrote them into the block
    // only, while e() resolves from the cached reply, so e(r2) read the LSDV fit
    // (0.7235263265398173) and e(df_m) the slope count 2 (w2_pweight.log X0_fe
    // PVAL e(r2) +1.132199730c722X-001 = r2_within 0.5373657181682785).
    // e(df_m) follows xtreg,fe's two arms: conventional VCE posts
    // `df_m = e(df_a) + dfb` (X0_fe and X3_aw_fe: 2 + 14 = 16), a cluster VCE
    // posts regress's own model df, which the oracle shows as one less than
    // the slope count: X1, X2, X4, X5, X7, X8, X9 (cluster id, 15 clusters)
    // and X6 (cluster cl2, 5 clusters) all 1 with k = 2, X13 2 with k = 3.
    // Other VCE shapes are not witnessed and keep the engine's value. The
    // block mirrors it (BUG-1291, below).
    // BUG-1291: e(df_b), the slope count the model F tests: r(dfr) under the
    // conventional VCE, the model test's dof under a cluster VCE
    // (Stata's absorb step); StataBE 2 for `xtreg y x1 x2, fe`
    // and for vce(cluster id) (with e(df_m) 31 and 1). The test drops the
    // constraints a clipped multiway V cannot carry, so its rank (the reply's
    // vcov_rank) bounds it: StataBE [aw=w] vce(cluster cg1 cg2 cg3) e(rank) 1,
    // e(df_b) 1.
    if (raw.contains("r2_within")) raw["r2"] = raw["r2_within"];
    // BUG-1482: the clustered V's rank r(rankc) on the absorb route
    // (xtreg,fe: a cluster variable other than the panel variable, or several):
    // the multiway reply's vcov_rank, else the rank of the one-way e_V. Below the
    // slope count it is e(rank), it bounds e(df_b) and e(df_m), and the model test
    // drops a constraint, so e(F) is missing. StataBE 19.5,
    // vce(cluster c2) with 2 clusters and 2 slopes: e(rank) 1, e(df_b) 1, e(df_m) 0,
    // e(F) . (vce(cluster c2 c3), min G 2, alike).
    std::optional<double> rankc;
    if (multiwayVce) {
      if (raw.contains("vcov_rank") && raw["vcov_rank"].is_number())
        rankc = raw["vcov_rank"].get<double>();
    } else if (clusterVce) {
      for (size_t i = 0; i + 1 < cobraArgs.size(); ++i)
        if (cobraArgs[i] == "--cluster" && cobraArgs[i + 1] != *session.panelvar)
          rankc = clusteredVRankM4(raw);
    }
    if (raw.contains("k_vars") && raw["k_vars"].is_number() && raw.contains("df_a") &&
        raw["df_a"].is_number()) {
      const double k = raw["k_vars"].get<double>();
      const std::string vt = raw.value("vcov_type", std::string());
      raw["df_b"] = rankc.has_value() ? std::min(k, *rankc) : k;
      if (rankc.has_value() && *rankc < k) {
        raw["F"] = nullptr;
        raw["p_F"] = nullptr;
        block["F"] = nullptr;
        block["p_F"] = nullptr;
      }
      if (vt == "iid") {
        raw["df_m"] = k + raw["df_a"].get<double>();
      } else if (vt.rfind("cluster(", 0) == 0 && k >= 1.0) {
        raw["df_m"] = k - 1.0;
      }
    }
    aliasPanelR2M4(raw);
    // FU-29 review: a weighted fit's reply has no sigma_u/rho/corr_u_xb/
    // r2_between/r2_overall (the engine's capture_panel_means stands down under
    // weights; engine follow-up FU-126), so those e() fields read `.` where
    // Stata posts numbers (w2_pweight.log X3_aw_fe PXT :352 sigma_u
    // +1.4e5921bc56b86X+000, rho +1.b6f09048fd04cX-003, corr
    // +1.0d40c44e4227aX-001; X4-X9 and X13 alike). The fit itself matches, so
    // it is kept, and the gap is named on the block's `warnings` (rendered as
    // `note:` lines, BUG-112(8)) rather than left as a silent `.`. Only the
    // fields actually absent are named; an unweighted fit carries all five and
    // gets no note (its block stays freeze-identical).
    if (weighted && raw.is_object()) {
      static const std::vector<std::pair<const char*, const char*>> kPanelScalars = {
          {"sigma_u", "sigma_u"}, {"rho", "rho"}, {"corr", "corr_u_xb"},
          {"r2_b", "r2_b"},       {"r2_o", "r2_o"}};
      std::vector<std::string> absent;
      for (const auto& [eName, key] : kPanelScalars) {
        if (!raw.contains(key) || !raw[key].is_number()) absent.push_back(std::string("e(") + eName + ")");
      }
      if (!absent.empty()) {
        std::string list;
        for (size_t i = 0; i < absent.size(); ++i) {
          if (i > 0) list += (i + 1 == absent.size()) ? " and " : ", ";
          list += absent[i];
        }
        if (!block.contains("warnings") || !block["warnings"].is_array()) block["warnings"] = ojson::array();
        block["warnings"].push_back(list + (absent.size() == 1 ? " is" : " are") +
                                    " not computed for weighted xtreg, fe in this engine (left missing)");
      }
    }
    // FU-402: the built-in multiway e() (no e(N_clust), e(rank) of the clipped V).
    const ojson rawFit = raw;
    if (multiwayVce) applyBuiltinMultiwayReplyM4(raw);
    // The model-based scalars (Stata's absorb step with cmd xtreg_fe):
    // e(rmse) = sqrt(r(mse)) and e(r2_a) = 1 - mse/(sst_xt/(N-1)), mse = rss / r(dfe). Under any cluster VCE (one-way, vce(robust) =
    // cluster(panelvar) here, multiway; Stata's kclust > 0) the panels nest in
    // every cluster (checked above), so r(dfe) = N - rank_mb charges no absorbed
    // panel mean: StataBE (fixture w2s_multiway_class) vce(cluster cg1 cg2),
    // vce(cluster id) and vce(cluster cg1) e(rmse) 1.4571896494673038 =
    // sqrt(375.84/177) and e(r2_a) .9408651668053871, where the engine's within dof
    // (148) gave 1.5936 and .93932 -- that dof is e(sigma_e)'s (xtreg,fe).
    // Under the conventional VCE r(dfe) is that within dof, whose rmse the reply
    // carries, and e(r2_a) .929277935976713 (BUG-1042; the reply's adj_r2 is the
    // LSDV fit's .93932); e(rank) there is r(rank) = rank_mb, 3 (a cluster VCE's
    // e(rank) is the clustered V's, the slope count k_vars or the multiway
    // vcov_rank). Weighted fits follow the same rules on the reply's weighted sums
    // (StataBE [aw=w] conventional e(r2_a) .93017655599106241, vce(cluster id)
    // e(rmse) 1.4200006643786318 and .94161655529196175, [pw=w] alike; [fw=w] with
    // N = 450 .93798739005530563, and 1.4128349298420197 / .94201057951480482;
    // [aw=w] vce(cluster cg1 cg2) 1.4200006643786312 / .94161655529196187).
    // e(df_m) under the robust/cluster VCE is r(dfr) + r(dfax)
    // (Stata's absorb step, put back after the model test),
    // with r(dfr) = min(rank, rankc) - cons (Stata's absorb solver): rank the
    // model-based rank e(rank_mb) (the fitted columns, _cons included), rankc the
    // clustered V's rank e(rank) (the engine's vcov_rank), cons 1 when _cons has a
    // positive variance, and r(dfax) 0 because xtreg, fe absorbs only
    // the panel. StataBE (fixture above): rank_mb 3, rank 2 ->
    // e(df_m) 1, weighted ([aw=w] vce(cluster cg1 cg2)) too. e(df_b) is the
    // model test's dof (postENameListsXtregFeM4).
    const std::string vcovTypeFit = rawFit.value("vcov_type", std::string());
    const bool clusterFit = multiwayVce || vcovTypeFit.rfind("cluster(", 0) == 0;
    if ((clusterFit || vcovTypeFit == "iid") && raw.is_object() && raw.contains("tss_within") &&
        raw["tss_within"].is_number() && raw.contains("rss") && raw["rss"].is_number() &&
        raw.contains("n_obs") && raw["n_obs"].is_number() && raw.contains("dof_resid") &&
        raw["dof_resid"].is_number() && raw.contains("e_V") && raw["e_V"].is_object() &&
        raw["e_V"].contains("names") && raw["e_V"]["names"].is_array()) {
      const double n = raw["n_obs"].get<double>();
      const double rankMb = static_cast<double>(raw["e_V"]["names"].size());
      const double dfe = clusterFit ? n - rankMb : raw["dof_resid"].get<double>();
      const double rssV = raw["rss"].get<double>(), sstXt = raw["tss_within"].get<double>();
      if (dfe > 0 && n > 1 && sstXt > 0) {
        const double mse = rssV / dfe;
        if (clusterFit) raw["rmse"] = std::sqrt(mse);
        raw["adj_r2"] = 1.0 - mse / (sstXt / (n - 1.0));
        if (!clusterFit) raw["k_vars"] = rankMb;  // e(rank) = r(rank)
        // BUG-1482: a one-way cluster VCE's e(rank) is r(rankc) too (the multiway
        // reply's k_vars already is, applyBuiltinMultiwayReplyM4).
        if (rankc.has_value() && !multiwayVce) raw["k_vars"] = *rankc;
        if (rankc.has_value()) {
          // cons: the _cons column's variance is positive in the clustered V.
          double cons = 0.0;
          const auto& ev = raw["e_V"];
          if (ev.contains("matrix") && ev["matrix"].is_array()) {
            const auto& names = ev["names"];
            for (size_t j = 0; j < names.size(); ++j) {
              if (!names[j].is_string()) continue;
              const std::string nm = names[j].get<std::string>();
              if (nm != "_cons" && nm != "(intercept)") continue;
              const auto& mat = ev["matrix"];
              if (j < mat.size() && mat[j].is_array() && j < mat[j].size() &&
                  mat[j][j].is_number() && mat[j][j].get<double>() > 0.0)
                cons = 1.0;
            }
          }
          const double dfax = 0.0;  // xtreg, fe: only the panel is absorbed
          const double dfr = std::min(rankMb, *rankc) - cons;
          if (dfr >= 0.0) raw["df_m"] = dfr + dfax;
        }
      }
    }
    // BUG-1291 / BUG-1042: the block carries the e() scalars eststo/esttab read off
    // it (eststo_esttab.cpp escalarsFrom: e(df_m), e(r2_a), e(rmse)) as the cached
    // reply resolves them, where it had k + N_g - 1, the LSDV adj_r2 and the
    // within-dof rmse under every VCE; and e(df_b), the numerator of xtreg's header
    // F(e(df_b), e(df_r)) (xtreg,fe's display), which the console prints:
    // StataBE "F(2, 148) = 1191.52", under vce(cluster id) "F(2, 29) = 1142.39",
    // where the block's df_m printed F(31, ...).
    for (const char* key : {"df_m", "df_b", "adj_r2", "rmse"})
      if (raw.contains(key) && raw[key].is_number()) block[key] = raw[key];
    // e(cmd) is `xtreg` (w2v_margins_corpus F1_fit `cmd=xtreg|N=200`); the replay
    // machinery keeps keying on cobraCmd "feols".
    cacheLastModelM4(session, "feols", cobraArgs, block, raw, esampleValues, "xtreg");
    if (multiwayVce) postBuiltinMultiwayEM4(session, rawFit);
    // Stata's ordered `: e(scalars)' for this fit (w2s_multiway_class mw_xt_cg_scalars).
    if (multiwayVce && !weighted) postENameListsXtregFeM4(session, raw, parsed);
    else session.eNameContract.reset();
    // e(vce) after a default-VCE `xtreg, fe` is `conventional` (w2_pweight.log
    // X0_fe PSTR `120|103|.|conventional|||||`; X3_aw_fe alike). setLastModel()
    // published lm.vcov's engine spelling `iid`, which the replay machinery keys
    // on, so only the macro store is corrected. vce(robust)/cluster keep
    // `cluster` (X1/X2 PSTR). Same KNOWN GAP as runRegress's `ols`: a later
    // setLastModel() on this model (setEstHandleM4) republishes `iid`.
    if (session.lastModel().has_value() && session.lastModel()->vcov == "iid") {
      session.eval.setEString("vce", "conventional");
    }
    // BUG-1516: e(model) "fe" (xtreg,fe; StataBE 19.5 i17.do).
    if (session.lastModel().has_value()) putEStringNL(session, "model", "fe");
    return block;
  }

  // Random effects.
  // BUG-657: same `_xt, i()` refusal as the fixed-effects arm above (xtreg.ado
  // calls it before choosing the model), r(459).
  if (!session.panelvar.has_value() || session.panelvar->empty()) {
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  }
  // FU-29: xtreg,re's `syntax` declares no weight, so any weight type is
  // Stata's r(101) "weights not allowed" (w2_pweight.log X12_pw_re). Until the
  // registry row opened to AllWeights the tokenizer refused it as r(198).
  if (parsed.weightType.has_value() && parsed.weightVar.has_value() &&
      !parsed.weightVar->empty()) {
    return ojson(eval::errorBlock("engine_error", "weights not allowed", std::nullopt, std::nullopt,
                                  std::nullopt, "r(101)"));
  }
  for (const auto& [k, v] : parsed.opts) {
    // BUG-055/P2 side effect: legacy `cluster(v)` (no vce()/robust) now
    // maps to `vce(cluster v)` inside vcovFromOpts (shared by this RE
    // path), so it must be caught HERE too — otherwise it would sail past
    // this guard and reach cobra's RE call as an unsupported --vcov/
    // --cluster flag pair instead of getting this same clean message.
    if ((k == "vce" || k == "vcov" || k == "robust" || k == "cluster") &&
        !(v.isBool && !v.boolValue)) {
      return engineErrorBlockM4("cobra xtreg (random-effects) does not support vcov options.",
                                "unsupported_option");
    }
  }
  EstimationArgvResult built = buildEstimationArgvM4("xtreg", parsed, session);
  if (built.errored) return built.errorBlock;
  std::vector<std::string> cobraArgs = built.argv;
  cobraArgs.push_back("--ivar");
  cobraArgs.push_back(*session.panelvar);
  cobraArgs.push_back("--emit-v");  // BUG-1196: e(V), joined by cacheLastModelM4
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(cobraArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, "xtreg", &session);
  aliasPanelR2M4(raw);
  // BUG-1516: xtreg,re posts e(Tbar), the harmonic mean of the panel sizes
  // (the reply's T_h), e(Tcon) whether they are all equal, e(N_g), and e(rank)
  // the rank of the full-rank V (every coefficient, _cons included). StataBE 19.5
  // (30 panels of 6): e(Tbar) 6, e(Tcon) 1, e(rank) 3. Cached reply only.
  if (raw.is_object()) {
    if (raw.contains("T_h") && raw["T_h"].is_number()) raw["Tbar"] = raw["T_h"];
    if (raw.contains("g_min") && raw["g_min"].is_number() && raw.contains("g_max") &&
        raw["g_max"].is_number())
      raw["Tcon"] = raw["g_min"].get<double>() == raw["g_max"].get<double>() ? 1.0 : 0.0;
    if (raw.contains("n_groups") && raw["n_groups"].is_number()) raw["N_g"] = raw["n_groups"];
    if (raw.contains("e_V") && raw["e_V"].is_object() && raw["e_V"].contains("names") &&
        raw["e_V"]["names"].is_array())
      raw["k_vars"] = raw["e_V"]["names"].size();
  }
  cacheLastModelM4(session, "xtreg", cobraArgs, block, raw);
  if (session.lastModel().has_value()) putEStringNL(session, "model", "re");
  return block;
}

// ===========================================================================
// P7 — ordered / multinomial / zero-inflated / fractional MLE
//
// These engine verbs take `--vce iid|robust|cluster --cluster C` (the ML family
// spelling), NOT buildEstimationArgvM4's `--vcov`, so they share a small builder
// of their own rather than bending the shared one and risking the frozen output
// of every existing caller.


}  // namespace piet::handlers
