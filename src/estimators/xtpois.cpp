// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/xtpois.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/linalg.h"
#include "estimators/logit.h"  // MlObjective / ml_newton_path: Stata's ml path

namespace cecon {

namespace {

struct Group {
    int64_t start, end;  // [start, end) into the working arrays (sorted by group)
};

}  // namespace

XtpoisFeResult xtpoisson_fe(const double* X_in, int64_t n_obs, int k,
                            const double* y_in, const double* ivar_in,
                            const double* offset_in,
                            int maxit, double tol) {
    XtpoisFeResult R;
    R.k = k;

    // ---- 1. Sort rows by ivar (stable ordering on first appearance) ----
    // Build perm so that perm[r] is the original row index, ordered such that
    // all rows with the same ivar are contiguous (in first-appearance order
    // of distinct ivar values).
    std::unordered_map<double, int> idx_of_ivar;
    idx_of_ivar.reserve(static_cast<size_t>(n_obs / 4 + 1));
    std::vector<int> grp_of_row(static_cast<size_t>(n_obs));
    std::vector<std::vector<int64_t>> rows_per_grp;
    rows_per_grp.reserve(8192);
    for (int64_t r = 0; r < n_obs; ++r) {
        double v = ivar_in[r];
        auto it = idx_of_ivar.find(v);
        int gi;
        if (it == idx_of_ivar.end()) {
            gi = static_cast<int>(rows_per_grp.size());
            idx_of_ivar[v] = gi;
            rows_per_grp.emplace_back();
        } else {
            gi = it->second;
        }
        grp_of_row[static_cast<size_t>(r)] = gi;
        rows_per_grp[static_cast<size_t>(gi)].push_back(r);
    }

    // ---- 2. Drop groups with sum(y) == 0 or size == 1 ----
    std::vector<int> keep_grp;
    keep_grp.reserve(rows_per_grp.size());
    int64_t n_dropped_zero = 0, n_dropped_size = 0;
    for (size_t gi = 0; gi < rows_per_grp.size(); ++gi) {
        const auto& rows = rows_per_grp[gi];
        if (rows.size() < 2) { ++n_dropped_size; continue; }
        double sy = 0;
        for (int64_t r : rows) sy += y_in[r];
        if (sy <= 0) { ++n_dropped_zero; continue; }
        keep_grp.push_back(static_cast<int>(gi));
    }
    R.n_dropped_zero = n_dropped_zero;
    R.n_dropped_size = n_dropped_size;
    R.n_groups = static_cast<int64_t>(keep_grp.size());
    if (!keep_grp.empty()) {
        int64_t tot = 0;
        R.g_min = -1;
        for (int gi : keep_grp) {
            const int64_t T = static_cast<int64_t>(rows_per_grp[static_cast<size_t>(gi)].size());
            R.g_min = R.g_min < 0 ? T : std::min(R.g_min, T);
            R.g_max = std::max(R.g_max, T);
            tot += T;
        }
        R.g_avg = static_cast<double>(tot) / static_cast<double>(keep_grp.size());
    }

    // Pack working arrays in group-contiguous order.
    std::vector<Group> groups;
    groups.reserve(keep_grp.size());
    int64_t n_w = 0;
    for (int gi : keep_grp) n_w += static_cast<int64_t>(rows_per_grp[static_cast<size_t>(gi)].size());
    R.n_obs = n_w;

    std::vector<double> X(static_cast<size_t>(n_w) * k);  // column-major n_w * k
    std::vector<double> y(static_cast<size_t>(n_w));
    std::vector<double> off(static_cast<size_t>(n_w), 0.0);

    int64_t cursor = 0;
    for (int gi : keep_grp) {
        const auto& rows = rows_per_grp[static_cast<size_t>(gi)];
        Group g{cursor, cursor + static_cast<int64_t>(rows.size())};
        groups.push_back(g);
        for (int64_t r : rows) {
            y[static_cast<size_t>(cursor)] = y_in[r];
            if (offset_in) off[static_cast<size_t>(cursor)] = offset_in[r];
            for (int c = 0; c < k; ++c) {
                X[static_cast<size_t>(c) * n_w + cursor] = X_in[static_cast<int64_t>(c) * n_obs + r];
            }
            ++cursor;
        }
    }

    // ---- 3. Stata's ml path on the conditional log-likelihood ----
    // xtpoisson.ado fits the fe model with `ml model d2 xtps_fe ..., search(off)`
    // from b = 0 under `difficult` (its default): ml_newton_path's step and stop
    // rules (logit.h), the log likelihood with its constants
    //   sum_i [lnG(n_i + 1) - sum_t lnG(y_it + 1) + sum_t y_it ln p_it],
    // so e(ic), e(ilog) and the iterate it stops at are Stata's (bank 8e730ee7
    // `xtpoisson fatal beertax unemp, fe`: Iteration 0 -2080.7349, e(ic) 3).
    std::vector<double> xb(static_cast<size_t>(n_w), 0.0);
    std::vector<double> p(static_cast<size_t>(n_w), 0.0);
    std::vector<double> xbar(static_cast<size_t>(k), 0.0);
    std::vector<double> dx(static_cast<size_t>(k), 0.0);
    std::vector<double> per_group_score;  // for robust SE; n_groups * k
    double cst = 0;
    for (const auto& g : groups) {
        double n_i = 0;
        for (int64_t i = g.start; i < g.end; ++i) n_i += y[static_cast<size_t>(i)];
        cst += std::lgamma(n_i + 1.0);
        for (int64_t i = g.start; i < g.end; ++i) cst -= std::lgamma(y[static_cast<size_t>(i)] + 1.0);
    }
    struct FeObjective final : MlObjective {
        const std::vector<double>& X;
        const std::vector<double>& y;
        const std::vector<double>& off;
        const std::vector<Group>& groups;
        int64_t n_w;
        int k;
        double cst;
        FeObjective(const std::vector<double>& X_, const std::vector<double>& y_,
                    const std::vector<double>& off_, const std::vector<Group>& g_, int64_t n_, int k_,
                    double c_)
            : X(X_), y(y_), off(off_), groups(g_), n_w(n_), k(k_), cst(c_) {}
        double run(const std::vector<double>& b, std::vector<double>* g, std::vector<double>* A) {
            const size_t K = static_cast<size_t>(k);
            if (g) g->assign(K, 0.0);
            if (A) A->assign(K * K, 0.0);
            std::vector<double> xb(static_cast<size_t>(n_w)), p(static_cast<size_t>(n_w)), xbar(K), dx(K);
            for (int64_t i = 0; i < n_w; ++i) {
                double s = off[static_cast<size_t>(i)];
                for (int c = 0; c < k; ++c) s += X[static_cast<size_t>(c) * n_w + i] * b[static_cast<size_t>(c)];
                xb[static_cast<size_t>(i)] = s;
            }
            double ll = cst;
            for (const auto& gr : groups) {
                double m = -1e300;
                for (int64_t i = gr.start; i < gr.end; ++i) m = std::max(m, xb[static_cast<size_t>(i)]);
                double denom = 0;
                for (int64_t i = gr.start; i < gr.end; ++i) {
                    p[static_cast<size_t>(i)] = std::exp(xb[static_cast<size_t>(i)] - m);
                    denom += p[static_cast<size_t>(i)];
                }
                const double lden = std::log(denom) + m;
                double n_i = 0;
                for (int64_t i = gr.start; i < gr.end; ++i) {
                    p[static_cast<size_t>(i)] /= denom;
                    n_i += y[static_cast<size_t>(i)];
                    ll += y[static_cast<size_t>(i)] * (xb[static_cast<size_t>(i)] - lden);
                }
                if (!g && !A) continue;
                for (int c = 0; c < k; ++c) {
                    double s = 0;
                    for (int64_t i = gr.start; i < gr.end; ++i)
                        s += p[static_cast<size_t>(i)] * X[static_cast<size_t>(c) * n_w + i];
                    xbar[static_cast<size_t>(c)] = s;
                }
                for (int64_t i = gr.start; i < gr.end; ++i) {
                    const double r = y[static_cast<size_t>(i)] - n_i * p[static_cast<size_t>(i)];
                    const double w = n_i * p[static_cast<size_t>(i)];
                    for (int c = 0; c < k; ++c)
                        dx[static_cast<size_t>(c)] = X[static_cast<size_t>(c) * n_w + i] - xbar[static_cast<size_t>(c)];
                    for (int c = 0; c < k; ++c) {
                        if (g) (*g)[static_cast<size_t>(c)] += r * dx[static_cast<size_t>(c)];
                        if (A && w != 0)
                            for (int q = 0; q < k; ++q)
                                (*A)[static_cast<size_t>(c) * K + q] += w * dx[static_cast<size_t>(c)] * dx[static_cast<size_t>(q)];
                    }
                }
            }
            return ll;
        }
        double loglik(const std::vector<double>& b) override { return run(b, nullptr, nullptr); }
        double evaluate(const std::vector<double>& b, std::vector<double>& g, std::vector<double>& A) override {
            return run(b, &g, &A);
        }
    };
    FeObjective obj(X, y, off, groups, n_w, k, cst);
    MlMaxOptions mo;
    mo.maxiter = maxit > 0 ? std::max(maxit, 300) : 300;
    mo.difficult = true;
    mo.stata_invsym = true;
    (void)tol;
    MlPath path = ml_newton_path(obj, std::vector<double>(static_cast<size_t>(k), 0.0), mo);
    if (path.failed || path.b.size() != static_cast<size_t>(k) || !std::isfinite(path.ll)) {
        R.converged = false;
        R.iters = path.ic;
        return R;
    }
    R.beta = path.b;
    R.iters = path.ic;
    R.converged = path.converged;
    R.loglik = path.ll;
    R.ilog = path.ilog;
    R.gradient = path.gradient;
    std::vector<double> negH(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> negH_inv(static_cast<size_t>(k) * k, 0.0);

    // ---- 4. IID and robust VCE ----
    // Recompute -H, per-group scores at the final β.
    for (int64_t i = 0; i < n_w; ++i) {
        double s = off[static_cast<size_t>(i)];
        for (int c = 0; c < k; ++c) s += X[static_cast<size_t>(c) * n_w + i] * R.beta[static_cast<size_t>(c)];
        xb[static_cast<size_t>(i)] = s;
    }
    std::fill(negH.begin(), negH.end(), 0.0);
    per_group_score.assign(static_cast<size_t>(R.n_groups) * k, 0.0);
    size_t gi_idx = 0;
    for (const auto& g : groups) {
        double m = -1e300;
        for (int64_t i = g.start; i < g.end; ++i)
            if (xb[static_cast<size_t>(i)] > m) m = xb[static_cast<size_t>(i)];
        double denom = 0;
        for (int64_t i = g.start; i < g.end; ++i) {
            double e = std::exp(xb[static_cast<size_t>(i)] - m);
            p[static_cast<size_t>(i)] = e;
            denom += e;
        }
        double n_i = 0;
        for (int64_t i = g.start; i < g.end; ++i) {
            p[static_cast<size_t>(i)] /= denom;
            n_i += y[static_cast<size_t>(i)];
        }
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int64_t i = g.start; i < g.end; ++i)
                s += p[static_cast<size_t>(i)] * X[static_cast<size_t>(c) * n_w + i];
            xbar[static_cast<size_t>(c)] = s;
        }
        // per-group score — centred, same argument as the Newton score above.
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int64_t i = g.start; i < g.end; ++i) {
                double r = y[static_cast<size_t>(i)] - n_i * p[static_cast<size_t>(i)];
                s += r * (X[static_cast<size_t>(c) * n_w + i] - xbar[static_cast<size_t>(c)]);
            }
            per_group_score[gi_idx * k + static_cast<size_t>(c)] = s;
        }
        ++gi_idx;
        for (int64_t i = g.start; i < g.end; ++i) {
            double w = n_i * p[static_cast<size_t>(i)];
            if (w == 0) continue;
            for (int c = 0; c < k; ++c)
                dx[static_cast<size_t>(c)] = X[static_cast<size_t>(c) * n_w + i]
                                            - xbar[static_cast<size_t>(c)];
            for (int c = 0; c < k; ++c) {
                double wd = w * dx[static_cast<size_t>(c)];
                double* col = negH.data() + static_cast<size_t>(c) * k;
                for (int rr = 0; rr < k; ++rr)
                    col[static_cast<size_t>(rr)] += wd * dx[static_cast<size_t>(rr)];
            }
        }
    }
    std::copy(negH.begin(), negH.end(), negH_inv.begin());
    if (la::spd_inverse(negH_inv.data(), k) != 0) {
        R.converged = false;
        return R;
    }
    // IID SE = sqrt(diag((-H)^{-1}))
    R.V_iid = negH_inv;
    R.se_iid.assign(static_cast<size_t>(k), 0.0);
    for (int c = 0; c < k; ++c)
        R.se_iid[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, negH_inv[static_cast<size_t>(c) * k + c]));

    // G = Σ_i s_i s_i'    (k*k symmetric, column-major)
    std::vector<double> G(static_cast<size_t>(k) * k, 0.0);
    for (int64_t gi = 0; gi < R.n_groups; ++gi) {
        for (int c = 0; c < k; ++c) {
            double sc = per_group_score[static_cast<size_t>(gi) * k + c];
            for (int r = 0; r < k; ++r)
                G[static_cast<size_t>(c) * k + r] += sc * per_group_score[static_cast<size_t>(gi) * k + r];
        }
    }
    // Stata's xtpoisson, fe with vce(robust) applies NO cluster small-sample
    // correction (it implements the Wooldridge 1999 formula as-is). This is
    // different from cluster-robust in regress / reghdfe (which use M/(M-1) *
    // (N-1)/(N-k)). We follow Stata here so SEs match the golden.
    double q = 1.0;

    // V_robust = (-H)^{-1} * G * (-H)^{-1} * q
    std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
    // tmp = negH_inv * G
    for (int c = 0; c < k; ++c)
        for (int r = 0; r < k; ++r) {
            double s = 0;
            for (int j = 0; j < k; ++j)
                s += negH_inv[static_cast<size_t>(j) * k + r] * G[static_cast<size_t>(c) * k + j];
            tmp[static_cast<size_t>(c) * k + r] = s;
        }
    std::vector<double> Vr(static_cast<size_t>(k) * k, 0.0);
    for (int c = 0; c < k; ++c)
        for (int r = 0; r < k; ++r) {
            double s = 0;
            for (int j = 0; j < k; ++j)
                s += tmp[static_cast<size_t>(j) * k + r] * negH_inv[static_cast<size_t>(c) * k + j];
            Vr[static_cast<size_t>(c) * k + r] = q * s;
        }
    R.V_robust = Vr;
    R.se_robust.assign(static_cast<size_t>(k), 0.0);
    for (int c = 0; c < k; ++c)
        R.se_robust[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, Vr[static_cast<size_t>(c) * k + c]));

    return R;
}

}  // namespace cecon
