// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — multinomial (polytomous) logit MLE, matching Stata `mlogit`.
//
// Outcomes are recoded to 0..K-1 by SORTED DISTINCT VALUE. One outcome is the
// BASE: its equation is normalized to η ≡ 0 (all its coefficients are fixed at
// zero and are not parameters), so with the remaining K−1 equations
//
//     p_im = exp(η_im) / (1 + Σ_l exp(η_il)),     η_im = x_i·β_m,  l over non-base
//     p_i,base = 1 / (1 + Σ_l exp(η_il))
//
// and LL_i = log p_{i,y_i}. The per-observation score for equation m is
// (1[y_i = m] − p_im)·x_i.
//
// θ is the (K−1) equation blocks STACKED, equation-major, non-base outcomes in
// ascending outcome order, and within a block the design's own column order (which
// for load_glm is [expanded x…, intercept]). The score-block order is the same, so
// the sandwich meat's blocks line up with the bread's — a transposed block here
// yields wrong-but-plausible cluster SEs, which is why the two orders are stated
// once and shared.
//
// Analytic gradient and Hessian; spd_inverse(−H) for the model-based VCE. The optimizer is ml's
// Newton-Raphson from Stata's start (mlogit_fit below).
#pragma once
#include "core/threads.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/linalg.h"
#include "estimators/ml_constraints.h"

