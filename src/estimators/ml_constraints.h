// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — linear equality constraints R·θ = r on an ml Newton-Raphson fit
// (Stata's `constraints()` option of the ml-based estimators).
//
// Method (ml's documented treatment of constraints): the free parameter space is
// θ = T·z + a, where the columns of T are an orthonormal basis of the null space
// of R and a = R'(RR')^{-1} r is the minimum-norm point satisfying the
// constraints. The starting values are projected onto the constraint set
// (θ0 <- T·T'·θ0 + a), each Newton step is taken in z, so in θ it is
//     step = T · inv(T'(−H)T) · T' · g,
// and the variance of θ is V = T · inv(T'(−H)T) · T' (zero along every direction
// the constraints fix). Newton's method is affine invariant, so the path in θ —
// and with it the iteration log — does not depend on which basis T is used. A
// coefficient a constraint fixes by itself has an exactly zero row of T, so it
// stays at its fixed value to the bit and its variance is exactly 0.
//
// Usage inside an optimizer that already runs on V = inv(−H): build an MlCns,
// project the start with project(), and replace spd_inverse(−H) with
// inverse(−H): every step, convergence statistic g'·V·g and the final V then
// carry the constraints.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct MlCns {
    int np = 0;               // parameters
    int m = 0;                // independent constraints kept
    int nfree = 0;            // np - m
    std::vector<double> Q;    // m x np, row-major: orthonormal basis of R's row space
    std::vector<double> T;    // np x nfree, col-major: orthonormal basis of R's null space
    std::vector<double> a;    // np: minimum-norm point with R·a = r
    std::vector<int> dropped; // 0-based input rows that were linearly dependent on earlier ones
    bool active() const { return m > 0; }
};

// R is nrow x np row-major, r length nrow. A row that is (numerically) a linear
// combination of earlier rows is dropped, as Stata's makecns drops a redundant
// constraint; returns false when such a row contradicts the earlier ones (no θ
// satisfies all of them) or a row is all zero with a nonzero right-hand side.
inline bool ml_cns_build(const std::vector<double>& R, const std::vector<double>& r, int nrow, int np,
                         MlCns& out) {
    out = MlCns();
    out.np = np;
    std::vector<double> Rk, rk;  // the independent rows
    std::vector<double> Q;       // their Gram-Schmidt orthonormal basis
    int m = 0;
    for (int i = 0; i < nrow; ++i) {
        std::vector<double> v(R.begin() + static_cast<std::ptrdiff_t>(i) * np,
                              R.begin() + static_cast<std::ptrdiff_t>(i + 1) * np);
        double n0 = 0.0;
        for (double x : v) n0 += x * x;
        n0 = std::sqrt(n0);
        for (int pass = 0; pass < 2; ++pass)
            for (int q = 0; q < m; ++q) {
                double d = 0.0;
                for (int j = 0; j < np; ++j) d += Q[static_cast<size_t>(q) * np + j] * v[static_cast<size_t>(j)];
                if (d != 0.0)
                    for (int j = 0; j < np; ++j) v[static_cast<size_t>(j)] -= d * Q[static_cast<size_t>(q) * np + j];
            }
        double nv = 0.0;
        for (double x : v) nv += x * x;
        nv = std::sqrt(nv);
        if (!(n0 > 0.0) || nv <= 1e-10 * n0) {
            // dependent: consistent only if r_i equals the same combination of the kept r
            // (checked below once a is known)
            out.dropped.push_back(i);
            continue;
        }
        for (double& x : v) x /= nv;
        Q.insert(Q.end(), v.begin(), v.end());
        Rk.insert(Rk.end(), R.begin() + static_cast<std::ptrdiff_t>(i) * np,
                  R.begin() + static_cast<std::ptrdiff_t>(i + 1) * np);
        rk.push_back(r[static_cast<size_t>(i)]);
        ++m;
    }
    out.m = m;
    out.nfree = np - m;
    out.Q = Q;
    out.a.assign(static_cast<size_t>(np), 0.0);
    if (m > 0) {
        // a = Rk' (Rk Rk')^{-1} rk
        std::vector<double> G(static_cast<size_t>(m) * m, 0.0);
        for (int i = 0; i < m; ++i)
            for (int j = 0; j < m; ++j) {
                double s = 0.0;
                for (int p = 0; p < np; ++p)
                    s += Rk[static_cast<size_t>(i) * np + p] * Rk[static_cast<size_t>(j) * np + p];
                G[static_cast<size_t>(j) * m + i] = s;
            }
        if (la::spd_inverse(G.data(), m) != 0) return false;
        std::vector<double> y(static_cast<size_t>(m), 0.0);
        for (int i = 0; i < m; ++i)
            for (int j = 0; j < m; ++j) y[static_cast<size_t>(i)] += G[static_cast<size_t>(j) * m + i] * rk[static_cast<size_t>(j)];
        for (int p = 0; p < np; ++p) {
            double s = 0.0;
            for (int i = 0; i < m; ++i) s += Rk[static_cast<size_t>(i) * np + p] * y[static_cast<size_t>(i)];
            out.a[static_cast<size_t>(p)] = s;
        }
    }
    // a dropped row must hold at a (it is implied by the kept ones)
    for (int i : out.dropped) {
        double s = 0.0, sc = std::fabs(r[static_cast<size_t>(i)]);
        for (int p = 0; p < np; ++p) {
            s += R[static_cast<size_t>(i) * np + p] * out.a[static_cast<size_t>(p)];
            sc = std::max(sc, std::fabs(R[static_cast<size_t>(i) * np + p]));
        }
        if (std::fabs(s - r[static_cast<size_t>(i)]) > 1e-8 * (sc + 1.0)) return false;
    }
    // T: the unit vectors, in order, orthogonalised against Q and the accepted columns
    std::vector<double> T;
    int nt = 0;
    for (int e = 0; e < np && nt < out.nfree; ++e) {
        std::vector<double> v(static_cast<size_t>(np), 0.0);
        v[static_cast<size_t>(e)] = 1.0;
        for (int pass = 0; pass < 2; ++pass) {
            for (int q = 0; q < m; ++q) {
                double d = 0.0;
                for (int j = 0; j < np; ++j) d += Q[static_cast<size_t>(q) * np + j] * v[static_cast<size_t>(j)];
                if (d != 0.0)
                    for (int j = 0; j < np; ++j) v[static_cast<size_t>(j)] -= d * Q[static_cast<size_t>(q) * np + j];
            }
            for (int c = 0; c < nt; ++c) {
                double d = 0.0;
                for (int j = 0; j < np; ++j) d += T[static_cast<size_t>(c) * np + j] * v[static_cast<size_t>(j)];
                if (d != 0.0)
                    for (int j = 0; j < np; ++j) v[static_cast<size_t>(j)] -= d * T[static_cast<size_t>(c) * np + j];
            }
        }
        double nv = 0.0;
        for (double x : v) nv += x * x;
        nv = std::sqrt(nv);
        if (nv <= 1e-8) continue;
        if (nv != 1.0)
            for (double& x : v) x /= nv;
        T.insert(T.end(), v.begin(), v.end());
        ++nt;
    }
    if (nt != out.nfree) return false;
    out.T = std::move(T);
    return true;
}

