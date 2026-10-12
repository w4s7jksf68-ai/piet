// cecon — LIML / Fuller k-class IV estimator, matching Stata `ivregress liml`.
// k-class: β_κ = {X'(I−κM_Z)X}^{-1} X'(I−κM_Z)y, X=[endog, exog incl const],
// M_Z=I−Z(Z'Z)^{-1}Z' (Z=all instruments). κ_LIML = min generalized eigenvalue of
// Ỹ'M_X1 Ỹ v = κ Ỹ'M_Z Ỹ v, Ỹ=[y, endog], M_X1 annihilator of the included exog.
// Fuller(α): κ = κ_LIML − α/(N−L), L = #instruments. κ=1 ⇒ 2SLS. VCE = σ̂²·bread.
#pragma once
#include <cmath>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct LimlResult {
    std::vector<double> beta, se, vcov;
    double kappa = 0, sigma2 = 0;
    int64_t n = 0;
    double dof_r = 0;
    bool ok = false;
};

namespace liml {
// out(k x k) = A' B^{-1} A where A is (m x k), B^{-1} given (m x m). Returns A'B⁻¹A.
inline std::vector<double> quad(const std::vector<double>& A, int64_t m, int k,
                                const std::vector<double>& Binv) {
    // T = B^{-1} A  (m x k)
    std::vector<double> T(static_cast<size_t>(m) * k, 0.0);
    for (int c = 0; c < k; ++c)
        for (int64_t i = 0; i < m; ++i) {
            double s = 0;
            for (int64_t j = 0; j < m; ++j) s += Binv[static_cast<size_t>(j) * m + i] * A[static_cast<int64_t>(c) * m + j];
            T[static_cast<int64_t>(c) * m + i] = s;
        }
    std::vector<double> out(static_cast<size_t>(k) * k, 0.0);
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b) {
            double s = 0;
            for (int64_t i = 0; i < m; ++i) s += A[static_cast<int64_t>(a) * m + i] * T[static_cast<int64_t>(b) * m + i];
            out[static_cast<size_t>(a) * k + b] = s;
        }
    return out;
}
}  // namespace liml

