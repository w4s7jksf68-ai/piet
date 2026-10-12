// cecon — Stata-matching detection and omission of collinear regressors.
//
// Rank-deficient designs arise mainly from factor-variable interactions with
// structurally empty cells (e.g. `i.foreign##i.rep78` on sysuse auto, where
// foreign cars have rep78 in {3,4,5} only, so 1.foreign == 1.foreign#3.rep78 +
// 1.foreign#4.rep78 + 1.foreign#5.rep78 exactly). The model fit is unique, but
// the coefficient parameterization is not, so we must drop the SAME column Stata
// drops to match `regress` coefficient-for-coefficient.
//
// Stata's `regress` sweeps the design left to right and omits a term once it is
// collinear with the already-swept (kept) terms — i.e. it keeps the EARLIER term
// and drops the LATER one. A forward modified Gram-Schmidt over the design
// columns (the same rank-revealing device used by the White IM-test in
// diagtests.h) reproduces that rule exactly: process columns in order, accept a
// column only if it has a component orthogonal to the accepted set, otherwise
// mark it omitted. CoBrA generates factor-variable columns in Stata's term order
// (main effects by ascending subset size, levels ascending within a factor), so
// the later/higher-indexed collinear term is the one dropped — as in Stata.
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
// set is the one dropped — matching Stata's sweep order. A full-rank design
// returns all ones (the pass is then a no-op for the caller).
//
// Robustness: modified Gram-Schmidt with one reorthogonalization pass ("twice is
// enough"). The acceptance test is scale-invariant per column (residual norm
// relative to the column's own norm == sin of its angle to the accepted span),
// so exact collinearity from 0/1 indicator arithmetic yields a residual ratio at
// machine-epsilon level, far below `tol`, while merely-correlated full-rank
// columns stay well above it.
inline std::vector<char> collinear_keep_mask(const double* X, int64_t n, int k,
                                             double tol = 1e-7) {
    std::vector<char> keep(static_cast<size_t>(k), 1);
    std::vector<std::vector<double>> Q;  // orthonormal basis of the accepted columns
    Q.reserve(static_cast<size_t>(k));
    std::vector<double> v(static_cast<size_t>(n));
    for (int j = 0; j < k; ++j) {
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
