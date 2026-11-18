// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Random-effects Poisson with gamma-distributed effects (xtpoisson, re in Stata;
// the default xtpoisson model).
//
// With u_i ~ Gamma(theta, theta) (mean 1, variance alpha = 1/theta) multiplying the
// conditional mean lambda_it = exp(x_it b + offset_it), the panel's marginal
// log likelihood is closed form:
//   ll_i = Sum_t [y_it ln lambda_it - lnGamma(y_it + 1)] + theta ln theta - lnGamma(theta)
//          + lnGamma(Y_i + theta) - (Y_i + theta) ln(Lambda_i + theta),
// Y_i = Sum_t y_it, Lambda_i = Sum_t lambda_it. The parameters are (b, ln alpha), the
// metric Stata posts (e(b) `depvar:x ... /lnalpha`). Analytic gradient and Hessian.
//
// The fit follows Stata's: a pooled Poisson comparison model (e(ll_c)) fitted to its
// MLE, then ml's Newton-Raphson (ml_newton_path) on the full model from (the Poisson
// b, ln alpha = 0) -- vs-stata bank f8d5160a `xtpoisson fatal beertax unemp`:
// iteration 0 -3931.0015 is that point -- with ml's step rules (a full step halved
// while it does not improve the log likelihood, else extended by 1/8, 1/4, ... of the
// step while each extension improves; m-marquardt at a non-concave point) and stop
// rule ((mreldif(b) <= 1e-6 or reldif(ll) <= 1e-7) and g inv(-H) g' < 1e-5).
// V = invsym(-H); the cluster-robust sandwich sums the panel scores.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "core/linalg.h"
#include "estimators/logit.h"  // MlObjective, ml_newton_path: Stata's ml iteration

