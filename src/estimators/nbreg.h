// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// nbreg — Negative binomial (NB2) regression. Count model that allows
// over-dispersion via a gamma-mixed Poisson:
//
//   Pr(Y=y | μ, α) = Γ(y + α^-1) / (Γ(y+1) Γ(α^-1))
//                    * (α^-1 / (α^-1 + μ))^{α^-1}
//                    * (μ / (α^-1 + μ))^y
//
// We estimate (β, lnα). μ_i = exp(X_i β + offset_i). The log-likelihood is
// concave in β for fixed α and the joint NR step on (β, lnα) is standard.
#pragma once
#include <cstdint>
#include <vector>

namespace cecon {

struct NbregResult {
    int k = 0;                       // # covariates (incl. intercept if user-included)
    int64_t n_obs = 0;
    std::vector<double> beta;        // length k
    std::vector<double> se_iid;      // length k
    std::vector<double> se_robust;   // length k (cluster-robust if cluster supplied, else HC)
    double alpha = 0;                // dispersion parameter
    double lnalpha = 0;
    double se_lnalpha_iid = 0;
    // The full (k+1)x(k+1) inverse observed information over (β, lnα), the
    // matrix se_iid and se_lnalpha_iid are read from (BUG-1196: Stata's nbreg
    // e(V) is this matrix, `/lnalpha` last). Empty at the α→0 boundary, where
    // the fit is the Poisson limit and lnα has no information.
    std::vector<double> vcov_iid;
    // The (k+1)x(k+1) robust (or cluster-robust, with cluster_var) sandwich over
    // (β, lnα) that se_robust is read from. Empty at the α→0 boundary.
    std::vector<double> vcov_robust;
    double loglik = 0;
    int iters = 0;
    bool converged = false;
    // The ml path's record (empty / false at the α→0 boundary): the full fit's log
    // likelihood at iteration 0..iters (e(ilog)) and gradient over (β, lnα) at the
    // posted iterate (e(gradient)); the constant-only fit's maximum (e(ll_0)), when
    // there is a constant; the Poisson comparison fit's log likelihood (e(ll_c)).
    bool boundary = false;
    std::vector<double> ilog;
    std::vector<double> gradient;
    bool has_ll_0 = false;
    double ll_0 = 0;
    double ll_c = 0;
};

// Inputs (column-major X = n_obs * k, no internal intercept — caller passes a
// constant column if desired). cluster_var is optional (nullptr → robust = HC).
// weights (optional, length n_obs; BUG-1537): Stata's fweight/iweight/pweight w_i.
// The log-likelihood and its derivatives are Sum w_i (.); a cluster's score total
// is Sum w_i s_i; the unclustered meat is Sum (w_i s_i)(w_i s_i)' on the rows, or
// under an fweight (freq) Sum w_i s_i s_i' with N = Sum w. nullptr = unweighted.
NbregResult nbreg(const double* X, int64_t n_obs, int k,
                  const double* y, const double* offset = nullptr,
                  const double* cluster_var = nullptr,
                  int maxit = 100, double tol = 1e-10,
                  const double* weights = nullptr, bool freq = false,
                  int cons_col = -2);
// cons_col: the constant's column of X, or -1 when the model has none (noconstant:
// no constant-only fit, no e(ll_0), even when a regressor is identically 1); -2
// (the default) takes the first all-ones column as the constant.

}  // namespace cecon
