// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Heckman sample-selection model, two-step (Heckit) estimator matching
// Stata `heckman depvar x, select(s = z) twostep`. Step 1: probit s on Z → γ̂, V_p.
// Step 2: inverse Mills m_j=φ(z_jγ̂)/Φ(z_jγ̂), δ_j=m_j(m_j+z_jγ̂); augmented OLS of y
// on [X, m] over selected obs; σ̂²=(e'e+β_m²Σδ)/N_sel, ρ̂=β_m/σ̂. Heckman (1979)
// sandwich VCE V = σ̂²(W'W)⁻¹[W'RW + Q](W'W)⁻¹ with R=diag(1−ρ̂²δ), Q=ρ̂²(W'DZ)V_p(Z'DW).
#pragma once
#include <cmath>
#include <map>
#include <vector>

#include "core/linalg.h"
#include "estimators/probit.h"

namespace cecon {

struct HeckmanResult {
    std::vector<double> beta, se;          // outcome eq incl. constant (k_x), then λ appended
    std::vector<std::string> out_names;
    std::vector<double> sel_beta, sel_se;  // selection eq (k_z)
    double lambda = 0, se_lambda = 0, sigma = 0, rho = 0;
    // before the |rho| > 1 rule (the ML start's sigma and rho) and the first
    // step's probit log likelihood (e(ll) of that probit)
    double sigma_raw = 0, rho_raw = 0, prbll = 0;
    // the second step's VCE over [X, lambda] ((kx+1) x (kx+1)) and the probit's
    // (kz x kz): heckman.ado's Heck2 posts them as one block-diagonal e(V)
    std::vector<double> Vreg, Vprb;
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
    // Step 1 is NOT the default probit. Stata's heckman fits the selection probit
    // with asis, a capped iteration count and no coefficient table, and takes bprb = e(b),
    // Vprb = e(V). For a non-robust vce the probit runs with ltol(0) tol(1e-4) plus
    // halfsteponly (MlMaxOptions, logit.h), measured on StataBE. Under it the posted b is
    // p0 + delta (never evaluated) and V = invsym(-H(p0)) with rule-4 rows dropped:
    // the iteration's own Hessian, which BinaryMlResult::bread carries. Z's last
    // column is the selection constant (cmd_heckman), the start's b0 slot.
    MlMaxOptions popt;
    popt.tol = 1e-4;
    popt.ltol = 0.0;
    popt.nrtol = 1e-5;
    popt.maxiter = 300;   // min(1000, c(maxiter)) at the default c(maxiter)
    popt.doopt = true;
    popt.step_forward = false;
    popt.stata_invsym = true;
    ProbitResult pr = probit_fit_ml(Z, n, kz, S, popt, nullptr, last_column_is_constant(Z, n, kz));
    r.sel_beta = pr.beta;
    std::vector<double> Vp = pr.bread_oim;
    if (pr.separated || Vp.size() != static_cast<size_t>(kz) * kz) Vp = pr.V;  // H not invertible
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
    r.sigma_raw = sigma;
    r.rho_raw = rho;
    r.prbll = pr.ll;
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
    r.Vreg = V;
    r.Vprb = Vp;
    r.beta.assign(b.begin(), b.end());  // includes lambda as last
    r.se.resize(kw);
    for (int c = 0; c < kw; ++c) r.se[c] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * kw + c]));
    r.se_lambda = r.se[kx];
    r.converged = pr.converged;
    r.ok = true;
    return r;
}

// ---- full maximum likelihood (Stata's default heckman) -----------------------
// Parameters (β kx, γ kz, athrho, lnsigma); rho = tanh(athrho), sigma = exp(lnsigma).
// A selected row (y observed) contributes
//   ln Φ((zγ + rho e) / sqrt(1 - rho²)) - e²/2 - lnsigma - ln √(2π),  e = (y - xβ)/sigma,
// a non-selected row ln Φ(-zγ). Stata starts ml at the two-step estimates (the
// augmented regression's β without the Mills coefficient, the first step's γ,
// athrho = atanh of rho clipped to [-.85, .85], lnsigma = ln of the two-step sigma)
// with search(off), and iterates Newton-Raphson under ml's rules (ml_newton_path).
struct HeckmanMlResult {
    std::vector<double> b, V, Voim, grad, ilog;
    double ll = 0, prbll = 0, regll = 0;
    int ic = 0;
    bool converged = false, ok = false;
    int64_t n = 0, n_selected = 0;
};

