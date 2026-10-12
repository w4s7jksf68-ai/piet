// cecon — tobit (censored-normal regression) MLE, matching Stata `tobit, ll() ul()`.
// Left-censored obs (y<=ll) contribute logΦ((ll-xβ)/σ); right-censored (y>=ul)
// contribute log(1-Φ((ul-xβ)/σ)); uncensored contribute the normal log-density.
// Optimized in (β, lnσ) by Newton-Raphson (analytic gradient + central-difference
// Hessian = observed information); σ and SE(σ)=σ·SE(lnσ) reported in σ-metric to
// match classic tobit's /sigma. Coef SEs are reparameterization-invariant.
#pragma once
#include "core/threads.h"
#include <cmath>
#include <vector>

#include "core/linalg.h"

namespace cecon {

struct TobitResult {
    std::vector<double> beta;   // length k
    std::vector<double> se;     // length k (coefficients)
    double sigma = 0, se_sigma = 0;       // residual SD and its SE
    double variance = 0, se_variance = 0; // residual variance σ² (= Stata var(e.y))
    double ll = 0;
    int64_t n = 0, n_lc = 0, n_rc = 0, n_unc = 0;
    int iters = 0;
    bool converged = false;
    bool ok = false;
    std::vector<double> vcov;   // (k+1)*(k+1) in (β, lnσ) metric
};

namespace tob {
inline double ncdf(double x) { return 0.5 * std::erfc(-x * 0.70710678118654752440); }
inline double npdf(double x) { return 0.39894228040143267794 * std::exp(-0.5 * x * x); }

// Log-likelihood and analytic gradient at theta=(β[0..k-1], lnσ). X col-major n*k.
// scores (P5, tobit vce(robust)): optional n*np COLUMN-major output (same layout
// convention as X — column p, row i at scores[p*n+i]) — when non-null, filled with
// the SAME per-observation gradient contribution that would otherwise only be
// summed into *grad, so la::xtx(scores, n, np, ...) directly gives the robust
// meat = Sum_i s_i s_i'. bread stays tobit_fit's existing observed-information
// inverse. nullptr (the default, and every pre-P5 call site) costs one extra
// branch per obs and writes nothing, so callers that don't ask for scores are
// unaffected.
inline double ll_grad(const double* X, int64_t n, int k, const double* y,
                      const std::vector<int8_t>& cls,  // 0 unc, 1 left, 2 right
                      double ll_lim, double ul_lim, const std::vector<double>& theta,
                      std::vector<double>* grad, std::vector<double>* scores = nullptr) {
    double g = theta[k];
    double sigma = std::exp(g);
    double LL = 0;
    int np = k + 1;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * np, 0.0);
    double* S = scores ? scores->data() : nullptr;
    const double log2pi = 1.8378770664093454836;
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int p = 0; p < k; ++p) xb += X[static_cast<int64_t>(p) * n + i] * theta[p];
        // grad and S are computed by two INDEPENDENT branches below, deliberately
        // not sharing a "v" intermediate: `(*grad)[p] += c * X[...]` is one fused
        // expression (eligible for -ffp-contract FMA fusion), and hoisting that
        // product into a shared local temp — even though mathematically the same
        // value — forces a separate rounding step at the temp's assignment, which
        // measurably changed the last bit or two of every downstream SE (caught by
        // the P5 R2 byte-identity check against the pre-P5 baseline). Two branches
        // costs one redundant `c`/`a`/`del` recompute when BOTH grad and S are
        // requested (never true in this file's own call sites — tobit_fit's Newton
        // loop passes grad only, tobit_robust_vcov passes scores only) in exchange
        // for the grad path staying bit-for-bit what it was before P5.
        if (cls[i] == 0) {  // uncensored
            double z = (y[i] - xb) / sigma;
            LL += -g - 0.5 * log2pi - 0.5 * z * z;
            if (grad) {
                double c = z / sigma;
                for (int p = 0; p < k; ++p) (*grad)[p] += c * X[static_cast<int64_t>(p) * n + i];
                (*grad)[k] += z * z - 1.0;
            }
            if (S) {
                double c = z / sigma;
                for (int p = 0; p < k; ++p) S[static_cast<int64_t>(p) * n + i] = c * X[static_cast<int64_t>(p) * n + i];
                S[static_cast<int64_t>(k) * n + i] = z * z - 1.0;
            }
        } else if (cls[i] == 1) {  // left-censored
            double a = (ll_lim - xb) / sigma;
            double Phi = ncdf(a);
            if (Phi < 1e-300) Phi = 1e-300;
            LL += std::log(Phi);
            if (grad) {
                double lam = npdf(a) / Phi;
                double c = -lam / sigma;
                for (int p = 0; p < k; ++p) (*grad)[p] += c * X[static_cast<int64_t>(p) * n + i];
                (*grad)[k] += -lam * a;
            }
            if (S) {
                double lam = npdf(a) / Phi;
                double c = -lam / sigma;
                for (int p = 0; p < k; ++p) S[static_cast<int64_t>(p) * n + i] = c * X[static_cast<int64_t>(p) * n + i];
                S[static_cast<int64_t>(k) * n + i] = -lam * a;
            }
        } else {  // right-censored
            double a = (ul_lim - xb) / sigma;
            double Sv = 1.0 - ncdf(a);
            if (Sv < 1e-300) Sv = 1e-300;
            LL += std::log(Sv);
            if (grad) {
                double del = npdf(a) / Sv;
                double c = del / sigma;
                for (int p = 0; p < k; ++p) (*grad)[p] += c * X[static_cast<int64_t>(p) * n + i];
                (*grad)[k] += del * a;
            }
            if (S) {
                double del = npdf(a) / Sv;
                double c = del / sigma;
                for (int p = 0; p < k; ++p) S[static_cast<int64_t>(p) * n + i] = c * X[static_cast<int64_t>(p) * n + i];
                S[static_cast<int64_t>(k) * n + i] = del * a;
            }
        }
    }
    return LL;
}
}  // namespace tob

