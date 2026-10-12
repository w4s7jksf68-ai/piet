// cecon — ordered logit / ordered probit MLE, matching Stata `ologit` / `oprobit`.
//
// The outcome is recoded to categories 0..J-1 by SORTED DISTINCT VALUE (Stata's
// own rule: the cutpoints are indexed by the ordering of the observed levels, not
// by their numeric magnitude). With cutpoints κ_0 < … < κ_{J-2} and the linear
// index xβ (NO intercept — it is not identified against the cutpoints), category j
// has probability
//
//     P_ij = F(κ_j − xβ) − F(κ_{j−1} − xβ),      F(κ_{−1}−·) ≡ 0, F(κ_{J−1}−·) ≡ 1
//
// so LL_i = log P_ij. F/f are the logistic CDF/pdf (f = F(1−F)) or the standard
// normal CDF/pdf. The cutpoints are carried DIRECTLY (not as an increment
// parametrization): Stata prints /cut1 /cut2 with standard errors in that metric,
// and a delta parametrization would report different cut SEs.
//
// Ordering (κ_0 < κ_1 < …) is maintained without a constraint: a candidate θ that
// crosses two cutpoints drives some P_ij to ≤ 0, whose clamped log is a very large
// negative number, so the backtracking line search rejects the step.
//
// Optimizer follows the house new-MLE recipe (see estimators/tobit.h): analytic
// gradient, central-difference numerical Hessian of that gradient, symmetrize,
// spd_inverse(−H) for the model-based VCE, 40-halving backtracking line search,
// maxit 200, convergence |ΔLL| < 1e-9·(|LL|+1e-9).
#pragma once
#include "core/threads.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/distributions.h"
#include "core/linalg.h"

namespace cecon {

enum class OrdLink { Logit, Probit };

struct OglmResult {
    std::vector<double> beta;    // k slopes
    std::vector<double> se;      // k slope SEs (model-based / observed information)
    std::vector<double> cuts;    // J-1 cutpoints, ascending
    std::vector<double> se_cut;  // J-1 cutpoint SEs (same metric as `cuts`)
    std::vector<double> vcov;    // np*np, np = k + (J-1); slopes first, then cutpoints
    std::vector<int64_t> cat_n;  // per-category row counts
    double ll = 0;               // log-likelihood at the optimum
    double ll_0 = 0;             // cutpoints-only (β=0) log-likelihood = Σ_j n_j·ln(n_j/n)
    int64_t n = 0;
    int n_cats = 0;
    int iters = 0;
    bool converged = false;
    bool ok = false;
};

namespace ogl {

inline double ncdf(double x) { return 0.5 * std::erfc(-x * 0.70710678118654752440); }
inline double npdf(double x) { return 0.39894228040143267794 * std::exp(-0.5 * x * x); }
// Logistic CDF, branch-split so neither exp() overflows.
inline double lcdf(double x) {
    if (x >= 0) return 1.0 / (1.0 + std::exp(-x));
    double e = std::exp(x);
    return e / (1.0 + e);
}

// Φ^{-1}(p) built on the shared z_critical (core/distributions.h) rather than a
// second copy of Acklam's coefficients — z_critical(α) is the upper 1−α/2 normal
// quantile, so the two-sided α naming quantile p is 2(1−p) above the median and
// mirrors below it. Used only for ordered-probit starting cutpoints.
inline double norm_ppf(double p) {
    if (p <= 0.0 || p >= 1.0) return 0.0;
    if (p == 0.5) return 0.0;
    if (p > 0.5) return z_critical(2.0 * (1.0 - p));
    return -z_critical(2.0 * p);
}

// Log-likelihood + analytic gradient at θ = (β[0..k-1], κ[0..J-2]). X is col-major
// n*k WITHOUT an intercept column. `cat` holds the 0-based category of each row.
//
// scores: optional n*np COLUMN-major output (same layout as X — column p, row i at
// scores[p*n+i]); when non-null it is filled with the SAME per-observation gradient
// contribution that would otherwise only be summed into *grad, so la::xtx(scores,
// n, np, ...) gives the robust meat Σ_i s_i s_i' directly (and summing score ROWS
// within a cluster first gives the cluster meat).
//
// As in tobit.h, the grad and scores accumulations are written as two INDEPENDENT
// branches that deliberately do not share an intermediate: `(*grad)[p] += c *
// X[...]` is one fused expression eligible for -ffp-contract FMA fusion, and
// hoisting the product into a shared temp forces an extra rounding step that moves
// the last bits of every downstream SE. No call site here ever asks for both at
// once (the Newton loop passes grad only, the sandwich passes scores only), so the
// duplicated recompute costs nothing in practice.
inline double ll_grad(const double* X, int64_t n, int k, const int* cat, int J, OrdLink link,
                      const std::vector<double>& theta, std::vector<double>* grad,
                      std::vector<double>* scores = nullptr) {
    const int nc = J - 1;  // number of cutpoints
    const int np = k + nc;
    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int p = 0; p < k; ++p) xb += X[static_cast<int64_t>(p) * n + i] * theta[p];
        const int j = cat[i];
        // f1/f2 stay 0 at the ±∞ boundaries (j == 0 has no lower cut, j == J-1 no
        // upper cut), which is exactly f(±∞) = 0 — no special-casing downstream.
        double F1 = 0.0, f1 = 0.0, F2 = 1.0, f2 = 0.0;
        if (link == OrdLink::Logit) {
            if (j > 0) { double a1 = theta[k + j - 1] - xb; F1 = lcdf(a1); f1 = F1 * (1.0 - F1); }
            if (j < nc) { double a2 = theta[k + j] - xb; F2 = lcdf(a2); f2 = F2 * (1.0 - F2); }
        } else {
            if (j > 0) { double a1 = theta[k + j - 1] - xb; F1 = ncdf(a1); f1 = npdf(a1); }
            if (j < nc) { double a2 = theta[k + j] - xb; F2 = ncdf(a2); f2 = npdf(a2); }
        }
        double P = F2 - F1;
        // A cut-ordering violation lands here; log() of the clamp is ≈ −690 per
        // row, so the line search rejects the step instead of carrying a NaN into
        // the Hessian.
        if (P < 1e-300) P = 1e-300;
        LL += std::log(P);
        if (grad) {
            double c = (f1 - f2) / P;
            for (int p = 0; p < k; ++p) (*grad)[p] += c * X[static_cast<int64_t>(p) * n + i];
            if (j > 0) (*grad)[k + j - 1] += -f1 / P;
            if (j < nc) (*grad)[k + j] += f2 / P;
        }
        if (S) {
            double c = (f1 - f2) / P;
            for (int p = 0; p < k; ++p)
                S[static_cast<int64_t>(p) * n + i] = c * X[static_cast<int64_t>(p) * n + i];
            if (j > 0) S[static_cast<int64_t>(k + j - 1) * n + i] = -f1 / P;
            if (j < nc) S[static_cast<int64_t>(k + j) * n + i] = f2 / P;
        }
    }
    return LL;
}

