#include "common.h"
#include "commands.h"

#include <limits>

#include "estimators/logit.h"
#include "estimators/probit.h"
#include "estimators/tobit.h"
#include "estimators/heckman.h"
#include "estimators/qreg.h"
#include "vcov/vcov.h"  // hc_meat/cluster_meat — P5 pweight logit sandwich reuses these low-level helpers

#include "vcov/svy_vcov.h"

namespace cecon {
namespace cli {

// qreg: quantile regression (Koenker-Bassett). Frisch-Newton IP + iid (fitted-
// sparsity, Hall-Sheather) SEs; t(df_r) inference. Matches Stata `qreg`.
// --vce robust (+ optional --weight W --wtype pw): Stata's `qreg ..., vce(robust)`
// and `qreg ... [pw=w], vce(robust)` through qreg_fit_robust (the Hendricks-
// Koenker sandwich; oracle w2b_estim.log:818 / :791, formula in qreg.cpp).
int cmd_qreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra qreg --data f.hbin --y Y --x X1,X2 [--tau 0.5] [--level 95] "
                             "[--vce iid|robust] [--weight W --wtype pw] [--keepif EXPR] [--out f.json]\n"
                             "  --weight W --wtype pw    Stata pweight; requires --vce robust.\n");
        return 2;
    }
    const std::string vce = opt(a, "--vce", "iid");
    if (vce != "iid" && vce != "robust") {
        std::fprintf(stderr, "qreg: --vce must be one of iid|robust (got %s).\n", vce.c_str());
        return 2;
    }
    // pweight: Stata's qreg takes fweight/iweight/pweight ([R] qreg); only the
    // pweight + vce(robust) pair is oracle-pinned (w2b_estim.log:791). A pweight
    // without vce(robust) has no captured record of which VCE Stata computes, so
    // it is refused rather than answered with a guessed one.
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "qreg: --wtype requires --weight");
        if (wtype != "pw" && wtype != "pweight")
            throw EngineError("unsupported", "qreg: --wtype must be pw (only pweight is supported)");
        if (vce != "robust")
            throw EngineError("unsupported",
                "qreg: [pweight=] is supported only with vce(robust) in this engine");
        wkind = cecon::WeightKind::Probability;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    double tau = std::stod(opt(a, "--tau", "0.5"));
    double level = std::stod(opt(a, "--level", "95"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "qreg", y, split_csv(xcsv), {}, weight_var, wkind);
    if (g.n == 0) { std::fprintf(stderr, "qreg: no rows after dropping missing.\n"); return 1; }
    const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
    QregResult r = (vce == "robust")
        ? qreg_fit_robust(g.X.data(), g.n, g.k, g.y.data(), wptr, tau, g.names, level)
        : qreg_fit(g.X.data(), g.n, g.k, g.y.data(), tau, g.names, level);
    if (!r.converged) std::fprintf(stderr, "qreg: warning — interior-point did not fully converge.\n");
    const double dof = r.dof_resid;
    // BUG-281: the CI multiplier must follow --level. `level` already reached
    // qreg_fit (the Hall-Sheather bandwidth, hence the SE, is level-dependent —
    // matching Stata), but this multiplier was hardcoded at the 95% alpha, so
    // `--level 90` printed a 90%-bandwidth se under a 95% t. Measured vs StataBE
    // 19.5 (auto, qreg price mpg weight, level(90)): Stata ll(mpg)
    // -2.49302234902794e+02, ours was -285.681 (t at .025 instead of .05).
    // Same alpha dance as tobit: exact 0.05 when level==95, else 1-level/100.
    const double tc = (dof > 0) ? t_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0, dof) : 0.0;
    json coefs = json::array();
    for (int j = 0; j < g.k; ++j) {
        double se = r.se[j], t = (se > 0) ? r.beta[j] / se : 0.0;
        coefs.push_back({{"name", g.names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t},
                         {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0},
                         {"ci_lo", r.beta[j] - tc * se}, {"ci_hi", r.beta[j] + tc * se}});
    }
    json out = {{"estimator", "qreg"}, {"tau", tau}, {"n_obs", r.n}, {"coefs", coefs},
                {"sparsity", r.sparsity}, {"bwidth", r.bwidth}, {"sum_adev", r.sum_adev},
                {"df_r", dof}, {"converged", r.converged}};
    if (vce == "robust") {
        // qreg_fit_robust computes no sparsity (NaN, qreg.h), so the field is
        // absent rather than a null; the default iid output is unchanged.
        out.erase("sparsity");
        out["vce"] = "robust";
        if (r.n_crossing > 0) out["n_crossing"] = r.n_crossing;
    }
    if (wkind != cecon::WeightKind::None) { out["weight_var"] = weight_var; out["wtype"] = "pw"; }
    emit(out, opt(a, "--out"));
    return 0;
}

