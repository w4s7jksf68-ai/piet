#include "common.h"
#include "commands.h"

#include "estimators/ivml.h"

// P7 wave — ivprobit, ivtobit (Stata's `ivprobit` / `ivtobit`, the full-information
// MLE forms, not the two-step ones).
//
// Both verbs are the same two-equation model with a different observation rule on
// the structural outcome, so they share run_ivml() below and differ only in the
// IvKind they pass, the censoring options they accept, and the extra sd(e.y) row
// ivtobit reports.
//
// VARLISTS. Stata writes `ivprobit y1 x (y2 = z)`; parsing that parenthesized form
// is PIET's job, so this CLI takes the three lists explicitly: --y, --x (structural
// exogenous), --endog (the endogenous regressor), --instruments (the excluded
// instruments). The two designs are then assembled as
//     Xs = [endog, x…, intercept]   (structural — Stata's coefficient table order)
//     Xw = [x…, instruments…, intercept]  (reduced form — the "Exogenous: x z" footer)
// from ONE load_glm pass over y + endog + instruments + x, so both equations and
// every reported statistic share a single listwise-deleted sample. That single pass
// is also why the two commands legitimately report different N on the same data —
// ivprobit never touches the tobit outcome, so its sample keeps rows the tobit run
// drops (4,800 vs 4,778 on the P7 wave fixture).
//
// A two-step (Newey / Amemiya) estimator would land close on the structural
// coefficients but has no corr(e.endog,e.y) or sd(e.·) rows at all, and no Wald
// test of exogeneity built from them — the oracle prints standard errors for those
// rows, which only full-information ML produces. estimators/ivml.h is the FIML.

namespace cecon {
namespace cli {

namespace {

enum class IvKind { Probit, Tobit };

// Index of `name` in `names`, or -1. Plain (non-factor) variables come back out of
// build_model under their own name, which is why --endog/--instruments are
// restricted to plain names below: the lookup is what pins each requested variable
// to its column no matter how the factor expansion of --x reshuffled the rest.
int find_name(const std::vector<std::string>& names, const std::string& name) {
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i);
    return -1;
}

int run_ivml(const std::vector<std::string>& a, IvKind kind, const char* cmd) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string endog_csv = opt(a, "--endog"), instr_csv = opt(a, "--instruments");
    // --vce default "iid": opt() with a default leaves an omitted flag unconsumed,
    // so a plain invocation keeps the model-based (observed-information) SEs.
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    const bool is_tobit = (kind == IvKind::Tobit);
    std::string lls = is_tobit ? opt(a, "--ll") : std::string();
    std::string uls = is_tobit ? opt(a, "--ul") : std::string();

