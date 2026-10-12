// Binary logit (MLE via IRLS / Newton-Raphson). A building block for the csdid
// DR-IPW propensity score; exposes the pieces the influence functions need.
#pragma once
#include <cstdint>
#include <vector>

namespace cecon {

struct LogitResult {
    int k = 0;
    std::vector<double> beta;    // coefficients (k)
    std::vector<double> p;       // fitted probabilities (n)
    std::vector<double> bread;   // (X'WX)^{-1}, k*k — the MLE asymptotic cov / inverse info
    int iters = 0;
    bool converged = false;
    bool separated = false;
    bool separated_perfect = false;
};

// Fit binary logit y ~ X. X is column-major n*k (caller includes an intercept
// column if wanted); y in {0,1}. Deviance-convergence IRLS.
// weights (P5, pweight): optional per-row weight, length n, nullptr = unweighted
// (byte-identical to before weights existed). When present, the IRLS working
// weight becomes w_i = weights_i * p_i(1-p_i) — the pseudo-likelihood MLE that
// maximizes Sum_i weights_i * loglik_i — while the working RESPONSE z_i still
// divides by the UNweighted (floor-clamped) p(1-p), matching standard weighted
// Fisher scoring (the weight only rescales the normal equations, not z). r.bread
// is then (X' diag(w) X)^{-1} at convergence: the weighted-observed-information
// bread a caller needs for a pweight sandwich VCE (logit is a canonical link, so
// observed info == Fisher info, unlike probit's BUG-140 case).
// offset (BUG-331): optional per-row term ADDED to the linear predictor with a
// coefficient fixed at 1 — eta_i = X_i beta + offset_i — Stata's `offset()`.
// nullptr = no offset, byte-identical to before this parameter existed. Only eta
// shifts: the score is still X'(y-p) and the information still X'WX, so `bread`
// and every downstream VCE are unchanged in form.
LogitResult logit_fit(const double* X, int64_t n, int k, const double* y,
                      int maxit = 100, double tol = 1e-14,
                      const double* weights = nullptr,
                      const double* offset = nullptr);

}  // namespace cecon
