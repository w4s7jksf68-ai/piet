// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/xtreg.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "core/linalg.h"

namespace cecon {

namespace {

struct Group { int64_t start, end; };

// Solve (X'X) β = X'y by Cholesky. Returns 0 on success.
int ols_solve(const double* X, int64_t n, int k, const double* y,
              std::vector<double>& beta, std::vector<double>& XtXi) {
    XtXi.assign(static_cast<size_t>(k) * k, 0.0);
    la::xtx(X, n, k, XtXi.data());
    if (la::spd_inverse(XtXi.data(), k) != 0) return 1;
    std::vector<double> Xty(static_cast<size_t>(k));
    la::xtv(X, n, k, y, Xty.data());
    beta.assign(static_cast<size_t>(k), 0.0);
    la::matvec(XtXi.data(), k, Xty.data(), beta.data());
    return 0;
}

}  // namespace

XtregReResult xtreg_re(const double* X_in, int64_t n_obs, int k,
                       const double* y_in, const double* ivar_in, bool emit_within,
                       const double* cluster_in, bool sa) {
    XtregReResult R;
    R.k = k;

    // ---- 1. Build group index, sort rows by group (first-appearance order).
    // Skip rows with NaN in y or any covariate (Stata's standard behavior).
    std::unordered_map<double, int> idx_of;
    std::vector<std::vector<int64_t>> rows_per_g;
    rows_per_g.reserve(8192);
    for (int64_t r = 0; r < n_obs; ++r) {
        if (std::isnan(y_in[r])) continue;
        bool x_ok = true;
        for (int c = 0; c < k; ++c)
            if (std::isnan(X_in[static_cast<int64_t>(c) * n_obs + r])) { x_ok = false; break; }
        if (!x_ok) continue;
        double v = ivar_in[r];
        auto it = idx_of.find(v);
        int gi;
        if (it == idx_of.end()) {
            gi = static_cast<int>(rows_per_g.size());
            idx_of[v] = gi;
            rows_per_g.emplace_back();
        } else gi = it->second;
        rows_per_g[static_cast<size_t>(gi)].push_back(r);
    }
    int64_t n_g = static_cast<int64_t>(rows_per_g.size());
    R.n_groups = n_g;

    // Pack working arrays group-contiguous.
    std::vector<Group> groups(static_cast<size_t>(n_g));
    int64_t N = n_obs;
    std::vector<double> X(static_cast<size_t>(N) * k);  // col-major N*k (no intercept)
    std::vector<double> y(static_cast<size_t>(N));
    std::vector<double> clv(cluster_in != nullptr ? static_cast<size_t>(N) : 0);
    int64_t cur = 0;
    for (int64_t gi = 0; gi < n_g; ++gi) {
        const auto& rs = rows_per_g[static_cast<size_t>(gi)];
        groups[static_cast<size_t>(gi)] = {cur, cur + static_cast<int64_t>(rs.size())};
        for (int64_t r : rs) {
            if (cluster_in != nullptr) clv[static_cast<size_t>(cur)] = cluster_in[r];
            y[static_cast<size_t>(cur)] = y_in[r];
            for (int c = 0; c < k; ++c)
                X[static_cast<size_t>(c) * N + cur] = X_in[static_cast<int64_t>(c) * n_obs + r];
            ++cur;
        }
    }
    R.n_obs = N;

    // ---- 2. Group means ȳ_i and X̄_i, plus grand means.
    std::vector<double> ybar(static_cast<size_t>(n_g), 0.0);
    std::vector<double> Xbar(static_cast<size_t>(n_g) * k, 0.0);
    double y_gm = 0;
    std::vector<double> X_gm(static_cast<size_t>(k), 0.0);
    for (int64_t gi = 0; gi < n_g; ++gi) {
        const auto& g = groups[static_cast<size_t>(gi)];
        double Ti = static_cast<double>(g.end - g.start);
        double sy = 0;
        for (int64_t i = g.start; i < g.end; ++i) sy += y[static_cast<size_t>(i)];
        ybar[static_cast<size_t>(gi)] = sy / Ti;
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int64_t i = g.start; i < g.end; ++i)
                s += X[static_cast<size_t>(c) * N + i];
            Xbar[static_cast<size_t>(gi) * k + static_cast<size_t>(c)] = s / Ti;
        }
    }
    y_gm = 0;
    for (int64_t i = 0; i < N; ++i) y_gm += y[static_cast<size_t>(i)];
    y_gm /= static_cast<double>(N);
    for (int c = 0; c < k; ++c) {
        double s = 0;
        for (int64_t i = 0; i < N; ++i) s += X[static_cast<size_t>(c) * N + i];
        X_gm[static_cast<size_t>(c)] = s / static_cast<double>(N);
    }

    // ---- 3. Within FE residuals: e_it = (y_it - ȳ_i) - (X_it - X̄_i) β_w.
    // Time-invariant covariates (within-demeaned = 0 for all rows) give a
    // singular within OLS — Stata drops them from the within regression while
    // keeping them in the GLS step. We detect and exclude them here.
    std::vector<double> y_dm(static_cast<size_t>(N));
    std::vector<double> X_dm(static_cast<size_t>(N) * k);
    for (int64_t gi = 0; gi < n_g; ++gi) {
        const auto& g = groups[static_cast<size_t>(gi)];
        double yb = ybar[static_cast<size_t>(gi)];
        for (int64_t i = g.start; i < g.end; ++i)
            y_dm[static_cast<size_t>(i)] = y[static_cast<size_t>(i)] - yb;
        for (int c = 0; c < k; ++c) {
            double xb = Xbar[static_cast<size_t>(gi) * k + static_cast<size_t>(c)];
            for (int64_t i = g.start; i < g.end; ++i)
                X_dm[static_cast<size_t>(c) * N + i] =
                    X[static_cast<size_t>(c) * N + i] - xb;
        }
    }
    // Identify time-varying columns (within column has |max| > tol).
    std::vector<int> var_cols;
    for (int c = 0; c < k; ++c) {
        double m = 0;
        for (int64_t i = 0; i < N; ++i) {
            double v = std::fabs(X_dm[static_cast<size_t>(c) * N + i]);
            if (v > m) m = v;
        }
        if (m > 1e-12) var_cols.push_back(c);
    }
    int kv = static_cast<int>(var_cols.size());
    // dof = N - n_g - kv (FE absorbs n_g group means; within OLS estimates kv slopes;
    // each time-invariant dropped covariate would be absorbed by FE, contributing 0).
    const double dof_e = static_cast<double>(N) - static_cast<double>(n_g) - static_cast<double>(kv);
    R.df_e_within = dof_e;
    std::vector<double> X_dm_v(static_cast<size_t>(N) * std::max(1, kv));
    for (int j = 0; j < kv; ++j) {
        int c = var_cols[static_cast<size_t>(j)];
        std::copy(X_dm.begin() + static_cast<int64_t>(c) * N,
                  X_dm.begin() + static_cast<int64_t>(c + 1) * N,
                  X_dm_v.begin() + static_cast<int64_t>(j) * N);
    }
    std::vector<double> beta_w_v, XtXi_w_v;
    if (kv > 0) {
        if (ols_solve(X_dm_v.data(), N, kv, y_dm.data(), beta_w_v, XtXi_w_v) != 0) {
            R.ok = false; return R;
        }
    }
    // SSR_within = Σ (y_dm - X_dm_v β_w_v)²
    double SSR_w = 0;
    for (int64_t i = 0; i < N; ++i) {
        double yh = 0;
        for (int j = 0; j < kv; ++j)
            yh += X_dm_v[static_cast<size_t>(j) * N + i] * beta_w_v[static_cast<size_t>(j)];
        double e = y_dm[static_cast<size_t>(i)] - yh;
        SSR_w += e * e;
    }
    double sigma_e2 = SSR_w / dof_e;
    R.sigma_e = std::sqrt(std::max(0.0, sigma_e2));
    if (emit_within && dof_e > 0) {
        // Recover the within fit on the original scale. The intercept is
        // ȳ - x̄β; its covariance includes the sampling variance of ȳ and
        // the variance of x̄β. Invariant columns stay zero on the full stripe.
        const int width = k + 1;
        R.within_beta.assign(static_cast<size_t>(width), 0.0);
        R.within_vcov.assign(static_cast<size_t>(width) * width, 0.0);
        double intercept = y_gm;
        double intercept_var = sigma_e2 / static_cast<double>(N);
        for (int j = 0; j < kv; ++j) {
            const int column = var_cols[static_cast<size_t>(j)];
            R.within_beta[column] = beta_w_v[j];
            intercept -= X_gm[column] * beta_w_v[j];
            double intercept_cov = 0.0;
            for (int i = 0; i < kv; ++i) {
                const int row = var_cols[static_cast<size_t>(i)];
                const double cov = sigma_e2 * XtXi_w_v[i + static_cast<size_t>(j) * kv];
                R.within_vcov[row + static_cast<size_t>(column) * width] = cov;
                intercept_cov -= X_gm[row] * cov;
            }
            R.within_vcov[column + static_cast<size_t>(k) * width] = intercept_cov;
            R.within_vcov[k + static_cast<size_t>(column) * width] = intercept_cov;
            intercept_var -= X_gm[column] * intercept_cov;
        }
        R.within_beta[k] = intercept;
        R.within_vcov[k + static_cast<size_t>(k) * width] = intercept_var;
    }

    // ---- 4. Between OLS: ȳ_i ~ X̄_i + 1 (UNWEIGHTED on n_g obs). Returns
    // (α_b, β_b) and SSR_b = Σ (ȳ_i - α_b - X̄_i β_b)².
    std::vector<double> Xb_aug(static_cast<size_t>(n_g) * (k + 1));   // [Xbar | 1]
    for (int c = 0; c < k; ++c)
        for (int64_t gi = 0; gi < n_g; ++gi)
            Xb_aug[static_cast<size_t>(c) * n_g + gi] =
                Xbar[static_cast<size_t>(gi) * k + static_cast<size_t>(c)];
    for (int64_t gi = 0; gi < n_g; ++gi) Xb_aug[static_cast<size_t>(k) * n_g + gi] = 1.0;
    std::vector<double> beta_b, XtXi_b;
    if (ols_solve(Xb_aug.data(), n_g, k + 1, ybar.data(), beta_b, XtXi_b) != 0) {
        R.ok = false; return R;
    }
    double SSR_b = 0;
    for (int64_t gi = 0; gi < n_g; ++gi) {
        double yh = 0;
        for (int c = 0; c < k + 1; ++c)
            yh += Xb_aug[static_cast<size_t>(c) * n_g + gi] * beta_b[static_cast<size_t>(c)];
        double e = ybar[static_cast<size_t>(gi)] - yh;
        SSR_b += e * e;
    }

    // T_h = harmonic mean of T_i. T_i = group size.
    double inv_T = 0;
    for (const auto& g : groups) inv_T += 1.0 / static_cast<double>(g.end - g.start);
    double T_h = static_cast<double>(n_g) / inv_T;
    R.T_h = T_h;

    // σ_u² (Stata default σ_uT): max(0, SSR_b/(n - K) - σ_e² / T_h). Here K = k+1.
    double sigma_u2 = SSR_b / (static_cast<double>(n_g) - static_cast<double>(k + 1)) - sigma_e2 / T_h;
    bool t_const = true;
    for (const auto& g : groups)
        if (g.end - g.start != groups.front().end - groups.front().start) { t_const = false; break; }
    if (sa && !t_const) {
        // Baltagi-Chang small-sample Swamy-Arora sigma_u (see the header).
        const int ka = k + 1;
        double ssr_star = 0;
        std::vector<double> Xw(static_cast<size_t>(n_g) * ka);  // rows sqrt(T_i) x̄_i
        std::vector<double> B(static_cast<size_t>(ka) * ka, 0.0);  // Σ T_i² x̄_i x̄_i'
        for (int64_t gi = 0; gi < n_g; ++gi) {
            const double Ti = static_cast<double>(groups[static_cast<size_t>(gi)].end
                                                  - groups[static_cast<size_t>(gi)].start);
            double yh = 0;
            for (int c = 0; c < ka; ++c)
                yh += Xb_aug[static_cast<size_t>(c) * n_g + gi] * beta_b[static_cast<size_t>(c)];
            const double e = ybar[static_cast<size_t>(gi)] - yh;
            ssr_star += Ti * e * e;
            for (int c = 0; c < ka; ++c)
                Xw[static_cast<size_t>(c) * n_g + gi] =
                    std::sqrt(Ti) * Xb_aug[static_cast<size_t>(c) * n_g + gi];
            for (int a = 0; a < ka; ++a)
                for (int b = 0; b < ka; ++b)
                    B[static_cast<size_t>(b) * ka + a] += Ti * Ti *
                        Xb_aug[static_cast<size_t>(a) * n_g + gi] * Xb_aug[static_cast<size_t>(b) * n_g + gi];
        }
        std::vector<double> Ainv, unused;
        if (ols_solve(Xw.data(), n_g, ka, ybar.data(), unused, Ainv) != 0) {
            R.ok = false; return R;
        }
        double tr = 0;
        for (int a = 0; a < ka; ++a)
            for (int b = 0; b < ka; ++b)
                tr += Ainv[static_cast<size_t>(b) * ka + a] * B[static_cast<size_t>(a) * ka + b];
        sigma_u2 = (ssr_star - (static_cast<double>(n_g) - static_cast<double>(ka)) * sigma_e2) /
                   (static_cast<double>(N) - tr);
        if (!std::isfinite(sigma_u2)) sigma_u2 = 0;
    }
    if (sigma_u2 < 0) sigma_u2 = 0;
    R.sigma_u = std::sqrt(sigma_u2);
    R.rho = (sigma_u2 + sigma_e2 > 0)
              ? (sigma_u2 / (sigma_u2 + sigma_e2)) : 0.0;

    // ---- 5. θ_i = 1 - σ_e / sqrt(T_i σ_u² + σ_e²).
    std::vector<double> theta(static_cast<size_t>(n_g));
    for (int64_t gi = 0; gi < n_g; ++gi) {
        double Ti = static_cast<double>(groups[static_cast<size_t>(gi)].end
                                       - groups[static_cast<size_t>(gi)].start);
        double denom = std::sqrt(Ti * sigma_u2 + sigma_e2);
        theta[static_cast<size_t>(gi)] = (denom > 0) ? (1.0 - R.sigma_e / denom) : 0.0;
    }

    // ---- 6. GLS transform: y* = y - θ ȳ, X* = X - θ X̄, c* = (1 - θ).
    // Run OLS on [X* | c*] ~ y*.
    std::vector<double> y_star(static_cast<size_t>(N));
    std::vector<double> X_star(static_cast<size_t>(N) * (k + 1));
    for (int64_t gi = 0; gi < n_g; ++gi) {
        const auto& g = groups[static_cast<size_t>(gi)];
        double th = theta[static_cast<size_t>(gi)];
        double yb = ybar[static_cast<size_t>(gi)];
        for (int64_t i = g.start; i < g.end; ++i)
            y_star[static_cast<size_t>(i)] = y[static_cast<size_t>(i)] - th * yb;
        for (int c = 0; c < k; ++c) {
            double xb = Xbar[static_cast<size_t>(gi) * k + static_cast<size_t>(c)];
            for (int64_t i = g.start; i < g.end; ++i)
                X_star[static_cast<size_t>(c) * N + i] =
                    X[static_cast<size_t>(c) * N + i] - th * xb;
        }
        for (int64_t i = g.start; i < g.end; ++i)
            X_star[static_cast<size_t>(k) * N + i] = 1.0 - th;
    }

    // The GLS regression with cluster() leaves out the rows whose cluster is missing;
    // the variance components above used every row (see the header).
    if (cluster_in != nullptr &&
        std::any_of(clv.begin(), clv.end(), [](double v) { return std::isnan(v); })) {
        int64_t Ng = 0;
        for (double v : clv) if (!std::isnan(v)) ++Ng;
        std::vector<double> Xg(static_cast<size_t>(Ng) * (k + 1)), yg(static_cast<size_t>(Ng)),
            cg(static_cast<size_t>(Ng));
        int64_t w = 0;
        for (int64_t i = 0; i < N; ++i) {
            if (std::isnan(clv[static_cast<size_t>(i)])) continue;
            yg[static_cast<size_t>(w)] = y_star[static_cast<size_t>(i)];
            cg[static_cast<size_t>(w)] = clv[static_cast<size_t>(i)];
            for (int c = 0; c < k + 1; ++c)
                Xg[static_cast<size_t>(c) * Ng + w] = X_star[static_cast<size_t>(c) * N + i];
            ++w;
        }
        X_star.swap(Xg);
        y_star.swap(yg);
        clv.swap(cg);
        N = Ng;
        R.n_obs = Ng;
        if (N <= k + 1) { R.ok = false; return R; }
    }
    std::vector<double> beta_re, XtXi_re;
    if (ols_solve(X_star.data(), N, k + 1, y_star.data(), beta_re, XtXi_re) != 0) {
        R.ok = false; return R;
    }
    // SE: standard OLS asymptotics on transformed system. Var = σ²_resid * (X*'X*)^{-1}.
    // Stata's "conventional" SE uses σ² estimated from the transformed residuals
    // with denominator N - k - 1 (incl. the transformed constant).
    double SSR_re = 0;
    for (int64_t i = 0; i < N; ++i) {
        double yh = 0;
        for (int c = 0; c < k + 1; ++c)
            yh += X_star[static_cast<size_t>(c) * N + i] * beta_re[static_cast<size_t>(c)];
        double e = y_star[static_cast<size_t>(i)] - yh;
        SSR_re += e * e;
    }
    double sigma2_re = SSR_re / (static_cast<double>(N) - static_cast<double>(k + 1));
    R.rmse = std::sqrt(sigma2_re);
    R.beta.assign(beta_re.begin(), beta_re.end());
    R.se.assign(static_cast<size_t>(k + 1), 0.0);
    // Full FGLS covariance V = sigma2_re * (X*'X*)^{-1}  ((k+1)*(k+1), col-major),
    // exposed for the Hausman test (needs the full V of the common slopes).
    R.vcov_matrix.assign(static_cast<size_t>(k + 1) * (k + 1), 0.0);
    for (size_t i = 0; i < R.vcov_matrix.size(); ++i)
        R.vcov_matrix[i] = sigma2_re * XtXi_re[i];
    if (cluster_in != nullptr) {
        // Stata's regress cluster() sandwich on the transformed system (see the header).
        const int ka = k + 1;
        std::unordered_map<double, int64_t> cidx;
        std::vector<double> score;  // G * ka, row g = sum over the cluster of x*_i e*_i
        for (int64_t i = 0; i < N; ++i) {
            double yh = 0;
            for (int c = 0; c < ka; ++c)
                yh += X_star[static_cast<size_t>(c) * N + i] * beta_re[static_cast<size_t>(c)];
            const double e = y_star[static_cast<size_t>(i)] - yh;
            auto it = cidx.find(clv[static_cast<size_t>(i)]);
            int64_t g;
            if (it == cidx.end()) {
                g = static_cast<int64_t>(cidx.size());
                cidx.emplace(clv[static_cast<size_t>(i)], g);
                score.resize(score.size() + static_cast<size_t>(ka), 0.0);
            } else {
                g = it->second;
            }
            for (int c = 0; c < ka; ++c)
                score[static_cast<size_t>(g) * ka + c] += X_star[static_cast<size_t>(c) * N + i] * e;
        }
        const int64_t G = static_cast<int64_t>(cidx.size());
        R.n_clusters = G;
        if (G >= 2 && N > ka) {
            std::vector<double> meat(static_cast<size_t>(ka) * ka, 0.0);
            for (int64_t g = 0; g < G; ++g)
                for (int a = 0; a < ka; ++a)
                    for (int b = 0; b < ka; ++b)
                        meat[static_cast<size_t>(b) * ka + a] +=
                            score[static_cast<size_t>(g) * ka + a] * score[static_cast<size_t>(g) * ka + b];
            const double fac = (static_cast<double>(N) - 1.0) / (static_cast<double>(N) - ka) *
                               static_cast<double>(G) / (static_cast<double>(G) - 1.0);
            std::vector<double> tmp(static_cast<size_t>(ka) * ka, 0.0);
            for (int a = 0; a < ka; ++a)  // tmp = XtXi * meat
                for (int b = 0; b < ka; ++b) {
                    double acc = 0;
                    for (int m = 0; m < ka; ++m)
                        acc += XtXi_re[static_cast<size_t>(m) * ka + a] * meat[static_cast<size_t>(b) * ka + m];
                    tmp[static_cast<size_t>(b) * ka + a] = acc;
                }
            for (int a = 0; a < ka; ++a)  // V = fac * tmp * XtXi
                for (int b = 0; b < ka; ++b) {
                    double acc = 0;
                    for (int m = 0; m < ka; ++m)
                        acc += tmp[static_cast<size_t>(m) * ka + a] * XtXi_re[static_cast<size_t>(b) * ka + m];
                    R.vcov_matrix[static_cast<size_t>(b) * ka + a] = fac * acc;
                }
        } else {
            std::fill(R.vcov_matrix.begin(), R.vcov_matrix.end(), std::nan(""));
        }
    }
    for (int c = 0; c < k + 1; ++c)
        R.se[static_cast<size_t>(c)] = std::sqrt(std::max(0.0,
            R.vcov_matrix[static_cast<size_t>(c) * (k + 1) + c]));

    return R;
}

