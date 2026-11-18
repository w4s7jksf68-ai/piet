// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Stata-matching detection and omission of collinear regressors.
//
// Rank-deficient designs arise mainly from factor-variable interactions with
// structurally empty cells (e.g. `i.foreign##i.rep78` on sysuse auto, where
// foreign cars have rep78 in {3,4,5} only, so 1.foreign == 1.foreign#3.rep78 +
// 1.foreign#4.rep78 + 1.foreign#5.rep78 exactly). The model fit is unique, but
// the coefficient parameterization is not, so we must drop the SAME column Stata
// drops to match `regress` coefficient-for-coefficient.
//
// The forward pass retains the original term order for callers whose contract
// requires it (instrument and differenced-design selection). OLS uses the
// pivoted pass below: scaled proportional columns need not retain the earlier
// term (w2ak_estout_family m7 and w2an_coefplot_family om).
#pragma once
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace cecon {

// Forward rank-revealing pass over a column-major design X (n x k). Returns a
// length-k keep mask: keep[j]==1 to retain column j, ==0 to omit it because it
// lies in the span of the earlier KEPT columns (to relative tolerance `tol`) or
// is all-zero. Earlier columns always win, so the LATER member of a collinear
// set is the one dropped. A full-rank design
// returns all ones (the pass is then a no-op for the caller).
//
// Robustness: modified Gram-Schmidt with one reorthogonalization pass ("twice is
// enough"). The acceptance test is scale-invariant per column (residual norm
// relative to the column's own norm == sin of its angle to the accepted span),
// so exact collinearity from 0/1 indicator arithmetic yields a residual ratio at
// machine-epsilon level, far below `tol`, while merely-correlated full-rank
// columns stay well above it.
// `late` (optional, size k): columns flagged there are scanned after every other
// column, each group in design order (ModelData::late_cells).
inline std::vector<char> collinear_keep_mask(const double* X, int64_t n, int k,
                                             double tol = 1e-7,
                                             const std::vector<char>* late = nullptr) {
    std::vector<char> keep(static_cast<size_t>(k), 1);
    std::vector<std::vector<double>> Q;  // orthonormal basis of the accepted columns
    Q.reserve(static_cast<size_t>(k));
    std::vector<double> v(static_cast<size_t>(n));
    std::vector<int> order;
    order.reserve(static_cast<size_t>(k));
    const bool useLate = late != nullptr && late->size() == static_cast<size_t>(k);
    for (int pass = 0; pass < (useLate ? 2 : 1); ++pass)
        for (int j = 0; j < k; ++j)
            if (!useLate || ((*late)[static_cast<size_t>(j)] != 0) == (pass == 1)) order.push_back(j);
    for (int j : order) {
        const double* xj = X + static_cast<int64_t>(j) * n;
        double orig = 0.0;
        for (int64_t t = 0; t < n; ++t) { v[static_cast<size_t>(t)] = xj[t]; orig += xj[t] * xj[t]; }
        orig = std::sqrt(orig);
        if (orig <= 0.0) { keep[static_cast<size_t>(j)] = 0; continue; }  // all-zero column
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& q : Q) {
                double d = 0.0;
                for (int64_t t = 0; t < n; ++t) d += q[static_cast<size_t>(t)] * v[static_cast<size_t>(t)];
                for (int64_t t = 0; t < n; ++t) v[static_cast<size_t>(t)] -= d * q[static_cast<size_t>(t)];
            }
        double nrm = 0.0;
        for (int64_t t = 0; t < n; ++t) nrm += v[static_cast<size_t>(t)] * v[static_cast<size_t>(t)];
        nrm = std::sqrt(nrm);
        if (nrm / orig < tol) { keep[static_cast<size_t>(j)] = 0; continue; }  // collinear
        for (int64_t t = 0; t < n; ++t) v[static_cast<size_t>(t)] /= nrm;
        Q.push_back(v);
    }
    return keep;
}

