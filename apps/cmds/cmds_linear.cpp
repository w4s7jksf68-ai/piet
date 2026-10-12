#include "common.h"
#include "commands.h"

#include "estimators/ols.h"
#include "estimators/liml.h"
#include "estimators/reg3.h"
#include "inference/wildboot.h"
// feols.h/iv2sls.h/fepois.h: cmd_feols/cmd_iv/cmd_fepois below only call the
// shared run_feols/run_iv/run_fepois(_multi) wrappers (apps/cmds/common.cpp),
// which return plain json — these three headers are not strictly required for
// THIS file to compile, but are included defensively since they're the natural
// estimator family for ols/iv/feols/fepois/liml/wildboot/reg3.
#include "estimators/feols.h"
#include "estimators/iv2sls.h"
#include "estimators/fepois.h"

namespace cecon {
namespace cli {

int cmd_ols(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), yname = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || yname.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra ols --data f.hbin --y Y --x X1,X2 [--no-intercept] [--out f.json]\n");
        return 2;
    }
    bool intercept = !has(a, "--no-intercept");
    // e(sample) T1a: build_design keeps its listwise row set private (core/matrix.cpp),
    // so there is no admitted-row list to emit. Refuse by name rather than write a
    // mask recomputed outside the fit; feols fits the same regression and emits it.
    if (esample_request(a, "ols").active())
        throw EngineError("unsupported",
            "ols: --esample-out is not supported (the design builder does not expose its "
            "kept rows); use feols, which fits the same model and emits them",
            "--esample-out");
    const Dataset& ds = acquire_data(data);
    DesignMatrix dm = build_design(ds, yname, split_csv(xcsv), intercept);
    OlsResult r = ols_fit(dm);
    json coef, sec, seh, th;
    for (int j = 0; j < r.k; ++j) {
        coef[r.names[j]] = r.beta[j];
        sec[r.names[j]] = r.se_classic[j];
        seh[r.names[j]] = r.se_hc1[j];
        th[r.names[j]] = tstat(r.beta[j], r.se_hc1[j]);
    }
    json out = {{"estimator", "ols"},  {"depvar", yname}, {"n_obs", r.n},
                {"n_params", r.k},     {"n_dropped", r.n_dropped},
                {"r2", r.r2},          {"sigma2", r.sigma2},
                {"coef", coef},        {"se_classic", sec},
                {"se_hc1", seh},       {"tstat_hc1", th}};
    // p-values + 95% CI use HC1 SE and residual dof (Stata regress, robust).
    add_inference(out, r.names, r.beta, r.se_hc1, static_cast<double>(r.n - r.k));
    add_omitted(out, r.omitted, r.full_names);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_iv(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y");
    std::string endog = opt(a, "--endog"), z = opt(a, "--instruments", opt(a, "--z"));
    if (data_missing(data) || y.empty() || endog.empty() || z.empty()) {
        std::fprintf(stderr,
            "usage: cobra iv --data f.hbin --y Y[,Y2,...] --endog D --instruments Z1,Z2\n"
            "                [--x EXOG] [--fe F1,F2] [--vcov iid|hc1|cluster|twoway] [--cluster C]\n"
            "                [--gmm] [--small] [--keep-singletons] [--keepif EXPR] [--out f.json]\n"
            "                [--savefirst [--savefprefix P]]\n"
            "                [--esample-out PATH [--esample-index COL --esample-n N]]\n"
            "  --gmm        efficient two-step GMM (Stata `ivregress gmm`): robust weight matrix +\n"
            "               robust SEs by default; over-identified GMM differs from 2SLS. With\n"
            "               --vcov cluster, uses the cluster-robust GMM weight matrix + SEs.\n"
            "  --small      Stata `ivregress ..., small`: restores the N-K small-sample sigma2 /\n"
            "               HC1 / cluster factors and t/F inference. Default (no --small) matches\n"
            "               `ivregress`'s own default: asymptotic sigma2=RSS/N, HC0 robust SEs, no\n"
            "               cluster small-sample factor, and z/chi2 inference. Ignored with --gmm\n"
            "               (GMM's SEs are always the asymptotic sandwich).\n"
            "  --y          one outcome, or a comma list to batch several outcomes that share the\n"
            "               SAME endog/instruments/FE design (FE absorption + first-stage done once).\n"
            "  --endog      comma list of endogenous regressors.\n"
            "  --instruments (alias --z) excluded instruments; need >= #endog (over-id => Hansen J).\n"
            "  --x          exogenous controls; supports i(col)/i.col factor expansion and the Stata\n"
            "               factor operators ib#. / ibn. / i(numlist). / o. (e.g. --x \"ib2.tb,rh\").\n"
            "  --fe         high-dimensional fixed effects to absorb (FWL within transform).\n"
            "  --vcov       iid (default) | hc1 | cluster (needs --cluster) | twoway.\n"
            "  --savefirst  emit each endogenous regressor's FIRST-STAGE OLS (ivreg2 `savefirst`),\n"
            "               so a consumer can register it as a stored estimate: JSON key `first_stage`\n"
            "               (keyed by endogenous regressor) plus `firsteqs` (the ivreg2 equation names).\n"
            "               The stage is `regress ENDOG EXCLUDED_IVS EXOG` under the PARENT's vce with\n"
            "               the small-sample correction ALWAYS on, so --small does not move it.\n"
            "  --savefprefix P  prefix for those names (ivreg2 savefprefix()); default `_ivreg2_`.\n"
            "  --esample-out PATH  write e(sample): one 1/0 line per --data row (before --keepif),\n"
            "               1 = the row entered the fit; the JSON gains esample_n (# of 1s).\n"
            "               --esample-index COL --esample-n N: COL is each row's 1-based position\n"
            "               in an N-row frame; the file then has N lines, scattered through COL.\n"
            "  Diagnostics: first_stage_F[_iid] (homoskedastic), kp_wald_F (robust/cluster, e(widstat)),\n"
            "               eff_F (Montiel-Olea-Pflueger), kp_lm (under-id), hansen_j (over-id).\n"
            "  NOTE: iv DROPS singleton FE groups by default (matching feols/fepois/reghdfe/ivreghdfe;\n"
            "        Correia 2015). Pass --keep-singletons to retain them (changes N / residual DoF /\n"
            "        clustered SEs).\n"
            "  Example: cobra iv --data p.hbin --y lncardio,lnresp --endog lnpm --instruments q1,q2,q3 \\\n"
            "                    --x \"i(tbin),rh\" --fe sgg,ym --vcov cluster --cluster sgg\n");
        return 2;
    }
    std::vector<std::string> clusters = split_csv(opt(a, "--cluster"));
    std::vector<std::string> xs = split_csv(opt(a, "--x"));
    std::vector<std::string> endogs = split_csv(endog);
    std::vector<std::string> insts = split_csv(z);
    std::vector<std::string> ys = split_csv(y);
    bool gmm = has(a, "--gmm");
    bool small = has(a, "--small");  // Stata `ivregress ..., small`; default = asymptotic
    // ivreg2 `savefirst [savefprefix(P)]`. The prefix is data, not a flag: an empty
    // one is the DOCUMENTED default `_ivreg2_`, so it is passed through as "" and
    // resolved where the name is minted (ivreg2_first_eqname in common.cpp).
    bool savefirst = has(a, "--savefirst");
    std::string savefprefix = opt(a, "--savefprefix");
    if (!savefprefix.empty() && !savefirst)
        throw EngineError("bad_input", "iv: --savefprefix requires --savefirst");
    bool drop_singleton = !has(a, "--keep-singletons");  // default drop, matches feols/ivreghdfe
    // Stata `ivregress gmm` defaults to a robust weight matrix + robust SEs. When the
    // user asks for --gmm without an explicit --vcov, default to hc1 so the SEs match.
    std::string vcov = opt(a, "--vcov", gmm ? "hc1" : "iid");
    cecon::TsMeta tsm = parse_tsmeta(a);
    const EsampleRequest esreq = esample_request(a, "iv");
    EsampleRows esrows;
    EsampleRows* es_p = esreq.active() ? &esrows : nullptr;
    // E1: --x / --endog / --instruments may all carry L./F./D./S. tokens (see
    // build_iv_model's ts_prepass calls) — route `if` through plan_keepif so they
    // materialize against the FULL series before the sample filter applies.
    const std::string keepif = opt(a, "--keepif");
    KeepifPlan kp = plan_keepif(data, keepif, &tsm, {&xs, &endogs, &insts});
    const std::vector<char>* mask_p = kp.active ? &kp.mask : nullptr;
    // --emit-v: opt-in full e(V) of the fitted model (see add_e_v). A batch has one V per
    // outcome and no per-outcome slot for it, so it is refused rather than ignored.
    const bool emit_v = has(a, "--emit-v");
    if (emit_v && ys.size() > 1)
        throw EngineError("unsupported",
            "iv: --emit-v is not supported with a multi-outcome --y batch; run one outcome per invocation");
    json out;
    if (ys.size() > 1)
        out = run_iv_multi(kp.data, ys, xs, endogs, insts, split_csv(opt(a, "--fe")), vcov,
                           clusters, gmm, &tsm, drop_singleton, mask_p, small, savefirst,
                           savefprefix, es_p);
    else
        out = run_iv(kp.data, y, xs, endogs, insts, split_csv(opt(a, "--fe")), vcov, clusters,
                     gmm, &tsm, drop_singleton, mask_p, small, savefirst, savefprefix, es_p,
                     emit_v);
    emit_esample(esreq, "iv", data, keepif, kp.active, esrows, out);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_feols(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra feols --data f.hbin --y Y[,Y2,...] --x X1,X2 "
                             "[--fe F1,F2] [--vcov iid|hc1|cluster] [--cluster C] "
                             "[--no-intercept] [--level P] [--weight W --wtype aw|fw|iw|pw] "
                             "[--keepif EXPR] [--esample-out PATH [--esample-index COL --esample-n N]] "
                             "[--out f.json]\n"
                             "  --x terms: plain numeric, i(col)/i.col factor indicators (base=lowest dropped),\n"
                             "    c(col) continuous, and interactions joined by # / ## (Stata factor vars):\n"
                             "      i(treat)##i(post)  DID main effects + cross   c(x)##c(x)  x and x^2\n"
                             "      i(g)##c(x)  group slopes      i(a)#i(b)  interaction only (full cross - base cell)\n"
                             "  Stata factor operators (parity):\n"
                             "      ib#.col   custom base level # (e.g. ib2.rep78); i may be omitted (b2.rep78)\n"
                             "      ibn.col   no base requested (last level auto-dropped for collinearity)\n"
                             "      i(2/4).col / i(2 4).col  only those levels (lowest selected = base);\n"
                             "                obs OUTSIDE the set fold into the reference group (NOT dropped)\n"
                             "      o.col     omit operator: contributes no columns/dof (\"controlled for\" label)\n"
                             "    (cells must be populated; empty-cell unbalanced factor x factor may differ from Stata.)\n"
                             "  --no-intercept  Stata `regress ..., noconstant`; rejected together with --fe\n"
                             "                  (fixed effects already absorb the constant).\n"
                             "  --level P       confidence level for ci_lo/ci_hi, 10 < P <= 99.99 (default 95).\n"
                             "  --weight W --wtype T  Stata estimation weights [aw=]/[fw=]/[iw=]/[pw=].\n"
                             "                  aw/pw report N = rows; fw/iw report N = sum(W). pw additionally\n"
                             "                  makes --vcov hc1/cluster the linearized (survey) sandwich.\n"
                             "                  With --fe (aw/fw/pw only) the cells are demeaned with the weights.\n"
                             "                  pw requires --vcov hc1, cluster or twoway (Stata has no iid VCE\n"
                             "                  under pweight).\n"
                             "  --esample-out PATH  write e(sample): one 1/0 line per --data row (before\n"
                             "                  --keepif), 1 = the row entered the fit (after listwise, weight,\n"
                             "                  cluster, TS-operator and singleton losses); the JSON gains\n"
                             "                  esample_n = # of 1s (== n_obs; == n_rows under fw/iw).\n"
                             "  --esample-index COL --esample-n N  COL is each loaded row's 1-based position\n"
                             "                  in an N-row frame; the file then has N lines, scattered via COL.\n"
                             "  --keep-nested-fe-dof  Stata `areg` clustered dof: the cluster small-sample\n"
                             "                  factor charges every absorbed FE, even one nested in the\n"
                             "                  cluster (default: reghdfe/xtreg,fe exempt it). Needs --fe and\n"
                             "                  --vcov cluster|twoway.\n");
        return 2;
    }
    std::string vcov_type = opt(a, "--vcov", "iid");
    std::vector<std::string> clusters = split_csv(opt(a, "--cluster"));
    std::vector<std::string> xs = split_csv(xcsv);
    std::vector<std::string> fes = split_csv(opt(a, "--fe"));
    bool drop_singleton = !has(a, "--keep-singletons");
    bool no_intercept = has(a, "--no-intercept");
    if (no_intercept && !fes.empty()) {
        std::fprintf(stderr, "feols: --no-intercept is not valid with --fe "
                             "(fixed effects already absorb the constant).\n");
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "feols: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    // Stata weights. --wtype names the KIND, which is the whole difference between
    // them (see WeightKind in core/model.h); an unrecognized spelling is rejected
    // rather than silently treated as one of the four, because picking the wrong
    // kind changes N, the dof and every SE without changing β — the exact shape of
    // a silently wrong answer.
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "feols: --wtype requires --weight");
        if (wtype.empty() || wtype == "aw" || wtype == "aweight") wkind = cecon::WeightKind::Analytic;
        else if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else
            throw EngineError("bad_input",
                "feols: --wtype must be one of aw|fw|iw|pw (got '" + wtype + "')");
        // Weighted absorption is implemented (feols_fit demeans with the weights).
        // pweight has no iid VCE in Stata: anything run_feols would fit as iid (the
        // default, or a spelling it does not map to a sandwich) is refused rather
        // than returning a conventional SE Stata never reports -- with or without
        // --fe. The no-FE half closes the silent-IID class at the engine boundary:
        // PIET's regress (runRegress) and the shared M4 builder push `--vcov hc1`
        // for a bare pweight (BUG-536, w2_pweight.log R2_pw == R3), and no PIET
        // post-estimation replay forwards `--wtype pw` to feols without the fit's
        // vcov, so the only caller left fitting pw as iid was a silent wrong answer.
        // `--svy` is exempt: it replaces the VCE entirely (and refuses --vcov).
        // iweight + --fe stays refused: Stata's absorbing estimators do not take
        // iweights (w2_pweight.log X11_iw_fe: xtreg [iweight=], fe is r(101)), so
        // fitting one would answer a model Stata refuses.
        if (wkind == cecon::WeightKind::Importance && !fes.empty())
            throw EngineError("bad_input",
                "feols: [iweight=] is not allowed with --fe (Stata's areg/reghdfe/xtreg,fe "
                "take aweight, fweight and pweight only)");
        const bool sandwich = vcov_type == "hc1" || vcov_type == "HC1" ||
                              vcov_type == "cluster" || vcov_type == "twoway" ||
                              vcov_type == "multiway";
        if (wkind == cecon::WeightKind::Probability && !sandwich && !has(a, "--svy"))
            throw EngineError("bad_input",
                "feols: [pweight=] requires --vcov hc1, cluster or twoway; Stata has no "
                "iid VCE under pweight");
    }
    // BUG-568: Stata areg's clustered dof. areg charges every absorbed parameter in
    // the cluster (N-1)/(N-K) factor, including an FE nested in the cluster, where
    // reghdfe and xtreg,fe (this command's default) exempt it: w2b_estim.log
    // areg_a_cl se[x1] .0722686736 vs reghdfe_cluster_ctl .0663182832 on the same
    // fit. Only meaningful with a cluster VCE and absorbed FE; anywhere else the
    // flag would be silently ignored, so it is refused instead.
    const bool keep_nested_fe_dof = has(a, "--keep-nested-fe-dof");
    if (keep_nested_fe_dof) {
        const bool cluster_vce = vcov_type == "cluster" || vcov_type == "twoway" ||
                                 vcov_type == "multiway";
        if (!cluster_vce || fes.empty())
            throw EngineError("bad_input",
                "feols: --keep-nested-fe-dof requires --fe and --vcov cluster or twoway (it "
                "selects which absorbed parameters the cluster small-sample factor charges)");
    }
    cecon::TsMeta tsm = parse_tsmeta(a);
    std::vector<std::string> ys = split_csv(y);
    // --emit-v: opt-in full e(V) of the fitted model (see add_e_v). The svy and the
    // multi-outcome paths have no e_V slot, so they refuse it rather than ignore it.
    const bool emit_v = has(a, "--emit-v");
    if (emit_v && (has(a, "--svy") || ys.size() > 1))
        throw EngineError("unsupported",
            "feols: --emit-v is not supported with --svy or a multi-outcome --y batch");
    const EsampleRequest esreq = esample_request(a, "feols");
    EsampleRows esrows;
    EsampleRows* es_p = esreq.active() ? &esrows : nullptr;
    // E1: --x may carry L./F./D./S. tokens (build_model's ts_prepass) — route `if`
    // through plan_keepif so they materialize against the FULL series first.
    const std::string keepif = opt(a, "--keepif");
    KeepifPlan kp = plan_keepif(data, keepif, &tsm, {&xs});
    const std::vector<char>* mask_p = kp.active ? &kp.mask : nullptr;

    // P7 `svy: regress`. A survey design is not a variant of --vcov: it replaces
    // both the meat and the reference distribution, so it takes its own path and
    // rejects the combinations that would otherwise be silently ignored.
    if (has(a, "--svy")) {
        std::string psu = opt(a, "--psu"), strata = opt(a, "--strata");
        if (psu.empty() || strata.empty())
            throw EngineError("bad_input", "feols: --svy requires both --psu and --strata");
        if (wkind != cecon::WeightKind::Probability)
            throw EngineError("bad_input",
                "feols: --svy requires --weight W --wtype pw (a survey design is a "
                "probability-weighted sample)");
        if (!fes.empty())
            throw EngineError("unsupported", "feols: --svy is not supported together with --fe");
        if (ys.size() > 1)
            throw EngineError("unsupported",
                "feols: --svy is not supported with a multi-outcome --y batch");
        if (has(a, "--vcov") || !clusters.empty())
            throw EngineError("bad_input",
                "feols: --svy supplies its own linearized variance; --vcov/--cluster are "
                "not accepted with it");
        if (no_intercept)
            throw EngineError("unsupported", "feols: --svy is not supported with --no-intercept");
        json svy_out = run_feols_svy(kp.data, y, xs, weight_var, psu, strata, &tsm, mask_p, level,
                                     es_p);
        emit_esample(esreq, "feols", data, keepif, kp.active, esrows, svy_out);
        emit(svy_out, opt(a, "--out"));
        return 0;
    }

    json out;
    if (ys.size() > 1) {
        // The multi-outcome batch shares one absorbed design across outcomes and has
        // no weight plumbing. Rejecting is mandatory, not tidiness: falling through
        // would run the batch UNweighted and return numbers that look right.
        if (wkind != cecon::WeightKind::None)
            throw EngineError("unsupported",
                "feols: weights are not supported with a multi-outcome --y batch; "
                "run one outcome per invocation");
        // Same reason: the batch has no keep_nested_fe_dof plumbing, so it would
        // answer with the reghdfe dof under an areg request.
        if (keep_nested_fe_dof)
            throw EngineError("unsupported",
                "feols: --keep-nested-fe-dof is not supported with a multi-outcome --y "
                "batch; run one outcome per invocation");
        out = run_feols_multi(kp.data, ys, xs, fes, vcov_type, clusters, drop_singleton,
                              &tsm, mask_p, no_intercept, level, level_set, es_p);
    } else {
        out = run_feols(kp.data, y, xs, fes, vcov_type, clusters, drop_singleton, &tsm,
                        mask_p, no_intercept, level, level_set, weight_var, wkind, es_p,
                        keep_nested_fe_dof, emit_v);
    }
    emit_esample(esreq, "feols", data, keepif, kp.active, esrows, out);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_fepois(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra fepois --data f.hbin --y COUNT[,C2,...] --x X1,X2 "
                             "[--fe F] [--offset O] [--vcov iid|hc1|cluster|twoway] [--cluster C] "
                             "[--weight W --wtype pw] [--keepif EXPR]\n"
                             "  --weight W --wtype pw  Stata pweight (P5): forces --vcov hc1/robust or\n"
                             "                  cluster (Stata has no iid VCE under pweight); not valid\n"
                             "                  with --fe (weighted absorption is not implemented).\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    bool drop_zero_fe = !has(a, "--keep-zero-fe");  // default: drop all-zero FE cells (ppmlhdfe)
    bool scale_x2 = (opt(a, "--scale") == "x2");    // glm quasi-Poisson dispersion-scaled SEs
    std::string vcov_type = opt(a, "--vcov", "iid");
    // Stata pweight: only pweight is meaningful for `poisson`/`fepois` as an ML
    // command (aw/iw are not valid Stata weight types there; fw would need
    // integer-frequency semantics this engine doesn't implement for fepois), and
    // pweight always forces a sandwich VCE — Stata has no iid pweight estimator.
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "fepois: --wtype requires --weight");
        if (wtype != "pw" && wtype != "pweight")
            throw EngineError("bad_input", "fepois: --wtype must be pw (only pweight is supported)");
        wkind = cecon::WeightKind::Probability;
        if (vcov_type == "iid")
            throw EngineError("bad_input",
                "fepois: [pweight=] requires --vcov hc1/robust or cluster; Stata has no "
                "iid VCE under pweight");
        if (!split_csv(opt(a, "--fe")).empty())
            throw EngineError("unsupported",
                "fepois: weights are not supported together with --fe "
                "(weighted fixed-effect absorption is not implemented in this engine)");
    }
    std::vector<std::string> ys = split_csv(y);
    json out;
    if (ys.size() > 1) {
        // Mirrors feols_multi's weight rejection: the shared design across
        // outcomes has no weight plumbing, so silently running unweighted would
        // return numbers that look right but aren't.
        if (wkind != cecon::WeightKind::None)
            throw EngineError("unsupported",
                "fepois: weights are not supported with a multi-outcome --y batch; "
                "run one outcome per invocation");
        out = run_fepois_multi(data, ys, split_csv(xcsv), split_csv(opt(a, "--fe")),
                               opt(a, "--offset"), vcov_type,
                               split_csv(opt(a, "--cluster")), drop_zero_fe, scale_x2);
    } else {
        out = run_fepois(data, y, split_csv(xcsv), split_csv(opt(a, "--fe")),
                         opt(a, "--offset"), vcov_type,
                         split_csv(opt(a, "--cluster")), drop_zero_fe, scale_x2,
                         weight_var, wkind);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_wildboot(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y");
    std::string clcsv = opt(a, "--cluster"), endog = opt(a, "--endog");
    if (data_missing(data) || y.empty() || clcsv.empty()) {
        std::fprintf(stderr, "usage:\n"
            "  OLS: cobra wildboot --data f.hbin --y Y --x X1,X2 --param X1 --cluster C "
            "[--fe F] [--null 0] [--reps 9999] [--seed 42] [--weights rademacher|webb]\n"
            "  IV:  cobra wildboot --data f.hbin --y Y --x EXOG --endog D --instruments Z "
            "--cluster C [--fe F] [--null 0] [--reps 9999] [--seed 42]\n");
        return 2;
    }
    double r = std::stod(opt(a, "--null", "0"));
    int reps = std::stoi(opt(a, "--reps", "9999"));
    if (reps < 1) { std::fprintf(stderr, "wildboot: --reps must be >= 1.\n"); return 2; }
    uint64_t seed = std::stoull(opt(a, "--seed", "42"));
    BootWeights wt = (opt(a, "--weights") == "webb") ? BootWeights::Webb : BootWeights::Rademacher;
    const Dataset& ds = acquire_data(data);

    auto t0 = std::chrono::steady_clock::now();
    WildBootResult R;
    int64_t n_obs = 0;
    std::vector<std::string> fv_warnings;  // BUG-112(8): md is branch-scoped, hoist the notes
    bool is_iv = !endog.empty();
    if (is_iv) {
        IvModelData md = build_iv_model(ds, y, split_csv(opt(a, "--x")), split_csv(endog),
                                        split_csv(opt(a, "--instruments", opt(a, "--z"))),
                                        split_csv(opt(a, "--fe")), split_csv(clcsv));
        n_obs = md.n;
        fv_warnings = md.warnings;
        R = wild_cluster_bootstrap_iv(md, r, reps, seed, 0, wt);
    } else {
        std::string param = opt(a, "--param"), xcsv = opt(a, "--x");
        if (param.empty() || xcsv.empty()) {
            std::fprintf(stderr, "OLS wildboot needs --x and --param\n");
            return 2;
        }
        ModelData md = build_model(ds, y, split_csv(xcsv), split_csv(opt(a, "--fe")),
                                   split_csv(clcsv));
        int j = -1;
        for (int i = 0; i < static_cast<int>(md.dm.xnames.size()); ++i)
            if (md.dm.xnames[i] == param) j = i;
        if (j < 0) {
            std::fprintf(stderr, "param '%s' not among regressors\n", param.c_str());
            return 2;
        }
        n_obs = md.dm.n;
        fv_warnings = md.warnings;
        R = wild_cluster_bootstrap(md, j, r, reps, seed, 0, wt);
    }
    auto t1 = std::chrono::steady_clock::now();

    json out = {
        {"estimator", "wild_cluster_bootstrap"},
        {"param", R.param},
        {"null_beta", R.null_beta},
        {"beta", R.beta},
        {"se", R.se},
        {"t0", R.t0},
        {"p_symmetric", R.p_symmetric},
        {"p_equaltail", R.p_equaltail},
        {"reps", R.reps},
        {"weights", R.weights},
        {"webb_fallback", R.webb_fallback},
        {"n_clusters", R.n_clusters},
        {"n_obs", n_obs},
        {"seed", R.seed},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    add_warnings(out, fv_warnings);  // BUG-112(8): absent-key when empty
    emit(out, opt(a, "--out"));
    return 0;
}

// liml — LIML / Fuller k-class IV estimator (Stata ivregress liml).
int cmd_liml(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), endog = opt(a, "--endog");
    std::string inst = opt(a, "--instruments"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || endog.empty() || inst.empty()) {
        std::fprintf(stderr, "usage: cobra liml --data f.hbin --y Y --endog D1[,D2] --instruments Z1,Z2 "
                             "[--x EXOG] [--fuller A] [--small] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    bool small = has(a, "--small");
    double fuller = std::stod(opt(a, "--fuller", "0"));
    const Dataset& ds = acquire_data(data);
    auto endogs = split_csv(endog), insts = split_csv(inst), exogs = split_csv(xcsv);
    int p = static_cast<int>(endogs.size()), m = static_cast<int>(insts.size());
    int kx1 = static_cast<int>(exogs.size()) + 1;  // +constant
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "liml", y);
    std::vector<const double*> Yr, Xr, Zr, all = {yraw};
    for (auto& s : endogs) { Yr.push_back(require_num(ds, "liml", s)); all.push_back(Yr.back()); }
    for (auto& s : exogs)  { Xr.push_back(require_num(ds, "liml", s)); all.push_back(Xr.back()); }
    for (auto& s : insts)  { Zr.push_back(require_num(ds, "liml", s)); all.push_back(Zr.back()); }
    auto keep = listwise_keep(all, n_raw);
    int64_t n = static_cast<int64_t>(keep.size());
    if (n == 0) { std::fprintf(stderr, "liml: no rows after dropping missing.\n"); return 1; }
    auto gv = [&](const double* p2) { return gather_rows(p2, keep); };
    std::vector<double> yv = gv(yraw);
    std::vector<double> Y(static_cast<size_t>(n) * p), X1(static_cast<size_t>(n) * kx1), Z2(static_cast<size_t>(n) * m);
    for (int c = 0; c < p; ++c) { auto col = gv(Yr[c]); std::copy(col.begin(), col.end(), Y.data() + static_cast<int64_t>(c) * n); }
    for (int c = 0; c + 1 < kx1; ++c) { auto col = gv(Xr[c]); std::copy(col.begin(), col.end(), X1.data() + static_cast<int64_t>(c) * n); }
    for (int64_t i = 0; i < n; ++i) X1[static_cast<int64_t>(kx1 - 1) * n + i] = 1.0;
    for (int c = 0; c < m; ++c) { auto col = gv(Zr[c]); std::copy(col.begin(), col.end(), Z2.data() + static_cast<int64_t>(c) * n); }

    LimlResult r = liml_fit(yv, n, Y, p, X1, kx1, Z2, m, small, fuller);
    if (!r.ok) { std::fprintf(stderr, "liml: estimation failed (underidentified / singular?).\n"); return 1; }
    std::vector<std::string> names = endogs;
    for (auto& s : exogs) names.push_back(s);
    names.push_back("(intercept)");
    double dof = r.dof_r, tc = (small && dof > 0) ? t_critical(0.05, dof) : 1.959963984540054;
    json coefs = json::array();
    for (size_t j = 0; j < names.size(); ++j) {
        double se = r.se[j], t = (se > 0) ? r.beta[j] / se : 0.0;
        double pval = small ? ((se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0)
                            : z_twosided_pvalue(t);
        coefs.push_back({{"name", names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t}, {"p", pval},
                         {"ci_lo", r.beta[j] - tc * se}, {"ci_hi", r.beta[j] + tc * se}});
    }
    json out = {{"estimator", fuller > 0 ? "fuller" : "liml"}, {"kappa", r.kappa},
                {"fuller_alpha", fuller}, {"n_obs", r.n}, {"df_r", dof}, {"sigma2", r.sigma2},
                {"small", small}, {"coefs", coefs}};
    emit(out, opt(a, "--out"));
    return 0;
}

// reg3 — systems estimation (SUR / 3SLS). Repeatable --eq "dep=rhs1,rhs2".
int cmd_reg3(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), method = opt(a, "--method", "sure");
    std::vector<std::string> eqspecs = opt_all(a, "--eq");
    if (data_missing(data) || eqspecs.size() < 2) {
        std::fprintf(stderr, "usage: cobra reg3 --data f.hbin --eq \"y1=x1,x2\" --eq \"y2=x3\" "
                             "[--method sure|3sls] [--exog Z1,Z2] [--keepif EXPR] [--out f.json]\n"
                             "  SUR (default) or 3SLS (RHS vars that are also a depvar are endogenous).\n");
        return 2;
    }
    bool threesls = (method == "3sls");
    // parse equations
    std::vector<std::string> deps;
    std::vector<std::vector<std::string>> rhs;
    for (auto& s : eqspecs) {
        auto eq = s.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "reg3: bad --eq '%s'\n", s.c_str()); return 2; }
        deps.push_back(s.substr(0, eq));
        rhs.push_back(split_csv(s.substr(eq + 1)));
    }
    int M = static_cast<int>(deps.size());
    auto is_dep = [&](const std::string& v) { for (auto& d : deps) if (d == v) return true; return false; };
    // instrument set = exogenous RHS (not a depvar) + --exog extras + constant
    std::vector<std::string> instv;
    auto add_inst = [&](const std::string& v) { for (auto& x : instv) if (x == v) return; instv.push_back(v); };
    for (auto& rl : rhs) for (auto& v : rl) if (!is_dep(v)) add_inst(v);
    for (auto& v : split_csv(opt(a, "--exog"))) if (!v.empty()) add_inst(v);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    // gather all variables for a joint listwise sample
    std::vector<const double*> must;
    for (auto& d : deps) must.push_back(require_num(ds, "reg3", d));
    for (auto& rl : rhs) for (auto& v : rl) must.push_back(require_num(ds, "reg3", v));
    for (auto& v : instv) must.push_back(require_num(ds, "reg3", v));
    auto keep = listwise_keep(must, n_raw);
    int64_t n = static_cast<int64_t>(keep.size());
    if (n == 0) { std::fprintf(stderr, "reg3: no rows after dropping missing.\n"); return 1; }
    auto getcol = [&](const std::string& v) { return gather_rows(require_num(ds, "reg3", v), keep); };

    std::vector<Reg3Eq> eqs(M);
    for (int i = 0; i < M; ++i) {
        int k = static_cast<int>(rhs[i].size()) + 1;
        eqs[i].k = k;
        eqs[i].depname = deps[i];
        eqs[i].y = getcol(deps[i]);
        eqs[i].Z.assign(static_cast<size_t>(n) * k, 0.0);
        eqs[i].endog.assign(k, 0);
        for (int c = 0; c + 1 < k; ++c) {
            auto col = getcol(rhs[i][c]);
            std::copy(col.begin(), col.end(), eqs[i].Z.data() + static_cast<int64_t>(c) * n);
            eqs[i].names.push_back(rhs[i][c]);
            if (threesls && is_dep(rhs[i][c])) eqs[i].endog[c] = 1;
        }
        for (int64_t t = 0; t < n; ++t) eqs[i].Z[static_cast<int64_t>(k - 1) * n + t] = 1.0;
        eqs[i].names.push_back("(intercept)");
    }
    int kw = static_cast<int>(instv.size()) + 1;
    std::vector<double> W(static_cast<size_t>(n) * kw, 0.0);
    for (int c = 0; c + 1 < kw; ++c) { auto col = getcol(instv[c]); std::copy(col.begin(), col.end(), W.data() + static_cast<int64_t>(c) * n); }
    for (int64_t t = 0; t < n; ++t) W[static_cast<int64_t>(kw - 1) * n + t] = 1.0;

    Reg3Result r = reg3_fit(eqs, n, W, kw);
    if (!r.ok) { std::fprintf(stderr, "reg3: estimation failed (collinear / underidentified?).\n"); return 1; }
    json eqsj = json::array();
    for (int i = 0; i < M; ++i) {
        json cj = json::array();
        for (int c = 0; c < eqs[i].k; ++c) {
            int idx = r.koff[i] + c;
            double se = r.se[idx];
            cj.push_back({{"name", eqs[i].names[c]}, {"b", r.beta[idx]}, {"se", se},
                          {"t", se > 0 ? r.beta[idx] / se : 0.0}});
        }
        eqsj.push_back({{"depvar", deps[i]}, {"coefs", cj}});
    }
    json sig = json::array();
    for (int i = 0; i < M; ++i) { json row = json::array(); for (int j = 0; j < M; ++j) row.push_back(r.Sigma[static_cast<size_t>(i) * M + j]); sig.push_back(row); }
    json out = {{"estimator", "reg3"}, {"method", threesls ? "3sls" : "sure"}, {"n_obs", n},
                {"n_equations", M}, {"equations", eqsj}, {"sigma", sig}};
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