// heckman — sample-selection model, two-step (Heckit). --select "S=Z1,Z2".
int cmd_heckman(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x"), selspec = opt(a, "--select");
    if (data_missing(data) || y.empty() || xcsv.empty() || selspec.empty()) {
        std::fprintf(stderr, "usage: cobra heckman --data f.hbin --y Y --x X1,X2 --select \"S=Z1,Z2\" "
                             "[--twostep] [--level P] [--keepif EXPR] [--out f.json]\n"
                             "  two-step Heckit (probit selection + inverse-Mills correction).\n"
                             "  --level P     ci_lo/ci_hi on BOTH equations (asymptotic z, matching\n"
                             "                Stata's own `heckman` table) at P%; default 95, matching\n"
                             "                Stata's default table (BUG-202, BUG-279).\n");
        return 2;
    }
    // --level P (BUG-202): heckman (even --twostep here) reports z/P>|z| in Stata,
    // not t — same asymptotic-normal convention as logit/probit/ivtobit, applied to
    // BOTH the outcome equation (coefs) and the selection equation (select_coefs).
    // Only `coefs` feeds the shared `--format tidy` coeftable (tidy_coeftable only
    // looks at the top-level "coefs"/"coef" keys) — select_coefs' ci_lo/ci_hi is
    // still emitted for a caller reading the native JSON shape directly.
    // BUG-279: default level is 95, like Stata's — the plain invocation emits the
    // 95% interval unprompted rather than a null column.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "heckman: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    auto eq = selspec.find('=');
    if (eq == std::string::npos) { std::fprintf(stderr, "heckman: --select must be \"S=Z1,Z2\".\n"); return 2; }
    std::string selvar = selspec.substr(0, eq);
    auto zvars = split_csv(selspec.substr(eq + 1));
    auto xvars = split_csv(xcsv);
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* Sr = require_num(ds, "heckman", selvar);
    const double* yr = require_num(ds, "heckman", y);
    int kz = static_cast<int>(zvars.size()) + 1, kx = static_cast<int>(xvars.size()) + 1;
    std::vector<const double*> zr, xr, must = {Sr};
    for (auto& s : zvars) { zr.push_back(require_num(ds, "heckman", s)); must.push_back(zr.back()); }
    for (auto& s : xvars) { xr.push_back(require_num(ds, "heckman", s)); must.push_back(xr.back()); }
    auto keep = listwise_keep(must, n_raw);  // y allowed missing (non-selected)
    int64_t n = static_cast<int64_t>(keep.size());
    if (n == 0) { std::fprintf(stderr, "heckman: no rows after dropping missing.\n"); return 1; }
    auto gv = [&](const double* p) { return gather_rows(p, keep); };
    std::vector<double> S = gv(Sr), yv = gv(yr);
    std::vector<double> Z(static_cast<size_t>(n) * kz), X(static_cast<size_t>(n) * kx);
    for (int c = 0; c + 1 < kz; ++c) { auto col = gv(zr[c]); std::copy(col.begin(), col.end(), Z.data() + static_cast<int64_t>(c) * n); }
    for (int64_t i = 0; i < n; ++i) Z[static_cast<int64_t>(kz - 1) * n + i] = 1.0;
    for (int c = 0; c + 1 < kx; ++c) { auto col = gv(xr[c]); std::copy(col.begin(), col.end(), X.data() + static_cast<int64_t>(c) * n); }
    for (int64_t i = 0; i < n; ++i) X[static_cast<int64_t>(kx - 1) * n + i] = 1.0;
    std::vector<char> sel(n);
    for (int64_t i = 0; i < n; ++i) sel[i] = (S[i] == 1.0 && !std::isnan(yv[i])) ? 1 : 0;

    HeckmanResult r = heckman_twostep(Z.data(), n, kz, S.data(), X.data(), kx, yv.data(), sel);
    if (!r.ok) { std::fprintf(stderr, "heckman: estimation failed.\n"); return 1; }
    std::vector<std::string> onames = xvars; onames.push_back("(intercept)");
    std::vector<std::string> snames = zvars; snames.push_back("(intercept)");
    json oc = json::array();
    for (int j = 0; j < kx; ++j) {
        json row = {{"name", onames[j]}, {"b", r.beta[j]}, {"se", r.se[j]},
                    {"t", r.se[j] > 0 ? r.beta[j] / r.se[j] : 0.0}};
        if (r.se[j] > 0) {
            row["ci_lo"] = r.beta[j] - zc * r.se[j];
            row["ci_hi"] = r.beta[j] + zc * r.se[j];
        }
        oc.push_back(std::move(row));
    }
    json sc = json::array();
    for (int j = 0; j < kz; ++j) {
        json row = {{"name", snames[j]}, {"b", r.sel_beta[j]}, {"se", r.sel_se[j]},
                    {"t", r.sel_se[j] > 0 ? r.sel_beta[j] / r.sel_se[j] : 0.0}};
        if (r.sel_se[j] > 0) {
            row["ci_lo"] = r.sel_beta[j] - zc * r.sel_se[j];
            row["ci_hi"] = r.sel_beta[j] + zc * r.sel_se[j];
        }
        sc.push_back(std::move(row));
    }
    json out = {{"estimator", "heckman"}, {"method", "twostep"}, {"n_obs", r.n},
                {"n_selected", r.n_selected}, {"coefs", oc}, {"select_coefs", sc},
                {"lambda", r.lambda}, {"se_lambda", r.se_lambda},
                {"rho", r.rho}, {"sigma", r.sigma}};
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    emit(out, opt(a, "--out"));
    return 0;
}