namespace cecon {

struct XtpoisReResult {
    int k = 0;                  // regressors including the constant (when present)
    int64_t n_obs = 0, n_groups = 0;
    int64_t g_min = 0, g_max = 0;
    double g_avg = 0;
    std::vector<double> theta;  // b (k), then ln alpha
    std::vector<double> V;      // (k+1)^2, inv(-H)
    std::vector<double> gradient;
    std::vector<double> ilog;
    std::vector<double> pois_b;  // the comparison Poisson fit
    double ll = 0, ll_c = 0;
    int iters = 0;
    bool converged = false, pois_converged = false, ok = false;
};

namespace xtpre {

inline double digamma(double x) {
    double r = 0.0;
    while (x < 6.0) {
        r -= 1.0 / x;
        x += 1.0;
    }
    const double f = 1.0 / (x * x);
    return r + std::log(x) - 0.5 / x -
           f * (1.0 / 12 - f * (1.0 / 120 - f * (1.0 / 252 - f * (1.0 / 240 - f / 132))));
}

inline double trigamma(double x) {
    double r = 0.0;
    while (x < 6.0) {
        r += 1.0 / (x * x);
        x += 1.0;
    }
    const double f = 1.0 / (x * x);
    // 1/x + 1/(2x^2) + 1/(6x^3) - 1/(30x^5) + 1/(42x^7) - 1/(30x^9) + 5/(66x^11)
    return r + 1.0 / x + f / 2.0 +
           (f / x) * (1.0 / 6 - f * (1.0 / 30 - f * (1.0 / 42 - f * (1.0 / 30 - f * 5.0 / 66))));
}

// Panels as runs of row indices (rows grouped by id, in their order of first
// appearance within each id's sorted position).
inline std::vector<std::vector<int64_t>> panels(const double* ivar, int64_t n) {
    std::map<double, std::vector<int64_t>> m;
    for (int64_t i = 0; i < n; ++i) m[ivar[i]].push_back(i);
    std::vector<std::vector<int64_t>> g;
    g.reserve(m.size());
    for (auto& kv : m) g.push_back(std::move(kv.second));
    return g;
}

// The panel's alpha-dependent pieces at a large theta = 1/alpha, where lnGamma,
// digamma and trigamma of theta and of Y + theta agree to most of their digits and
// their differences cancel: written as ln1p and differences of Stirling series, so
// an overdispersion near zero (the alpha -> 0 boundary) is evaluated as accurately
// as any other. Returns
//   part = lnGamma(Y+t) - lnGamma(t) - (Y+t) ln1p(L/t) - Y ln t      (ll's panel term)
//   gk   = the panel's score of ln alpha
//   t2h  = t^2 x the panel's second derivative in theta,
//          tL/(L+t) - t^2 (psi'(t) - psi'(Y+t)) - t^2 (L-Y)/(L+t)^2
// (with L = the panel's sum of lambda, Y its sum of y).
struct LargeThetaPanel {
    double part, gk, t2h;
};
inline LargeThetaPanel large_theta_panel(double t, double Y, double L) {
    const double u = Y / t, s = t + Y;
    const double l1u = std::log1p(u), l1L = std::log1p(L / t);
    // Stirling: lnGamma(x) = (x - 1/2) ln x - x + ln(2 pi)/2 + c(x)
    auto c = [](double x) {
        const double r = 1.0 / x, r2 = r * r;
        return r * (1.0 / 12 - r2 * (1.0 / 360 - r2 * (1.0 / 1260 - r2 / 1680)));
    };
    const double part = (s - 0.5) * l1u - Y + (c(s) - c(t)) - s * l1L;
    // psi(x) = ln x - 1/(2x) - 1/(12x^2) + 1/(120x^4) - 1/(252x^6)
    const double d1 = -Y / (t * s);                        // 1/s - 1/t
    const double d2 = -Y * (2 * t + Y) / (t * t * s * s);  // 1/s^2 - 1/t^2
    auto p4 = [](double x) { const double r2 = 1.0 / (x * x); return r2 * r2 * (1.0 / 120 - r2 / 252); };
    const double dpsi = l1u - 0.5 * d1 - d2 / 12 + (p4(s) - p4(t));
    const double gk = t * l1L - t * dpsi + t * (Y - L) / (t + L);
    // psi'(x) = 1/x + 1/(2x^2) + 1/(6x^3) - 1/(30x^5) + 1/(42x^7)
    const double d3 = Y * (3 * t * t + 3 * t * Y + Y * Y) / (t * t * t * s * s * s);  // 1/t^3 - 1/s^3
    auto q5 = [](double x) { const double r = 1.0 / x, r2 = r * r; return r * r2 * r2 * (-1.0 / 30 + r2 / 42); };
    const double dtri = -d1 - 0.5 * d2 + d3 / 6 + (q5(t) - q5(s));  // psi'(t) - psi'(s)
    const double t2h = t * L / (L + t) - t * t * dtri - t * t * (L - Y) / ((L + t) * (L + t));
    return {part, gk, t2h};
}
// theta from which large_theta_panel is used: the Stirling series are exact to
// double precision there, and the direct differences already lose digits.
constexpr double kLargeTheta = 1e3;

// Full-model log likelihood; with grad / negH (np = k+1) also its derivatives, and
// with S (G x np, row-major) each panel's score.
inline double re_eval(const double* X, int64_t n, int k, const double* y, const double* off,
                      const std::vector<std::vector<int64_t>>& G, const std::vector<double>& th,
                      std::vector<double>* grad, std::vector<double>* negH,
                      std::vector<double>* S = nullptr) {
    const int np = k + 1;
    // Stata's evaluator holds ln alpha at -30 or above (below it alpha is zero to
    // every digit the likelihood has).
    const double a = std::max(th[static_cast<size_t>(k)], -30.0);
    const double t = std::exp(-a);  // theta = 1/alpha
    const bool large = t >= kLargeTheta;
    double ll = 0.0;
    if (grad) grad->assign(static_cast<size_t>(np), 0.0);
    if (negH) negH->assign(static_cast<size_t>(np) * np, 0.0);
    if (S) S->assign(G.size() * static_cast<size_t>(np), 0.0);
    std::vector<double> s1(static_cast<size_t>(k)), gi(static_cast<size_t>(np));
    std::vector<double> lam;
    for (size_t gidx = 0; gidx < G.size(); ++gidx) {
        const auto& rows = G[gidx];
        double Y = 0.0, L = 0.0;
        std::fill(s1.begin(), s1.end(), 0.0);
        std::fill(gi.begin(), gi.end(), 0.0);
        lam.assign(rows.size(), 0.0);
        for (size_t q = 0; q < rows.size(); ++q) {
            const int64_t i = rows[q];
            double eta = off ? off[i] : 0.0;
            for (int c = 0; c < k; ++c) eta += X[static_cast<int64_t>(c) * n + i] * th[static_cast<size_t>(c)];
            const double l = std::exp(eta);
            lam[q] = l;
            Y += y[i];
            L += l;
            ll += y[i] * eta - std::lgamma(y[i] + 1.0);
            for (int c = 0; c < k; ++c) {
                const double x = X[static_cast<int64_t>(c) * n + i];
                gi[static_cast<size_t>(c)] += y[i] * x;
                s1[static_cast<size_t>(c)] += l * x;
            }
        }
        LargeThetaPanel lt{0.0, 0.0, 0.0};
        if (large) {
            lt = large_theta_panel(t, Y, L);
            ll += lt.part;
        } else {
            ll += t * std::log(t) - std::lgamma(t) + std::lgamma(Y + t) - (Y + t) * std::log(L + t);
        }
        if (!grad && !negH && !S) continue;
        const double w = (Y + t) / (L + t);
        for (int c = 0; c < k; ++c) gi[static_cast<size_t>(c)] -= w * s1[static_cast<size_t>(c)];
        const double dth =
            large ? 0.0 : std::log(t) + 1.0 - digamma(t) + digamma(Y + t) - std::log(L + t) - w;
        gi[static_cast<size_t>(k)] = large ? lt.gk : -t * dth;
        if (grad)
            for (int p = 0; p < np; ++p) (*grad)[static_cast<size_t>(p)] += gi[static_cast<size_t>(p)];
        if (S)
            for (int p = 0; p < np; ++p) (*S)[gidx * static_cast<size_t>(np) + p] = gi[static_cast<size_t>(p)];
        if (negH) {
            std::vector<double>& A = *negH;
            const double d2 = (L + t) * (L + t);
            // b,b: w Sum lam x x' - (Y + t)/(L + t)^2 s1 s1'
            for (size_t q = 0; q < rows.size(); ++q) {
                const int64_t i = rows[q];
                for (int c = 0; c < k; ++c) {
                    const double xc = X[static_cast<int64_t>(c) * n + i];
                    for (int e = c; e < k; ++e)
                        A[static_cast<size_t>(c) * np + e] += w * lam[q] * xc * X[static_cast<int64_t>(e) * n + i];
                }
            }
            for (int c = 0; c < k; ++c)
                for (int e = c; e < k; ++e)
                    A[static_cast<size_t>(c) * np + e] -= (Y + t) / d2 * s1[static_cast<size_t>(c)] * s1[static_cast<size_t>(e)];
            // b,a: -d2ll/db da = -t s1 (L - Y)/(L + t)^2
            for (int c = 0; c < k; ++c)
                A[static_cast<size_t>(c) * np + k] += -t * s1[static_cast<size_t>(c)] * (L - Y) / d2;
            // a,a: -(t^2 d2ll/dt2 + t dll/dt)
            if (large) {
                // -(t^2 h_tt + t dth) with t dth = -gk
                A[static_cast<size_t>(k) * np + k] -= lt.t2h - lt.gk;
            } else {
                const double h_tt = 1.0 / t - trigamma(t) + trigamma(Y + t) - 1.0 / (L + t) - (L - Y) / d2;
                A[static_cast<size_t>(k) * np + k] -= t * t * h_tt + t * dth;
            }
        }
    }
    if (negH)
        for (int r = 0; r < np; ++r)
            for (int c = r + 1; c < np; ++c)
                (*negH)[static_cast<size_t>(c) * np + r] = (*negH)[static_cast<size_t>(r) * np + c];
    return ll;
}

// The pooled Poisson log likelihood, gradient and negative Hessian.
inline double pois_eval(const double* X, int64_t n, int k, const double* y, const double* off,
                        const std::vector<double>& b, std::vector<double>& g, std::vector<double>& A) {
    double ll = 0.0;
    g.assign(static_cast<size_t>(k), 0.0);
    A.assign(static_cast<size_t>(k) * k, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double eta = off ? off[i] : 0.0;
        for (int c = 0; c < k; ++c) eta += X[static_cast<int64_t>(c) * n + i] * b[static_cast<size_t>(c)];
        const double l = std::exp(eta);
        ll += y[i] * eta - l - std::lgamma(y[i] + 1.0);
        for (int c = 0; c < k; ++c) {
            const double xc = X[static_cast<int64_t>(c) * n + i];
            g[static_cast<size_t>(c)] += (y[i] - l) * xc;
            for (int e = 0; e < k; ++e) A[static_cast<size_t>(c) * k + e] += l * xc * X[static_cast<int64_t>(e) * n + i];
        }
    }
    return ll;
}

}  // namespace xtpre

// X col-major n*k (the constant, when the model has one, among the columns at
// `cons_col`, -1 = none); y counts; ivar the panel ids; off the offset or nullptr.
inline XtpoisReResult xtpoisson_re_gamma(const double* X, int64_t n, int k, const double* y,
                                         const double* ivar, const double* off, int cons_col,
                                         int max_iter = 300) {
    XtpoisReResult r;
    r.k = k;
    r.n_obs = n;
    const auto G = xtpre::panels(ivar, n);
    r.n_groups = static_cast<int64_t>(G.size());
    if (G.empty()) return r;
    r.g_min = r.g_max = static_cast<int64_t>(G.front().size());
    for (const auto& g : G) {
        r.g_min = std::min<int64_t>(r.g_min, static_cast<int64_t>(g.size()));
        r.g_max = std::max<int64_t>(r.g_max, static_cast<int64_t>(g.size()));
    }
    r.g_avg = static_cast<double>(n) / static_cast<double>(G.size());

    // ---- the comparison Poisson model: Newton-Raphson to the MLE, from the
    // constant-only point (or 0 without a constant).
    std::vector<double> b(static_cast<size_t>(k), 0.0), g, A, step(static_cast<size_t>(k));
    if (cons_col >= 0) {
        double sy = 0.0, so = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            sy += y[i];
            so += off ? std::exp(off[i]) : 1.0;
        }
        if (sy > 0 && so > 0) b[static_cast<size_t>(cons_col)] = std::log(sy / so);
    }
    double llp = xtpre::pois_eval(X, n, k, y, off, b, g, A);
    for (int it = 0; it < 200; ++it) {
        std::vector<double> Ai = A;
        if (la::spd_inverse(Ai.data(), k) != 0) break;
        la::matvec(Ai.data(), k, g.data(), step.data());
        std::vector<double> cand(static_cast<size_t>(k));
        double mx = 0.0;
        for (int c = 0; c < k; ++c) {
            cand[static_cast<size_t>(c)] = b[static_cast<size_t>(c)] + step[static_cast<size_t>(c)];
            mx = std::max(mx, std::fabs(step[static_cast<size_t>(c)]) / (std::fabs(b[static_cast<size_t>(c)]) + 1.0));
        }
        std::vector<double> g2, A2;
        double l2 = xtpre::pois_eval(X, n, k, y, off, cand, g2, A2);
        for (int h = 0; h < 60 && !(l2 >= llp); ++h) {
            for (int c = 0; c < k; ++c)
                cand[static_cast<size_t>(c)] = (cand[static_cast<size_t>(c)] + b[static_cast<size_t>(c)]) / 2.0;
            l2 = xtpre::pois_eval(X, n, k, y, off, cand, g2, A2);
        }
        b = cand;
        llp = l2;
        g.swap(g2);
        A.swap(A2);
        if (mx < 1e-12) {
            r.pois_converged = true;
            break;
        }
    }
    r.pois_b = b;
    r.ll_c = llp;