// Rank-revealing column pivoting for least squares. Retain the column with the
// largest unexplained norm at each step (the diagonal Schur-complement pivot),
// with original order breaking ties. Protect the model's intercept before the
// other pivots. Masks stay in ORIGINAL order, so the solve and coefficient order
// do not change on a full-rank design. The ordinary forward pass is the fast
// path: only a deficient design allocates the pivoted residual matrix.
inline std::vector<char> collinear_pivot_keep_mask(const double* X, int64_t n, int k,
                                                   int intercept = -1, double tol = 1e-7) {
    std::vector<char> forward = collinear_keep_mask(X, n, k, tol);
    bool full_rank = true;
    for (char c : forward) full_rank = full_rank && c;
    if (full_rank) return forward;

    std::vector<double> residual(X, X + static_cast<int64_t>(k) * n);
    std::vector<double> orig(static_cast<size_t>(k), 0.0), norm(static_cast<size_t>(k), 0.0);
    std::vector<char> keep(static_cast<size_t>(k), 0), done(static_cast<size_t>(k), 0);
    for (int j = 0; j < k; ++j) {
        const double* x = X + static_cast<int64_t>(j) * n;
        for (int64_t i = 0; i < n; ++i) orig[j] += x[i] * x[i];
        norm[j] = orig[j];
    }
    for (int step = 0; step < k; ++step) {
        int pivot = -1;
        if (step == 0 && intercept >= 0 && intercept < k) pivot = intercept;
        else {
            for (int j = 0; j < k; ++j)
                if (!done[j] && (pivot < 0 || norm[j] > norm[pivot])) pivot = j;
        }
        if (pivot < 0) break;
        done[pivot] = 1;
        if (!(orig[pivot] > 0.0) || norm[pivot] / orig[pivot] < tol * tol) continue;
        keep[pivot] = 1;
        double* q = residual.data() + static_cast<int64_t>(pivot) * n;
        const double invnorm = 1.0 / std::sqrt(norm[pivot]);
        for (int64_t i = 0; i < n; ++i) q[i] *= invnorm;
        for (int j = 0; j < k; ++j) {
            if (done[j]) continue;
            double* v = residual.data() + static_cast<int64_t>(j) * n;
            for (int pass = 0; pass < 2; ++pass) {
                double dot = 0.0;
                for (int64_t i = 0; i < n; ++i) dot += q[i] * v[i];
                for (int64_t i = 0; i < n; ++i) v[i] -= dot * q[i];
            }
            norm[j] = 0.0;
            for (int64_t i = 0; i < n; ++i) norm[j] += v[i] * v[i];
        }
    }
    return keep;
}