// θ <- T·T'·θ + a  (= θ − Q'Q·θ + a)
inline void ml_cns_project(const MlCns& c, std::vector<double>& theta) {
    if (!c.active()) return;
    const int np = c.np;
    std::vector<double> out(static_cast<size_t>(np), 0.0);
    for (int col = 0; col < c.nfree; ++col) {
        double d = 0.0;
        for (int j = 0; j < np; ++j) d += c.T[static_cast<size_t>(col) * np + j] * theta[static_cast<size_t>(j)];
        if (d != 0.0)
            for (int j = 0; j < np; ++j) out[static_cast<size_t>(j)] += d * c.T[static_cast<size_t>(col) * np + j];
    }
    for (int j = 0; j < np; ++j) theta[static_cast<size_t>(j)] = out[static_cast<size_t>(j)] + c.a[static_cast<size_t>(j)];
}

// The gradient a constrained fit reports: its component in the free directions,
// T·T'·g (zero along what the constraints fix), as ml posts e(gradient).
inline void ml_cns_project_gradient(const MlCns& c, std::vector<double>& g) {
    if (!c.active()) return;
    const int np = c.np;
    for (int q = 0; q < c.m; ++q) {
        double d = 0.0;
        for (int j = 0; j < np; ++j) d += c.Q[static_cast<size_t>(q) * np + j] * g[static_cast<size_t>(j)];
        if (d != 0.0)
            for (int j = 0; j < np; ++j) g[static_cast<size_t>(j)] -= d * c.Q[static_cast<size_t>(q) * np + j];
    }
}

