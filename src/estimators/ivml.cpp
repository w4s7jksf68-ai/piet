#include "ivml.h"

#include <cmath>
#include <vector>

#include "core/threads.h"
#include "probit.h"
#include "tobit.h"

// Implementation notes for the two fits below.
//
// STARTING VALUES mirror the prefit chain Stata itself prints ("Fitting exogenous
// probit model" / "Fitting exogenous tobit model", then "Fitting full model"),
// which is also the stability design — the reduced form is a closed-form OLS and
// the structural part starts from an already-converged exogenous fit, so the full
// Newton loop only has to find ρ:
//   1. OLS of y2 on Xw          → Π̂ and lnσ_v = ½·ln(RSS/n)   (the Gaussian MLE,
//                                  divisor n rather than n−kw)
//   2. exogenous probit / tobit of y1 on Xs → δ̂ (and lnσ_e for ivtobit)
//   3. athrho = 0                (independence: the joint LL at this θ is exactly
//                                  the sum of the two prefit LLs)
//
// The Newton loop is the house recipe verbatim: analytic gradient, central-
// difference numerical Hessian of that gradient, symmetrized, step = (−H)^{-1}·g
// with a 40-halving backtracking line search that only accepts a non-decreasing
// LL, and |ΔLL| < 1e-9·(|LL|+1e-9) as the convergence test.

namespace cecon {
namespace {

// Central-difference Hessian of an analytic gradient, symmetrized. `gradf` fills
// its second argument with the analytic gradient at the θ given first.
// Column j of this Hessian depends on nothing but theta, j and the gradient function,
// and it writes column j of H and nothing else. There is no accumulator shared across
// j, so evaluating the columns concurrently produces the identical matrix — there is
// no summation order to reorder. That matters because these 2·np full-data gradient
// passes ARE the cost of a Newton step here: everything else in the iteration is one
// gradient plus a handful of line-search likelihoods.
// Scratch is thread_local rather than caller-supplied for the same reason it is in
// xtlogit's panel loop: parallel_for builds its workers for the duration of one call,
// so each worker allocates once per call instead of once per column.
template <class GradF>
void num_hessian(GradF&& gradf, const std::vector<double>& theta, int np, int64_t n,
                 std::vector<double>& H) {
    H.assign(static_cast<size_t>(np) * np, 0.0);
    parallel_for(np, [&](int64_t jj) {
        const int j = static_cast<int>(jj);
        thread_local std::vector<double> gp, gm, tp, tm;
        gp.resize(static_cast<size_t>(np));
        gm.resize(static_cast<size_t>(np));
        const double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(j)]) + 1.0);
        tp = theta;
        tm = theta;
        tp[static_cast<size_t>(j)] += h;
        tm[static_cast<size_t>(j)] -= h;
        gradf(tp, &gp);
        gradf(tm, &gm);
        for (int i = 0; i < np; ++i)
            H[static_cast<size_t>(i) * np + j] =
                (gp[static_cast<size_t>(i)] - gm[static_cast<size_t>(i)]) / (2 * h);
    }, threads_for_work(static_cast<int64_t>(np) * n));
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            const double sv = 0.5 * (H[static_cast<size_t>(i) * np + j] + H[static_cast<size_t>(j) * np + i]);
            H[static_cast<size_t>(i) * np + j] = H[static_cast<size_t>(j) * np + i] = sv;
        }
}

// Newton-Raphson maximization shared by both fits. `llf(θ, grad_or_null)` returns
// the LL and, when grad is non-null, fills it with the analytic gradient.
template <class LLF>
void newton_max(LLF&& llf, std::vector<double>& theta, int np, int64_t n, IvmlResult& r) {
    auto gradf = [&](const std::vector<double>& t, std::vector<double>* g) { llf(t, g); };
    std::vector<double> grad(static_cast<size_t>(np)), H, negH, step(static_cast<size_t>(np));
    // Backtracking candidate, reused every iteration: fully rewritten before any read.
    std::vector<double> cand(static_cast<size_t>(np));
    double prevLL = -1e300;
    int it = 0;
    for (; it < 200; ++it) {
        const double LL = llf(theta, &grad);
        num_hessian(gradf, theta, np, n, H);
        negH.assign(H.size(), 0.0);
        for (size_t i = 0; i < H.size(); ++i) negH[i] = -H[i];
        if (la::spd_inverse(negH.data(), np) != 0) break;
        la::matvec(negH.data(), np, grad.data(), step.data());  // step = (−H)^{-1}·grad
        double t = 1.0;
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int i = 0; i < np; ++i)
                cand[static_cast<size_t>(i)] = theta[static_cast<size_t>(i)] + t * step[static_cast<size_t>(i)];
            candLL = llf(cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            t *= 0.5;
        }
        theta = cand;
        if (std::fabs(candLL - prevLL) < 1e-9 * (std::fabs(prevLL) + 1e-9)) {
            prevLL = candLL;
            ++it;
            r.converged = true;
            break;
        }
        prevLL = candLL;
    }
    r.iters = it;
}

