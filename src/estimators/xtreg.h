// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// xtreg, re — random-effects (Swamy-Arora) panel-data linear regression.
//
// Variance components (Stata default, "σ_uT"):
//   σ_e² = Σ_it e_it² / (N - n - K + 1)       e = within-FE residual
//   σ_u² = max{0, SSR_b/(n - K) - σ_e² / T_h}  SSR_b on group means
//                                              T_h = harmonic mean of T_i
//   θ_i  = 1 - σ_e / sqrt(T_i σ_u² + σ_e²)
// Then OLS on transformed (y - θ_i ȳ_i, X - θ_i X̄_i, (1 - θ_i)).
#pragma once
#include <cstdint>
#include <vector>

namespace cecon {

struct XtregReResult {
    int k = 0;                   // # covariates EXCLUDING intercept
    int64_t n_obs = 0, n_groups = 0;
    std::vector<double> beta;    // length k+1; LAST entry is the intercept
    std::vector<double> se;      // length k+1, same order
    std::vector<double> vcov_matrix;  // (k+1)*(k+1) col-major FGLS covariance
    double sigma_e = 0;          // sqrt(σ_e²)
    double sigma_u = 0;          // sqrt(σ_u²)
    double rho = 0;              // σ_u² / (σ_u² + σ_e²)
    double T_h = 0;              // harmonic mean of T_i
    double rmse = 0;             // residual SD in the GLS-transformed regression
    std::vector<double> within_beta;  // optional within fit, including intercept
    std::vector<double> within_vcov;  // full column-major covariance of within_beta
    // residual dof of the within regression, N - n_groups - kv, where kv counts
    // only the covariates with within-panel variation (time-invariant ones drop
    // out of that regression). Stata refuses the fit r(2001) when it is <= 0.
    double df_e_within = 0;
    int64_t n_clusters = 0;      // > 0 after a cluster-robust VCE (cluster != nullptr)
    bool ok = true;
};

// Inputs: X is n_obs * k column-major (NO intercept — added internally).
// cluster (optional, n_obs values): the cluster-robust VCE of the GLS step, Stata's
// regress cluster() sandwich on the transformed system [X* | 1 - theta]: V =
// (N-1)/(N-(k+1)) * G/(G-1) * (X*'X*)^-1 (sum_g s_g s_g') (X*'X*)^-1, s_g the sum over
// cluster g of x*_i e*_i (xtreg, re vce(robust) clusters on the panel variable).
// A NaN cluster value keeps its row in the variance components (sigma_e, sigma_u,
// theta) and leaves only the GLS regression, as Stata's xtreg, re does: its sample
// marks out the varlist and the panel variable, and only the final transformed
// regression with cluster() drops the rows whose cluster is missing. n_obs of the
// result is then that regression's row count.
// sa: the Baltagi-Chang small-sample Swamy-Arora sigma_u when the panel sizes vary,
//   σ_u² = max{0, (SSR*_b - (n - K) σ_e²) / (N - tr)},  SSR*_b = Σ_i T_i ê_b,i²
//   (ê_b the between residuals), tr = trace{(Σ T_i x̄_i x̄_i')^-1 Σ T_i² x̄_i x̄_i'}
//   with x̄_i the panel means including the constant. On a balanced panel the
//   default formula is used (the two agree there).
XtregReResult xtreg_re(const double* X, int64_t n_obs, int k,
                       const double* y, const double* ivar, bool emit_within = false,
                       const double* cluster = nullptr, bool sa = false);

// xtreg, be — between effects (BUG-1515): OLS of the panel means ȳ_i on X̄_i and
// a constant, one row per panel, unweighted (xtreg, be without `wls`).
// V = s² (X̄'X̄)^-1 with s² = rss / (n_groups - k - 1).
struct XtregBeResult {
    int k = 0;                   // # covariates EXCLUDING intercept
    int64_t n_obs = 0, n_groups = 0;
    std::vector<double> beta;    // length k+1; LAST entry is the intercept
    std::vector<double> se;      // length k+1, same order
    std::vector<double> vcov_matrix;  // (k+1)*(k+1) col-major
    double rss = 0, mss = 0;     // of the regression on the means
    double df_r = 0;             // n_groups - k - 1
    bool ok = false;
};
XtregBeResult xtreg_be(const double* X, int64_t n_obs, int k,
                       const double* y, const double* ivar);

}  // namespace cecon
