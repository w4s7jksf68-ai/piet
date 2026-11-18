// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — reg3: systems estimation (SUR / 3SLS) matching Stata `reg3`.
// Stage 1: project endogenous regressors onto the full instrument set W → Ẑ_i
//   (SUR has no endogenous regressors, so Ẑ_i = Z_i). Stage 2: Σ=I first pass
//   (eq-by-eq OLS/2SLS), residuals e_i = y_i − Z_i β_i^(0) from ACTUAL regressors,
//   Σ̂_ij = e_i'e_j/n. Stage 3: stacked GLS over the Kronecker structure
//   β = {Ẑ'(Σ̂⁻¹⊗I)Ẑ}⁻¹ Ẑ'(Σ̂⁻¹⊗I)y, V = {Ẑ'(Σ̂⁻¹⊗I)Ẑ}⁻¹.
#pragma once
#include <cmath>
#include <string>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct Reg3Eq {
    std::vector<double> Z;        // n*k col-major (incl. constant)
    int k = 0;
    std::vector<double> y;        // n
    std::vector<char> endog;      // length k: 1 if that column is endogenous
    std::string depname;
    std::vector<std::string> names;
};

struct Reg3Result {
    int M = 0;
    int ic = 0;                  // iterated fits after the first (isure); 0 otherwise
    bool converged = true;
    int64_t n = 0;
    std::vector<int> koff;       // stacked offset of each equation's first param
    std::vector<double> beta;    // stacked (K)
    std::vector<double> se;      // stacked (K)
    std::vector<double> vcov;    // K*K
    std::vector<double> Sigma;   // M*M (divided by n)
    bool ok = false;
};