// tobit — censored-normal regression MLE (Stata `tobit, ll() ul()`).
int cmd_tobit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string lls = opt(a, "--ll"), uls = opt(a, "--ul");
    // --vce (P5): default "iid" so an omitted flag reproduces the exact prior
    // behavior/output shape (opt() with a default never marks the flag consumed
    // when absent) — see below, every new branch is gated on vce=="robust".
    std::string vce = opt(a, "--vce", "iid");
    if (data_missing(data) || y.empty() || xcsv.empty() || (lls.empty() && uls.empty())) {
        std::fprintf(stderr, "usage: cobra tobit --data f.hbin --y Y --x X1,X2 [--ll L] [--ul U] "
                             "[--vce iid|robust] [--level P] [--keepif EXPR] [--out f.json]\n"
                             "  censored-normal MLE; at least one of --ll/--ul required.\n"
                             "  --vce robust  Stata `tobit ..., vce(robust)` sandwich VCE (P5).\n"
                             "  --level P     coeftable ci_lo/ci_hi at P% (t(df_r), matching Stata's\n"
                             "                own tobit table); default 95, matching Stata's default\n"
                             "                table (BUG-202, BUG-279).\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust") {
        std::fprintf(stderr, "tobit: --vce must be one of iid|robust (got %s).\n", vce.c_str());
        return 2;
    }
    // --level P (BUG-202): tobit had no --level at all, so ci_lo/ci_hi was
    // unconditionally absent. Stata's own `tobit` table reports t/P>|t| (not z),
    // matching this command's EXISTING p-value dispatch two lines below
    // (t_twosided_pvalue(t, dof)) — so the CI multiplier is t_critical(., dof) for
    // the SAME dof, not the z used by the pure-ML logit/probit family.
    // BUG-279: the level DEFAULT is 95, like Stata's — the plain invocation emits
    // the 95% interval unprompted, so `cobra tobit` alone matches Stata's default
    // table for every caller (not just a PIET that knows to inject level(95)).
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "tobit: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    bool has_ll = !lls.empty(), has_ul = !uls.empty();
    double ll_lim = has_ll ? std::stod(lls) : 0.0, ul_lim = has_ul ? std::stod(uls) : 0.0;
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    GlmInputs g = load_glm(ds, "tobit", y, split_csv(xcsv));
    if (g.n == 0) { std::fprintf(stderr, "tobit: no rows after dropping missing.\n"); return 1; }
    TobitResult r = tobit_fit(g.X.data(), g.n, g.k, g.y.data(), has_ll, ll_lim, has_ul, ul_lim);
    if (!r.ok) { std::fprintf(stderr, "tobit: MLE failed.\n"); return 1; }
    // BUG-280: Stata's tobit residual df is e(N) - e(df_m) — it subtracts ONLY the
    // slope regressors, never the intercept and never sigma. Measured against
    // StataBE 19.5 on seven specs (fatalities inc_cens with ul(): 1/2/4 slopes ->
    // df_r 335/334/332; 2 slopes noconstant -> 334, same as WITH _cons; 2 slopes
    // two-limit ll()+ul() -> 334; auto price with ll(3000): 1/2 slopes -> 73/72).
    // load_glm always appends the intercept, so slopes = g.k - 1. The old
    // `r.n - g.k - 1` subtracted intercept+sigma and landed on invttail(332,.)
    // where Stata is invttail(334,.) — wrong in the last digits of every tobit
    // CI AND p-value (this dof feeds t_twosided_pvalue below).
    double dof = static_cast<double>(r.n - (g.k - 1));
    // vce(robust): only computed when explicitly requested — the DEFAULT (no
    // --vce) path below keeps reading r.se/r.se_sigma/r.se_variance exactly as
    // before, byte-for-byte, so existing pins (freeze corpus `tobit y x1 x2,
    // ll(0)`, tests/unit/test_tobit.cpp) are untouched.
    std::vector<double> Vrobust;
    const int np = g.k + 1;
    if (vce == "robust") {
        // ML vce(robust) small-sample multiplier: N/(N-1) — the SAME factor Stata
        // applies to logit/probit's plain vce(robust) (see cmd_logit above).
        // Verified exact vs the P5 oracle `tobit y x1 x2 i.region, ll(0)
        // vce(robust)` pin (coef/se match to <1e-6 relative).
        double factor = r.n > 1 ? static_cast<double>(r.n) / static_cast<double>(r.n - 1) : 1.0;
        Vrobust = tobit_robust_vcov(g.X.data(), g.n, g.k, g.y.data(), has_ll, ll_lim, has_ul, ul_lim,
                                    r, factor);
        if (Vrobust.empty()) {
            std::fprintf(stderr, "tobit: vce(robust) failed (non-invertible sandwich).\n");
            return 1;
        }
    }
    const double tc = (dof > 0) ? t_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0, dof) : 0.0;
    json coefs = json::array();
    for (int j = 0; j < g.k; ++j) {
        double se = (vce == "robust") ? std::sqrt(std::max(0.0, Vrobust[static_cast<size_t>(j) * np + j]))
                                      : r.se[j];
        double t = (se > 0) ? r.beta[j] / se : 0.0;
        json row = {{"name", g.names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t},
                    {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0}};
        if (se > 0 && dof > 0) {
            row["ci_lo"] = r.beta[j] - tc * se;
            row["ci_hi"] = r.beta[j] + tc * se;
        }
        coefs.push_back(std::move(row));
    }
    double se_sigma = r.se_sigma, se_variance = r.se_variance;
    if (vce == "robust") {
        double se_lnsig = std::sqrt(std::max(0.0, Vrobust[static_cast<size_t>(g.k) * np + g.k]));
        se_sigma = r.sigma * se_lnsig;         // same delta-method shape as tobit_fit's se_sigma
        se_variance = 2.0 * r.sigma * se_sigma;
    }
    json out = {{"estimator", "tobit"}, {"n_obs", r.n}, {"n_uncensored", r.n_unc},
                {"n_left_censored", r.n_lc}, {"n_right_censored", r.n_rc},
                {"sigma", r.sigma}, {"se_sigma", se_sigma},
                {"variance", r.variance}, {"se_variance", se_variance},
                {"ll", r.ll}, {"iters", r.iters}, {"converged", r.converged}, {"coefs", coefs}};
    if (has_ll) out["ll_limit"] = ll_lim;
    if (has_ul) out["ul_limit"] = ul_lim;
    if (has(a, "--vce")) out["vce"] = vce;  // absent-key on the default path (byte-stable)
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    emit(out, opt(a, "--out"));
    return 0;
}

