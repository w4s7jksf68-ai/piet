// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/logit.h"
#include "estimators/zicount.h"
#include "vcov/vcov.h"  // cluster_meat — fracreg's cluster sandwich reuses the shared helper

// P7 wave — zip, zinb, fracreg.
//
//   zip     zero-inflated Poisson           (Stata `zip y x…, inflate(_cons)`)
//   zinb    zero-inflated negative binomial (Stata `zinb y x…, inflate(_cons)`)
//   fracreg fractional-response quasi-MLE   (Stata `fracreg logit y x…`)
//
// The mixture math lives in src/estimators/zicount.h/.cpp; `fracreg` reuses
// logit.cpp's IRLS unchanged (the ONLY difference from `logit` is that the
// outcome is validated as 0 ≤ y ≤ 1 rather than y ∈ {0,1}, and that there is no
// iid VCE — Stata's fracreg is a quasi-MLE and always reports a sandwich).

namespace cecon {
namespace cli {

namespace {

// Two-sided normal p-value for a z statistic (the asymptotic inference Stata
// prints for all three of these commands).
double z_pvalue(double z) {
    return z_twosided_pvalue(z);
}

// Slope-block Wald χ² → the three header keys. `ks` slopes (the design contract
// puts the intercept LAST, so they are the leading ks parameters). When the block
// is numerically degenerate — zi::wald_chi2 says so — the statistic is reported
// MISSING (chi2/p null, df 0) instead of being read off a singular inverse; that
// is exactly what Stata prints for this wave's `fracreg logit` (`Wald chi2(0) = .`,
// `Prob > chi2 = .`), whose outcome is the model's own inverse-logit and whose
// robust meat is therefore at machine noise.
void add_wald(json& out, const std::vector<double>& V, int np,
              const std::vector<double>& beta, int ks) {
    double chi2 = 0;
    if (ks > 0 && zi::wald_chi2(V.data(), np, beta.data(), ks, chi2)) {
        out["chi2"] = chi2;
        out["df_chi2"] = ks;
        out["p_chi2"] = chi2_pvalue(chi2, static_cast<double>(ks));
    } else {
        out["chi2"] = nullptr;
        out["df_chi2"] = 0;
        out["p_chi2"] = nullptr;
    }
}

// Shared body of `zip` and `zinb` — identical option surface, sample handling and
// output shape; `nb` picks the count kernel (Poisson vs NB2) and the extra
// /lnalpha + alpha block.
int run_zi(const std::vector<std::string>& a, bool nb) {
    const char* cmd = nb ? "zinb" : "zip";
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    // Stata's inflate() is mandatory and takes a varlist; this build implements the
    // constant-only inflation equation, so `_cons` is the only accepted value and
    // the default. Anything else is an HONEST reject, never a silently-ignored
    // option that would report a constant-only fit under a varlist's name.
    std::string inflate = opt(a, "--inflate", "_cons");
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra %s --data f.hbin --y Y --x X1,X2 [--inflate _cons] "
                     "[--vce iid|robust|cluster] [--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  zero-inflated %s MLE with a constant-only inflation equation.\n",
                     cmd, nb ? "negative binomial" : "Poisson");
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
    if (inflate != "_cons")
        throw EngineError("unsupported",
                          std::string(cmd) + ": only --inflate _cons (a constant-only inflation "
                          "equation) is implemented; got '" + inflate + "'");
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "%s: --level must be in [10, 99.99] (got %s).\n", cmd,
                     opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, cmd, y, split_csv(xcsv), cluster_names);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    for (double v : g.y)
        if (v < 0.0 || v != std::floor(v))
            throw EngineError("bad_input",
                              std::string(cmd) + ": outcome '" + y +
                              "' must be a non-negative count (found " + std::to_string(v) + ")");

    ZiResult r = nb ? zinb_fit(g.X.data(), g.n, g.k, g.y.data())
                    : zip_fit(g.X.data(), g.n, g.k, g.y.data());
    if (!r.ok) { std::fprintf(stderr, "%s: MLE failed.\n", cmd); return 1; }
    if (!r.converged)
        std::fprintf(stderr, "%s: warning — the likelihood did not fully converge.\n", cmd);