    // ---- the full model from (Poisson b, ln alpha = 0), by ml's technique nr
    // (ml_newton_path): -H inverted by invsym with the concavity test, a non-concave
    // point stepping by m-marquardt rather than ending the fit. A design whose
    // overdispersion is near zero has its maximum at the alpha -> 0 boundary: the
    // iterations walk ln alpha down until the stop rule holds, as Stata's do.
    struct ReObjective final : MlObjective {
        const double* X;
        int64_t n;
        int k;
        const double* y;
        const double* off;
        const std::vector<std::vector<int64_t>>& G;
        ReObjective(const double* X_, int64_t n_, int k_, const double* y_, const double* off_,
                    const std::vector<std::vector<int64_t>>& G_)
            : X(X_), n(n_), k(k_), y(y_), off(off_), G(G_) {}
        double loglik(const std::vector<double>& p) override {
            return xtpre::re_eval(X, n, k, y, off, G, p, nullptr, nullptr);
        }
        double evaluate(const std::vector<double>& p, std::vector<double>& g,
                        std::vector<double>& A) override {
            return xtpre::re_eval(X, n, k, y, off, G, p, &g, &A);
        }
    };
    const int np = k + 1;
    std::vector<double> th(b);
    th.push_back(0.0);
    ReObjective obj(X, n, k, y, off, G);
    MlMaxOptions mo;
    mo.maxiter = max_iter;
    mo.stata_invsym = true;
    MlPath path = ml_newton_path(obj, th, mo);
    r.ilog = path.ilog;
    r.iters = path.ic;
    if (path.failed || !std::isfinite(path.ll) || path.b.size() != static_cast<size_t>(np)) return r;
    r.converged = path.converged;
    r.ll = path.ll;
    r.theta = path.b;
    r.V.assign(static_cast<size_t>(np) * np, 0.0);
    for (int a = 0; a < np; ++a)
        for (int c = 0; c < np; ++c)
            r.V[static_cast<size_t>(a) * np + c] =
                0.5 * (path.inv_curvature[static_cast<size_t>(a) * np + c] +
                       path.inv_curvature[static_cast<size_t>(c) * np + a]);
    xtpre::re_eval(X, n, k, y, off, G, r.theta, &r.gradient, nullptr);
    r.ok = true;
    return r;
}