namespace {

// Stata `logit/probit ..., noconstant` (--no-intercept). load_glm's public
// contract puts the intercept in the LAST design column (common.cpp), so with
// X column-major the constant is dropped by truncating X to its first k-1
// columns and popping the trailing "(intercept)" name.
void drop_glm_intercept(GlmInputs& g, const char* cmd) {
    if (g.k < 2)
        throw EngineError("bad_input", std::string(cmd) +
                          ": noconstant with no regressors leaves an empty model");
    g.k -= 1;
    g.X.resize(static_cast<size_t>(g.n) * static_cast<size_t>(g.k));
    g.names.pop_back();
}

// Header statistics of a no-constant binary model. There is no constant-only
// model nested in it, so Stata prints neither the LR test nor a pseudo-R2: it
// prints a Wald chi2 on ALL k coefficients (w2b_estim.log:692 `probit d x1 x2,
// r nocons` Wald chi2(2) = 19.17; :719 `probit d x1 x2, noconstant` Wald
// chi2(2) = 18.97). `w` is the pweight (nullptr unweighted): the reported log
// (pseudo)likelihood is then the weighted one.
void add_mle_fit_nocons(json& out, const std::vector<double>& p, const std::vector<double>& y,
                        const double* w, int64_t n, const std::vector<double>& beta,
                        const std::vector<double>& V, int k) {
    const double eps = 1e-12;
    double ll = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        double pi = std::min(1.0 - eps, std::max(eps, p[static_cast<size_t>(i)]));
        double yi = y[static_cast<size_t>(i)];
        double wi = w ? w[static_cast<size_t>(i)] : 1.0;
        ll += wi * (yi * std::log(pi) + (1.0 - yi) * std::log(1.0 - pi));
    }
    out["ll"] = ll;
    out["ll_0"] = nullptr;
    out["r2_pseudo"] = nullptr;
    out["df_chi2"] = k;
    std::vector<double> Vi(V.begin(), V.begin() + static_cast<std::ptrdiff_t>(k) * k);
    double chi2 = std::numeric_limits<double>::quiet_NaN();
    if (k > 0 && la::spd_inverse(Vi.data(), k) == 0) {
        std::vector<double> tmp(static_cast<size_t>(k));
        la::matvec(Vi.data(), k, beta.data(), tmp.data());
        chi2 = 0.0;
        for (int j = 0; j < k; ++j) chi2 += beta[static_cast<size_t>(j)] * tmp[static_cast<size_t>(j)];
    }
    out["chi2"] = std::isfinite(chi2) ? json(chi2) : json(nullptr);
    out["p_chi2"] = std::isfinite(chi2) ? json(chi2_pvalue(chi2, k)) : json(nullptr);
}

}  // namespace