// Numerical (central-difference) Hessian of ll_grad's analytic gradient, symmetrized.
inline void hessian(const double* X, int64_t n, int k, const int* cat, int J, OrdLink link,
                    const std::vector<double>& theta, std::vector<double>& H) {
    const int np = static_cast<int>(theta.size());
    H.assign(static_cast<size_t>(np) * np, 0.0);
    // Column j reads only theta and j and writes only H[:, j] -- no accumulator is
    // shared between columns -- so evaluating them concurrently returns the identical
    // matrix, with no summation order to reorder. These 2*np full-data gradient passes
    // are what a Newton step costs here. (PERFSWEEP H: same class as ivml/zicount.)
    parallel_for(np, [&](int64_t jj) {
        const int j = static_cast<int>(jj);
        thread_local std::vector<double> gp, gm, tp, tm;
        gp.resize(static_cast<size_t>(np));
        gm.resize(static_cast<size_t>(np));
        double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(j)]) + 1.0);
        tp = theta; tm = theta;
        tp[static_cast<size_t>(j)] += h; tm[static_cast<size_t>(j)] -= h;
        ll_grad(X, n, k, cat, J, link, tp, &gp);
        ll_grad(X, n, k, cat, J, link, tm, &gm);
        for (int i = 0; i < np; ++i)
            H[static_cast<size_t>(i) * np + j] =
                (gp[static_cast<size_t>(i)] - gm[static_cast<size_t>(i)]) / (2 * h);
    }, threads_for_work(static_cast<int64_t>(np) * n));
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            double s = 0.5 * (H[static_cast<size_t>(i) * np + j] + H[static_cast<size_t>(j) * np + i]);
            H[static_cast<size_t>(i) * np + j] = H[static_cast<size_t>(j) * np + i] = s;
        }
}

}  // namespace ogl

// Recode y to 0-based categories by sorted distinct value. Returns the distinct
// values (ascending — the labels Stata prints); `cat` is filled per row.
//
// ONE definition, shared: `mlogit` recodes its outcome by exactly the same rule
// (see mlogit.h's header comment), and cmds_mlogit.cpp calls this function rather
// than growing a second copy — two spellings of one coding rule is how the coding
// and the reported cat_values start disagreeing silently.
inline std::vector<double> ord_categories(const double* y, int64_t n, std::vector<int>& cat) {
    std::vector<double> vals(y, y + n);
    std::sort(vals.begin(), vals.end());
    vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    cat.resize(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i)
        cat[static_cast<size_t>(i)] =
            static_cast<int>(std::lower_bound(vals.begin(), vals.end(), y[i]) - vals.begin());
    return vals;
}