    const int np = r.np;
    std::vector<double> V = r.vcov;  // iid = observed-information inverse
    int n_clusters = 0;
    if (vce == "robust") {
        // ML vce(robust) small-sample multiplier N/(N-1) — the ML factor (q_c) Stata
        // applies to logit/probit/tobit/nbreg, NOT the regress-style N/(N-k). VERIFIED
        // against the P7 oracle (`zip`/`zinb ... vce(robust)`, N=4,671): every SE,
        // including /lnalpha, matches to <3.5e-6 relative in tests/unit/test_zicount.cpp.
        // The rivals were run against those same pins and REJECTED — a bare sandwich
        // is off by 1.07e-4 relative and N/(N-np) by 3.21e-4, i.e. 10x and 32x the
        // pin's 1e-5 tolerance. Measured, not inherited on faith.
        double q = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
        V = zi_robust_vcov(g.X.data(), g.n, g.k, g.y.data(), r, q);
    } else if (vce == "cluster") {
        if (g.cluster.empty() || g.cluster[0].n_levels < 2)
            throw EngineError("bad_input",
                              std::string(cmd) + ": vce(cluster) requires at least 2 clusters, but the "
                              "cluster variable has " + std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));
        n_clusters = g.cluster[0].n_levels;
        // ML cluster factor G/(G-1) — the same one cmd_logit's cluster branch carries
        // (verified there against the P5 oracle). NOT oracle-pinned for zip/zinb: this
        // wave's oracle only exercises vce(robust). At G=N it reduces to the N/(N-1)
        // above, which IS pinned.
        double q = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
        std::vector<int> codes(g.cluster[0].codes.begin(), g.cluster[0].codes.end());
        V = zi_cluster_vcov(g.X.data(), g.n, g.k, g.y.data(), r, codes.data(), n_clusters, q);
    }
    if (static_cast<int>(V.size()) != np * np) {
        std::fprintf(stderr, "%s: vce(%s) failed (non-invertible sandwich).\n", cmd, vce.c_str());
        return 1;
    }
    auto se_at = [&](int j) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(j) * np + j])); };

    json coefs = json::array();
    for (int j = 0; j < g.k; ++j) {
        double se = se_at(j), b = r.beta[static_cast<size_t>(j)], z = tstat(b, se);
        coefs.push_back({{"name", g.names[static_cast<size_t>(j)]}, {"b", b}, {"se", se},
                         {"z", z}, {"p", (se > 0) ? z_pvalue(z) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    json infl = json::array();
    {
        double se = se_at(g.k), b = r.gamma0, z = tstat(b, se);
        infl.push_back({{"name", "(intercept)"}, {"b", b}, {"se", se},
                        {"z", z}, {"p", (se > 0) ? z_pvalue(z) : 1.0},
                        {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }

    json out = {{"estimator", cmd}, {"inflate_spec", "_cons"}, {"vce", vce},
                {"n_obs", r.n}, {"n_zero", r.n_zero}, {"n_nonzero", r.n_nonzero},
                {"n_dropped_missing", g.n_dropped},
                {"converged", r.converged}, {"iters", r.iters},
                {"coefs", coefs}, {"inflate", infl},
                {"ll", r.ll}, {"ll_const", r.ll_const}};
    if (nb) {
        double se_lna = se_at(g.k + 1);
        out["lnalpha"] = {{"b", r.lnalpha}, {"se", se_lna},
                          {"ci_lo", r.lnalpha - zc * se_lna}, {"ci_hi", r.lnalpha + zc * se_lna}};
        // α = exp(lnα); se(α) = α·se(lnα) (delta rule), CI = exp of the lnα CI — the
        // transform Stata's own alpha row prints, and the one the oracle's
        // .0568374 × .1058344 = .0060153 arithmetic confirms.
        out["alpha"] = {{"b", r.alpha}, {"se", r.alpha * se_lna},
                        {"ci_lo", std::exp(r.lnalpha - zc * se_lna)},
                        {"ci_hi", std::exp(r.lnalpha + zc * se_lna)}};
    }
    // Wald χ² on the count-equation SLOPES only (the intercept is the last design
    // column; the inflation and lnα rows are not part of the model test).
    add_wald(out, V, np, r.beta, g.k - 1);
    // With the default VCE Stata's model test is the likelihood-ratio statistic
    // against the constant-only fit (both equations' constants): bank f3102717
    // `zip ten age grade, inflate(_cons)` LR chi2(2) 130.93315817298753 =
    // 2(e(ll) - e(ll_0)) = 2(-8102.7752646 + 8168.2418437), where the Wald said 131.30.
    if (vce == "iid" && g.k - 1 > 0 && std::isfinite(r.ll_const) && r.ll_const != 0.0) {
        const double lr = 2.0 * (r.ll - r.ll_const);
        out["chi2"] = lr;
        out["df_chi2"] = g.k - 1;
        out["p_chi2"] = chi2_pvalue(lr, static_cast<double>(g.k - 1));
        out["chi2type"] = "LR";
    }
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace

int cmd_zip(const std::vector<std::string>& a) { return run_zi(a, false); }

int cmd_zinb(const std::vector<std::string>& a) { return run_zi(a, true); }

// The Bernoulli quasi-log-likelihood of `fracreg logit` for y in [0,1]:
// sum_i y_i eta_i - ln(1 + exp(eta_i)), score y_i - p_i, curvature p_i (1 - p_i).
class FracLogitObjective final : public MlObjective {
public:
    FracLogitObjective(const double* X, int64_t n, int k, const double* y)
        : X_(X), n_(n), k_(k), y_(y), eta_(static_cast<size_t>(n)), s_(static_cast<size_t>(n)),
          scaled_(static_cast<size_t>(n) * static_cast<size_t>(k)) {}
    double loglik(const std::vector<double>& b) override {
        index(b);
        double ll = 0.0;
        for (int64_t i = 0; i < n_; ++i) ll += term(eta_[static_cast<size_t>(i)], y_[i]);
        return ll;
    }
    double evaluate(const std::vector<double>& b, std::vector<double>& g, std::vector<double>& A) override {
        index(b);
        double ll = 0.0;
        for (int64_t i = 0; i < n_; ++i) {
            const size_t ii = static_cast<size_t>(i);
            const double e = eta_[ii];
            ll += term(e, y_[i]);
            const double p = 1.0 / (1.0 + std::exp(-e));
            s_[ii] = y_[i] - p;
            const double root = std::sqrt(std::max(0.0, p * (1.0 - p)));
            for (int c = 0; c < k_; ++c)
                scaled_[static_cast<size_t>(c) * static_cast<size_t>(n_) + ii] =
                    X_[static_cast<size_t>(c) * static_cast<size_t>(n_) + ii] * root;
        }
        g.assign(static_cast<size_t>(k_), 0.0);
        A.assign(static_cast<size_t>(k_) * static_cast<size_t>(k_), 0.0);
        la::xtv(X_, n_, k_, s_.data(), g.data());
        la::xtx(scaled_.data(), n_, k_, A.data());
        return ll;
    }

private:
    void index(const std::vector<double>& b) { la::mat_vec(X_, n_, k_, b.data(), eta_.data()); }
    // y eta - ln(1 + e^eta), without overflow for a large |eta|
    static double term(double e, double y) {
        const double soft = e > 0 ? e + std::log1p(std::exp(-e)) : std::log1p(std::exp(e));
        return y * e - soft;
    }
    const double* X_;
    int64_t n_;
    int k_;
    const double* y_;
    std::vector<double> eta_, s_, scaled_;
};

// Stata's starting point for fracreg: the linear regression of y on the design.
std::vector<double> frac_ols_start(const double* X, int64_t n, int k, const double* y) {
    std::vector<double> A(static_cast<size_t>(k) * static_cast<size_t>(k), 0.0), b(static_cast<size_t>(k), 0.0),
        xy(static_cast<size_t>(k), 0.0);
    la::xtx(X, n, k, A.data());
    la::xtv(X, n, k, y, xy.data());
    if (la::spd_inverse(A.data(), k) != 0) return {};
    for (int r = 0; r < k; ++r)
        for (int c = 0; c < k; ++c)
            b[static_cast<size_t>(r)] += A[static_cast<size_t>(r) * static_cast<size_t>(k) + static_cast<size_t>(c)] *
                                         xy[static_cast<size_t>(c)];
    return b;
}

// fracreg — fractional response quasi-MLE (Stata `fracreg logit y x…`): ml's
// modified Newton-Raphson on the Bernoulli quasi-likelihood from the linear
// regression's coefficients, stopping where ml's documented rule stops (StataBE
// 19.5 bank 2bef1615 `fracreg logit fr weight length`: e(ic) 3, e(ll)
// -44.450904071827146; e(ll_0) is the constant-only fit's, started at mean(y) the
// same way). The bread of the sandwich is the inverse of -H at the final iterate.
int cmd_fracreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string link = opt(a, "--link", "logit");
    std::string vce = opt(a, "--vce", "robust");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra fracreg --data f.hbin --y Y --x X1,X2 [--link logit] "
                             "[--vce robust|cluster] [--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                             "  fractional-response quasi-MLE for 0 <= y <= 1; the VCE is always a\n"
                             "  sandwich (Stata's fracreg has no iid VCE).\n");
        return 2;
    }
    if (link != "logit")
        throw EngineError("unsupported",
                          "fracreg: only --link logit is implemented (got '" + link + "')");
    if (vce != "robust" && vce != "cluster")
        throw EngineError("bad_input",
                          "fracreg: --vce must be robust or cluster; the quasi-MLE has no iid VCE "
                          "(got '" + vce + "')");
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "fracreg: --vce cluster requires --cluster C.\n");
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "fracreg: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, "fracreg", y, split_csv(xcsv), cluster_names);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    for (double v : g.y)
        if (!(v >= 0.0 && v <= 1.0))
            throw EngineError("bad_input",
                              "fracreg: outcome '" + y + "' must satisfy 0 <= y <= 1 (found " +
                              std::to_string(v) + ")");

    MlMaxOptions mo;
    if (has(a, "--iterate")) mo.maxiter = std::stoi(opt(a, "--iterate"));
    std::vector<double> start = frac_ols_start(g.X.data(), g.n, g.k, g.y.data());
    if (start.empty()) { std::fprintf(stderr, "fracreg: quasi-MLE failed (singular information).\n"); return 1; }
    FracLogitObjective obj(g.X.data(), g.n, g.k, g.y.data());
    const MlPath path = ml_newton_path(obj, start, mo);
    bool finite = !path.failed && std::isfinite(path.ll) && path.inv_curvature.size() ==
                  static_cast<size_t>(g.k) * static_cast<size_t>(g.k);
    for (double v : path.b) finite = finite && std::isfinite(v);
    if (!finite) { std::fprintf(stderr, "fracreg: quasi-MLE failed (singular information).\n"); return 1; }
    struct { std::vector<double> beta, bread; } r{path.b, path.inv_curvature};

    // Sandwich: score e_i = y_i - p_i (the quasi-likelihood's), bread = inv(-H).
    std::vector<double> eta(static_cast<size_t>(g.n));
    la::mat_vec(g.X.data(), g.n, g.k, r.beta.data(), eta.data());
    std::vector<double> e(static_cast<size_t>(g.n)), pr(static_cast<size_t>(g.n));
    for (int64_t i = 0; i < g.n; ++i) {
        pr[static_cast<size_t>(i)] = 1.0 / (1.0 + std::exp(-eta[static_cast<size_t>(i)]));
        e[static_cast<size_t>(i)] = g.y[static_cast<size_t>(i)] - pr[static_cast<size_t>(i)];
    }
    std::vector<double> meat(static_cast<size_t>(g.k) * g.k, 0.0);
    double factor;
    int n_clusters = 0;
    if (vce == "robust") {
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = e[static_cast<size_t>(i)], e2 = ei * ei;
            for (int c = 0; c < g.k; ++c) {
                double xc = g.X[static_cast<int64_t>(c) * g.n + i];
                for (int d = 0; d < g.k; ++d)
                    meat[static_cast<size_t>(c) * g.k + d] += e2 * xc * g.X[static_cast<int64_t>(d) * g.n + i];
            }
        }
        // Same ML small-sample multiplier as logit/probit/tobit vce(robust).
        factor = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
    } else {
        cecon::cluster_meat(g.X.data(), g.n, g.k, e.data(), g.cluster[0], meat.data(), &n_clusters);
        if (n_clusters < 2)
            throw EngineError("bad_input",
                              "fracreg: vce(cluster) requires at least 2 clusters, but the cluster "
                              "variable has " + std::to_string(n_clusters));
        factor = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
    }
    std::vector<double> V(static_cast<size_t>(g.k) * g.k, 0.0), tmp(static_cast<size_t>(g.k) * g.k, 0.0);
    la::sandwich(r.bread.data(), meat.data(), g.k, factor, V.data(), tmp.data());

    json coefs = json::array();
    for (int j = 0; j < g.k; ++j) {
        double se = std::sqrt(std::max(0.0, V[static_cast<size_t>(j) * g.k + j]));
        double b = r.beta[static_cast<size_t>(j)], z = tstat(b, se);
        coefs.push_back({{"name", g.names[static_cast<size_t>(j)]}, {"b", b}, {"se", se},
                         {"z", z}, {"p", (se > 0) ? z_pvalue(z) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    const double ll = path.ll;
    // e(ll_0): the constant-only quasi-likelihood fit, under the same maximize options
    double ll0 = 0.0;
    {
        std::vector<double> ones(static_cast<size_t>(g.n), 1.0);
        double ybar = 0.0;
        for (double v : g.y) ybar += v;
        ybar /= static_cast<double>(g.n);
        FracLogitObjective o0(ones.data(), g.n, 1, g.y.data());
        const MlPath p0 = ml_newton_path(o0, std::vector<double>{ybar}, mo);
        ll0 = (!p0.failed && std::isfinite(p0.ll)) ? p0.ll : frac::null_loglik(g.y.data(), g.n);
        if (ybar <= 0.0 || ybar >= 1.0) ll0 = 0.0;
    }
    json out = {{"estimator", "fracreg"}, {"depvar", y}, {"link", "logit"}, {"vce", vce},
                {"n_obs", g.n}, {"n_dropped_missing", g.n_dropped},
                {"converged", path.converged}, {"iters", path.ic},
                {"coefs", coefs}, {"ll", ll}};
    // ml's record: e(ic), e(ilog), e(gradient) and e(V_modelbased) (inverse of -H)
    out["ic"] = path.ic;
    out["ilog"] = path.ilog;
    {
        json gv = json::array();
        for (double v : path.gradient) gv.push_back(std::isfinite(v) ? json(v) : json(nullptr));
        out["e_gradient"] = {{"names", g.names}, {"values", gv}};
        json tmp;
        add_e_v(tmp, g.names, path.inv_curvature);
        out["e_Vmb"] = std::move(tmp["e_V"]);
    }
    if (has(a, "--emit-v")) add_e_v(out, g.names, V);
    if (ll0 != 0.0) {
        out["ll_0"] = ll0;
        out["r2_pseudo"] = 1.0 - ll / ll0;
    } else {  // degenerate outcome (ȳ at 0 or 1) — the null model is undefined
        out["ll_0"] = nullptr;
        out["r2_pseudo"] = nullptr;
    }
    add_wald(out, V, g.k, r.beta, g.k - 1);
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