// OLS of y2 on Xw. Fills pi (kw) and lnsv; returns false on a singular Xw'Xw.
bool reduced_form_start(const double* Xw, int64_t n, int kw, const double* y2,
                        std::vector<double>& pi, double& lnsv) {
    std::vector<double> XtX(static_cast<size_t>(kw) * kw), Xty(static_cast<size_t>(kw));
    la::xtx(Xw, n, kw, XtX.data());
    la::xtv(Xw, n, kw, y2, Xty.data());
    if (la::spd_inverse(XtX.data(), kw) != 0) return false;
    pi.assign(static_cast<size_t>(kw), 0.0);
    la::matvec(XtX.data(), kw, Xty.data(), pi.data());
    double rss = 0;
    for (int64_t i = 0; i < n; ++i) {
        double f = 0;
        for (int p = 0; p < kw; ++p) f += Xw[static_cast<int64_t>(p) * n + i] * pi[static_cast<size_t>(p)];
        const double e = y2[i] - f;
        rss += e * e;
    }
    if (!(rss > 0)) return false;
    lnsv = 0.5 * std::log(rss / static_cast<double>(n));
    return true;
}

// Final VCE + reporting fields shared by both fits: (−H)^{-1} in θ's metric, the
// per-parameter SEs, and the delta-method σ/ρ rows.
//   ρ  = tanh(athrho)            → se(ρ)  = (1−ρ²)·se(athrho)
//   σ  = exp(lnσ)                → se(σ)  = σ·se(lnσ)
// Both are the same first-order delta method the house tobit already uses for
// /sigma; Stata reports corr and sd in these transformed metrics, not in θ's.
void finish(IvmlResult& r, const std::vector<double>& theta, const std::vector<double>& H,
            int ks, int kw, int np, bool with_sigma_e) {
    std::vector<double> V(H.size());
    for (size_t i = 0; i < V.size(); ++i) V[i] = -H[i];
    if (la::spd_inverse(V.data(), np) != 0) return;
    r.vcov = V;
    r.theta = theta;
    r.ks = ks;
    r.kw = kw;
    r.np = np;
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };
    r.beta.assign(theta.begin(), theta.begin() + ks);
    r.se.resize(static_cast<size_t>(ks));
    for (int c = 0; c < ks; ++c) r.se[static_cast<size_t>(c)] = sd(c);
    r.pi.assign(theta.begin() + ks, theta.begin() + ks + kw);
    r.se_pi.resize(static_cast<size_t>(kw));
    for (int c = 0; c < kw; ++c) r.se_pi[static_cast<size_t>(c)] = sd(ks + c);
    r.i_athrho = ks + kw;
    r.i_lnsv = ks + kw + 1;
    r.i_lnse = with_sigma_e ? ks + kw + 2 : -1;
    r.athrho = theta[static_cast<size_t>(r.i_athrho)];
    r.se_athrho = sd(r.i_athrho);
    r.rho = std::tanh(r.athrho);
    r.se_rho = (1.0 - r.rho * r.rho) * r.se_athrho;
    r.ln_sigma_v = theta[static_cast<size_t>(r.i_lnsv)];
    r.se_ln_sigma_v = sd(r.i_lnsv);
    r.sigma_v = std::exp(r.ln_sigma_v);
    r.se_sigma_v = r.sigma_v * r.se_ln_sigma_v;
    if (with_sigma_e) {
        r.ln_sigma_e = theta[static_cast<size_t>(r.i_lnse)];
        r.se_ln_sigma_e = sd(r.i_lnse);
        r.sigma_e = std::exp(r.ln_sigma_e);
        r.se_sigma_e = r.sigma_e * r.se_ln_sigma_e;
    }
    r.ok = true;
}

}  // namespace