namespace heck {
inline double ln_ncdf(double x) {
    if (x > -30.0) return std::log(ncdf(x));
    return -0.5 * x * x - std::log(-x) - 0.91893853320467274178;
}
// φ(x)/Φ(x), stable in the lower tail
inline double mills(double x) {
    if (x > -30.0) return npdf(x) / ncdf(x);
    return -x;
}
}  // namespace heck

class HeckmanMlObjective final : public MlObjective {
public:
    HeckmanMlObjective(const double* Z, int64_t n, int kz, const double* X, int kx, const double* y,
                       const std::vector<char>& sel)
        : Z_(Z), n_(n), kz_(kz), X_(X), kx_(kx), y_(y), sel_(sel) {}
    // ll at b; when g != nullptr the gradient, and when S != nullptr each row's
    // score (n x np, column-major)
    double value(const std::vector<double>& b, std::vector<double>* g, std::vector<double>* S) const {
        const int np = kx_ + kz_ + 2;
        const double tau = b[static_cast<size_t>(kx_ + kz_)], kap = b[static_cast<size_t>(kx_ + kz_ + 1)];
        if (!std::isfinite(tau) || !std::isfinite(kap) || std::fabs(tau) > 18.0) return -HUGE_VAL;
        const double rho = std::tanh(tau), sigma = std::exp(kap), r = std::sqrt(1.0 - rho * rho);
        if (!(r > 0) || !(sigma > 0)) return -HUGE_VAL;
        if (g) g->assign(static_cast<size_t>(np), 0.0);
        if (S) S->assign(static_cast<size_t>(n_) * np, 0.0);
        double ll = 0;
        for (int64_t i = 0; i < n_; ++i) {
            double zg = 0;
            for (int c = 0; c < kz_; ++c) zg += Z_[static_cast<int64_t>(c) * n_ + i] * b[static_cast<size_t>(kx_ + c)];
            double sx = 0, sz = 0, st = 0, sk = 0;  // d ll_i / d(xβ / per x), d/d zγ, d/dτ, d/dκ
            if (!sel_[static_cast<size_t>(i)]) {
                ll += heck::ln_ncdf(-zg);
                sz = -heck::mills(-zg);
            } else {
                double xb = 0;
                for (int c = 0; c < kx_; ++c) xb += X_[static_cast<int64_t>(c) * n_ + i] * b[static_cast<size_t>(c)];
                const double e = (y_[i] - xb) / sigma;
                const double a = (zg + rho * e) / r;
                ll += heck::ln_ncdf(a) - 0.5 * e * e - kap - 0.91893853320467274178;
                const double lam = heck::mills(a);
                sx = -lam * rho / (r * sigma) + e / sigma;     // times x
                sz = lam / r;                                  // times z
                st = lam * (e + rho * zg) / r;
                sk = -lam * rho * e / r + e * e - 1.0;
            }
            if (g || S) {
                for (int c = 0; c < kx_; ++c) {
                    const double v = sx * X_[static_cast<int64_t>(c) * n_ + i];
                    if (g) (*g)[static_cast<size_t>(c)] += v;
                    if (S) (*S)[static_cast<size_t>(c) * n_ + i] = v;
                }
                for (int c = 0; c < kz_; ++c) {
                    const double v = sz * Z_[static_cast<int64_t>(c) * n_ + i];
                    if (g) (*g)[static_cast<size_t>(kx_ + c)] += v;
                    if (S) (*S)[static_cast<size_t>(kx_ + c) * n_ + i] = v;
                }
                if (g) {
                    (*g)[static_cast<size_t>(kx_ + kz_)] += st;
                    (*g)[static_cast<size_t>(kx_ + kz_ + 1)] += sk;
                }
                if (S) {
                    (*S)[static_cast<size_t>(kx_ + kz_) * n_ + i] = st;
                    (*S)[static_cast<size_t>(kx_ + kz_ + 1) * n_ + i] = sk;
                }
            }
        }
        return ll;
    }
    double loglik(const std::vector<double>& b) override { return value(b, nullptr, nullptr); }
    // A = -H by Richardson-extrapolated central differences of the analytic
    // gradient (truncation O(h^4)).
    double evaluate(const std::vector<double>& b, std::vector<double>& g, std::vector<double>& A) override {
        const size_t m = b.size();
        const double ll = value(b, &g, nullptr);
        A.assign(m * m, 0.0);
        if (!std::isfinite(ll)) return ll;
        std::vector<double> g1, g2, g3, g4, t(b);
        for (size_t j = 0; j < m; ++j) {
            const double h = 1e-4 * (std::fabs(b[j]) + 1e-2);
            t = b; t[j] = b[j] + h; value(t, &g1, nullptr);
            t[j] = b[j] - h; value(t, &g2, nullptr);
            t[j] = b[j] + 2 * h; value(t, &g3, nullptr);
            t[j] = b[j] - 2 * h; value(t, &g4, nullptr);
            for (size_t i = 0; i < m; ++i)
                A[i * m + j] = -(8.0 * (g1[i] - g2[i]) - (g3[i] - g4[i])) / (12.0 * h);
        }
        for (size_t i = 0; i < m; ++i)
            for (size_t j = i + 1; j < m; ++j)
                A[i * m + j] = A[j * m + i] = 0.5 * (A[i * m + j] + A[j * m + i]);
        return ll;
    }

private:
    const double* Z_;
    int64_t n_;
    int kz_;
    const double* X_;
    int kx_;
    const double* y_;
    const std::vector<char>& sel_;
};