// Robust (sandwich) VCE for a converged tobit_fit result. V = factor * bread *
// meat * bread, meat = Sum_i s_i s_i' from tob::ll_grad's per-observation score
// (same (β, lnσ) metric as r.vcov), bread = r.vcov (tobit_fit's own converged
// observed-information inverse — reused as-is, no re-derivation). `factor` is the
// caller-supplied ML small-sample multiplier (see cmds_mle.cpp's cmd_tobit for
// which one matches Stata `tobit ..., vce(robust)` — verified against the P5
// oracle rather than assumed). Returns an empty vector if r.vcov is empty (fit
// failed) or the meat is not invertible-compatible with bread's shape.
inline std::vector<double> tobit_robust_vcov(const double* X, int64_t n, int k, const double* y,
                                             bool has_ll, double ll_lim, bool has_ul, double ul_lim,
                                             const TobitResult& r, double factor) {
    const int np = k + 1;
    if (static_cast<int>(r.vcov.size()) != np * np || !r.ok) return {};
    std::vector<int8_t> cls(static_cast<size_t>(n), 0);
    for (int64_t i = 0; i < n; ++i) {
        if (has_ll && y[i] <= ll_lim) cls[static_cast<size_t>(i)] = 1;
        else if (has_ul && y[i] >= ul_lim) cls[static_cast<size_t>(i)] = 2;
    }
    std::vector<double> theta(r.beta.begin(), r.beta.end());
    theta.push_back(std::log(r.sigma));
    std::vector<double> S;
    tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, theta, nullptr, &S);
    std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
    la::xtx(S.data(), n, np, meat.data());
    std::vector<double> V(static_cast<size_t>(np) * np, 0.0), tmp(static_cast<size_t>(np) * np, 0.0);
    la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    return V;
}