IvmlResult ivprobit_fit(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                        const double* y1, const double* y2) {
    IvmlResult r;
    r.n = n;
    if (n <= 0 || ks <= 0 || kw <= 0) return r;
    const int np = ks + kw + 2;
    std::vector<double> theta(static_cast<size_t>(np), 0.0);

    std::vector<double> pi;
    double lnsv = 0;
    if (!reduced_form_start(Xw, n, kw, y2, pi, lnsv)) return r;
    for (int p = 0; p < kw; ++p) theta[static_cast<size_t>(ks + p)] = pi[static_cast<size_t>(p)];
    theta[static_cast<size_t>(ks + kw)] = 0.0;   // athrho
    theta[static_cast<size_t>(ks + kw + 1)] = lnsv;

    // Exogenous probit prefit of y1 on Xs (y2 treated as exogenous).
    ProbitResult pr = probit_fit(Xs, n, ks, y1, 100, 1e-14);
    if (static_cast<int>(pr.beta.size()) == ks)
        for (int p = 0; p < ks; ++p) theta[static_cast<size_t>(p)] = pr.beta[static_cast<size_t>(p)];

    auto llf = [&](const std::vector<double>& t, std::vector<double>* g) {
        return ivm::ivprobit_ll_grad(Xs, Xw, n, ks, kw, y1, y2, t, g);
    };
    r.ll_exog = llf(theta, nullptr);  // joint LL at ρ = 0 (= exogenous probit + reduced form)
    newton_max(llf, theta, np, n, r);

    std::vector<double> grad(static_cast<size_t>(np)), H;
    r.ll = llf(theta, &grad);
    num_hessian([&](const std::vector<double>& t, std::vector<double>* g) { llf(t, g); }, theta, np, n, H);
    finish(r, theta, H, ks, kw, np, /*with_sigma_e=*/false);
    return r;
}

IvmlResult ivtobit_fit(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                       const double* y1, const double* y2, bool has_ll, double ll_lim,
                       bool has_ul, double ul_lim) {
    IvmlResult r;
    r.n = n;
    if (n <= 0 || ks <= 0 || kw <= 0) return r;
    const int np = ks + kw + 3;
    // Censoring classification — the SAME rule as tobit.h (y <= ll / y >= ul), so
    // the counts Stata prints in its header come out of one shared convention.
    std::vector<int8_t> cls(static_cast<size_t>(n), 0);
    for (int64_t i = 0; i < n; ++i) {
        if (has_ll && y1[i] <= ll_lim) { cls[static_cast<size_t>(i)] = 1; ++r.n_lc; }
        else if (has_ul && y1[i] >= ul_lim) { cls[static_cast<size_t>(i)] = 2; ++r.n_rc; }
        else ++r.n_unc;
    }

    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    std::vector<double> pi;
    double lnsv = 0;
    if (!reduced_form_start(Xw, n, kw, y2, pi, lnsv)) return r;
    for (int p = 0; p < kw; ++p) theta[static_cast<size_t>(ks + p)] = pi[static_cast<size_t>(p)];
    theta[static_cast<size_t>(ks + kw)] = 0.0;   // athrho
    theta[static_cast<size_t>(ks + kw + 1)] = lnsv;

    // Exogenous tobit prefit of y1 on Xs — it seeds δ AND lnσ_e.
    TobitResult tr = tobit_fit(Xs, n, ks, y1, has_ll, ll_lim, has_ul, ul_lim);
    if (!tr.ok || !(tr.sigma > 0)) return r;
    for (int p = 0; p < ks; ++p) theta[static_cast<size_t>(p)] = tr.beta[static_cast<size_t>(p)];
    theta[static_cast<size_t>(ks + kw + 2)] = std::log(tr.sigma);

    auto llf = [&](const std::vector<double>& t, std::vector<double>* g) {
        return ivm::ivtobit_ll_grad(Xs, Xw, n, ks, kw, y1, y2, cls.data(), ll_lim, ul_lim, t, g);
    };
    r.ll_exog = llf(theta, nullptr);
    newton_max(llf, theta, np, n, r);

    std::vector<double> grad(static_cast<size_t>(np)), H;
    r.ll = llf(theta, &grad);
    num_hessian([&](const std::vector<double>& t, std::vector<double>* g) { llf(t, g); }, theta, np, n, H);
    finish(r, theta, H, ks, kw, np, /*with_sigma_e=*/true);
    return r;
}

std::vector<double> ivprobit_scores(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                                    const double* y1, const double* y2, const IvmlResult& r) {
    std::vector<double> S;
    if (!r.ok) return S;
    ivm::ivprobit_ll_grad(Xs, Xw, n, ks, kw, y1, y2, r.theta, nullptr, &S);
    return S;
}

std::vector<double> ivtobit_scores(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                                   const double* y1, const double* y2, bool has_ll, double ll_lim,
                                   bool has_ul, double ul_lim, const IvmlResult& r) {
    std::vector<double> S;
    if (!r.ok) return S;
    std::vector<int8_t> cls(static_cast<size_t>(n), 0);
    for (int64_t i = 0; i < n; ++i) {
        if (has_ll && y1[i] <= ll_lim) cls[static_cast<size_t>(i)] = 1;
        else if (has_ul && y1[i] >= ul_lim) cls[static_cast<size_t>(i)] = 2;
    }
    ivm::ivtobit_ll_grad(Xs, Xw, n, ks, kw, y1, y2, cls.data(), ll_lim, ul_lim, r.theta, nullptr, &S);
    return S;
}

}  // namespace cecon