// cluster: optional per-row cluster ids (nullptr = none); robust: the sandwich.
inline HeckmanMlResult heckman_ml(const double* Z, int64_t n, int kz, const double* S,
                                  const double* X, int kx, const double* y,
                                  const std::vector<char>& sel, int maxiter, bool robust,
                                  const double* cluster) {
    HeckmanMlResult out;
    out.n = n;
    HeckmanResult tw = heckman_twostep(Z, n, kz, S, X, kx, y, sel);
    if (!tw.ok) return out;
    out.n_selected = tw.n_selected;
    out.prbll = tw.prbll;
    // the comparison regression of y on X over the selected rows
    {
        std::vector<int64_t> rows;
        for (int64_t i = 0; i < n; ++i) if (sel[static_cast<size_t>(i)]) rows.push_back(i);
        const int64_t ns = static_cast<int64_t>(rows.size());
        std::vector<double> W(static_cast<size_t>(ns) * kx), ys(ns), XtX(static_cast<size_t>(kx) * kx), Xty(kx), bo(kx);
        for (int64_t j = 0; j < ns; ++j) {
            ys[static_cast<size_t>(j)] = y[rows[static_cast<size_t>(j)]];
            for (int c = 0; c < kx; ++c) W[static_cast<size_t>(c) * ns + j] = X[static_cast<int64_t>(c) * n + rows[static_cast<size_t>(j)]];
        }
        la::xtx(W.data(), ns, kx, XtX.data());
        la::xtv(W.data(), ns, kx, ys.data(), Xty.data());
        if (la::spd_inverse(XtX.data(), kx) != 0) return out;
        la::matvec(XtX.data(), kx, Xty.data(), bo.data());
        double rss = 0;
        for (int64_t j = 0; j < ns; ++j) {
            double f = 0;
            for (int c = 0; c < kx; ++c) f += W[static_cast<size_t>(c) * ns + j] * bo[static_cast<size_t>(c)];
            rss += (ys[static_cast<size_t>(j)] - f) * (ys[static_cast<size_t>(j)] - f);
        }
        const double N = static_cast<double>(ns);
        out.regll = -0.5 * N * (std::log(2.0 * M_PI) + std::log(rss / N) + 1.0);
    }
    std::vector<double> start;
    for (int c = 0; c < kx; ++c) start.push_back(tw.beta[static_cast<size_t>(c)]);
    for (int c = 0; c < kz; ++c) start.push_back(tw.sel_beta[static_cast<size_t>(c)]);
    double sigma0 = tw.sigma_raw, rho0 = tw.rho_raw;
    if (!std::isfinite(rho0)) rho0 = 0;
    if (sigma0 == 0) sigma0 = .001;
    const double rc = std::max(std::min(rho0, .85), -.85);
    start.push_back(0.5 * std::log((1 + rc) / (1 - rc)));
    start.push_back(std::log(sigma0));
    HeckmanMlObjective obj(Z, n, kz, X, kx, y, sel);
    MlMaxOptions mo;
    mo.maxiter = maxiter >= 0 ? maxiter : 300;
    // ml's technique nr: -H inverted by invsym with the concavity test, and a point
    // where it is not concave takes the m-marquardt step (Stata's "(not concave)"
    // iterations) instead of ending the fit. The two-step start is often such a point.
    mo.stata_invsym = true;
    MlPath path = ml_newton_path(obj, start, mo);
    if (path.failed) return out;
    const size_t np = path.b.size();
    out.b = path.b;
    out.ilog = path.ilog;
    out.ic = path.ic;
    out.converged = path.converged;
    out.ll = path.ll;
    out.Voim.assign(np * np, 0.0);
    for (size_t i = 0; i < np; ++i)
        for (size_t j = 0; j < np; ++j)
            out.Voim[i * np + j] = 0.5 * (path.inv_curvature[i * np + j] + path.inv_curvature[j * np + i]);
    obj.value(out.b, &out.grad, nullptr);
    out.V = out.Voim;
    if (robust) {
        std::vector<double> Sc;
        obj.value(out.b, nullptr, &Sc);
        std::vector<double> meat(np * np, 0.0);
        double factor = 1.0;
        if (cluster) {
            std::map<double, std::vector<double>> sums;
            for (int64_t i = 0; i < n; ++i) {
                auto& v = sums[cluster[i]];
                if (v.empty()) v.assign(np, 0.0);
                for (size_t c = 0; c < np; ++c) v[c] += Sc[c * static_cast<size_t>(n) + static_cast<size_t>(i)];
            }
            for (const auto& kv : sums)
                for (size_t a = 0; a < np; ++a)
                    for (size_t c = 0; c < np; ++c) meat[a * np + c] += kv.second[a] * kv.second[c];
            const double G = static_cast<double>(sums.size());
            factor = G > 1 ? G / (G - 1) : 1.0;
        } else {
            for (int64_t i = 0; i < n; ++i)
                for (size_t a = 0; a < np; ++a) {
                    const double sa = Sc[a * static_cast<size_t>(n) + static_cast<size_t>(i)];
                    for (size_t c = 0; c < np; ++c) meat[a * np + c] += sa * Sc[c * static_cast<size_t>(n) + static_cast<size_t>(i)];
                }
            factor = n > 1 ? static_cast<double>(n) / static_cast<double>(n - 1) : 1.0;
        }
        std::vector<double> tmp(np * np, 0.0);
        out.V.assign(np * np, 0.0);
        for (size_t a = 0; a < np; ++a)
            for (size_t c = 0; c < np; ++c) {
                double s2 = 0;
                for (size_t d = 0; d < np; ++d) s2 += out.Voim[a * np + d] * meat[d * np + c];
                tmp[a * np + c] = s2;
            }
        for (size_t a = 0; a < np; ++a)
            for (size_t c = 0; c < np; ++c) {
                double s2 = 0;
                for (size_t d = 0; d < np; ++d) s2 += tmp[a * np + d] * out.Voim[d * np + c];
                out.V[a * np + c] = factor * s2;
            }
        for (size_t a = 0; a < np; ++a)
            for (size_t c = a + 1; c < np; ++c)
                out.V[a * np + c] = out.V[c * np + a] = 0.5 * (out.V[a * np + c] + out.V[c * np + a]);
    }
    out.ok = true;
    return out;
}

}  // namespace cecon