// Stata `regress`'s choice of the omitted member of a collinear set, for an
// unweighted fit with a constant and no absorbed effects: the sweep of the
// CENTERED cross-product matrix, taking at each step the column with the largest
// remaining diagonal (the earlier one when two are equal) and dropping a column
// whose diagonal has fallen to the tolerance. Identified on StataBE 19.5 (bank):
// five 0/1 dummies that sum to one beside the constant drop the one whose share
// leaves it last (`regress price F2 F1 L0 L1 L2` on auto omits F2; the nlsw88
// age bands omit L0 -- in both the last two diagonals are equal in exact
// arithmetic and the centered sweep's rounding decides, which the uncentered
// Gram-Schmidt pass above decided the other way), a rescaled copy drops the
// smaller column (`mpg weight w2 m2` with w2 = 2*weight, m2 = mpg+1 omits weight
// and m2), an identical copy the later one (`w2 mpg weight` with w2 = weight
// omits weight), a constant column always. Full-rank designs take the forward
// pass's all-ones mask unchanged.
inline std::vector<char> collinear_sweep_keep_mask(const double* X, int64_t n, int k,
                                                   int intercept, double tol = 1e-7) {
    std::vector<char> forward = collinear_keep_mask(X, n, k, tol);
    bool full_rank = true;
    for (char c : forward) full_rank = full_rank && c;
    if (full_rank) return forward;

    std::vector<int> cols;
    for (int j = 0; j < k; ++j)
        if (j != intercept) cols.push_back(j);
    const size_t m = cols.size();
    std::vector<double> mean(m, 0.0);
    for (size_t a = 0; a < m; ++a) {
        const double* x = X + static_cast<int64_t>(cols[a]) * n;
        double s = 0.0;
        for (int64_t i = 0; i < n; ++i) s += x[i];
        mean[a] = s / static_cast<double>(n);
    }
    std::vector<double> A(m * m, 0.0);
    for (size_t a = 0; a < m; ++a) {
        const double* xa = X + static_cast<int64_t>(cols[a]) * n;
        for (size_t b = 0; b < m; ++b) {
            const double* xb = X + static_cast<int64_t>(cols[b]) * n;
            double s = 0.0;
            for (int64_t i = 0; i < n; ++i) s += (xa[i] - mean[a]) * (xb[i] - mean[b]);
            A[a * m + b] = s;
        }
    }
    std::vector<double> orig(m);
    for (size_t a = 0; a < m; ++a) orig[a] = A[a * m + a];
    const std::vector<double> A0 = A;  // the centered cross-product before the sweep
    std::vector<char> keep(static_cast<size_t>(k), 0), done(m, 0);
    if (intercept >= 0 && intercept < k) keep[static_cast<size_t>(intercept)] = 1;
    for (size_t step = 0; step < m; ++step) {
        long p = -1;
        for (size_t j = 0; j < m; ++j)
            if (!done[j] && (p < 0 || A[j * m + j] > A[static_cast<size_t>(p) * m + static_cast<size_t>(p)]))
                p = static_cast<long>(j);
        if (p < 0) break;
        const size_t q = static_cast<size_t>(p);
        done[q] = 1;
        if (!(orig[q] > 0.0) || A[q * m + q] <= orig[q] * tol * tol) continue;  // collinear
        keep[static_cast<size_t>(cols[q])] = 1;
        const double d = A[q * m + q];
        for (size_t i = 0; i < m; ++i) {
            if (i == q) continue;
            for (size_t j = 0; j < m; ++j) {
                if (j == q) continue;
                A[i * m + j] -= A[i * m + q] * A[q * m + j] / d;
            }
        }
        for (size_t i = 0; i < m; ++i) {
            if (i != q) A[i * m + q] = A[q * m + i] = 0.0;
        }
        A[q * m + q] = 0.0;
    }
    // The sweep found the design full rank where the forward pass did not (a
    // borderline case at the tolerance): the pivoted pass decides.
    bool all_kept = true;
    for (char c : keep) all_kept = all_kept && c;
    std::vector<char> pivoted = collinear_pivot_keep_mask(X, n, k, intercept, tol);
    if (all_kept || keep == pivoted) return pivoted;
    // The sweep's choice is taken only for the dependency shapes the bank measured:
    // each omitted column a copy of ONE kept column up to scale and shift (72b55920
    // `mpg weight w2 m2`), or a member of a set of 0/1 dummies that adds up to the
    // constant (the F2 F1 L0 L1 L2 and nlsw88 age-band designs above). Any other
    // dependency (a variable equal to a sum of several others) keeps the pivoted
    // pass's choice: no Stata answer has been measured for it.
    std::vector<size_t> kept;
    for (size_t a = 0; a < m; ++a)
        if (keep[static_cast<size_t>(cols[a])] && orig[a] > 0.0) kept.push_back(a);
    const auto is01 = [&](size_t a) {
        const double* x = X + static_cast<int64_t>(cols[a]) * n;
        for (int64_t i = 0; i < n; ++i)
            if (x[i] != 0.0 && x[i] != 1.0) return false;
        return true;
    };
    for (size_t d = 0; d < m; ++d) {
        if (keep[static_cast<size_t>(cols[d])] || !(orig[d] > 0.0)) continue;
        // Least squares of the centered column d on the centered kept columns
        // (Cholesky of their cross-product, positive definite: they are independent).
        const size_t q = kept.size();
        std::vector<double> L(q * q, 0.0), beta(q, 0.0);
        bool ok = true;
        for (size_t i = 0; ok && i < q; ++i)
            for (size_t j = 0; j <= i; ++j) {
                double v = A0[kept[i] * m + kept[j]];
                for (size_t t = 0; t < j; ++t) v -= L[i * q + t] * L[j * q + t];
                if (i == j) {
                    if (!(v > 0.0)) { ok = false; break; }
                    L[i * q + i] = std::sqrt(v);
                } else {
                    L[i * q + j] = v / L[j * q + j];
                }
            }
        if (!ok) return pivoted;
        for (size_t i = 0; i < q; ++i) {
            double v = A0[kept[i] * m + d];
            for (size_t t = 0; t < i; ++t) v -= L[i * q + t] * beta[t];
            beta[i] = v / L[i * q + i];
        }
        for (size_t i = q; i-- > 0;) {
            double v = beta[i];
            for (size_t t = i + 1; t < q; ++t) v -= L[t * q + i] * beta[t];
            beta[i] = v / L[i * q + i];
        }
        size_t members = 0;
        bool dummies = is01(d);
        double shift = mean[d];  // the constant in x_d = c + SUM beta_i x_i
        for (size_t i = 0; i < q; ++i) {
            shift -= beta[i] * mean[kept[i]];
            if (std::fabs(beta[i]) * std::sqrt(orig[kept[i]]) > 1e-6 * std::sqrt(orig[d])) {
                ++members;
                dummies = dummies && is01(kept[i]);
            }
        }
        // a dummy set completes to the constant only when c is not 0
        if (members > 1 && !(dummies && std::fabs(shift) > 1e-6)) return pivoted;
    }
    return keep;
}

// Compact a column-major design `X` (length n*k) in place, retaining only the
// columns with keep[j]!=0 (moved left, preserving order) and shrinking `names`
// to match. Updates `k` to the surviving count and resizes `X`. Returns the
// names of the dropped columns in their original order (for the `omitted[]`
// report). No-op returning {} if every column is kept.
inline std::vector<std::string> compact_columns(std::vector<double>& X, int64_t n, int& k,
                                                std::vector<std::string>& names,
                                                const std::vector<char>& keep) {
    std::vector<std::string> omitted;
    std::vector<std::string> surviving;
    surviving.reserve(static_cast<size_t>(k));
    int dst = 0;
    for (int j = 0; j < k; ++j) {
        if (keep[static_cast<size_t>(j)]) {
            if (dst != j)
                std::memmove(X.data() + static_cast<int64_t>(dst) * n,
                             X.data() + static_cast<int64_t>(j) * n,
                             static_cast<size_t>(n) * sizeof(double));
            surviving.push_back(std::move(names[static_cast<size_t>(j)]));
            ++dst;
        } else {
            omitted.push_back(std::move(names[static_cast<size_t>(j)]));
        }
    }
    k = dst;
    X.resize(static_cast<size_t>(n) * static_cast<size_t>(dst));
    names = std::move(surviving);
    return omitted;
}

}  // namespace cecon