namespace cecon {

struct MlogitResult {
    int n_cats = 0;              // K
    int base = 0;                // 0-based index of the base outcome among the K
    std::vector<double> beta;    // (K-1)*k, equation-major (non-base, ascending)
    std::vector<double> se;      // same layout as beta (model-based)
    std::vector<double> vcov;    // np*np, np = (K-1)*k, same layout
    std::vector<int64_t> cat_n;  // per-outcome row counts
    double ll = 0;               // log-likelihood at the optimum
    double ll_0 = 0;             // constants-only closed form Σ_m n_m·ln(n_m/n)
    int64_t n = 0;
    int iters = 0;
    bool converged = false;
    bool ok = false;
    // ml's iteration log (the log likelihood at the start and after each Newton
    // iteration) and the gradient at the reported θ: e(ilog), e(gradient).
    std::vector<double> ilog;
    std::vector<double> gradient;
};

namespace mlg {

// Log-likelihood + analytic gradient at θ (np = (K-1)*k, equation-major). X is
// col-major n*k INCLUDING its intercept column. `cat` holds the 0-based outcome of
// each row; `eq_of_cat[c]` is the equation index of outcome c, or -1 for the base.
//
// scores: optional n*np COLUMN-major output (column p, row i at scores[p*n+i]) —
// filled with the SAME per-observation gradient contribution that is otherwise only
// summed into *grad, so la::xtx(scores, n, np, ...) gives the robust meat directly
// and summing score ROWS within a cluster first gives the cluster meat.
//
// As in tobit.h/oglm.h, the grad and scores accumulations are two INDEPENDENT
// branches that deliberately do not share an intermediate: `(*grad)[o] += c *
// X[...]` is one fused expression eligible for -ffp-contract FMA fusion, and
// hoisting the product into a shared temp forces an extra rounding step that moves
// the last bits of every downstream SE. No call site asks for both at once (the
// Newton loop passes grad only, the sandwich passes scores only).
inline double ll_grad(const double* X, int64_t n, int k, const int* cat, int K,
                      const std::vector<int>& eq_of_cat, const std::vector<double>& theta,
                      std::vector<double>* grad, std::vector<double>* scores = nullptr,
                      const double* wt = nullptr) {
    const int neq = K - 1;
    const int np = neq * k;
    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;
    std::vector<double> eta(static_cast<size_t>(neq)), p(static_cast<size_t>(neq));
    for (int64_t i = 0; i < n; ++i) {
        double emax = 0.0;  // the base equation's η ≡ 0 is always in the pool
        for (int e = 0; e < neq; ++e) {
            double v = 0;
            const double* b = theta.data() + static_cast<size_t>(e) * k;
            for (int c = 0; c < k; ++c) v += X[static_cast<int64_t>(c) * n + i] * b[c];
            eta[static_cast<size_t>(e)] = v;
            if (v > emax) emax = v;
        }
        double denom = std::exp(-emax);  // = exp(0 − emax), the base equation
        for (int e = 0; e < neq; ++e) {
            double ex = std::exp(eta[static_cast<size_t>(e)] - emax);
            p[static_cast<size_t>(e)] = ex;
            denom += ex;
        }
        for (int e = 0; e < neq; ++e) p[static_cast<size_t>(e)] /= denom;
        const int ei = eq_of_cat[static_cast<size_t>(cat[i])];  // -1 for the base outcome
        double lin = (ei >= 0) ? eta[static_cast<size_t>(ei)] : 0.0;
        // `wt`: observation weights (fweight / iweight / pweight), 1 when null; a
        // weight of exactly 1 leaves every product unchanged.
        const double wi = wt ? wt[i] : 1.0;
        LL += wi * (lin - emax - std::log(denom));
        if (grad) {
            for (int e = 0; e < neq; ++e) {
                double c = wi * (((e == ei) ? 1.0 : 0.0) - p[static_cast<size_t>(e)]);
                double* g = grad->data() + static_cast<size_t>(e) * k;
                for (int q = 0; q < k; ++q) g[q] += c * X[static_cast<int64_t>(q) * n + i];
            }
        }
        if (S) {
            for (int e = 0; e < neq; ++e) {
                double c = wi * (((e == ei) ? 1.0 : 0.0) - p[static_cast<size_t>(e)]);
                for (int q = 0; q < k; ++q)
                    S[(static_cast<int64_t>(e) * k + q) * n + i] = c * X[static_cast<int64_t>(q) * n + i];
            }
        }
    }
    return LL;
}

// Analytic Hessian (Stata's mlogit evaluator is d2): block (e, f) of H is
//     −Σ_i p_ie (1[e = f] − p_if) x_i x_i'
// accumulated in the upper triangle and mirrored, so H is exactly symmetric. The
// central-difference Hessian this replaced moved the model-based V by ~1e-9 relative,
// enough to put a clustered Wald chi2 1e-7 off StataBE (bank 0c684b5b).
inline void hessian(const double* X, int64_t n, int k, const int* cat, int K,
                    const std::vector<int>& eq_of_cat, const std::vector<double>& theta,
                    std::vector<double>& H, const double* wt = nullptr) {
    (void)cat;
    (void)eq_of_cat;
    const int neq = K - 1;
    const int np = static_cast<int>(theta.size());
    H.assign(static_cast<size_t>(np) * np, 0.0);
    std::vector<double> eta(static_cast<size_t>(neq)), p(static_cast<size_t>(neq)), x(static_cast<size_t>(k));
    for (int64_t i = 0; i < n; ++i) {
        double emax = 0.0;
        for (int q = 0; q < k; ++q) x[static_cast<size_t>(q)] = X[static_cast<int64_t>(q) * n + i];
        for (int e = 0; e < neq; ++e) {
            double v = 0;
            const double* b = theta.data() + static_cast<size_t>(e) * k;
            for (int c = 0; c < k; ++c) v += x[static_cast<size_t>(c)] * b[c];
            eta[static_cast<size_t>(e)] = v;
            if (v > emax) emax = v;
        }
        double denom = std::exp(-emax);
        for (int e = 0; e < neq; ++e) {
            double ex = std::exp(eta[static_cast<size_t>(e)] - emax);
            p[static_cast<size_t>(e)] = ex;
            denom += ex;
        }
        for (int e = 0; e < neq; ++e) p[static_cast<size_t>(e)] /= denom;
        const double wi = wt ? wt[i] : 1.0;
        for (int e = 0; e < neq; ++e)
            for (int f = e; f < neq; ++f) {
                const double w = wi * p[static_cast<size_t>(e)] *
                                 ((e == f ? 1.0 : 0.0) - p[static_cast<size_t>(f)]);
                for (int a = 0; a < k; ++a) {
                    const int r = e * k + a;
                    const double wa = w * x[static_cast<size_t>(a)];
                    for (int c = (e == f ? a : 0); c < k; ++c)
                        H[static_cast<size_t>(r) * np + (f * k + c)] -= wa * x[static_cast<size_t>(c)];
                }
            }
    }
    for (int r = 0; r < np; ++r)
        for (int c = r + 1; c < np; ++c)
            H[static_cast<size_t>(c) * np + r] = H[static_cast<size_t>(r) * np + c];
}

}  // namespace mlg

// X is col-major n*k INCLUDING the intercept column. `base` is the 0-based index of
// the base outcome among the K sorted distinct outcome values.
//
// Optimizer: Stata's own iterate, as oglm_fit / binary_ml_fit run it. `cons_col` is
// the intercept's column (-1: none); with one, the start is the constants-only MLE
// (each equation's constant ln(n_m/n_base), slopes 0), whose LL is ll_0 -- the value
// StataBE prints as iteration 0 (bank e9b3681e `mlogit race age grade wage`:
// -1418.3264). Newton-Raphson on inv(−H): a full step, halved toward the old point
// while it does not improve LL, else extended by 1/8, 1/4, ... of the step while each
// extension improves; stop when (mreldif(θ) <= 1e-6 or reldif(LL) <= 1e-7) and
// g·inv(−H)·g' < 1e-5 at the new point (ml's defaults), or after `max_iter`
// iterations (iterate(#), 300 by default), when `converged` is false. V = inv(−H)
// at the final θ.
inline MlogitResult mlogit_fit(const double* X, int64_t n, int k, const int* cat, int K, int base,
                               int cons_col = -1, int max_iter = 300, const double* wt = nullptr,
                               const MlCns* cns = nullptr) {
    MlogitResult r;
    r.n = n;
    r.n_cats = K;
    r.base = base;
    if (K < 2 || n <= 0 || base < 0 || base >= K) return r;
    const int neq = K - 1, np = neq * k;

    r.cat_n.assign(static_cast<size_t>(K), 0);
    for (int64_t i = 0; i < n; ++i) ++r.cat_n[static_cast<size_t>(cat[i])];
    // The outcome totals the null model reads: row counts, or the weight sums.
    std::vector<double> catw(static_cast<size_t>(K), 0.0);
    double totw = static_cast<double>(n);
    for (int m = 0; m < K; ++m) catw[static_cast<size_t>(m)] = static_cast<double>(r.cat_n[static_cast<size_t>(m)]);
    if (wt) {
        std::fill(catw.begin(), catw.end(), 0.0);
        totw = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            catw[static_cast<size_t>(cat[i])] += wt[i];
            totw += wt[i];
        }
    }
    for (int m = 0; m < K; ++m) {
        double nm = catw[static_cast<size_t>(m)];
        if (nm > 0) r.ll_0 += nm * std::log(nm / totw);
    }
    // Outcome -> equation index (ascending outcome order, base excluded).
    std::vector<int> eq_of_cat(static_cast<size_t>(K), -1);
    for (int m = 0, e = 0; m < K; ++m)
        if (m != base) eq_of_cat[static_cast<size_t>(m)] = e++;

    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    const double nb = catw[static_cast<size_t>(base)];
    if (cons_col >= 0 && cons_col < k && nb > 0) {
        for (int m = 0; m < K; ++m) {
            const int e = eq_of_cat[static_cast<size_t>(m)];
            const double nm = catw[static_cast<size_t>(m)];
            if (e >= 0 && nm > 0)
                theta[static_cast<size_t>(e) * k + cons_col] = std::log(nm / nb);
        }
    }