int cmd_logit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string vce = opt(a, "--vce", "iid");
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra logit --data f.hbin --y Y --x X1,X2 [--vce iid|robust|cluster] "
                             "[--cluster C] [--weight W --wtype pw] [--no-intercept] [--level P] [--out f.json]\n"
                             "  --vce cluster            needs --cluster C.\n"
                             "  --weight W --wtype pw    Stata pweight (P5): forces vce(robust) or\n"
                             "                           vce(cluster ...) — Stata has no iid VCE under\n"
                             "                           pweight — and reweights the MLE by Sum w_i*loglik_i.\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "logit: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "logit: --vce cluster requires --cluster C.\n");
        return 2;
    }
    // Stata pweight: only pweight is meaningful for an ML command (aw/fw/iw are not
    // valid weight types for `logit`), and pweight always forces a sandwich VCE —
    // there is no iid pweight estimator in Stata, so reject that combination rather
    // than silently reporting a model-based SE that doesn't match anything Stata emits.
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "logit: --wtype requires --weight");
        if (wtype != "pw" && wtype != "pweight")
            throw EngineError("bad_input", "logit: --wtype must be pw (only pweight is supported)");
        wkind = cecon::WeightKind::Probability;
        // --svy is its own (linearized) sandwich, so it satisfies this requirement
        // without naming a --vce; without the exemption `svy: logit` would be
        // rejected for the very thing it supplies.
        if (vce == "iid" && !has(a, "--svy"))
            throw EngineError("bad_input",
                "logit: [pweight=] requires vce(robust) or vce(cluster ...); Stata has no "
                "iid VCE under pweight");
    }
    // --level P (B.0.3): asymptotic z Wald p + CI per coef, emitted ONLY when passed so
    // the default output stays byte-identical (no golden re-pin); `logit y x, level(90)`
    // now yields a real 90% CI instead of the silently-ignored option that was BUG-110's
    // concrete gate-① instance. add_inference's asymptotic branch is 95%-fixed, so the CI
    // is built inline (like qreg) from the level-driven z critical.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "logit: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    // BUG-282: `zc` was gated on level_set, so a DEFAULT `cobra logit` produced no
    // critical value and the emission below produced no p/ci_lo/ci_hi at all —
    // while Stata prints a 95% interval unprompted. `level` already defaults to
    // 95 above, and the alpha dance keeps 95 at the literal 0.05 so existing
    // --level 95 bytes are unchanged; only the DEFAULT path gains output.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    // BUG-333: `iterate(#)` was reachable in logit_fit (logit.h:32 `int maxit = 100`)
    // but had no CLI surface, so the call site below hard-coded 100 and PIET could
    // only reject the option. Stata's maximize_options caps at 16000. probit's
    // sibling site is deliberately NOT touched: BUG-333 measured logit only, and
    // an unmeasured row stays unimplemented.
    int logit_maxit = 100;
    if (has(a, "--iterate")) {
        logit_maxit = std::stoi(opt(a, "--iterate", "100"));
        if (logit_maxit < 1 || logit_maxit > 16000) {
            std::fprintf(stderr, "logit: --iterate must be in [1, 16000] (got %s).\n",
                         opt(a, "--iterate").c_str());
            return 2;
        }
    }
    // P7 `svy: logit`. The point estimates are the ordinary pweighted logit; svy
    // replaces the meat (stratified, over PSU totals), the reference distribution
    // (t with design dof, not z), and the model test (adjusted Wald F, not chi2).
    const bool svy = has(a, "--svy");
    std::string psu_var = opt(a, "--psu"), strata_var = opt(a, "--strata");
    if (svy) {
        if (psu_var.empty() || strata_var.empty())
            throw EngineError("bad_input", "logit: --svy requires both --psu and --strata");
        if (wkind != cecon::WeightKind::Probability)
            throw EngineError("bad_input",
                "logit: --svy requires --weight W --wtype pw (a survey design is a "
                "probability-weighted sample)");
        if (has(a, "--vce") || !cluster_var.empty())
            throw EngineError("bad_input",
                "logit: --svy supplies its own linearized variance; --vce/--cluster are not "
                "accepted with it");
    } else if (!psu_var.empty() || !strata_var.empty()) {
        throw EngineError("bad_input", "logit: --psu/--strata are only meaningful with --svy");
    }
    // Stata `noconstant`. Not combined with --svy: its adjusted Wald F below
    // tests the slopes as "every column but the trailing intercept", which a
    // no-constant design does not have, and no svy+noconstant record is captured.
    const bool no_intercept = has(a, "--no-intercept");
    if (no_intercept && svy)
        throw EngineError("unsupported", "logit: --svy is not supported with --no-intercept");
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    if (svy) { cluster_names.clear(); cluster_names.push_back(psu_var); cluster_names.push_back(strata_var); }
    // BUG-331: Stata's `offset()` — a fixed-coefficient term in the linear
    // predictor. Stata implements it on logit/probit (StataBE 19.5 oracle,
    // 2026-08-17: `logit foreign mpg, offset(lnw)` is rc 0); we did not, so a
    // model Stata answers had no answer here.
    const std::string offset_var = opt(a, "--offset");
    GlmInputs g = load_glm(ds, "logit", y, split_csv(xcsv), cluster_names, weight_var, wkind,
                           offset_var);
    if (g.n == 0) { std::fprintf(stderr, "logit: no rows after dropping missing.\n"); return 1; }
    if (!check_binary(g.y, "logit", y)) return 2;
    if (no_intercept) drop_glm_intercept(g, "logit");
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "logit: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));
    const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
    const double* offptr = g.offset.empty() ? nullptr : g.offset.data();
    LogitResult r = logit_fit(g.X.data(), g.n, g.k, g.y.data(), logit_maxit, 1e-14, wptr, offptr);
    if (r.separated_perfect) {
        std::fprintf(stderr, "logit: outcome %s predicts data perfectly; r(2000)\n", y.c_str());
        return 2;
    }
    // V = bread (iid) or a sandwich bread*meat*bread. meat's per-row score is
    // e_i = (y_i - p_i), scaled by the raw pweight when one is present (wptr): under
    // pweight the score contribution to the SUM-of-weighted-loglik gradient is
    // w_i*(y_i-p_i)*x_i, so pre-scaling e by w_i and reusing the SAME hc_meat/
    // cluster_meat helpers as the unweighted case gives the pweight-correct meat
    // with no separate code path (mirrors feols's √w̃ row-scaling precedent, but
    // here only e — not X — needs the scale, since the meat is linear in e).
    // bread stays r.bread — logit_fit already folded the SAME raw weight into its
    // IRLS working weight, so r.bread is the weighted-observed-information inverse
    // (logit's link is canonical, so observed info == Fisher info; no BUG-140
    // probit-style expected/observed split needed here).
    std::vector<double> V = r.bread;
    int n_clusters = 0;
    int svy_df = 0, svy_n_psu = 0, svy_n_strata = 0;
    double svy_pop = 0.0;
    if (svy) {
        // Scores w_i (y_i − p_i) x_i. X is NOT weight-scaled on this path (logit_fit
        // folds the weight into its IRLS working weight instead), so the raw design
        // is the right one to multiply here — unlike feols, whose design arrives
        // pre-scaled by sqrt(w̃).
        std::vector<double> S(static_cast<size_t>(g.n) * g.k, 0.0);
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = g.y[static_cast<size_t>(i)] - r.p[static_cast<size_t>(i)];
            if (wptr) ei *= wptr[static_cast<size_t>(i)];
            for (int c = 0; c < g.k; ++c)
                S[static_cast<size_t>(c) * g.n + i] = ei * g.X[static_cast<int64_t>(c) * g.n + i];
        }
        cecon::SvyMeatResult sm =
            cecon::svy_linearized_meat(S.data(), g.n, g.k, g.cluster[0], g.cluster[1], wptr);
        if (!sm.singleton_stratum.empty())
            throw EngineError("bad_input",
                "logit: svy stratum variable '" + sm.singleton_stratum +
                    "' has a stratum with a single sampling unit; the linearized variance "
                    "is undefined there (Stata reports missing)");
        if (!sm.ok)
            throw EngineError("bad_input",
                "logit: svy sampling units are not nested within strata");
        std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
        la::sandwich(r.bread.data(), sm.meat.data(), g.k, 1.0, V.data(), tmp.data());
        svy_n_psu = sm.n_psu;
        svy_n_strata = sm.n_strata;
        svy_pop = sm.pop_size;
        svy_df = sm.n_psu - sm.n_strata;
        if (svy_df <= 0)
            throw EngineError("bad_input",
                "logit: svy design degrees of freedom (#PSU − #strata) is not positive");
    } else if (vce == "robust" || vce == "cluster") {
        std::vector<double> e(static_cast<size_t>(g.n));
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = g.y[static_cast<size_t>(i)] - r.p[static_cast<size_t>(i)];
            if (wptr) ei *= wptr[static_cast<size_t>(i)];
            e[static_cast<size_t>(i)] = ei;
        }
        std::vector<double> meat(static_cast<size_t>(g.k) * g.k, 0.0);
        double factor;
        if (vce == "robust") {
            // Manual triple loop (NOT cecon::hc_meat) — preserves the EXACT
            // pre-P5 summation order/expression form so the unweighted vce(robust)
            // path stays byte-identical (hc_meat's la::xtx accumulates in a
            // different order; mathematically equal but not bit-identical under
            // -ffp-contract, which caught the drift against the P5 R2 baseline).
            for (int64_t i = 0; i < g.n; ++i) {
                double ei = e[static_cast<size_t>(i)];
                double e2 = ei * ei;
                for (int c = 0; c < g.k; ++c) {
                    double xc = g.X[static_cast<int64_t>(c) * g.n + i];
                    for (int d = 0; d < g.k; ++d)
                        meat[static_cast<size_t>(c) * g.k + d] += e2 * xc * g.X[static_cast<int64_t>(d) * g.n + i];
                }
            }
            // Stata applies the ML small-sample multiplier N/(N-1) to vce(robust)
            // (q_c, not the regress-style N/(N-k)); see g3_logit_probit_vce oracle.
            // Unchanged by pweight — pweight only changes the meat (via e above).
            factor = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
        } else {
            cecon::cluster_meat(g.X.data(), g.n, g.k, e.data(), g.cluster[0], meat.data(), &n_clusters);
            if (n_clusters < 2)
                throw EngineError("bad_input",
                    "logit: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
                    std::to_string(n_clusters));
            // ML cluster small-sample factor: G/(G-1) — NOT the regress-style CGM
            // (N-1)/(N-K) * G/(G-1) that vcov.cpp's compute_vcov uses for feols/2SLS.
            // Verified exact vs the P5 oracle `logit ... [pweight=pw], vce(cluster
            // id)` pin (200 clusters, N=4282): matches to <1e-6 relative. At G=N
            // (every obs its own cluster) this correctly reduces to the N/(N-1)
            // factor used for plain vce(robust) above.
            factor = static_cast<double>(n_clusters) / static_cast<double>(n_clusters - 1);
        }
        std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
        la::sandwich(r.bread.data(), meat.data(), g.k, factor, V.data(), tmp.data());
    }
    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        double se = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * g.k + c]));
        json row = {{"name", g.names[static_cast<size_t>(c)]}, {"beta", r.beta[c]},
                    {"se", se}, {"t", tstat(r.beta[c], se)},
                    {"se_iid", std::sqrt(std::max(0.0, r.bread[static_cast<size_t>(c) * g.k + c]))}};
        if (svy) {
            // Survey inference is t(design dof), not z — and it is emitted
            // unconditionally here, because under svy the p-value and CI are part
            // of the table Stata always prints, not a --level extra.
            const double t = tstat(r.beta[c], se);
            const double dd = static_cast<double>(svy_df);
            const double tcv = t_critical(1.0 - level / 100.0, dd);
            row["p"] = (se > 0) ? t_twosided_pvalue(t, dd) : 1.0;
            row["ci_lo"] = r.beta[c] - tcv * se;
            row["ci_hi"] = r.beta[c] + tcv * se;
        } else {
            // BUG-282: unconditional. Stata's logit table always carries P>|z| and
            // the 95% interval; gating them on --level made the default output a
            // strictly smaller table than the oracle's.
            double t = tstat(r.beta[c], se);
            row["p"] = (se > 0) ? z_twosided_pvalue(t) : 1.0;
            row["ci_lo"] = r.beta[c] - zc * se;
            row["ci_hi"] = r.beta[c] + zc * se;
        }
        coefs.push_back(row);
    }
    json out = {{"estimator", "logit"}, {"vce", vce}, {"n_obs", g.n},
                {"n_dropped_missing", g.n_dropped}, {"converged", r.converged},
                {"iters", r.iters}, {"coefs", coefs}};
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (wkind != cecon::WeightKind::None) { out["weight_var"] = weight_var; out["wtype"] = "pw"; }
    if (has(a, "--emit-v")) add_e_v(out, g.names, V);  // V = the matrix the se above came from
    if (svy) {
        out["vce"] = "svy_linearized";
        out["svy"] = {{"strata_var", strata_var}, {"psu_var", psu_var},
                      {"n_strata", svy_n_strata},  {"n_psu", svy_n_psu},
                      {"design_df", svy_df},       {"pop_size", svy_pop}};
        // Adjusted Wald F over the slopes, the same convention as `svy: regress`:
        // W·(d − q + 1)/(q·d) reported as F(q, d − q + 1). The intercept is the
        // LAST design column on this path (g.names.back()), not the first as in
        // feols — a detail worth stating, since testing for the wrong end silently
        // changes q and therefore both F and its degrees of freedom.
        const int q = g.k - 1;
        if (q > 0 && svy_df - q + 1 > 0) {
            std::vector<double> R(static_cast<size_t>(q) * g.k, 0.0), qv(static_cast<size_t>(q), 0.0);
            for (int j = 0; j < q; ++j) R[static_cast<size_t>(j) * g.k + j] = 1.0;
            cecon::WaldResult wr = cecon::wald_test(r.beta.data(), V.data(), g.k, R.data(), qv.data(), q);
            if (wr.ok) {
                const double f = wr.chi2 * static_cast<double>(svy_df - q + 1) /
                                 (static_cast<double>(q) * static_cast<double>(svy_df));
                out["F"] = f;
                out["p_F"] = f_pvalue(f, static_cast<double>(q), static_cast<double>(svy_df - q + 1));
                out["svy"]["f_df1"] = q;
                out["svy"]["f_df2"] = svy_df - q + 1;
            }
        }
    }
    // use_wald: Stata switches the header statistic from LR chi2 to a WALD chi2
    // test on the slope coefficients (using the sandwich V) whenever vce(cluster)
    // or a pweight is in play (both are NEW P5 capabilities, no prior byte-identity
    // constraint). Plain, unweighted vce(robust) keeps the OLD LR-style
    // add_mle_fit call — Stata does technically switch to Wald there too, but that
    // path is one of P5's required byte-identity baselines (unweighted logit
    // "iid + robust"), so it is deliberately left as a pre-existing gap rather
    // than "fixed" out from under the R2 byte-identity requirement.
    bool use_wald = (vce == "cluster") || (wkind != cecon::WeightKind::None);
    if (no_intercept) {
        add_mle_fit_nocons(out, r.p, g.y, wptr, g.n, r.beta, V, g.k);
    } else if (!use_wald) {
        add_mle_fit(out, r.p, g.y, g.n, g.k - 1);  // ll/ll_0/r2_pseudo/chi2/p_chi2 (since contract A 1.5.0)
    } else {
        // The LR test needs the true MLE Hessian, which a
        // sandwich VCE no longer represents. ll/ll_0/r2_pseudo stay McFadden-style
        // but computed from the (possibly pweight-)weighted log-likelihoods, which
        // is what Stata labels "Log pseudolikelihood" / "Pseudo R2" in this case.
        double ll = 0.0, swy = 0.0, sw = 0.0;
        for (int64_t i = 0; i < g.n; ++i) {
            double wi = wptr ? wptr[static_cast<size_t>(i)] : 1.0;
            double pi = std::min(1.0 - 1e-12, std::max(1e-12, r.p[static_cast<size_t>(i)]));
            double yi = g.y[static_cast<size_t>(i)];
            ll += wi * (yi * std::log(pi) + (1.0 - yi) * std::log(1.0 - pi));
            swy += wi * yi;
            sw += wi;
        }
        out["ll"] = ll;
        int q = g.k - 1;  // slopes only — g.names.back() is always "(intercept)"
        out["df_chi2"] = q;
        double ybar = (sw > 0.0) ? swy / sw : 0.0;
        if (ybar <= 0.0 || ybar >= 1.0) {
            out["ll_0"] = nullptr; out["r2_pseudo"] = nullptr;
            out["chi2"] = nullptr; out["p_chi2"] = nullptr;
        } else {
            double ll0 = sw * (ybar * std::log(ybar) + (1.0 - ybar) * std::log(1.0 - ybar));
            out["ll_0"] = ll0;
            out["r2_pseudo"] = (ll0 != 0.0) ? 1.0 - ll / ll0 : 0.0;
            double chi2 = std::numeric_limits<double>::quiet_NaN();
            if (q > 0) {
                std::vector<double> Vss(static_cast<size_t>(q) * q), bs(static_cast<size_t>(q));
                for (int aidx = 0; aidx < q; ++aidx) {
                    bs[static_cast<size_t>(aidx)] = r.beta[static_cast<size_t>(aidx)];
                    for (int bidx = 0; bidx < q; ++bidx)
                        Vss[static_cast<size_t>(aidx) + static_cast<size_t>(bidx) * q] =
                            V[static_cast<size_t>(aidx) + static_cast<size_t>(bidx) * g.k];
                }
                if (la::spd_inverse(Vss.data(), q) == 0) {
                    std::vector<double> tmp2(static_cast<size_t>(q));
                    la::matvec(Vss.data(), q, bs.data(), tmp2.data());
                    double wchi = 0.0;
                    for (int aidx = 0; aidx < q; ++aidx) wchi += bs[static_cast<size_t>(aidx)] * tmp2[static_cast<size_t>(aidx)];
                    chi2 = wchi;
                }
            }
            out["chi2"] = std::isfinite(chi2) ? json(chi2) : json(nullptr);
            out["p_chi2"] = (std::isfinite(chi2) && q > 0) ? json(chi2_pvalue(chi2, q)) : json(nullptr);
        }
    }
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return r.converged ? 0 : 1;
}

