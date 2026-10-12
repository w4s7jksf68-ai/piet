// Binary probit (MLE via IRLS / Fisher scoring). Mirrors logit_fit.
#pragma once
#include <cstdint>
#include <vector>

namespace cecon {

struct ProbitResult {
    int k = 0;
    std::vector<double> beta;    // coefficients (k)
    std::vector<double> p;       // fitted Φ(Xβ) (n)
    std::vector<double> V;       // (X'WX)^{-1}, k*k — asymptotic covariance
    int iters = 0;
    bool converged = false;
    bool separated = false;
    bool separated_perfect = false;
};

// Fit binary probit y ~ X. X is column-major n*k (caller includes an intercept
// column if wanted); y in {0,1}. Fisher-scoring IRLS with the probit working
// weight w = φ(η)² / (Φ(η)(1−Φ(η))) and working response z = η + (y−Φ)/φ.
// offset (BUG-331): optional per-row term ADDED to the linear predictor with a
// coefficient fixed at 1 — eta_i = X_i beta + offset_i — Stata's `offset()`.
// nullptr = no offset, byte-identical to before. It shifts eta ONLY, so the
// working weight/response formulas above are unchanged in form; note that
// probit_observed_info must be given the SAME offset or its eta (and therefore
// the SEs Stata reports) would be computed at the wrong point.
ProbitResult probit_fit(const double* X, int64_t n, int k, const double* y,
                        int maxit = 100, double tol = 1e-14,
                        const double* offset = nullptr);

// ---- Observed-information covariance for a FITTED probit (BUG-140 / BUG-210) ----
//
// probit's link is NON-canonical, so the Fisher/expected information (X'WX)^{-1}
// that ProbitResult::V carries is NOT the covariance Stata reports. Stata uses the
// OBSERVED Hessian, whose per-row weight is lambda_i(lambda_i + eta_i), where
// lambda_i = (y_i - Phi_i) phi_i / (Phi_i(1 - Phi_i)) is the generalized residual
// and eta_i = x_i'beta. (For logit the link IS canonical and the two coincide,
// which is why LogitResult::bread needs no analogue of this.)
//
// BUG-140 fixed `probit`'s own default SE this way. BUG-210 is the SAME defect
// surviving in `margins`' two iid probit branches, and the mechanism was that the
// loop had been copy-pasted into three call sites and only one of them was swept.
// This is now the ONE definition; every site delegates here.
struct ProbitObservedInfo {
    std::vector<double> bread;   // (observed information)^{-1}, k*k column-major
    std::vector<double> meat;    // Sum_i s_i s_i' (s_i = lambda_i x_i), k*k — sandwich meat
    bool observed_spd = true;    // false => observed Hessian not SPD, `bread` fell back to r.V
};

// Both pieces are accumulated in ONE pass in the row order i = 0..n-1, with the
// (c, d) inner order the former copies used, so the floating-point summation order
// is unchanged from the loops this replaces. `meat` is filled unconditionally; it
// is cheap relative to the pass and callers that only want `bread` just ignore it.
ProbitObservedInfo probit_observed_info(const ProbitResult& r, const double* X,
                                        int64_t n, int k, const double* y,
                                        const double* offset = nullptr);

}  // namespace cecon