    // constraints(): the start projected onto R·θ = r (ml_constraints.h)
    if (cns != nullptr) ml_cns_project(*cns, theta);
    auto llat = [&](const std::vector<double>& th) {
        return mlg::ll_grad(X, n, k, cat, K, eq_of_cat, th, nullptr, nullptr, wt);
    };
    std::vector<double> H;
    // LL, gradient and V = inv(−H) at th; V empty when −H is not invertible.
    auto evaluate = [&](const std::vector<double>& th, std::vector<double>& g, std::vector<double>& Vinv) {
        g.assign(static_cast<size_t>(np), 0.0);
        const double LL = mlg::ll_grad(X, n, k, cat, K, eq_of_cat, th, &g, nullptr, wt);
        mlg::hessian(X, n, k, cat, K, eq_of_cat, th, H, wt);
        Vinv.assign(H.size(), 0.0);
        for (size_t i = 0; i < H.size(); ++i) Vinv[i] = -H[i];
        if ((cns != nullptr ? ml_cns_inverse(*cns, Vinv.data()) : la::spd_inverse(Vinv.data(), np)) != 0)
            Vinv.clear();
        return LL;
    };
    std::vector<double> grad, V, step(static_cast<size_t>(np));
    double LL = evaluate(theta, grad, V);
    r.ilog.push_back(LL);
    int it = 0;
    bool conv = false;
    std::vector<double> cand(static_cast<size_t>(np)), tryth(static_cast<size_t>(np)), dd(static_cast<size_t>(np));
    std::vector<double> g2, V2;
    while (it < max_iter && !conv && std::isfinite(LL) && !V.empty()) {
        la::matvec(V.data(), np, grad.data(), step.data());  // step = inv(−H) g
        for (int i = 0; i < np; ++i) cand[static_cast<size_t>(i)] = theta[static_cast<size_t>(i)] + step[static_cast<size_t>(i)];
        double f1 = llat(cand);
        if (!(f1 >= LL)) {
            for (int nh = 0; nh < 60 && !(f1 >= LL); ++nh) {
                for (int i = 0; i < np; ++i)
                    cand[static_cast<size_t>(i)] = (cand[static_cast<size_t>(i)] + theta[static_cast<size_t>(i)]) / 2.0;
                f1 = llat(cand);
            }
        } else {
            for (int i = 0; i < np; ++i) dd[static_cast<size_t>(i)] = 0.125 * step[static_cast<size_t>(i)];
            for (int nf = 0; nf < 100; ++nf) {
                for (int i = 0; i < np; ++i) tryth[static_cast<size_t>(i)] = cand[static_cast<size_t>(i)] + dd[static_cast<size_t>(i)];
                const double fc = llat(tryth);
                if (!(fc > f1)) break;
                cand = tryth;
                f1 = fc;
                for (double& v : dd) v *= 2.0;
            }
        }
        const double LL2 = evaluate(cand, g2, V2);
        ++it;
        r.ilog.push_back(LL2);
        if (!std::isfinite(LL2) || V2.empty()) { theta = cand; LL = LL2; V.swap(V2); break; }
        la::matvec(V2.data(), np, g2.data(), step.data());
        double nr = 0.0, pc = 0.0;
        for (int i = 0; i < np; ++i) {
            nr += g2[static_cast<size_t>(i)] * step[static_cast<size_t>(i)];
            pc = std::max(pc, std::fabs(cand[static_cast<size_t>(i)] - theta[static_cast<size_t>(i)]) /
                                  (std::fabs(theta[static_cast<size_t>(i)]) + 1.0));
        }
        const double rl = std::fabs(LL2 - LL) / (std::fabs(LL) + 1.0);
        conv = (pc <= 1e-6 || rl <= 1e-7) && nr < 1e-5;
        theta = cand;
        LL = LL2;
        grad.swap(g2);
        V.swap(V2);
    }
    r.converged = conv;
    r.iters = it;
    if (!std::isfinite(LL) || V.empty()) return r;
    r.ll = LL;
    r.vcov = V;
    r.gradient = grad;
    if (cns != nullptr) ml_cns_project_gradient(*cns, r.gradient);
    r.beta = theta;
    r.se.resize(static_cast<size_t>(np));
    for (int c = 0; c < np; ++c)
        r.se[static_cast<size_t>(c)] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * np + c]));
    r.ok = true;
    return r;
}

// Outcome -> equation index for a fitted model (-1 for the base outcome).
inline std::vector<int> mlogit_eq_of_cat(const MlogitResult& r) {
    std::vector<int> eq_of_cat(static_cast<size_t>(r.n_cats), -1);
    for (int m = 0, e = 0; m < r.n_cats; ++m)
        if (m != r.base) eq_of_cat[static_cast<size_t>(m)] = e++;
    return eq_of_cat;
}

// Per-observation score rows at the converged θ (n*np, col-major) — the sandwich
// meat's raw material for vce(robust) / vce(cluster). Parameter order matches
// r.vcov exactly (equation-major, design column order within an equation).
inline std::vector<double> mlogit_scores(const double* X, int64_t n, int k, const int* cat,
                                         const MlogitResult& r, const double* wt = nullptr) {
    std::vector<double> S;
    std::vector<int> eq_of_cat = mlogit_eq_of_cat(r);
    mlg::ll_grad(X, n, k, cat, r.n_cats, eq_of_cat, r.beta, nullptr, &S, wt);
    return S;
}

}  // namespace cecon