int cmd_probit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string vce = opt(a, "--vce", "iid");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra probit --data f.hbin --y Y --x X1,X2 [--vce iid|robust] [--no-intercept] [--level P] [--out f.json]\n");
        return 2;
    }
    // --level P (B.0.3): see cmd_logit — asymptotic z Wald p + CI per coef, flag-gated
    // (default output byte-identical), inline z critical from the level.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "probit: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    // BUG-282: unconditional, same reasoning as logit above.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const std::string offset_var = opt(a, "--offset");  // BUG-331, see cmd_logit
    GlmInputs g = load_glm(ds, "probit", y, split_csv(xcsv), {}, "", cecon::WeightKind::None,
                           offset_var);
    if (g.n == 0) { std::fprintf(stderr, "probit: no rows after dropping missing.\n"); return 1; }
    if (!check_binary(g.y, "probit", y)) return 2;
    // Stata `noconstant` (w2b_estim.log:706/:732, e(k) 2): see drop_glm_intercept.
    const bool no_intercept = has(a, "--no-intercept");
    if (no_intercept) drop_glm_intercept(g, "probit");
    const double* offptr = g.offset.empty() ? nullptr : g.offset.data();
    ProbitResult r = probit_fit(g.X.data(), g.n, g.k, g.y.data(), 100, 1e-14, offptr);
    if (r.separated_perfect) {
        std::fprintf(stderr, "probit: outcome %s predicts data perfectly; r(2000)\n", y.c_str());
        return 2;
    }
    // probit score:  s_i = (y-Φ) φ / (Φ(1-Φ)) · x   (λ_i = generalized residual).
    // meat = Σ s_i s_i'. Bread = OBSERVED-information inverse: Stata's probit uses
    // the observed Hessian, NOT the Fisher/expected info (X'WX)^{-1} that r.V
    // carries (probit is a non-canonical link, so the two differ in-sample; logit
    // is canonical, hence its bread stays r.bread). The observed information
    // weight is λ_i(λ_i+η_i); see g3_logit_probit_vce oracle. BUG-140: the DEFAULT
    // (iid) SE needs this same observed-information bread, not just vce(robust)'s
    // sandwich meat — Stata's plain `probit` already uses observed info, cobra's
    // iid branch was the only one still reading the Fisher-info r.V.
    //
    // BUG-210: this loop used to live here verbatim, and in two more places, and
    // `margins`' iid branches were left reading r.V because of that. It is now
    // ONE definition in src/estimators/probit.h; this is a delegation.
    // The SAME offset must reach the observed information, or the SEs would be
    // evaluated at a different eta than the fit converged to (probit.h).
    ProbitObservedInfo oi = probit_observed_info(r, g.X.data(), g.n, g.k, g.y.data(), offptr);
    const std::vector<double>& meat = oi.meat;
    const std::vector<double>& bread = oi.bread;
    std::vector<double> V = bread;   // default/iid SE (BUG-140: was r.V, the Fisher bread)
    if (vce == "robust") {
        // Stata applies the ML small-sample multiplier N/(N-1) to vce(robust)
        // (q_c, not the regress-style N/(N-k)); see g3_logit_probit_vce oracle.
        double rfac = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
        std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
        la::sandwich(bread.data(), meat.data(), g.k, rfac, V.data(), tmp.data());
    }
    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        double se = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * g.k + c]));
        json row = {{"name", g.names[static_cast<size_t>(c)]}, {"beta", r.beta[c]},
                    {"se", se}, {"t", tstat(r.beta[c], se)},
                    {"se_iid", std::sqrt(std::max(0.0, r.V[static_cast<size_t>(c) * g.k + c]))}};
        {
            // BUG-282: unconditional (Stata's probit table always carries them).
            double t = tstat(r.beta[c], se);
            row["p"] = (se > 0) ? z_twosided_pvalue(t) : 1.0;
            row["ci_lo"] = r.beta[c] - zc * se;
            row["ci_hi"] = r.beta[c] + zc * se;
        }
        coefs.push_back(row);
    }
    json out = {{"estimator", "probit"}, {"vce", vce}, {"n_obs", g.n},
                {"n_dropped_missing", g.n_dropped}, {"converged", r.converged},
                {"iters", r.iters}, {"coefs", coefs}};
    if (has(a, "--emit-v")) add_e_v(out, g.names, V);  // V = the matrix the se above came from
    if (no_intercept)
        add_mle_fit_nocons(out, r.p, g.y, nullptr, g.n, r.beta, V, g.k);
    else
        add_mle_fit(out, r.p, g.y, g.n, g.k - 1);  // ll/ll_0/r2_pseudo/chi2/p_chi2 (since contract A 1.5.0)
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return r.converged ? 0 : 1;
}

}  // namespace cli
}  // namespace cecon