// Each panel's score at the estimates (G x (k+1), row-major): the cluster-robust meat.
inline std::vector<double> xtpoisson_re_panel_scores(const double* X, int64_t n, int k,
                                                     const double* y, const double* ivar,
                                                     const double* off, const XtpoisReResult& r) {
    const auto G = xtpre::panels(ivar, n);
    std::vector<double> S;
    xtpre::re_eval(X, n, k, y, off, G, r.theta, nullptr, nullptr, &S);
    return S;
}

// xtpoisson's vce(robust) for the gamma model (xtpoisson.ado XTPOISSON_FE_ROBUST,
// `re`): `_robust score1 score2, v(e(V)) cluster(ivar) minus(0)` where score1 is
// each observation's score of xb and score2 the PANEL's score of lnalpha written on
// every observation of the panel -- so a panel's lnalpha contribution is T_i times
// its score -- and minus(0) leaves no M/(M-1) factor. Returns V (np x np).
inline std::vector<double> xtpoisson_re_stata_robust(const double* X, int64_t n, int k,
                                                     const double* y, const double* ivar,
                                                     const double* off, const XtpoisReResult& r) {
    const auto G = xtpre::panels(ivar, n);
    std::vector<double> S;
    xtpre::re_eval(X, n, k, y, off, G, r.theta, nullptr, nullptr, &S);
    const int np = k + 1;
    std::vector<double> meat(static_cast<size_t>(np) * np, 0.0), u(static_cast<size_t>(np));
    for (size_t gi = 0; gi < G.size(); ++gi) {
        for (int p = 0; p < np; ++p) u[static_cast<size_t>(p)] = S[gi * np + p];
        u[static_cast<size_t>(k)] *= static_cast<double>(G[gi].size());
        for (int a = 0; a < np; ++a)
            for (int b = 0; b < np; ++b) meat[static_cast<size_t>(a) * np + b] += u[static_cast<size_t>(a)] * u[static_cast<size_t>(b)];
    }
    std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
    for (int a = 0; a < np; ++a)
        for (int b = 0; b < np; ++b) {
            double s2 = 0;
            for (int c = 0; c < np; ++c) s2 += r.V[static_cast<size_t>(a) * np + c] * meat[static_cast<size_t>(c) * np + b];
            tmp[static_cast<size_t>(a) * np + b] = s2;
        }
    for (int a = 0; a < np; ++a)
        for (int b = 0; b < np; ++b) {
            double s2 = 0;
            for (int c = 0; c < np; ++c) s2 += tmp[static_cast<size_t>(a) * np + c] * r.V[static_cast<size_t>(c) * np + b];
            V[static_cast<size_t>(a) * np + b] = s2;
        }
    for (int a = 0; a < np; ++a)
        for (int b = a + 1; b < np; ++b)
            V[static_cast<size_t>(a) * np + b] = V[static_cast<size_t>(b) * np + a] =
                0.5 * (V[static_cast<size_t>(a) * np + b] + V[static_cast<size_t>(b) * np + a]);
    return V;
}

}  // namespace cecon
