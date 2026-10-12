#include "estimators/logit.h"

#include <cmath>
#include <vector>

#include "core/linalg.h"

namespace cecon {

LogitResult logit_fit(const double* X, int64_t n, int k, const double* y, int maxit, double tol,
                      const double* weights, const double* offset) {
    LogitResult r;
    r.k = k;
    std::vector<double> beta(k, 0.0), eta(n), w(n), z(n), sw(n);
    std::vector<double> Xs(static_cast<size_t>(n) * k), zs(n), bread(static_cast<size_t>(k) * k), Xtz(k);
    r.p.assign(n, 0.0);
    auto reweight = [&](const std::vector<double>& b) {
        la::mat_vec(X, n, k, b.data(), eta.data());
        for (int64_t i = 0; i < n; ++i) {
            // BUG-331: eta carries the offset; the working response subtracts it
            // back out, because IRLS regresses (eta - offset) + (y-p)/w on X. With
            // no offset both terms are 0 and every line below is the prior code.
            double off = offset ? offset[i] : 0.0;
            double e = std::max(-30.0, std::min(30.0, eta[i] + off));
            double p = 1.0 / (1.0 + std::exp(-e));
            r.p[i] = p;
            double wi = p * (1.0 - p);
            if (wi < 1e-12) wi = 1e-12;
            // z (the IRLS working response) always divides by the UNweighted,
            // floor-clamped p(1-p) — a per-row weight only rescales the normal
            // equations (see logit.h), not the pseudo-response.
            z[i] = (e - off) + (y[i] - p) / wi;
            if (weights) wi *= weights[i];
            w[i] = wi;
            sw[i] = std::sqrt(wi);
        }
    };
    // BUG-331 starting values. beta=0 means eta_0 = offset, and a real offset is
    // far from 0 (ln(weight) ~ 8 on the oracle fixture), so the first working
    // response is O(1/p(1-p)) ~ 3000 and IRLS walks off: measured, 100 iterations,
    // beta ~ 3e10, converged=false. The standard GLM remedy is to start from the
    // DATA rather than from beta=0 — mu_i = (y_i + 1/2)/2, eta_i = link(mu_i) —
    // which is offset-free and lands the first step near the answer.
    // Applied ONLY when an offset is present: the no-offset path keeps its beta=0
    // start and is byte-identical, iteration count included (verified against a
    // zero-valued offset column, which reproduces the no-offset fit exactly).
    if (offset) {
        // link(3/4) and link(1/4) for the logit link; y is binary so these two
        // values are the whole table.
        const double kEta1 = 1.0986122886681098, kEta0 = -1.0986122886681098;
        const double kMu1 = 0.75, kMu0 = 0.25, kW = 0.1875;  // mu(1-mu) at both
        for (int64_t i = 0; i < n; ++i) {
            double e0 = y[i] == 1.0 ? kEta1 : kEta0;
            double mu = y[i] == 1.0 ? kMu1 : kMu0;
            double wi = kW;
            z[i] = e0 + (y[i] - mu) / wi - offset[i];
            if (weights) wi *= weights[i];
            w[i] = wi;
            sw[i] = std::sqrt(wi);
        }
        for (int c = 0; c < k; ++c) {
            const double* xc = X + static_cast<size_t>(c) * n;
            double* sc = Xs.data() + static_cast<size_t>(c) * n;
            for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
        }
        for (int64_t i = 0; i < n; ++i) zs[i] = z[i] * sw[i];
        la::xtx(Xs.data(), n, k, bread.data());
        if (la::spd_inverse(bread.data(), k) == 0) {
            la::xtv(Xs.data(), n, k, zs.data(), Xtz.data());
            la::matvec(bread.data(), k, Xtz.data(), beta.data());
        }
    }

    double dev_old = 1e300;
    for (r.iters = 0; r.iters < maxit;) {
        reweight(beta);
        for (int c = 0; c < k; ++c) {
            const double* xc = X + static_cast<size_t>(c) * n;
            double* sc = Xs.data() + static_cast<size_t>(c) * n;
            for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
        }
        for (int64_t i = 0; i < n; ++i) zs[i] = z[i] * sw[i];
        la::xtx(Xs.data(), n, k, bread.data());
        if (la::spd_inverse(bread.data(), k) != 0) { r.separated = true; break; }
        la::xtv(Xs.data(), n, k, zs.data(), Xtz.data());
        la::matvec(bread.data(), k, Xtz.data(), beta.data());
        ++r.iters;
        // deviance at the new beta (weighted, when weights are present, so the
        // convergence monitor tracks the actual pseudo-likelihood objective)
        la::mat_vec(X, n, k, beta.data(), eta.data());
        double dev = 0;
        for (int64_t i = 0; i < n; ++i) {
            double e = std::max(-30.0, std::min(30.0, eta[i] + (offset ? offset[i] : 0.0)));
            double p = 1.0 / (1.0 + std::exp(-e));
            double di = -2.0 * (y[i] * std::log(p > 1e-15 ? p : 1e-15) +
                                (1 - y[i]) * std::log((1 - p) > 1e-15 ? (1 - p) : 1e-15));
            dev += weights ? weights[i] * di : di;
        }
        if (std::fabs(dev - dev_old) / (std::fabs(dev) + 0.1) < tol) { r.converged = true; break; }
        dev_old = dev;
    }
    // final probabilities + bread = (X'WX)^{-1} at the converged beta
    reweight(beta);
    bool predicts_all = true;
    double final_deviance = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        double p = std::min(1.0 - 1e-15, std::max(1e-15, r.p[i]));
        bool predicts_one = y[i] == 1.0 && p > 0.5;
        bool predicts_zero = y[i] == 0.0 && p < 0.5;
        if (!predicts_one && !predicts_zero) {
            predicts_all = false;
        }
        final_deviance += -2.0 * (y[i] * std::log(p) + (1.0 - y[i]) * std::log(1.0 - p));
    }
    r.separated_perfect = n > 0 && predicts_all && final_deviance / static_cast<double>(n) < 1e-6;
    for (int c = 0; c < k; ++c) {
        const double* xc = X + static_cast<size_t>(c) * n;
        double* sc = Xs.data() + static_cast<size_t>(c) * n;
        for (int64_t i = 0; i < n; ++i) sc[i] = xc[i] * sw[i];
    }
    la::xtx(Xs.data(), n, k, bread.data());
    la::spd_inverse(bread.data(), k);
    r.bread = std::move(bread);
    r.beta = std::move(beta);
    return r;
}

}  // namespace cecon
