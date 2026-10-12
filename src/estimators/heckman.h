// cecon — Heckman sample-selection model, two-step (Heckit) estimator matching
// Stata `heckman depvar x, select(s = z) twostep`. Step 1: probit s on Z → γ̂, V_p.
// Step 2: inverse Mills m_j=φ(z_jγ̂)/Φ(z_jγ̂), δ_j=m_j(m_j+z_jγ̂); augmented OLS of y
// on [X, m] over selected obs; σ̂²=(e'e+β_m²Σδ)/N_sel, ρ̂=β_m/σ̂. Heckman (1979)
// sandwich VCE V = σ̂²(W'W)⁻¹[W'RW + Q](W'W)⁻¹ with R=diag(1−ρ̂²δ), Q=ρ̂²(W'DZ)V_p(Z'DW).
#pragma once
#include <cmath>
#include <vector>

#include "core/linalg.h"
#include "estimators/probit.h"

namespace cecon {

struct HeckmanResult {
    std::vector<double> beta, se;          // outcome eq incl. constant (k_x), then λ appended
    std::vector<std::string> out_names;
    std::vector<double> sel_beta, sel_se;  // selection eq (k_z)
    double lambda = 0, se_lambda = 0, sigma = 0, rho = 0;
    int64_t n = 0, n_selected = 0;
    bool converged = false, ok = false;
};

namespace heck {
inline double ncdf(double x) { return 0.5 * std::erfc(-x * 0.70710678118654752440); }
inline double npdf(double x) { return 0.39894228040143267794 * std::exp(-0.5 * x * x); }
}  // namespace heck

// Z (n*kz col-major) + S (n, 0/1) for the probit; X (n*kx) + y (n, NaN where not
// selected) for the outcome. sel[j]=true marks selected rows (S==1 & y finite).
inline HeckmanResult heckman_twostep(const double* Z, int64_t n, int kz, const double* S,
                                     const double* X, int kx, const double* y,
                                     const std::vector<char>& sel) {
    HeckmanResult r;
    r.n = n;
    ProbitResult pr = probit_fit(Z, n, kz, S);
    r.sel_beta = pr.beta;
    // Stata's probit (and heckman's selection VCE + the Q term) use the OBSERVED
    // information, not the expected/IRLS one probit_fit returns. For probit the
    // per-obs observed weight is w_i = λ_i(λ_i + z_iγ), λ_i = generalized residual.
    std::vector<double> Vp(static_cast<size_t>(kz) * kz, 0.0);
    {
        std::vector<double> I(static_cast<size_t>(kz) * kz, 0.0);
        for (int64_t i = 0; i < n; ++i) {
            double zg = 0;
            for (int c = 0; c < kz; ++c) zg += Z[static_cast<int64_t>(c) * n + i] * pr.beta[c];
            double Phi = heck::ncdf(zg);
            if (Phi < 1e-300) Phi = 1e-300;
            if (Phi > 1.0 - 1e-16) Phi = 1.0 - 1e-16;
            double phi = heck::npdf(zg);
            double lam = (S[i] == 1.0) ? phi / Phi : -phi / (1.0 - Phi);
            double w = lam * (lam + zg);
            for (int c = 0; c < kz; ++c) {
                double zc = w * Z[static_cast<int64_t>(c) * n + i];
                for (int d = 0; d < kz; ++d)
                    I[static_cast<size_t>(c) * kz + d] += zc * Z[static_cast<int64_t>(d) * n + i];
            }
        }
        Vp = I;
        if (la::spd_inverse(Vp.data(), kz) != 0) Vp = pr.V;  // fall back if singular
    }
    r.sel_se.resize(kz);
    for (int c = 0; c < kz; ++c) r.sel_se[c] = std::sqrt(std::max(0.0, Vp[static_cast<size_t>(c) * kz + c]));

    std::vector<int64_t> S1;
    for (int64_t i = 0; i < n; ++i) if (sel[i]) S1.push_back(i);
    int64_t ns = static_cast<int64_t>(S1.size());
    r.n_selected = ns;
    int kw = kx + 1;  // [X, mills]
    if (ns <= kw) return r;

    std::vector<double> m(ns), delta(ns);
    for (int64_t j = 0; j < ns; ++j) {
        int64_t i = S1[j];
        double zg = 0;
        for (int c = 0; c < kz; ++c) zg += Z[static_cast<int64_t>(c) * n + i] * pr.beta[c];
        double Phi = heck::ncdf(zg);
        if (Phi < 1e-300) Phi = 1e-300;
        m[j] = heck::npdf(zg) / Phi;
        delta[j] = m[j] * (m[j] + zg);
    }
    // augmented design W = [X_sel, m]
    std::vector<double> W(static_cast<size_t>(ns) * kw), ysel(ns);
    for (int64_t j = 0; j < ns; ++j) {
        int64_t i = S1[j];
        for (int c = 0; c < kx; ++c) W[static_cast<int64_t>(c) * ns + j] = X[static_cast<int64_t>(c) * n + i];
        W[static_cast<int64_t>(kx) * ns + j] = m[j];
        ysel[j] = y[i];
    }
    std::vector<double> WtW(static_cast<size_t>(kw) * kw), Wty(kw), b(kw);
    la::xtx(W.data(), ns, kw, WtW.data());
    la::xtv(W.data(), ns, kw, ysel.data(), Wty.data());
    if (la::spd_inverse(WtW.data(), kw) != 0) return r;
    std::vector<double> WtWinv = WtW;
    la::matvec(WtWinv.data(), kw, Wty.data(), b.data());
    double beta_m = b[kx];
    // residuals + sigma, rho
    double ess = 0, sumdelta = 0;
    for (int64_t j = 0; j < ns; ++j) {
        double f = 0;
        for (int c = 0; c < kw; ++c) f += W[static_cast<int64_t>(c) * ns + j] * b[c];
        double e = ysel[j] - f;
        ess += e * e;
        sumdelta += delta[j];
    }
    double sigma2 = (ess + beta_m * beta_m * sumdelta) / static_cast<double>(ns);
    double sigma = std::sqrt(sigma2);
    double rho = beta_m / sigma;
    // Stata's default `rhosigma` rule keeps the two-step VCE admissible when
    // the unconstrained rho estimate leaves [-1, 1]: truncate rho and make
    // sigma consistent with lambda = rho*sigma before assembling the VCE.
    // (The augmented-regression coefficients, including lambda, do not change.)
    if (std::fabs(rho) > 1.0) {
        rho = std::copysign(1.0, rho);
        sigma = beta_m / rho;
        sigma2 = sigma * sigma;
    }
    r.sigma = sigma; r.rho = rho; r.lambda = beta_m;

    // sandwich: V = sigma2 * WtWinv * [W'RW + Q] * WtWinv
    std::vector<double> WRW(static_cast<size_t>(kw) * kw, 0.0);
    for (int64_t j = 0; j < ns; ++j) {
        double rj = 1.0 - rho * rho * delta[j];
        for (int c = 0; c < kw; ++c) {
            double wc = W[static_cast<int64_t>(c) * ns + j];
            for (int d = 0; d < kw; ++d)
                WRW[static_cast<size_t>(c) * kw + d] += rj * wc * W[static_cast<int64_t>(d) * ns + j];
        }
    }
    // WDZ = sum delta_j w_j z_j'  (kw x kz)
    std::vector<double> WDZ(static_cast<size_t>(kw) * kz, 0.0);
    for (int64_t j = 0; j < ns; ++j) {
        int64_t i = S1[j];
        for (int c = 0; c < kw; ++c) {
            double wc = delta[j] * W[static_cast<int64_t>(c) * ns + j];
            for (int e = 0; e < kz; ++e) WDZ[static_cast<size_t>(c) * kz + e] += wc * Z[static_cast<int64_t>(e) * n + i];
        }
    }
    // Q = rho^2 * WDZ * V_p * WDZ'   (kw x kw)
    std::vector<double> tmp(static_cast<size_t>(kw) * kz, 0.0);  // WDZ * V_p
    for (int c = 0; c < kw; ++c)
        for (int e = 0; e < kz; ++e) {
            double s = 0;
            for (int f = 0; f < kz; ++f) s += WDZ[static_cast<size_t>(c) * kz + f] * Vp[static_cast<size_t>(f) * kz + e];
            tmp[static_cast<size_t>(c) * kz + e] = s;
        }
    std::vector<double> mid = WRW;  // W'RW + Q
    for (int c = 0; c < kw; ++c)
        for (int d = 0; d < kw; ++d) {
            double q = 0;
            for (int e = 0; e < kz; ++e) q += tmp[static_cast<size_t>(c) * kz + e] * WDZ[static_cast<size_t>(d) * kz + e];
            mid[static_cast<size_t>(c) * kw + d] += rho * rho * q;
        }
    // V = sigma2 * WtWinv * mid * WtWinv
    std::vector<double> t2(static_cast<size_t>(kw) * kw, 0.0);  // WtWinv * mid
    for (int c = 0; c < kw; ++c)
        for (int d = 0; d < kw; ++d) {
            double s = 0;
            for (int e = 0; e < kw; ++e) s += WtWinv[static_cast<size_t>(c) * kw + e] * mid[static_cast<size_t>(e) * kw + d];
            t2[static_cast<size_t>(c) * kw + d] = s;
        }
    std::vector<double> V(static_cast<size_t>(kw) * kw, 0.0);
    for (int c = 0; c < kw; ++c)
        for (int d = 0; d < kw; ++d) {
            double s = 0;
            for (int e = 0; e < kw; ++e) s += t2[static_cast<size_t>(c) * kw + e] * WtWinv[static_cast<size_t>(e) * kw + d];
            V[static_cast<size_t>(c) * kw + d] = sigma2 * s;
        }
    r.beta.assign(b.begin(), b.end());  // includes lambda as last
    r.se.resize(kw);
    for (int c = 0; c < kw; ++c) r.se[c] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * kw + c]));
    r.se_lambda = r.se[kx];
    r.converged = pr.converged;
    r.ok = true;
    return r;
}

}  // namespace cecon
