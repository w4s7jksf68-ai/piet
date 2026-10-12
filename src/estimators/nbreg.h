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
    double loglik = 0;
    int iters = 0;
    bool converged = false;
};

// Inputs (column-major X = n_obs * k, no internal intercept — caller passes a
// constant column if desired). cluster_var is optional (nullptr → robust = HC).
NbregResult nbreg(const double* X, int64_t n_obs, int k,
                  const double* y, const double* offset = nullptr,
                  const double* cluster_var = nullptr,
                  int maxit = 100, double tol = 1e-10);

}  // namespace cecon