// X is col-major n*k (intercept included). has_ll/has_ul gate left/right censoring.
inline TobitResult tobit_fit(const double* X, int64_t n, int k, const double* y,
                             bool has_ll, double ll_lim, bool has_ul, double ul_lim) {
    TobitResult r;
    std::vector<int8_t> cls(n, 0);
    for (int64_t i = 0; i < n; ++i) {
        if (has_ll && y[i] <= ll_lim) { cls[i] = 1; ++r.n_lc; }
        else if (has_ul && y[i] >= ul_lim) { cls[i] = 2; ++r.n_rc; }
        else ++r.n_unc;
    }
    r.n = n;
    int np = k + 1;
    // start: OLS β over all obs, σ = sd of residuals
    std::vector<double> theta(np, 0.0);
    {
        std::vector<double> XtX(static_cast<size_t>(k) * k), Xty(k);
        la::xtx(X, n, k, XtX.data());
        la::xtv(X, n, k, y, Xty.data());
        if (la::spd_inverse(XtX.data(), k) != 0) return r;
        la::matvec(XtX.data(), k, Xty.data(), theta.data());
        double rss = 0;
        for (int64_t i = 0; i < n; ++i) {
            double f = 0;
            for (int p = 0; p < k; ++p) f += X[static_cast<int64_t>(p) * n + i] * theta[p];
            double e = y[i] - f; rss += e * e;
        }
        theta[k] = 0.5 * std::log(rss / n);  // ln(sigma)
    }

    std::vector<double> grad(np);
    double prevLL = -1e300;
    int it = 0;
    std::vector<double> H(static_cast<size_t>(np) * np);
    for (; it < 200; ++it) {
        double LL = tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, theta, &grad);
        // numerical Hessian = d(grad)/d(theta), central difference.
        // Column j reads only theta and j and writes only H[:, j] -- no accumulator is
        // shared between columns -- so evaluating them concurrently returns the
        // identical matrix, with no summation order to reorder. These 2*np full-data
        // gradient passes are what a Newton step costs here. (PERFSWEEP H.)
        parallel_for(np, [&](int64_t jj) {
            const int j = static_cast<int>(jj);
            thread_local std::vector<double> gp, gm, tp, tm;
            gp.resize(static_cast<size_t>(np));
            gm.resize(static_cast<size_t>(np));
            double h = 1e-6 * (std::fabs(theta[j]) + 1.0);
            tp = theta; tm = theta;
            tp[j] += h; tm[j] -= h;
            tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, tp, &gp);
            tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, tm, &gm);
            for (int i = 0; i < np; ++i)
                H[static_cast<size_t>(i) * np + j] = (gp[i] - gm[i]) / (2 * h);
        }, threads_for_work(static_cast<int64_t>(np) * n));
        for (int i = 0; i < np; ++i)
            for (int j = i + 1; j < np; ++j) {
                double s = 0.5 * (H[static_cast<size_t>(i) * np + j] + H[static_cast<size_t>(j) * np + i]);
                H[static_cast<size_t>(i) * np + j] = H[static_cast<size_t>(j) * np + i] = s;
            }
        // step = -H^{-1} grad  (H is negative definite at a max => -H pos def)
        std::vector<double> negH(static_cast<size_t>(np) * np);
        for (size_t i = 0; i < negH.size(); ++i) negH[i] = -H[i];
        std::vector<double> negHinv = negH;
        if (la::spd_inverse(negHinv.data(), np) != 0) break;
        std::vector<double> step(np);
        la::matvec(negHinv.data(), np, grad.data(), step.data());  // step = (-H)^{-1} grad
        // backtracking line search
        double t = 1.0;
        std::vector<double> cand(np);
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int i = 0; i < np; ++i) cand[i] = theta[i] + t * step[i];
            candLL = tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            t *= 0.5;
        }
        theta = cand;
        if (std::fabs(candLL - prevLL) < 1e-9 * (std::fabs(prevLL) + 1e-9)) { prevLL = candLL; ++it; r.converged = true; break; }
        prevLL = candLL;
    }
    r.iters = it;
    // final LL + observed-information VCE in (β, lnσ)
    double LL = tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, theta, &grad);
    parallel_for(np, [&](int64_t jj) {  // same column-independence as the loop's Hessian
        const int j = static_cast<int>(jj);
        thread_local std::vector<double> gp, gm, tp, tm;
        gp.resize(static_cast<size_t>(np));
        gm.resize(static_cast<size_t>(np));
        double h = 1e-6 * (std::fabs(theta[j]) + 1.0);
        tp = theta; tm = theta;
        tp[j] += h; tm[j] -= h;
        tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, tp, &gp);
        tob::ll_grad(X, n, k, y, cls, ll_lim, ul_lim, tm, &gm);
        for (int i = 0; i < np; ++i) H[static_cast<size_t>(i) * np + j] = (gp[i] - gm[i]) / (2 * h);
    }, threads_for_work(static_cast<int64_t>(np) * n));
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            double s = 0.5 * (H[static_cast<size_t>(i) * np + j] + H[static_cast<size_t>(j) * np + i]);
            H[static_cast<size_t>(i) * np + j] = H[static_cast<size_t>(j) * np + i] = s;
        }
    std::vector<double> V(static_cast<size_t>(np) * np);
    for (size_t i = 0; i < V.size(); ++i) V[i] = -H[i];
    if (la::spd_inverse(V.data(), np) != 0) return r;
    r.vcov = V;
    r.beta.assign(theta.begin(), theta.begin() + k);
    r.se.resize(k);
    for (int c = 0; c < k; ++c) r.se[c] = std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * np + c]));
    double g = theta[k];
    r.sigma = std::exp(g);
    double se_lnsig = std::sqrt(std::max(0.0, V[static_cast<size_t>(k) * np + k]));
    r.se_sigma = r.sigma * se_lnsig;             // delta method == direct σ-metric SE
    r.variance = r.sigma * r.sigma;              // Stata reports var(e.y) = σ²
    r.se_variance = 2.0 * r.sigma * r.se_sigma;  // delta method on σ²
    r.ll = LL;
    r.ok = true;
    return r;
}

}  // namespace cecon