    if (data_missing(data) || y.empty() || endog_csv.empty() || instr_csv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra %s --data f.hbin --y Y --endog E --instruments Z1,Z2 [--x X1,X2]\n"
                     "       %s[--vce iid|robust|cluster] [--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  %s with an endogenous regressor, by full-information ML:\n"
                     "    Y* = E*a + X*b + u ;  E = X*p1 + Z*p2 + v ;  (u,v) bivariate normal\n"
                     "  --endog E                the endogenous regressor (exactly one)\n"
                     "  --instruments Z1,Z2      the EXCLUDED instruments (--x stays in both equations)\n"
                     "%s"
                     "  --vce cluster            needs --cluster C.\n",
                     cmd, std::string(std::string(cmd).size() + 8, ' ').c_str(),
                     is_tobit ? "tobit" : "probit",
                     is_tobit ? "  --ll L / --ul U          censoring limits; at least one is required\n" : "");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "%s: --vce must be one of iid|robust|cluster (got %s).\n", cmd, vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "%s: --vce cluster requires --cluster C.\n", cmd);
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "%s: --level must be in [10, 99.99] (got %s).\n", cmd,
                     opt(a, "--level").c_str());
        return 2;
    }
    // Asymptotic z inference throughout — Stata's IV-ML models report z, not t.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    const bool has_ll = !lls.empty(), has_ul = !uls.empty();
    const double ll_lim = has_ll ? std::stod(lls) : 0.0;
    const double ul_lim = has_ul ? std::stod(uls) : 0.0;
    if (is_tobit && !has_ll && !has_ul)
        throw EngineError("bad_input",
            std::string(cmd) + ": at least one of --ll L / --ul U is required (an uncensored "
            "outcome is `ivregress`, not ivtobit)");
    if (is_tobit && has_ll && has_ul && !(ll_lim < ul_lim))
        throw EngineError("bad_input", std::string(cmd) + ": --ll must be strictly below --ul");

    std::vector<std::string> endog = split_csv(endog_csv);
    std::vector<std::string> instr = split_csv(instr_csv);
    std::vector<std::string> exog = xcsv.empty() ? std::vector<std::string>() : split_csv(xcsv);
    // ONE endogenous regressor. With p > 1 the conditional distribution of u given
    // the reduced-form residuals is driven by a full p×p Σ_v and a p-vector of
    // correlations, which is a different likelihood — not a loop over this one. An
    // honest reject is parity; a silent single-endog answer to a two-endog question
    // is not.
    if (endog.size() != 1)
        throw EngineError("unsupported",
            std::string(cmd) + ": exactly one endogenous regressor is supported (got " +
            std::to_string(endog.size()) + "); multiple endogenous regressors need the "
            "multivariate reduced form, which this build does not implement");
    if (instr.size() < endog.size())
        throw EngineError("bad_input",
            std::string(cmd) + ": the model is under-identified — " + std::to_string(instr.size()) +
            " excluded instrument(s) for " + std::to_string(endog.size()) + " endogenous regressor(s)");
    for (const auto& v : endog)
        if (v.find_first_of(".#()") != std::string::npos)
            throw EngineError("unsupported",
                std::string(cmd) + ": --endog takes plain variable names (got '" + v + "')");
    for (const auto& v : instr)
        if (v.find_first_of(".#()") != std::string::npos)
            throw EngineError("unsupported",
                std::string(cmd) + ": --instruments takes plain variable names (got '" + v + "')");
    for (const auto& v : exog)
        for (const auto& e : endog)
            if (v == e)
                throw EngineError("bad_input",
                    std::string(cmd) + ": '" + v + "' is listed in both --x and --endog");
    for (const auto& v : exog)
        for (const auto& z : instr)
            if (v == z)
                throw EngineError("bad_input",
                    std::string(cmd) + ": '" + v + "' is listed in both --x and --instruments");

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    // One load, one listwise sample, for BOTH equations.
    std::vector<std::string> allx = endog;
    allx.insert(allx.end(), instr.begin(), instr.end());
    allx.insert(allx.end(), exog.begin(), exog.end());
    GlmInputs g = load_glm(ds, cmd, y, allx, cluster_names);
    if (g.n == 0) { std::fprintf(stderr, "%s: no rows after dropping missing.\n", cmd); return 1; }
    if (g.k < 1 || g.names.back() != "(intercept)")
        throw EngineError("bad_input", std::string(cmd) + ": internal design has no intercept column");
    if (!is_tobit && !check_binary(g.y, cmd, y)) return 2;
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            std::string(cmd) + ": vce(cluster) requires at least 2 clusters, but the cluster "
            "variable has " + std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    // Column bookkeeping. Every requested endog/instrument must still be present —
    // if build_model dropped one as collinear, say so instead of silently fitting a
    // different model than the one asked for.
    const int icons = g.k - 1;
    std::vector<int> ie, iz, ix;
    std::vector<bool> claimed(static_cast<size_t>(g.k), false);
    claimed[static_cast<size_t>(icons)] = true;
    for (const auto& v : endog) {
        int j = find_name(g.names, v);
        if (j < 0 || j == icons)
            throw EngineError("bad_input",
                std::string(cmd) + ": endogenous regressor '" + v + "' is not in the fitted design "
                "(collinear or dropped)");
        ie.push_back(j);
        claimed[static_cast<size_t>(j)] = true;
    }
    for (const auto& v : instr) {
        int j = find_name(g.names, v);
        if (j < 0 || j == icons)
            throw EngineError("bad_input",
                std::string(cmd) + ": instrument '" + v + "' is not in the fitted design "
                "(collinear or dropped)");
        iz.push_back(j);
        claimed[static_cast<size_t>(j)] = true;
    }
    for (int j = 0; j < g.k; ++j)
        if (!claimed[static_cast<size_t>(j)]) ix.push_back(j);  // expanded --x, in load order

    const int ne = static_cast<int>(ie.size()), nz = static_cast<int>(iz.size());
    const int nx = static_cast<int>(ix.size());
    const int ks = ne + nx + 1, kw = nx + nz + 1;
    const int64_t n = g.n;
    std::vector<double> Xs(static_cast<size_t>(n) * ks), Xw(static_cast<size_t>(n) * kw);
    std::vector<std::string> sname, wname;
    auto put = [&](std::vector<double>& D, int dst, int src) {
        const double* s = g.X.data() + static_cast<int64_t>(src) * n;
        std::copy(s, s + n, D.data() + static_cast<int64_t>(dst) * n);
    };
    { int c = 0;
      for (int j : ie) { put(Xs, c++, j); sname.push_back(g.names[static_cast<size_t>(j)]); }
      for (int j : ix) { put(Xs, c++, j); sname.push_back(g.names[static_cast<size_t>(j)]); }
      put(Xs, c, icons); sname.push_back("_cons"); }
    { int c = 0;
      for (int j : ix) { put(Xw, c++, j); wname.push_back(g.names[static_cast<size_t>(j)]); }
      for (int j : iz) { put(Xw, c++, j); wname.push_back(g.names[static_cast<size_t>(j)]); }
      put(Xw, c, icons); wname.push_back("_cons"); }
    // y2 = the endogenous regressor's own column (Xs's first column, by construction).
    const double* y2 = Xs.data();

    IvmlResult r = is_tobit
        ? ivtobit_fit(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2, has_ll, ll_lim, has_ul, ul_lim)
        : ivprobit_fit(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2);
    if (!r.ok)
        throw EngineError("no_convergence",
            std::string(cmd) + ": MLE failed (singular information matrix or non-convergent fit).");
    const int np = r.np;

    // V = bread (iid) or a sandwich bread·meat·bread over the per-observation score
    // rows of the FULL θ — structural δ, reduced-form Π, athrho and the ln σ's all
    // in one block, so corr and sd inherit the robust variance too (Stata prints
    // "Robust std. err." over those rows as well, not just the coefficients).
    //
    // SMALL-SAMPLE FACTOR. Stata's ML multiplier, NOT the regression-style
    // (N−1)/(N−K)·G/(G−1) that vcov.cpp applies to feols/2SLS:
    //   vce(robust)  → N/(N−1)
    //   vce(cluster) → G/(G−1)
    // Same P5 precedent as logit/probit/tobit (cmds_mle.cpp) and the P7 ordered
    // models, and VERIFIED here rather than assumed — tests/unit/test_ivml.cpp
    // asserts every robust SE of the P7 oracle `ivprobit ybin x1 (endog=z),
    // vce(robust)` (N=4,800) and `ivtobit y x1 (endog=z), ll(0) vce(robust)`
    // (N=4,778) against N/(N−1) and matches within the wave tolerance; dropping the
    // factor moves se(endog) to .0386537 against the oracle's .0386577, a 1.0e-4
    // relative miss that those assertions reject.
    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S = is_tobit
            ? ivtobit_scores(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2, has_ll, ll_lim,
                             has_ul, ul_lim, r)
            : ivprobit_scores(Xs.data(), Xw.data(), n, ks, kw, g.y.data(), y2, r);
        int64_t rows = n;
        const Factor* fac = nullptr;
        double factor;
        if (vce == "cluster") {
            fac = &g.cluster[0];
            n_clusters = fac->n_levels;
            rows = n_clusters;
            factor = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
        } else {
            factor = n > 1 ? static_cast<double>(n) / (static_cast<double>(n) - 1.0) : 1.0;
        }
        std::vector<double> M(static_cast<size_t>(rows) * np, 0.0);
        for (int64_t i = 0; i < n; ++i) {
            int64_t rr = fac ? fac->codes[static_cast<size_t>(i)] : i;
            for (int p = 0; p < np; ++p)
                M[static_cast<int64_t>(p) * rows + rr] += S[static_cast<int64_t>(p) * n + i];
        }
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
        la::xtx(M.data(), rows, np, meat.data());
        std::vector<double> tmp(static_cast<size_t>(np) * np, 0.0);
        V.assign(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    }
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };
    auto zp = [](double z) {
        return z_twosided_pvalue(z);
    };

    json coefs = json::array();
    for (int c = 0; c < ks; ++c) {
        double b = r.beta[static_cast<size_t>(c)], se = sd(c);
        double z = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", sname[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                         {"p", (se > 0) ? zp(z) : 1.0}, {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    json rf = json::array();
    for (int c = 0; c < kw; ++c) {
        double b = r.pi[static_cast<size_t>(c)], se = sd(ks + c);
        double z = (se > 0) ? b / se : 0.0;
        rf.push_back({{"name", wname[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                      {"p", (se > 0) ? zp(z) : 1.0}, {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }

    // ANCILLARY ROWS. Reported in their TRANSFORMED metrics, exactly as Stata does:
    //   corr = tanh(athrho), se = (1−corr²)·se(athrho)   [delta method]
    //        CI = tanh(athrho ± z·se(athrho))            [NOT corr ± z·se(corr)]
    //   sd   = exp(lnσ),     se = sd·se(lnσ)             [delta method]
    //        CI = exp(lnσ ± z·se(lnσ))                   [NOT sd ± z·se(sd)]
    // The CIs are built by transforming the endpoints of the SYMMETRIC interval in
    // θ's own metric, which is why they come out asymmetric around the point
    // estimate — the oracle's corr CI [−.1364806, −.0072521] around −.0721692 is
    // .0643 below and .0649 above, and a b±z·se interval would be neither.
    const double ath = r.theta[static_cast<size_t>(r.i_athrho)], se_ath = sd(r.i_athrho);
    const double rho = std::tanh(ath);
    json corr = {{"name", "corr(e." + endog[0] + ",e." + y + ")"},
                 {"b", rho}, {"se", (1.0 - rho * rho) * se_ath},
                 {"ci_lo", std::tanh(ath - zc * se_ath)}, {"ci_hi", std::tanh(ath + zc * se_ath)},
                 {"athrho", ath}, {"se_athrho", se_ath}};
    json sds = json::array();
    auto sd_row = [&](const char* nm, int idx) {
        const double l = r.theta[static_cast<size_t>(idx)], sl = sd(idx);
        return json{{"name", nm}, {"b", std::exp(l)}, {"se", std::exp(l) * sl},
                    {"ci_lo", std::exp(l - zc * sl)}, {"ci_hi", std::exp(l + zc * sl)}};
    };
    if (is_tobit) sds.push_back(sd_row(("sd(e." + y + ")").c_str(), r.i_lnse));
    sds.push_back(sd_row(("sd(e." + endog[0] + ")").c_str(), r.i_lnsv));

    // Model Wald χ² over the structural SLOPES only — [endog, x…], excluding the
    // constant. That is Stata's chi2(2) here (endog + x1), not chi2(3).
    const int nslope = ks - 1;
    double chi2 = 0;
    if (nslope > 0) {
        std::vector<double> Vs(static_cast<size_t>(nslope) * nslope);
        for (int i = 0; i < nslope; ++i)
            for (int j = 0; j < nslope; ++j)
                Vs[static_cast<size_t>(i) * nslope + j] = V[static_cast<size_t>(i) * np + j];
        if (la::spd_inverse(Vs.data(), nslope) == 0)
            for (int i = 0; i < nslope; ++i)
                for (int j = 0; j < nslope; ++j)
                    chi2 += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * nslope + j] *
                            r.beta[static_cast<size_t>(j)];
    }
    // Wald test of exogeneity (corr = 0): the squared t-ratio of athrho, on χ²(1).
    // Taken in ATHRHO's metric — the delta-method se(corr) cancels against the
    // delta-method corr in the ratio only to first order, and Stata's printed
    // chi2(1) = 4.75 on the probit leg matches the athrho ratio, so this is the
    // form, not an equivalent-looking rewrite.
    const double chi2_exog = (se_ath > 0) ? (ath / se_ath) * (ath / se_ath) : 0.0;

    json out = {{"estimator", cmd}, {"n_obs", n}, {"n_dropped_missing", g.n_dropped},
                {"vce", vce}, {"coefs", coefs}, {"rf_coefs", rf},
                {"corr", corr}, {"sd", sds},
                {"endogenous", endog}, {"instruments", instr},
                {"ll", r.ll}, {"ll_exogenous", r.ll_exog},
                {"chi2", chi2}, {"df_chi2", nslope}, {"p_chi2", chi2_pvalue(chi2, nslope)},
                {"chi2_exogeneity", chi2_exog}, {"p_exogeneity", chi2_pvalue(chi2_exog, 1)},
                {"iters", r.iters}, {"converged", r.converged}};
    // The footer's "Exogenous:" list — the reduced form's regressors minus the
    // constant, i.e. the included exogenous x followed by the excluded instruments.
    { json ex = json::array();
      for (int c = 0; c < kw - 1; ++c) ex.push_back(wname[static_cast<size_t>(c)]);
      out["exogenous"] = ex; }
    if (is_tobit) {
        out["n_uncensored"] = r.n_unc;
        out["n_left_censored"] = r.n_lc;
        out["n_right_censored"] = r.n_rc;
        if (has_ll) out["ll_limit"] = ll_lim;
        if (has_ul) out["ul_limit"] = ul_lim;
    }
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace

int cmd_ivprobit(const std::vector<std::string>& a) { return run_ivml(a, IvKind::Probit, "ivprobit"); }

int cmd_ivtobit(const std::vector<std::string>& a) { return run_ivml(a, IvKind::Tobit, "ivtobit"); }

}  // namespace cli
}  // namespace cecon