// X is col-major n*k with NO intercept column (the caller must drop it — with an
// intercept in the design the information matrix is singular against the cutpoints).
inline OglmResult oglm_fit(const double* X, int64_t n, int k, const int* cat, int J, OrdLink link) {
    OglmResult r;
    r.n = n;
    r.n_cats = J;
    if (J < 2 || n <= 0) return r;
    const int nc = J - 1, np = k + nc;

    r.cat_n.assign(static_cast<size_t>(J), 0);
    for (int64_t i = 0; i < n; ++i) ++r.cat_n[static_cast<size_t>(cat[i])];
    // Constant-only (β=0) log-likelihood. LINK-FREE: at β=0 with the starting
    // cutpoints below, P_ij is exactly the sample share of category j, so this is
    // both the ll_0 behind Pseudo R² AND the iteration-0 value Stata prints.
    for (int j = 0; j < J; ++j) {
        double nj = static_cast<double>(r.cat_n[static_cast<size_t>(j)]);
        if (nj > 0) r.ll_0 += nj * std::log(nj / static_cast<double>(n));
    }

    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    {   // start: β = 0, κ_j = F^{-1}(cumulative share through j) — the closed-form
        // cutpoints-only fit, so the Newton loop starts AT the null model.
        double cum = 0;
        for (int j = 0; j < nc; ++j) {
            cum += static_cast<double>(r.cat_n[static_cast<size_t>(j)]) / static_cast<double>(n);
            theta[static_cast<size_t>(k + j)] =
                (link == OrdLink::Logit) ? std::log(cum / (1.0 - cum)) : ogl::norm_ppf(cum);
        }
    }

    std::vector<double> grad(static_cast<size_t>(np)), H, negH, step(static_cast<size_t>(np));
    double prevLL = -1e300;
    int it = 0;
    for (; it < 200; ++it) {
        double LL = ogl::ll_grad(X, n, k, cat, J, link, theta, &grad);
        ogl::hessian(X, n, k, cat, J, link, theta, H);
        negH.assign(H.size(), 0.0);
        for (size_t i = 0; i < H.size(); ++i) negH[i] = -H[i];
        if (la::spd_inverse(negH.data(), np) != 0) break;
        la::matvec(negH.data(), np, grad.data(), step.data());  // step = (−H)^{-1} grad
        double t = 1.0;
        std::vector<double> cand(static_cast<size_t>(np));
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int i = 0; i < np; ++i) cand[static_cast<size_t>(i)] = theta[static_cast<size_t>(i)] + t * step[static_cast<size_t>(i)];
            candLL = ogl::ll_grad(X, n, k, cat, J, link, cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            t *= 0.5;
        }
        theta = cand;
        if (std::fabs(candLL - prevLL) < 1e-9 * (std::fabs(prevLL) + 1e-9)) {
            prevLL = candLL; ++it; r.converged = true; break;
        }
        prevLL = candLL;
    }
    r.iters = it;
    r.ll = ogl::ll_grad(X, n, k, cat, J, link, theta, &grad);
    ogl::hessian(X, n, k, cat, J, link, theta, H);
    std::vector<double> V(H.size());
    for (size_t i = 0; i < V.size(); ++i) V[i] = -H[i];
    if (la::spd_inverse(V.data(), np) != 0) return r;
    r.vcov = V;
    r.beta.assign(theta.begin(), theta.begin() + k);
    r.cuts.assign(theta.begin() + k, theta.end());
    r.se.resize(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) r.se[c] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * np + c]));
    r.se_cut.resize(static_cast<size_t>(nc));
    for (int c = 0; c < nc; ++c)
        r.se_cut[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(k + c) * np + (k + c)]));
    r.ok = true;
    return r;
}

// Per-observation score rows at the converged θ (n*np, col-major) — the sandwich
// meat's raw material for vce(robust) / vce(cluster). Parameter order matches
// r.vcov exactly: slopes, then cutpoints.
inline std::vector<double> oglm_scores(const double* X, int64_t n, int k, const int* cat, int J,
                                       OrdLink link, const OglmResult& r) {
    std::vector<double> theta = r.beta;
    theta.insert(theta.end(), r.cuts.begin(), r.cuts.end());
    std::vector<double> S;
    ogl::ll_grad(X, n, k, cat, J, link, theta, nullptr, &S);
    return S;
}

}  // namespace cecon