// W = instrument set (n*kw, incl. constant). For pure SUR pass any W (unused when
// no column is flagged endogenous).
// iterate_max > 0 (sureg isure / reg3 ireg3): after the two-step fit, re-estimate the
// error covariance from the current residuals and refit until the coefficient vector
// moves by less than tol (mreldif) or iterate_max fits (Stata's count, see below).
inline Reg3Result reg3_fit(const std::vector<Reg3Eq>& eqs, int64_t n,
                           const std::vector<double>& W, int kw, int iterate_max = 0,
                           double tol = 1e-6) {
    Reg3Result r;
    int M = static_cast<int>(eqs.size());
    r.M = M; r.n = n;
    // (W'W)^-1 for projecting endogenous regressors
    std::vector<double> WtWinv(static_cast<size_t>(kw) * kw);
    la::xtx(W.data(), n, kw, WtWinv.data());
    bool anyEndog = false;
    for (auto& e : eqs) for (char c : e.endog) if (c) anyEndog = true;
    if (anyEndog && la::spd_inverse(WtWinv.data(), kw) != 0) return r;

    // Build Ẑ_i (projected) and stacked offsets
    std::vector<std::vector<double>> Zhat(M);
    r.koff.assign(M + 1, 0);
    for (int i = 0; i < M; ++i) {
        const Reg3Eq& e = eqs[i];
        Zhat[i] = e.Z;  // start with actual; replace endogenous columns by projection
        for (int c = 0; c < e.k; ++c) {
            if (!e.endog[c]) continue;
            const double* zc = e.Z.data() + static_cast<int64_t>(c) * n;
            std::vector<double> Wtz(kw, 0.0);
            for (int j = 0; j < kw; ++j) {
                double s = 0;
                const double* wj = W.data() + static_cast<int64_t>(j) * n;
                for (int64_t t = 0; t < n; ++t) s += wj[t] * zc[t];
                Wtz[j] = s;
            }
            std::vector<double> coef(kw);
            la::matvec(WtWinv.data(), kw, Wtz.data(), coef.data());
            double* hc = Zhat[i].data() + static_cast<int64_t>(c) * n;
            for (int64_t t = 0; t < n; ++t) {
                double s = 0;
                for (int j = 0; j < kw; ++j) s += W[static_cast<int64_t>(j) * n + t] * coef[j];
                hc[t] = s;
            }
        }
        r.koff[i + 1] = r.koff[i] + e.k;
    }
    int K = r.koff[M];

    // Stage 2: Σ=I first pass (eq-by-eq), residuals from ACTUAL Z, then Σ̂ = E'E/n
    std::vector<std::vector<double>> resid(M);
    std::vector<double> b_first;  // the stacked first-pass coefficients
    for (int i = 0; i < M; ++i) {
        const Reg3Eq& e = eqs[i];
        int k = e.k;
        std::vector<double> ZhZh(static_cast<size_t>(k) * k), Zhy(k), b(k);
        la::xtx(Zhat[i].data(), n, k, ZhZh.data());
        la::xtv(Zhat[i].data(), n, k, e.y.data(), Zhy.data());
        if (la::spd_inverse(ZhZh.data(), k) != 0) return r;
        la::matvec(ZhZh.data(), k, Zhy.data(), b.data());
        b_first.insert(b_first.end(), b.begin(), b.end());
        resid[i].resize(n);
        for (int64_t t = 0; t < n; ++t) {
            double f = 0;
            for (int c = 0; c < k; ++c) f += e.Z[static_cast<int64_t>(c) * n + t] * b[c];
            resid[i][t] = e.y[t] - f;
        }
    }
    std::vector<double> Sig(static_cast<size_t>(M) * M, 0.0);
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < M; ++j) {
            double s = 0;
            for (int64_t t = 0; t < n; ++t) s += resid[i][t] * resid[j][t];
            Sig[static_cast<size_t>(i) * M + j] = s / static_cast<double>(n);
        }
    // Stage 3 as a function of the error covariance (the GLS fit given Σ̂).
    const auto gls = [&](const std::vector<double>& Sg) -> bool {
        r.Sigma = Sg;
        std::vector<double> Siginv = Sg;
        if (la::spd_inverse(Siginv.data(), M) != 0) return false;

        // Stage 3: stacked GLS. sZpZ[(i,a),(j,b)] = Σ̂⁻¹[i,j] (Ẑ_i'Ẑ_j)[a,b];
        //          sZpy[(i,a)] = Σ_j Σ̂⁻¹[i,j] (Ẑ_i'y_j)[a].
        std::vector<double> sZpZ(static_cast<size_t>(K) * K, 0.0), sZpy(K, 0.0);
        for (int i = 0; i < M; ++i) {
            int ki = eqs[i].k, oi = r.koff[i];
            for (int j = 0; j < M; ++j) {
                int kj = eqs[j].k, oj = r.koff[j];
                double sij = Siginv[static_cast<size_t>(i) * M + j];
                // Ẑ_i'Ẑ_j (ki x kj)
                for (int a = 0; a < ki; ++a) {
                    const double* za = Zhat[i].data() + static_cast<int64_t>(a) * n;
                    for (int b = 0; b < kj; ++b) {
                        const double* zb = Zhat[j].data() + static_cast<int64_t>(b) * n;
                        double s = 0;
                        for (int64_t t = 0; t < n; ++t) s += za[t] * zb[t];
                        sZpZ[static_cast<size_t>(oi + a) * K + (oj + b)] += sij * s;
                    }
                    // contribution to sZpy: Σ̂⁻¹[i,j] Ẑ_i' y_j
                    double sy = 0;
                    for (int64_t t = 0; t < n; ++t) sy += za[t] * eqs[j].y[t];
                    sZpy[oi + a] += sij * sy;
                }
            }
        }
        std::vector<double> V = sZpZ;
        if (la::spd_inverse(V.data(), K) != 0) return false;
        r.vcov = V;
        r.beta.assign(K, 0.0);
        la::matvec(V.data(), K, sZpy.data(), r.beta.data());
        r.se.resize(K);
        for (int c = 0; c < K; ++c) r.se[c] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * K + c]));
        return true;
    };
    if (!gls(Sig)) return r;
    // isure: Stata's reg3 loop. Its pass 0 is the eq-by-eq fit (Σ = I), pass 1 the
    // two-step fit above; each later pass takes Σ̂ = E'E/n from the previous pass's
    // residuals (ACTUAL regressors) and stops once mreldif(b, b_prev) =
    // max |b - b_prev| / (|b_prev| + 1) < tol or the pass count reaches iterate_max.
    if (iterate_max > 0) {
        const auto mreldif = [](const std::vector<double>& x, const std::vector<double>& y) {
            double m = 0;
            for (size_t i = 0; i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - y[i]) / (std::fabs(y[i]) + 1.0));
            return m;
        };
        int itcnt = 1;
        double rdiff = mreldif(r.beta, b_first);
        while (!(itcnt >= iterate_max || rdiff < tol)) {
            const std::vector<double> prev = r.beta;
            std::vector<double> Sg(static_cast<size_t>(M) * M, 0.0);
            for (int i = 0; i < M; ++i) {
                const Reg3Eq& e = eqs[i];
                for (int64_t t = 0; t < n; ++t) {
                    double f = 0;
                    for (int c = 0; c < e.k; ++c) f += e.Z[static_cast<int64_t>(c) * n + t] * prev[static_cast<size_t>(r.koff[i] + c)];
                    resid[i][t] = e.y[t] - f;
                }
            }
            for (int i = 0; i < M; ++i)
                for (int j = 0; j < M; ++j) {
                    double s2 = 0;
                    for (int64_t t = 0; t < n; ++t) s2 += resid[i][t] * resid[j][t];
                    Sg[static_cast<size_t>(i) * M + j] = s2 / static_cast<double>(n);
                }
            if (!gls(Sg)) return r;
            ++itcnt;
            rdiff = mreldif(r.beta, prev);
        }
        r.ic = itcnt;
        r.converged = rdiff < tol;
    }
    r.ok = true;
    return r;
}

}  // namespace cecon