// A (np x np, full symmetric, holds −H) <- T · inv(T'·A·T) · T'. Returns the
// spd_inverse status of the reduced matrix (0 = success). Without constraints it
// is spd_inverse(A) itself.
inline int ml_cns_inverse(const MlCns& c, double* A) {
    const int np = c.np;
    if (!c.active()) return la::spd_inverse(A, np);
    const int nf = c.nfree;
    if (nf == 0) {
        for (int i = 0; i < np * np; ++i) A[i] = 0.0;
        return 0;
    }
    // AT = A·T (np x nf), M = T'·AT (nf x nf)
    std::vector<double> AT(static_cast<size_t>(np) * nf, 0.0), M(static_cast<size_t>(nf) * nf, 0.0);
    for (int col = 0; col < nf; ++col)
        for (int j = 0; j < np; ++j) {
            const double t = c.T[static_cast<size_t>(col) * np + j];
            if (t == 0.0) continue;
            for (int i = 0; i < np; ++i) AT[static_cast<size_t>(col) * np + i] += A[static_cast<size_t>(j) * np + i] * t;
        }
    for (int q = 0; q < nf; ++q)
        for (int p = 0; p < nf; ++p) {
            double s = 0.0;
            for (int i = 0; i < np; ++i) s += c.T[static_cast<size_t>(p) * np + i] * AT[static_cast<size_t>(q) * np + i];
            M[static_cast<size_t>(q) * nf + p] = s;
        }
    for (int q = 0; q < nf; ++q)  // symmetrise the rounding
        for (int p = q + 1; p < nf; ++p) {
            const double s = 0.5 * (M[static_cast<size_t>(q) * nf + p] + M[static_cast<size_t>(p) * nf + q]);
            M[static_cast<size_t>(q) * nf + p] = M[static_cast<size_t>(p) * nf + q] = s;
        }
    const int info = la::spd_inverse(M.data(), nf);
    if (info != 0) return info;
    // A = T·M·T'
    std::vector<double> TM(static_cast<size_t>(np) * nf, 0.0);  // T·M, np x nf
    for (int q = 0; q < nf; ++q)
        for (int p = 0; p < nf; ++p) {
            const double mv = M[static_cast<size_t>(q) * nf + p];
            if (mv == 0.0) continue;
            for (int i = 0; i < np; ++i) TM[static_cast<size_t>(q) * np + i] += c.T[static_cast<size_t>(p) * np + i] * mv;
        }
    for (int j = 0; j < np; ++j)
        for (int i = 0; i < np; ++i) {
            double s = 0.0;
            for (int q = 0; q < nf; ++q) s += TM[static_cast<size_t>(q) * np + i] * c.T[static_cast<size_t>(q) * np + j];
            A[static_cast<size_t>(j) * np + i] = s;
        }
    return 0;
}

// Wald statistic b'·W⁻·b over the sub-block `idx` of V, with W⁻ the generalized
// inverse that sweeps W's pivots in order and skips one whose remaining variance
// is (numerically) zero — what `test` does with the coefficients a constraint
// fixes. *df receives the number of pivots swept.
inline double ml_cns_wald(const std::vector<double>& V, int np, const std::vector<double>& theta,
                          const std::vector<int>& idx, int* df) {
    const int k = static_cast<int>(idx.size());
    std::vector<double> W(static_cast<size_t>(k) * k);
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < k; ++j)
            W[static_cast<size_t>(j) * k + i] = V[static_cast<size_t>(idx[static_cast<size_t>(j)]) * np + idx[static_cast<size_t>(i)]];
    std::vector<double> d0(static_cast<size_t>(k));
    for (int i = 0; i < k; ++i) d0[static_cast<size_t>(i)] = W[static_cast<size_t>(i) * k + i];
    std::vector<bool> swept(static_cast<size_t>(k), false);
    int rank = 0;
    for (int p = 0; p < k; ++p) {
        const double piv = W[static_cast<size_t>(p) * k + p];
        if (!(piv > 1e-13 * std::max(d0[static_cast<size_t>(p)], 0.0)) || !(piv > 0.0)) continue;
        // sweep on p
        for (int i = 0; i < k; ++i)
            for (int j = 0; j < k; ++j)
                if (i != p && j != p)
                    W[static_cast<size_t>(j) * k + i] -= W[static_cast<size_t>(p) * k + i] * W[static_cast<size_t>(j) * k + p] / piv;
        for (int i = 0; i < k; ++i)
            if (i != p) {
                W[static_cast<size_t>(p) * k + i] /= piv;
                W[static_cast<size_t>(i) * k + p] /= piv;
            }
        W[static_cast<size_t>(p) * k + p] = -1.0 / piv;
        swept[static_cast<size_t>(p)] = true;
        ++rank;
    }
    double chi2 = 0.0;
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < k; ++j)
            if (swept[static_cast<size_t>(i)] && swept[static_cast<size_t>(j)])
                chi2 -= theta[static_cast<size_t>(idx[static_cast<size_t>(i)])] * W[static_cast<size_t>(j) * k + i] *
                        theta[static_cast<size_t>(idx[static_cast<size_t>(j)])];
    if (df) *df = rank;
    return chi2;
}

}  // namespace cecon