XtregBeResult xtreg_be(const double* X, int64_t n_obs, int k,
                       const double* y, const double* ivar) {
    XtregBeResult R;
    R.k = k;
    R.n_obs = n_obs;
    // Panel means of y and X, panels in first-appearance order (the caller has
    // already dropped rows with a missing value).
    std::unordered_map<double, int64_t> idx_of;
    std::vector<double> ybar, Xsum;  // Xsum: n_g rows of k
    std::vector<int64_t> T;
    for (int64_t r = 0; r < n_obs; ++r) {
        auto it = idx_of.find(ivar[r]);
        int64_t g;
        if (it == idx_of.end()) {
            g = static_cast<int64_t>(T.size());
            idx_of.emplace(ivar[r], g);
            ybar.push_back(0.0);
            Xsum.resize(Xsum.size() + static_cast<size_t>(k), 0.0);
            T.push_back(0);
        } else {
            g = it->second;
        }
        ybar[static_cast<size_t>(g)] += y[r];
        for (int c = 0; c < k; ++c)
            Xsum[static_cast<size_t>(g) * k + static_cast<size_t>(c)] += X[static_cast<int64_t>(c) * n_obs + r];
        ++T[static_cast<size_t>(g)];
    }
    const int64_t n_g = static_cast<int64_t>(T.size());
    R.n_groups = n_g;
    const int kk = k + 1;
    if (n_g <= kk) return R;
    std::vector<double> Xb(static_cast<size_t>(n_g) * kk);  // [X̄ | 1], col-major
    for (int64_t g = 0; g < n_g; ++g) {
        const double Tg = static_cast<double>(T[static_cast<size_t>(g)]);
        ybar[static_cast<size_t>(g)] /= Tg;
        for (int c = 0; c < k; ++c)
            Xb[static_cast<size_t>(c) * n_g + g] = Xsum[static_cast<size_t>(g) * k + static_cast<size_t>(c)] / Tg;
        Xb[static_cast<size_t>(k) * n_g + g] = 1.0;
    }
    std::vector<double> beta, XtXi;
    if (ols_solve(Xb.data(), n_g, kk, ybar.data(), beta, XtXi) != 0) return R;
    double ymean = 0;
    for (int64_t g = 0; g < n_g; ++g) ymean += ybar[static_cast<size_t>(g)];
    ymean /= static_cast<double>(n_g);
    double rss = 0, tss = 0;
    for (int64_t g = 0; g < n_g; ++g) {
        double fit = 0;
        for (int c = 0; c < kk; ++c) fit += Xb[static_cast<size_t>(c) * n_g + g] * beta[static_cast<size_t>(c)];
        const double e = ybar[static_cast<size_t>(g)] - fit, d = ybar[static_cast<size_t>(g)] - ymean;
        rss += e * e;
        tss += d * d;
    }
    R.rss = rss;
    R.mss = tss - rss;
    R.df_r = static_cast<double>(n_g - kk);
    const double s2 = rss / R.df_r;
    R.beta = beta;
    R.vcov_matrix.assign(XtXi.size(), 0.0);
    for (size_t i = 0; i < XtXi.size(); ++i) R.vcov_matrix[i] = s2 * XtXi[i];
    R.se.assign(static_cast<size_t>(kk), 0.0);
    for (int c = 0; c < kk; ++c)
        R.se[static_cast<size_t>(c)] = std::sqrt(std::max(0.0, R.vcov_matrix[static_cast<size_t>(c) * kk + c]));
    R.ok = true;
    return R;
}

}  // namespace cecon