// y (n); Y = endogenous (p, col-major); X1 = included exogenous incl. constant (kx1);
// Z2 = excluded instruments (m). small => sigma2 = ESS/(N-k) and t-inference.
// fuller_alpha = 0 for plain LIML; >0 for Fuller(alpha). Set kappa_override>=0 to
// force a k-class value (e.g. 1.0 = 2SLS) instead of computing LIML.
inline LimlResult liml_fit(const std::vector<double>& y, int64_t n,
                           const std::vector<double>& Y, int p,
                           const std::vector<double>& X1, int kx1,
                           const std::vector<double>& Z2, int m,
                           bool small, double fuller_alpha = 0.0,
                           double kappa_override = -1.0) {
    LimlResult r;
    r.n = n;
    int kZ = kx1 + m;          // full instrument set Z = [X1, Z2]
    int kx = p + kx1;          // regressors X = [endog, X1]
    int ky = 1 + p;            // Ỹ = [y, endog]
    // assemble Z, X, Ytil (col-major)
    std::vector<double> Z(static_cast<size_t>(n) * kZ), X(static_cast<size_t>(n) * kx),
        Yt(static_cast<size_t>(n) * ky);
    std::copy(X1.begin(), X1.end(), Z.begin());
    std::copy(Z2.begin(), Z2.end(), Z.begin() + static_cast<int64_t>(kx1) * n);
    std::copy(Y.begin(), Y.end(), X.begin());
    std::copy(X1.begin(), X1.end(), X.begin() + static_cast<int64_t>(p) * n);
    std::copy(y.begin(), y.end(), Yt.begin());
    std::copy(Y.begin(), Y.end(), Yt.begin() + n);

    auto xtx_inv = [&](const std::vector<double>& M, int kk) {
        std::vector<double> G(static_cast<size_t>(kk) * kk);
        la::xtx(M.data(), n, kk, G.data());
        if (la::spd_inverse(G.data(), kk) != 0) return std::vector<double>();
        return G;
    };
    // M_Z and M_X1 cross-products for the eigenproblem
    std::vector<double> ZtZi = xtx_inv(Z, kZ), X1tX1i = xtx_inv(X1, kx1);
    if (ZtZi.empty() || X1tX1i.empty()) return r;
    // YtY (ky x ky), YtZ (ky x kZ), YtX1 (ky x kx1)
    auto cross = [&](const std::vector<double>& Am, int ka, const std::vector<double>& Bm, int kb) {
        std::vector<double> C(static_cast<size_t>(ka) * kb, 0.0);
        for (int a = 0; a < ka; ++a)
            for (int b = 0; b < kb; ++b) {
                double s = 0;
                for (int64_t i = 0; i < n; ++i) s += Am[static_cast<int64_t>(a) * n + i] * Bm[static_cast<int64_t>(b) * n + i];
                C[static_cast<size_t>(a) * kb + b] = s;
            }
        return C;
    };
    std::vector<double> YtY = cross(Yt, ky, Yt, ky);
    std::vector<double> YtZ = cross(Yt, ky, Z, kZ);    // ky x kZ
    std::vector<double> YtX1 = cross(Yt, ky, X1, kx1); // ky x kx1
    // A = Ỹ'M_X1 Ỹ = YtY - YtX1 (X1'X1)^-1 X1'Ỹ ;  B = Ỹ'M_Z Ỹ = YtY - YtZ(Z'Z)^-1 Z'Ỹ
    // (YtX1)' is kx1 x ky; reuse liml::quad with A-arg laid out m x k.
    auto proj_term = [&](const std::vector<double>& YtW, int kw, const std::vector<double>& WtWi) {
        // returns YtW (WtW)^-1 (YtW)'  (ky x ky). Build A2 = (YtW)' as (kw x ky)
        std::vector<double> A2(static_cast<size_t>(kw) * ky);
        for (int a = 0; a < ky; ++a)
            for (int w = 0; w < kw; ++w) A2[static_cast<int64_t>(a) * kw + w] = YtW[static_cast<size_t>(a) * kw + w];
        return liml::quad(A2, kw, ky, WtWi);  // (ky x ky) = A2' WtWi A2 = YtW WtWi YtW'
    };
    std::vector<double> A(static_cast<size_t>(ky) * ky), B(static_cast<size_t>(ky) * ky);
    {
        std::vector<double> px1 = proj_term(YtX1, kx1, X1tX1i);
        std::vector<double> pz = proj_term(YtZ, kZ, ZtZi);
        for (int i = 0; i < ky * ky; ++i) { A[i] = YtY[i] - px1[i]; B[i] = YtY[i] - pz[i]; }
    }
    double kappa;
    if (kappa_override >= 0) {
        kappa = kappa_override;
    } else {
        // B^{-1/2} via sym_eig(B); then eig(B^{-1/2} A B^{-1/2}); kappa = min eigenvalue
        std::vector<double> Bc = B, wB(ky);
        if (la::sym_eig(Bc.data(), ky, wB.data()) != 0) return r;
        std::vector<double> Bihalf(static_cast<size_t>(ky) * ky, 0.0);
        for (int j = 0; j < ky; ++j) {
            if (wB[j] <= 0) return r;
            double inv = 1.0 / std::sqrt(wB[j]);
            const double* vj = Bc.data() + static_cast<int64_t>(j) * ky;
            for (int c = 0; c < ky; ++c)
                for (int rr = 0; rr < ky; ++rr) Bihalf[static_cast<size_t>(c) * ky + rr] += inv * vj[rr] * vj[c];
        }
        // S = Bihalf * A * Bihalf
        std::vector<double> tmp(static_cast<size_t>(ky) * ky, 0.0), S(static_cast<size_t>(ky) * ky, 0.0);
        for (int a = 0; a < ky; ++a) for (int b = 0; b < ky; ++b) { double s = 0;
            for (int c = 0; c < ky; ++c) s += Bihalf[static_cast<size_t>(a) * ky + c] * A[static_cast<size_t>(c) * ky + b]; tmp[static_cast<size_t>(a) * ky + b] = s; }
        for (int a = 0; a < ky; ++a) for (int b = 0; b < ky; ++b) { double s = 0;
            for (int c = 0; c < ky; ++c) s += tmp[static_cast<size_t>(a) * ky + c] * Bihalf[static_cast<size_t>(c) * ky + b]; S[static_cast<size_t>(a) * ky + b] = s; }
        std::vector<double> wS(ky);
        if (la::sym_eig(S.data(), ky, wS.data()) != 0) return r;
        kappa = wS[0];  // ascending -> minimum
        if (fuller_alpha > 0) kappa -= fuller_alpha / static_cast<double>(n - kZ);
    }
    r.kappa = kappa;

    // k-class: M = X'(I-κM_Z)X = (1-κ)X'X + κ X'P_Z X ; rhs = (1-κ)X'y + κ X'P_Z y
    std::vector<double> XtX = cross(X, kx, X, kx);
    std::vector<double> Xty = cross(X, kx, Yt, 1);  // X'y is X' * (first col of Yt = y) -> kx x 1
    // X'P_Z X = (X'Z)(Z'Z)^-1(Z'X); X'P_Z y = (X'Z)(Z'Z)^-1(Z'y)
    std::vector<double> XtZ = cross(X, kx, Z, kZ);  // kx x kZ
    std::vector<double> Zty = cross(Z, kZ, Yt, 1);  // kZ x 1 (Z'y)
    // XPZX = XtZ ZtZi XtZ'
    std::vector<double> A2(static_cast<size_t>(kZ) * kx);
    for (int a = 0; a < kx; ++a) for (int w = 0; w < kZ; ++w) A2[static_cast<int64_t>(a) * kZ + w] = XtZ[static_cast<size_t>(a) * kZ + w];
    std::vector<double> XPZX = liml::quad(A2, kZ, kx, ZtZi);  // kx x kx
    // XPZy = XtZ ZtZi Zty
    std::vector<double> ZtZi_Zty(kZ, 0.0);
    for (int i = 0; i < kZ; ++i) { double s = 0; for (int j = 0; j < kZ; ++j) s += ZtZi[static_cast<size_t>(i) * kZ + j] * Zty[j]; ZtZi_Zty[i] = s; }
    std::vector<double> XPZy(kx, 0.0);
    for (int a = 0; a < kx; ++a) { double s = 0; for (int w = 0; w < kZ; ++w) s += XtZ[static_cast<size_t>(a) * kZ + w] * ZtZi_Zty[w]; XPZy[a] = s; }

    std::vector<double> M(static_cast<size_t>(kx) * kx), rhs(kx);
    for (int i = 0; i < kx * kx; ++i) M[i] = (1 - kappa) * XtX[i] + kappa * XPZX[i];
    for (int a = 0; a < kx; ++a) rhs[a] = (1 - kappa) * Xty[a] + kappa * XPZy[a];
    std::vector<double> Minv = M;
    if (la::spd_inverse(Minv.data(), kx) != 0) return r;
    r.beta.assign(kx, 0.0);
    la::matvec(Minv.data(), kx, rhs.data(), r.beta.data());
    // residuals from ACTUAL X, ESS, sigma2
    double ess = 0;
    for (int64_t i = 0; i < n; ++i) {
        double f = 0;
        for (int c = 0; c < kx; ++c) f += X[static_cast<int64_t>(c) * n + i] * r.beta[c];
        double e = y[i] - f; ess += e * e;
    }
    r.dof_r = static_cast<double>(n - kx);
    r.sigma2 = small ? ess / r.dof_r : ess / static_cast<double>(n);
    r.vcov.assign(static_cast<size_t>(kx) * kx, 0.0);
    for (int i = 0; i < kx * kx; ++i) r.vcov[i] = r.sigma2 * Minv[i];
    r.se.resize(kx);
    for (int c = 0; c < kx; ++c) r.se[c] = std::sqrt(std::max(0.0, r.vcov[static_cast<size_t>(c) * kx + c]));
    r.ok = true;
    return r;
}

}  // namespace cecon
