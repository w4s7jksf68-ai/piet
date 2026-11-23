// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "nlgmm.h"

#include "core/threads.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/linalg.h"
#include "vcov/vcov.h"

namespace cecon {

namespace {

constexpr int kMaxIter = 200;
constexpr int kMaxHalve = 30;

// Relative change in the objective below which the fit is declared converged.
// Both objectives are sums of squares, so a relative criterion is the scale-free
// one; 1e-13 is a few ulp above the point where the objective stops moving at
// all in double precision.
constexpr double kTolObj = 1e-13;

constexpr double kPi = 3.14159265358979323846;

// Solve A x = rhs for symmetric positive-definite A via la::spd_inverse (which
// overwrites its argument with the inverse). `A` is taken BY VALUE because it is
// destroyed; `Ainv`, when non-null, receives the inverse so a caller can reuse
// the factorisation for a covariance.
bool spd_solve(std::vector<double> A, int k, const std::vector<double>& rhs,
               std::vector<double>* x, std::vector<double>* Ainv) {
    if (la::spd_inverse(A.data(), k) != 0) return false;
    x->assign(static_cast<size_t>(k), 0.0);
    la::matvec(A.data(), k, rhs.data(), x->data());
    if (Ainv) *Ainv = A;
    return true;
}

// Levenberg damping: A += lambda * |diag(A)|, applied ONLY when plain
// Gauss-Newton's normal matrix will not factor.
void damp(std::vector<double>* A, int k, double lambda) {
    for (int i = 0; i < k; ++i) {
        double& d = (*A)[static_cast<size_t>(i) * k + i];
        d += lambda * (std::fabs(d) > 0 ? std::fabs(d) : 1.0);
    }
}

bool all_finite(const std::vector<double>& v) {
    for (double x : v)
        if (!std::isfinite(x)) return false;
    return true;
}

// Gauss-Newton step with the Levenberg escalation. Returns false when even the
// heavily damped system is unsolvable.
bool gn_step(const std::vector<double>& A, int k, const std::vector<double>& rhs,
             std::vector<double>* delta, std::vector<double>* Ainv) {
    if (spd_solve(A, k, rhs, delta, Ainv) && all_finite(*delta)) return true;
    for (double lambda = 1e-8; lambda <= 1e8; lambda *= 100.0) {
        std::vector<double> Ad = A;
        damp(&Ad, k, lambda);
        if (spd_solve(Ad, k, rhs, delta, Ainv) && all_finite(*delta)) return true;
    }
    return false;
}

}  // namespace

void nl_jacobian(const NlResidFn& resid, const std::vector<double>& b, int64_t n, int k,
                 std::vector<double>* J) {
    static const double kCbrtEps = std::cbrt(std::numeric_limits<double>::epsilon());
    J->assign(static_cast<size_t>(n) * static_cast<size_t>(k), 0.0);
    // Column j perturbs only b_j and writes only J[:, j] — no accumulator is shared
    // between columns — so evaluating them concurrently returns the identical Jacobian.
    // The perturbed parameter vectors have to become per-worker (the serial version
    // reused two, resetting the touched entry each time); everything each worker reads
    // besides them is const. (PERFSWEEP H: same class as the numeric Hessians.)
    parallel_for(k, [&](int64_t jj_) {
        const size_t jj = static_cast<size_t>(jj_);
        thread_local std::vector<double> bp, bm;
        bp = b;
        bm = b;
        const double h = kCbrtEps * std::max(std::fabs(b[jj]), 1.0);
        bp[jj] = b[jj] + h;
        bm[jj] = b[jj] - h;
        const double step = bp[jj] - bm[jj];  // the REALIZED step, not 2h — see the header
        double* col = J->data() + jj * static_cast<size_t>(n);
        for (int64_t i = 0; i < n; ++i) col[i] = (resid(bp, i) - resid(bm, i)) / step;
    }, threads_for_work(static_cast<int64_t>(k) * n));
}

// ---------------------------------------------------------------------------
// nl
// ---------------------------------------------------------------------------

NlResult nl_fit(const NlResidFn& resid_in, const double* y, int64_t n,
                const std::vector<double>& b0, const std::vector<std::string>& names,
                const double* w, bool robust) {
    NlResult r;
    r.names = names;
    r.n = n;
    r.k = static_cast<int>(b0.size());
    const int k = r.k;
    if (k == 0) { r.err = "nl: the model has no parameters"; return r; }
    if (n <= k) { r.err = "nl: need more observations than parameters"; return r; }

    // Weighted (BUG-1283): sqrt(w) * e_i(b) turns every sum below into its weighted
    // form -- the SS, J'J = J'WJ, and the robust meat's w e J.
    std::vector<double> sw;
    if (w) {
        sw.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) sw[static_cast<size_t>(i)] = std::sqrt(w[i]);
    }
    const NlResidFn resid = w ? NlResidFn([&](const std::vector<double>& bb, int64_t i) {
                                    return sw[static_cast<size_t>(i)] * resid_in(bb, i);
                                })
                              : resid_in;

    std::vector<double> b = b0, e(static_cast<size_t>(n));
    auto eval_rss = [&](const std::vector<double>& bb, std::vector<double>* out) {
        double s = 0;
        for (int64_t i = 0; i < n; ++i) {
            const double ei = resid(bb, i);
            if (out) (*out)[static_cast<size_t>(i)] = ei;
            s += ei * ei;
        }
        return s;
    };

    double rss = eval_rss(b, &e);
    if (!std::isfinite(rss)) {
        r.err = "nl: the residual is not finite at the initial values";
        return r;
    }
    r.iters.push_back({rss, false});

    std::vector<double> J, A, g, rhs, delta, Ainv, btry(static_cast<size_t>(k));
    for (int it = 0; it < kMaxIter; ++it) {
        // (J'J) d = -J'e, J = de/db.
        nl_jacobian(resid, b, n, k, &J);
        A.assign(static_cast<size_t>(k) * k, 0.0);
        la::xtx(J.data(), n, k, A.data());
        g.assign(static_cast<size_t>(k), 0.0);
        la::xtv(J.data(), n, k, e.data(), g.data());
        rhs.assign(static_cast<size_t>(k), 0.0);
        for (int j = 0; j < k; ++j) rhs[static_cast<size_t>(j)] = -g[static_cast<size_t>(j)];
        if (!gn_step(A, k, rhs, &delta, &Ainv)) {
            r.err = "nl: singular Gauss-Newton normal matrix (the derivative columns are collinear)";
            return r;
        }

        // Line search: full step, then halve until the objective improves. A step
        // that had to be shortened is Stata's "(backed up)".
        double scale = 1.0, rss_new = rss;
        bool improved = false, backed_up = false;
        for (int h = 0; h <= kMaxHalve; ++h) {
            for (int j = 0; j < k; ++j)
                btry[static_cast<size_t>(j)] = b[static_cast<size_t>(j)] + scale * delta[static_cast<size_t>(j)];
            rss_new = eval_rss(btry, nullptr);
            if (std::isfinite(rss_new) && rss_new < rss) { improved = true; break; }
            scale *= 0.5;
            backed_up = true;
        }
        if (!improved) {
            // The full step and every shortened one failed to improve: this IS the
            // optimum to the precision the objective can express. Stata's log ends
            // the same way — a final "(backed up)" line at an unchanged SS.
            r.iters.push_back({rss, true});
            r.converged = true;
            break;
        }
        b = btry;
        const double prev = rss;
        rss = eval_rss(b, &e);
        r.iters.push_back({rss, backed_up});
        if (std::fabs(prev - rss) <= kTolObj * (std::fabs(prev) + 1.0)) {
            r.converged = true;
            break;
        }
    }

    // Jacobian AT THE OPTIMUM for the covariance and the constant-term test: the
    // loop's last J belongs to the previous b.
    nl_jacobian(resid, b, n, k, &J);
    A.assign(static_cast<size_t>(k) * k, 0.0);
    la::xtx(J.data(), n, k, A.data());
    if (la::spd_inverse(A.data(), k) != 0) {
        r.err = "nl: (J'J) is singular at the converged parameters";
        return r;
    }

    r.beta = b;
    r.rss = rss;
    r.df_r = static_cast<double>(n - k);
    r.sigma2 = r.rss / r.df_r;

    // CONSTANT-TERM DETECTION. de/db_j is identically -1 exactly when b_j enters
    // additively, i.e. when the FITTED function's derivative column is the
    // constant 1 that Stata's note describes.
    //
    // TOLERANCE — measurement overruled the design note here. The note proposed
    // ~1e-12 relative; that is BELOW the noise floor of a central difference.
    // Differencing subtracts two O(1) residuals whose gap is O(h) ~ 1e-5, so the
    // quotient carries a cancellation error of order eps/h ~ 1e-11. On the P7
    // fixture the b0 column deviates from -1 by up to ~1e-11, which a 1e-12 test
    // would reject outright. 1e-8 sits three orders above that noise and orders
    // below any real covariate's variation, so it discriminates without being
    // fooled. tests/unit/test_nlgmm.cpp prints the measured worst deviation.
    constexpr double kConsTol = 1e-8;
    for (int j = 0; j < k && r.cons_param < 0; ++j) {
        const double* col = J.data() + static_cast<size_t>(j) * static_cast<size_t>(n);
        bool constant = true;
        for (int64_t i = 0; i < n && constant; ++i)
            constant = std::fabs((w ? col[i] / sw[static_cast<size_t>(i)] : col[i]) + 1.0) <= kConsTol;
        if (constant) r.cons_param = j;
    }

    double ybar = 0, sumw = 0;
    for (int64_t i = 0; i < n; ++i) {
        ybar += w ? w[i] * y[i] : y[i];
        sumw += w ? w[i] : 1.0;
    }
    ybar /= sumw;
    r.tss = 0;
    for (int64_t i = 0; i < n; ++i) {
        const double d = (r.cons_param >= 0) ? (y[i] - ybar) : y[i];
        r.tss += w ? w[i] * d * d : d * d;
    }
    r.df_t = (r.cons_param >= 0) ? static_cast<double>(n - 1) : static_cast<double>(n);
    r.df_m = (r.cons_param >= 0) ? static_cast<double>(k - 1) : static_cast<double>(k);
    r.mss = r.tss - r.rss;
    r.r2 = (r.tss > 0) ? 1.0 - r.rss / r.tss : 0.0;
    r.r2_adj = (r.tss > 0 && r.df_t > 0 && r.df_r > 0) ? 1.0 - (r.rss / r.df_r) / (r.tss / r.df_t) : 0.0;
    r.rmse = std::sqrt(r.sigma2);
    // "Res. dev." = -2 lnL of the Gaussian NLS model at its ML variance rss/n.
    r.res_dev = static_cast<double>(n) *
                (1.0 + std::log(2.0 * kPi) + std::log(r.rss / static_cast<double>(n)));
    if (w) {
        // Stata's nl: e(dev) = n*(1 + ln(2*pi*rss/n) - mean(ln w)), w normalized.
        double sum_lnw = 0;  // n * mean(ln w)
        for (int64_t i = 0; i < n; ++i) sum_lnw += std::log(w[i]);
        r.res_dev -= sum_lnw;
    }

    r.vcov.assign(static_cast<size_t>(k) * k, 0.0);
    if (robust) {
        // e is sqrt(w)*e and J sqrt(w)*J here, so e_i J_i is the pweighted score.
        std::vector<double> meat(static_cast<size_t>(k) * k);
        hc_meat(J.data(), n, k, e.data(), meat.data());
        la::sandwich(A.data(), meat.data(), k,
                     static_cast<double>(n) / static_cast<double>(n - k), r.vcov.data());
    } else {
        for (size_t idx = 0; idx < r.vcov.size(); ++idx) r.vcov[idx] = r.sigma2 * A[idx];
    }
    r.se.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        r.se[static_cast<size_t>(j)] = std::sqrt(std::max(0.0, r.vcov[static_cast<size_t>(j) * k + j]));
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// gmm
// ---------------------------------------------------------------------------

GmmResult gmm_fit(const NlResidFn& resid, const double* Z, int64_t n, int L,
                  const std::vector<double>& b0, const std::vector<std::string>& names) {
    GmmResult r;
    r.names = names;
    r.n = n;
    r.k = static_cast<int>(b0.size());
    r.n_moments = L;
    const int k = r.k;
    if (k == 0) { r.err = "gmm: the model has no parameters"; return r; }
    if (L < k) { r.err = "gmm: the model needs at least as many moments as parameters"; return r; }
    if (n <= k) { r.err = "gmm: need more observations than parameters"; return r; }
    const double N = static_cast<double>(n);
    const size_t Lz = static_cast<size_t>(L);

    // W = (Z'Z/N)^-1 — Stata's winitial(unadjusted), which `onestep` leaves in place.
    std::vector<double> W(Lz * Lz, 0.0);
    la::xtx(Z, n, L, W.data());
    for (double& w : W) w /= N;
    if (la::spd_inverse(W.data(), L) != 0) {
        r.err = "gmm: the instrument cross-product Z'Z is singular";
        return r;
    }

    std::vector<double> e(static_cast<size_t>(n)), gm(Lz);
    auto moments = [&](const std::vector<double>& bb) {
        for (int64_t i = 0; i < n; ++i) e[static_cast<size_t>(i)] = resid(bb, i);
        gm.assign(Lz, 0.0);
        la::xtv(Z, n, L, e.data(), gm.data());
        for (double& v : gm) v /= N;
    };
    auto quad = [&](const std::vector<double>& v) {
        double q = 0;
        for (int a = 0; a < L; ++a) {
            double s = 0;
            for (int c = 0; c < L; ++c) s += W[static_cast<size_t>(a) * Lz + c] * v[static_cast<size_t>(c)];
            q += v[static_cast<size_t>(a)] * s;
        }
        return q;
    };

    // G = (1/N) Z' de/db'   — L x k, column-major (G[j*L + m] is row m, col j).
    std::vector<double> J;
    auto build_G = [&](const std::vector<double>& bb, std::vector<double>* G) {
        nl_jacobian(resid, bb, n, k, &J);
        G->assign(Lz * static_cast<size_t>(k), 0.0);
        for (int j = 0; j < k; ++j) {
            const double* dcol = J.data() + static_cast<size_t>(j) * static_cast<size_t>(n);
            double* gcol = G->data() + static_cast<size_t>(j) * Lz;
            la::xtv(Z, n, L, dcol, gcol);
            for (int m = 0; m < L; ++m) gcol[m] /= N;
        }
    };
    // GW = G'W  (k x L, GW[j*L + c] is row j, col c) and A = (G'W) G  (k x k).
    auto build_GW_A = [&](const std::vector<double>& G, std::vector<double>* GW,
                          std::vector<double>* A) {
        GW->assign(static_cast<size_t>(k) * Lz, 0.0);
        for (int j = 0; j < k; ++j)
            for (int c = 0; c < L; ++c) {
                double s = 0;
                for (int m = 0; m < L; ++m)
                    s += G[static_cast<size_t>(j) * Lz + m] * W[static_cast<size_t>(m) * Lz + c];
                (*GW)[static_cast<size_t>(j) * Lz + c] = s;
            }
        A->assign(static_cast<size_t>(k) * k, 0.0);
        for (int j = 0; j < k; ++j)
            for (int c = 0; c < k; ++c) {
                double s = 0;
                for (int m = 0; m < L; ++m)
                    s += (*GW)[static_cast<size_t>(j) * Lz + m] * G[static_cast<size_t>(c) * Lz + m];
                (*A)[static_cast<size_t>(j) * k + c] = s;
            }
    };

    std::vector<double> b = b0;
    moments(b);
    double q = quad(gm);
    if (!std::isfinite(q)) {
        r.err = "gmm: the moment conditions are not finite at the initial values";
        return r;
    }
    r.iters.push_back({q, false});

    std::vector<double> G, GW, A, rhs, delta, Ainv, btry(static_cast<size_t>(k));
    for (int it = 0; it < kMaxIter; ++it) {
        build_G(b, &G);
        build_GW_A(G, &GW, &A);
        rhs.assign(static_cast<size_t>(k), 0.0);
        for (int j = 0; j < k; ++j) {
            double s = 0;
            for (int m = 0; m < L; ++m) s += GW[static_cast<size_t>(j) * Lz + m] * gm[static_cast<size_t>(m)];
            rhs[static_cast<size_t>(j)] = -s;
        }
        if (!gn_step(A, k, rhs, &delta, &Ainv)) {
            r.err = "gmm: singular G'WG (the moment conditions do not identify the parameters)";
            return r;
        }

        double scale = 1.0, q_new = q;
        bool improved = false, backed_up = false;
        for (int h = 0; h <= kMaxHalve; ++h) {
            for (int j = 0; j < k; ++j)
                btry[static_cast<size_t>(j)] = b[static_cast<size_t>(j)] + scale * delta[static_cast<size_t>(j)];
            moments(btry);
            q_new = quad(gm);
            if (std::isfinite(q_new) && q_new < q) { improved = true; break; }
            scale *= 0.5;
            backed_up = true;
        }
        if (!improved) {
            r.iters.push_back({q, true});
            r.converged = true;
            break;
        }
        b = btry;
        const double prev = q;
        q = q_new;
        r.iters.push_back({q, backed_up});
        if (std::fabs(prev - q) <= kTolObj * (std::fabs(prev) + 1.0)) {
            r.converged = true;
            break;
        }
    }

    // Covariance at the optimum — recomputed rather than reused from the loop,
    // whose last G belongs to the previous b.
    moments(b);
    q = quad(gm);
    build_G(b, &G);
    build_GW_A(G, &GW, &A);
    std::vector<double> bread = A;  // G'WG
    if (la::spd_inverse(bread.data(), k) != 0) {
        r.err = "gmm: G'WG is singular at the converged parameters";
        return r;
    }

    // S = (1/N) sum_i e_i^2 z_i z_i'   (L x L)
    std::vector<double> S(Lz * Lz, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        const double w = e[static_cast<size_t>(i)] * e[static_cast<size_t>(i)];
        for (int aa = 0; aa < L; ++aa) {
            const double za = w * Z[static_cast<size_t>(aa) * static_cast<size_t>(n) + i];
            for (int bb = 0; bb < L; ++bb)
                S[static_cast<size_t>(aa) * Lz + bb] += za * Z[static_cast<size_t>(bb) * static_cast<size_t>(n) + i];
        }
    }
    for (double& v : S) v /= N;

    // meat = (G'W) S (G'W)'   (k x k)
    std::vector<double> SW(static_cast<size_t>(k) * Lz, 0.0), meat(static_cast<size_t>(k) * k, 0.0);
    for (int j = 0; j < k; ++j)
        for (int c = 0; c < L; ++c) {
            double s = 0;
            for (int m = 0; m < L; ++m)
                s += GW[static_cast<size_t>(j) * Lz + m] * S[static_cast<size_t>(m) * Lz + c];
            SW[static_cast<size_t>(j) * Lz + c] = s;
        }
    for (int j = 0; j < k; ++j)
        for (int c = 0; c < k; ++c) {
            double s = 0;
            for (int m = 0; m < L; ++m)
                s += SW[static_cast<size_t>(j) * Lz + m] * GW[static_cast<size_t>(c) * Lz + m];
            meat[static_cast<size_t>(j) * k + c] = s;
        }

    // V = (1/N) bread * meat * bread. NO small-sample factor (see the header).
    r.vcov.assign(static_cast<size_t>(k) * k, 0.0);
    std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
    la::sandwich(bread.data(), meat.data(), k, 1.0 / N, r.vcov.data(), tmp.data());

    r.beta = b;
    r.q = q;
    r.se.assign(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        r.se[static_cast<size_t>(j)] = std::sqrt(std::max(0.0, r.vcov[static_cast<size_t>(j) * k + j]));
    r.ok = true;
    return r;
}


// ---------------------------------------------------------------------------
// gmm (full)
// ---------------------------------------------------------------------------

namespace {

// d e_i / d b_j for one equation. A residual that is a polynomial of degree <= 2
// in b_j (every linear-combination moment) has an EXACT central difference at
// any step, so the column is taken with the unit-scale step H = max(|b_j|, 1),
// whose rounding error eps*|e|/H is ~1e5 times below the small step's; the
// column is accepted only when the steps H and 2H agree to rounding on every
// row (a cubic or worse term would separate them). Otherwise the small-step
// central difference of nl_jacobian is used.
void gmm_jacobian(const GmmEqResidFn& resid, int eq, const std::vector<double>& b, int64_t n,
                  int k, std::vector<double>* J) {
    static const double kCbrtEps = std::cbrt(std::numeric_limits<double>::epsilon());
    const double eps = std::numeric_limits<double>::epsilon();
    J->assign(static_cast<size_t>(n) * static_cast<size_t>(k), 0.0);
    std::vector<double> bp = b, bm = b, bp2 = b, bm2 = b;
    for (int j = 0; j < k; ++j) {
        const size_t jj = static_cast<size_t>(j);
        double* col = J->data() + jj * static_cast<size_t>(n);
        const double H = std::max(std::fabs(b[jj]), 1.0);
        bp[jj] = b[jj] + H;
        bm[jj] = b[jj] - H;
        bp2[jj] = b[jj] + 2 * H;
        bm2[jj] = b[jj] - 2 * H;
        const double st1 = bp[jj] - bm[jj], st2 = bp2[jj] - bm2[jj];
        bool linear = true;
        for (int64_t i = 0; i < n && linear; ++i) {
            const double fp = resid(eq, bp, i), fm = resid(eq, bm, i);
            const double fp2 = resid(eq, bp2, i), fm2 = resid(eq, bm2, i);
            const double d1 = (fp - fm) / st1, d2 = (fp2 - fm2) / st2;
            const double noise =
                64 * eps * (std::fabs(fp) + std::fabs(fm) + std::fabs(fp2) + std::fabs(fm2)) / H;
            if (!std::isfinite(d1) || !std::isfinite(d2) ||
                std::fabs(d1 - d2) > noise + 1e-12 * std::fabs(d1))
                linear = false;
            else
                col[i] = d1;
        }
        if (!linear) {
            const double h = kCbrtEps * std::max(std::fabs(b[jj]), 1.0);
            bp[jj] = b[jj] + h;
            bm[jj] = b[jj] - h;
            const double step = bp[jj] - bm[jj];
            for (int64_t i = 0; i < n; ++i) col[i] = (resid(eq, bp, i) - resid(eq, bm, i)) / step;
        }
        bp[jj] = bm[jj] = bp2[jj] = bm2[jj] = b[jj];
    }
}

double mreldif(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]) / (std::fabs(b[i]) + 1.0));
    return m;
}


// Householder QR of the m x c col-major matrix B (m >= c not required): returns the
// c x c upper-triangular R with B'B = R'R (row j of R in R[j*c + ...], row-major).
// `extra` columns appended to B are transformed alongside (their first c entries
// returned in *qtExtra, c x e row-major) -- Q'd for a least-squares solve.
std::vector<double> householder_r(std::vector<double> B, int64_t m, int c, int e,
                                  std::vector<double>* qtExtra) {
    const int tot = c + e;
    auto at = [&](int64_t i, int j) -> double& { return B[static_cast<size_t>(j) * static_cast<size_t>(m) + static_cast<size_t>(i)]; };
    const int steps = static_cast<int>(std::min<int64_t>(m, c));
    std::vector<double> v(static_cast<size_t>(m));
    for (int j = 0; j < steps; ++j) {
        double norm = 0, scale = 0;
        for (int64_t i = j; i < m; ++i) scale = std::max(scale, std::fabs(at(i, j)));
        if (scale == 0) continue;
        for (int64_t i = j; i < m; ++i) { const double t = at(i, j) / scale; norm += t * t; }
        norm = scale * std::sqrt(norm);
        const double alpha = at(j, j) > 0 ? -norm : norm;
        for (int64_t i = j; i < m; ++i) v[static_cast<size_t>(i)] = at(i, j);
        v[static_cast<size_t>(j)] -= alpha;
        double vv = 0;
        for (int64_t i = j; i < m; ++i) vv += v[static_cast<size_t>(i)] * v[static_cast<size_t>(i)];
        if (vv == 0) continue;
        for (int col = j; col < tot; ++col) {
            double d = 0;
            for (int64_t i = j; i < m; ++i) d += v[static_cast<size_t>(i)] * at(i, col);
            const double f = 2 * d / vv;
            for (int64_t i = j; i < m; ++i) at(i, col) -= f * v[static_cast<size_t>(i)];
        }
    }
    std::vector<double> R(static_cast<size_t>(c) * static_cast<size_t>(c), 0.0);
    for (int a = 0; a < steps; ++a)
        for (int b = a; b < c; ++b) R[static_cast<size_t>(a) * static_cast<size_t>(c) + static_cast<size_t>(b)] = at(a, b);
    if (qtExtra) {
        qtExtra->assign(static_cast<size_t>(c) * static_cast<size_t>(e), 0.0);
        for (int a = 0; a < steps; ++a)
            for (int x = 0; x < e; ++x) (*qtExtra)[static_cast<size_t>(a) * static_cast<size_t>(e) + static_cast<size_t>(x)] = at(a, c + x);
    }
    return R;
}

// Inverse of the c x c upper-triangular R (row-major); false when singular to rounding.
bool upper_inverse(const std::vector<double>& R, int c, std::vector<double>* Ri) {
    double dmax = 0;
    for (int j = 0; j < c; ++j) dmax = std::max(dmax, std::fabs(R[static_cast<size_t>(j) * static_cast<size_t>(c) + static_cast<size_t>(j)]));
    Ri->assign(static_cast<size_t>(c) * static_cast<size_t>(c), 0.0);
    for (int j = 0; j < c; ++j) {
        const double d = R[static_cast<size_t>(j) * static_cast<size_t>(c) + static_cast<size_t>(j)];
        if (!(std::fabs(d) > dmax * 1e-15)) return false;
    }
    for (int col = 0; col < c; ++col)
        for (int row = col; row >= 0; --row) {
            double s = row == col ? 1.0 : 0.0;
            for (int t = row + 1; t <= col; ++t)
                s -= R[static_cast<size_t>(row) * static_cast<size_t>(c) + static_cast<size_t>(t)] *
                     (*Ri)[static_cast<size_t>(t) * static_cast<size_t>(c) + static_cast<size_t>(col)];
            (*Ri)[static_cast<size_t>(row) * static_cast<size_t>(c) + static_cast<size_t>(col)] =
                s / R[static_cast<size_t>(row) * static_cast<size_t>(c) + static_cast<size_t>(row)];
        }
    return true;
}

}  // namespace

// The weight matrices enter only through square roots: every S is formed as B'B/N
// from its rows B (sqrt(omega_i) h_i, the cluster sums u_g, sqrt(w_i) sigma z_i), and
// W = S^-1 = R^-1 R^-T from the QR of B, so a Gauss-Newton step is the
// least-squares solve of the whitened system R^-T (g + G d) and (G'WG)^-1 comes
// from a second QR. Forming S and inverting it squares the conditioning: with
// five clusters and five moments (bank 9aa98f1b) cond(S) is ~2e13 and the explicit
// inverse lost e(V) at 5e-6, the square-root form agrees with StataBE to 2.5e-8.
GmmExResult gmm_fit_ex(const GmmExSpec& sp) {
    GmmExResult r;
    const int M = sp.M;
    const int64_t n = sp.n;
    const int k = static_cast<int>(sp.b0.size());
    r.k = k;
    std::vector<int> off(static_cast<size_t>(M) + 1, 0);
    for (int m = 0; m < M; ++m) off[static_cast<size_t>(m) + 1] = off[static_cast<size_t>(m)] + sp.L[static_cast<size_t>(m)];
    const int L = off[static_cast<size_t>(M)];
    r.Ltot = L;
    const size_t Lz = static_cast<size_t>(L), kz = static_cast<size_t>(k), nz = static_cast<size_t>(n);
    if (k == 0) { r.err = "gmm: the model has no parameters"; r.err_code = "bad_input"; return r; }
    if (L < k) {
        r.err = "model is not identified; there are more parameters, " + std::to_string(k) +
                ", than instruments, " + std::to_string(L);
        r.err_code = "not_identified";
        return r;
    }
    const double Nw = sp.Nw, sqN = std::sqrt(Nw);
    auto wt = [&](int64_t i) { return sp.w ? sp.w[i] : 1.0; };
    auto om = [&](int64_t i) { return sp.omega ? sp.omega[i] : 1.0; };
    auto zv = [&](int m, int l, int64_t i) {
        return sp.Z[static_cast<size_t>(m)][static_cast<size_t>(l) * nz + static_cast<size_t>(i)];
    };
    auto Lm = [&](int m) { return sp.L[static_cast<size_t>(m)]; };
    auto Om = [&](int m) { return off[static_cast<size_t>(m)]; };

    std::vector<double> e(static_cast<size_t>(M) * nz), gm(Lz), tmp(nz);
    auto residuals = [&](const std::vector<double>& b) {
        for (int m = 0; m < M; ++m)
            for (int64_t i = 0; i < n; ++i) e[static_cast<size_t>(m) * nz + static_cast<size_t>(i)] = sp.resid(m, b, i);
    };
    auto moments = [&]() {
        for (int m = 0; m < M; ++m) {
            const double* em = e.data() + static_cast<size_t>(m) * nz;
            for (int64_t i = 0; i < n; ++i) tmp[static_cast<size_t>(i)] = wt(i) * em[i];
            for (int l = 0; l < Lm(m); ++l)
                gm[static_cast<size_t>(Om(m) + l)] = la::dot(sp.Z[static_cast<size_t>(m)] + static_cast<size_t>(l) * nz, tmp.data(), n) / Nw;
        }
    };
    std::vector<double> Jm;
    auto build_G = [&](const std::vector<double>& b, std::vector<double>* G) {  // L x k col-major
        G->assign(Lz * kz, 0.0);
        for (int m = 0; m < M; ++m) {
            gmm_jacobian(sp.resid, m, b, n, k, &Jm);
            for (int j = 0; j < k; ++j) {
                const double* dc = Jm.data() + static_cast<size_t>(j) * nz;
                for (int64_t i = 0; i < n; ++i) tmp[static_cast<size_t>(i)] = wt(i) * dc[i];
                for (int l = 0; l < Lm(m); ++l)
                    (*G)[static_cast<size_t>(j) * Lz + static_cast<size_t>(Om(m) + l)] =
                        la::dot(sp.Z[static_cast<size_t>(m)] + static_cast<size_t>(l) * nz, tmp.data(), n) / Nw;
            }
        }
    };
    // Rows B of an S estimate (S = B'B / N), col-major rows x L. type: 0 robust,
    // 1 unadjusted, 2 cluster, 3 the winitial(unadjusted) Z'Z/N (4: independent).
    auto build_B = [&](int type, const int64_t* clus, int64_t* rowsOut) -> std::vector<double> {
        std::vector<double> B;
        if (type == 0) {
            B.assign(nz * Lz, 0.0);
            for (int m = 0; m < M; ++m) {
                const double* em = e.data() + static_cast<size_t>(m) * nz;
                for (int l = 0; l < Lm(m); ++l)
                    for (int64_t i = 0; i < n; ++i)
                        B[static_cast<size_t>(Om(m) + l) * nz + static_cast<size_t>(i)] = std::sqrt(om(i)) * zv(m, l, i) * em[i];
            }
            *rowsOut = n;
        } else if (type == 2) {
            int64_t G = 0;
            for (int64_t i = 0; i < n; ++i) G = std::max(G, clus[i] + 1);
            B.assign(static_cast<size_t>(G) * Lz, 0.0);
            for (int m = 0; m < M; ++m) {
                const double* em = e.data() + static_cast<size_t>(m) * nz;
                for (int l = 0; l < Lm(m); ++l)
                    for (int64_t i = 0; i < n; ++i)
                        B[static_cast<size_t>(Om(m) + l) * static_cast<size_t>(G) + static_cast<size_t>(clus[i])] += wt(i) * zv(m, l, i) * em[i];
            }
            r.n_clust = G;
            *rowsOut = G;
        } else if (type == 1) {
            // sigma (M x M, weighted mean cross-products of the residuals) = C C'
            std::vector<double> sig(static_cast<size_t>(M) * static_cast<size_t>(M));
            for (int a = 0; a < M; ++a)
                for (int c = 0; c < M; ++c) {
                    const double* ea = e.data() + static_cast<size_t>(a) * nz;
                    const double* ec = e.data() + static_cast<size_t>(c) * nz;
                    for (int64_t i = 0; i < n; ++i) tmp[static_cast<size_t>(i)] = wt(i) * ea[i];
                    sig[static_cast<size_t>(a) * static_cast<size_t>(M) + static_cast<size_t>(c)] = la::dot(tmp.data(), ec, n) / Nw;
                }
            std::vector<double> C(static_cast<size_t>(M) * static_cast<size_t>(M), 0.0);  // lower Cholesky
            for (int a = 0; a < M; ++a)
                for (int c = 0; c <= a; ++c) {
                    double s = sig[static_cast<size_t>(a) * static_cast<size_t>(M) + static_cast<size_t>(c)];
                    for (int t = 0; t < c; ++t) s -= C[static_cast<size_t>(a) * static_cast<size_t>(M) + static_cast<size_t>(t)] * C[static_cast<size_t>(c) * static_cast<size_t>(M) + static_cast<size_t>(t)];
                    C[static_cast<size_t>(a) * static_cast<size_t>(M) + static_cast<size_t>(c)] =
                        a == c ? std::sqrt(std::max(0.0, s)) : (C[static_cast<size_t>(c) * static_cast<size_t>(M) + static_cast<size_t>(c)] > 0 ? s / C[static_cast<size_t>(c) * static_cast<size_t>(M) + static_cast<size_t>(c)] : 0.0);
                }
            const int64_t rows = n * M;
            B.assign(static_cast<size_t>(rows) * Lz, 0.0);
            for (int p = 0; p < M; ++p)
                for (int m = p; m < M; ++m) {
                    const double cmp = C[static_cast<size_t>(m) * static_cast<size_t>(M) + static_cast<size_t>(p)];
                    for (int l = 0; l < Lm(m); ++l)
                        for (int64_t i = 0; i < n; ++i)
                            B[static_cast<size_t>(Om(m) + l) * static_cast<size_t>(rows) + static_cast<size_t>(p) * nz + static_cast<size_t>(i)] =
                                std::sqrt(wt(i)) * cmp * zv(m, l, i);
                }
            *rowsOut = rows;
        } else {  // 3 / 4: initial unadjusted, full or block-diagonal across equations
            const bool indep = type == 4;
            const int64_t rows = indep ? n * M : n;
            B.assign(static_cast<size_t>(rows) * Lz, 0.0);
            for (int m = 0; m < M; ++m)
                for (int l = 0; l < Lm(m); ++l)
                    for (int64_t i = 0; i < n; ++i)
                        B[static_cast<size_t>(Om(m) + l) * static_cast<size_t>(rows) + (indep ? static_cast<size_t>(m) * nz : 0) + static_cast<size_t>(i)] =
                            std::sqrt(wt(i)) * zv(m, l, i);
            *rowsOut = rows;
        }
        for (double& v : B) v /= sqN;
        return B;
    };
    // S = B'B (compensated), for e(S)
    auto gram = [&](const std::vector<double>& B, int64_t rows) {
        std::vector<double> S(Lz * Lz);
        for (size_t a = 0; a < Lz; ++a)
            for (size_t c = a; c < Lz; ++c) {
                const double v = la::dot(B.data() + a * static_cast<size_t>(rows), B.data() + c * static_cast<size_t>(rows), rows);
                S[a * Lz + c] = S[c * Lz + a] = v;
            }
        return S;
    };
    // R^-1 (upper, row-major) of an S given by its rows; W = Ri Ri'
    auto sqrt_inv = [&](const std::vector<double>& B, int64_t rows, std::vector<double>* Ri) {
        const std::vector<double> R = householder_r(B, rows, L, 0, nullptr);
        return upper_inverse(R, L, Ri);
    };
    auto w_of = [&](const std::vector<double>& Ri) {
        std::vector<double> W(Lz * Lz, 0.0);
        for (size_t a = 0; a < Lz; ++a)
            for (size_t c = 0; c < Lz; ++c) {
                double s = 0;
                for (size_t t = std::max(a, c); t < Lz; ++t) s += Ri[a * Lz + t] * Ri[c * Lz + t];
                W[a * Lz + c] = s;
            }
        return W;
    };
    // whitened system: C = Ri' G (L x k), d = Ri' g
    std::vector<double> G;
    auto whiten = [&](const std::vector<double>& Ri, const std::vector<double>& v, size_t cols) {
        // out (L x cols, col-major) = Ri' v, v L x cols col-major
        std::vector<double> out(Lz * cols, 0.0);
        for (size_t c = 0; c < cols; ++c)
            for (size_t a = 0; a < Lz; ++a) {
                double s = 0;
                for (size_t t = 0; t <= a; ++t) s += Ri[t * Lz + a] * v[c * Lz + t];
                out[c * Lz + a] = s;
            }
        return out;
    };
    auto crit = [&](const std::vector<double>& Ri) {
        const std::vector<double> d = whiten(Ri, gm, 1);
        double q = 0;
        for (double x : d) q += x * x;
        return q;
    };
    // least-squares GN step: argmin || C delta + d ||
    auto ls_step = [&](const std::vector<double>& Ri, std::vector<double>* delta) -> bool {
        std::vector<double> CD = whiten(Ri, G, kz);
        const std::vector<double> d = whiten(Ri, gm, 1);
        CD.insert(CD.end(), d.begin(), d.end());
        std::vector<double> qtd;
        const std::vector<double> Rc = householder_r(CD, L, k, 1, &qtd);
        std::vector<double> Rci;
        if (!upper_inverse(Rc, k, &Rci)) return false;
        delta->assign(kz, 0.0);
        for (size_t a = 0; a < kz; ++a) {
            double s = 0;
            for (size_t t = a; t < kz; ++t) s += Rci[a * kz + t] * qtd[t];
            (*delta)[a] = -s;
        }
        return all_finite(*delta);
    };
    auto fit = [&](const std::vector<double>& Ri, std::vector<double>& b) -> bool {
        residuals(b);
        moments();
        double q = crit(Ri);
        if (!std::isfinite(q)) {
            r.err = "gmm: the moment conditions are not finite at the initial values";
            r.err_code = "bad_input";
            return false;
        }
        r.iters.push_back({q, false});
        std::vector<double> delta, btry(kz);
        bool conv = false;
        for (int it = 0; it < kMaxIter; ++it) {
            build_G(b, &G);
            if (!ls_step(Ri, &delta)) {
                r.err = "gmm: singular G'WG (the moment conditions do not identify the parameters)";
                r.err_code = "not_identified";
                return false;
            }
            double scale = 1.0, q_new = q;
            bool improved = false, backed_up = false;
            for (int h = 0; h <= kMaxHalve; ++h) {
                for (size_t j = 0; j < kz; ++j) btry[j] = b[j] + scale * delta[j];
                residuals(btry);
                moments();
                q_new = crit(Ri);
                if (std::isfinite(q_new) && q_new < q) { improved = true; break; }
                scale *= 0.5;
                backed_up = true;
            }
            if (!improved) { conv = true; break; }
            b = btry;
            const double prev = q;
            q = q_new;
            r.iters.push_back({q, backed_up});
            if (std::fabs(prev - q) <= kTolObj * (std::fabs(prev) + 1.0)) { conv = true; break; }
        }
        // Refinement: full steps while the criterion does not rise beyond rounding,
        // until the step itself vanishes (the criterion stops resolving b first).
        for (int it = 0; conv && it < 8; ++it) {
            residuals(b);
            moments();
            build_G(b, &G);
            if (!ls_step(Ri, &delta)) break;
            double step = 0;
            for (size_t j = 0; j < kz; ++j) step = std::max(step, std::fabs(delta[j]) / (std::fabs(b[j]) + 1.0));
            if (!(step > 4 * std::numeric_limits<double>::epsilon())) break;
            for (size_t j = 0; j < kz; ++j) btry[j] = b[j] + delta[j];
            residuals(btry);
            moments();
            const double q_new = crit(Ri);
            if (!(std::isfinite(q_new) && q_new <= q * (1 + 64 * std::numeric_limits<double>::epsilon()) + 1e-300)) break;
            b = btry;
            q = q_new;
            if (step < 1e-14) break;
        }
        residuals(b);
        moments();
        r.converged = conv;
        return true;
    };

    // initial weight matrix
    std::vector<double> Ri(Lz * Lz, 0.0);
    int64_t rows = 0;
    if (sp.winit == 1) {
        for (size_t a = 0; a < Lz; ++a) Ri[a * Lz + a] = 1.0;
    } else {
        const std::vector<double> B0 = build_B(sp.winit_independent ? 4 : 3, nullptr, &rows);
        if (!sqrt_inv(B0, rows, &Ri)) {
            r.err = "initial weight matrix is not positive definite; check your instruments() "
                    "specification or try another winitial() option";
            r.err_code = "not_pd";
            return r;
        }
    }
    std::vector<double> W = w_of(Ri);

    std::vector<double> b = sp.b0, Rnew;
    if (!fit(Ri, b)) return r;
    r.ic = 1;
    if (sp.estimator >= 1) {
        const int maxUpd = sp.estimator == 1 ? 1 : sp.igmm_iterate;
        for (int u = 0; u < maxUpd; ++u) {
            const std::vector<double> Bw = build_B(sp.wmatrix, sp.wclus, &rows);
            if (!sqrt_inv(Bw, rows, &Rnew)) {
                r.err = "gmm: the weight matrix is not positive definite";
                r.err_code = "not_pd";
                return r;
            }
            const std::vector<double> Wnew = w_of(Rnew);
            const std::vector<double> bprev = b;
            const double dW = mreldif(Wnew, W);
            Ri = Rnew;
            W = Wnew;
            if (!fit(Ri, b)) return r;
            ++r.ic;
            if (sp.estimator == 2 && (dW < sp.igmm_weps || mreldif(b, bprev) < sp.igmm_eps)) break;
        }
    }

    // inference at the optimum. With C = R^-T G = Qc Rc (thin QR),
    //   (G'WG)^-1 = Rc^-1 Rc^-T  and  W G (G'WG)^-1 = R^-1 Qc Rc^-T,
    // so V = Rc^-1 (Bv R^-1 Qc)'(Bv R^-1 Qc) Rc^-T / N: the ill-conditioned
    // bread is never formed and multiplied back (winitial(identity) on raw
    // scales puts cond(G'G) near 1e15).
    build_G(b, &G);
    std::vector<double> CI = whiten(Ri, G, kz);
    for (size_t a = 0; a < Lz; ++a)
        for (size_t c = 0; c < Lz; ++c) CI.push_back(a == c ? 1.0 : 0.0);  // [C | I]: I column-major
    std::vector<double> qt;  // k x L row-major: Qc'
    const std::vector<double> Rc = householder_r(CI, L, k, L, &qt);
    std::vector<double> Rci;
    if (!upper_inverse(Rc, k, &Rci)) {
        r.err = "gmm: G'WG is singular at the converged parameters";
        r.err_code = "not_identified";
        return r;
    }
    std::vector<double> Ainv(kz * kz, 0.0);  // (G'WG)^-1 = Rc^-1 Rc^-T
    for (size_t a = 0; a < kz; ++a)
        for (size_t c = 0; c < kz; ++c) {
            double s = 0;
            for (size_t t = std::max(a, c); t < kz; ++t) s += Rci[a * kz + t] * Rci[c * kz + t];
            Ainv[a * kz + c] = s;
        }
    r.Qcrit = crit(Ri);
    if (sp.estimator == 0) {
        // one-step: e(Q) and e(J) are the criterion under the unadjusted weight
        // matrix at the estimates, (sigma2 Z'Z/N)^-1 (bank 582506f3: Q .15113963306522,
        // where the robust S^-1 would give the step-2 start value .13935043).
        std::vector<double> Ru;
        const std::vector<double> Bu = build_B(1, nullptr, &rows);
        r.Q = sqrt_inv(Bu, rows, &Ru) ? crit(Ru) : r.Qcrit;
    } else {
        r.Q = r.Qcrit;
    }
    r.J = Nw * r.Q;
    const std::vector<double> Bv = build_B(sp.vce, sp.vclus, &rows);
    std::vector<double> P(Lz * kz, 0.0);  // col-major L x k: R^-1 Qc
    for (size_t j = 0; j < kz; ++j)
        for (size_t a = 0; a < Lz; ++a) {
            double s = 0;
            for (size_t t = a; t < Lz; ++t) s += Ri[a * Lz + t] * qt[j * Lz + t];
            P[j * Lz + a] = s;
        }
    std::vector<double> BT(static_cast<size_t>(rows) * kz, 0.0);  // rows x k
    for (size_t j = 0; j < kz; ++j)
        for (size_t a = 0; a < Lz; ++a) {
            const double t = P[j * Lz + a];
            if (t == 0) continue;
            const double* bc = Bv.data() + a * static_cast<size_t>(rows);
            double* oc = BT.data() + j * static_cast<size_t>(rows);
            for (int64_t i = 0; i < rows; ++i) oc[i] += t * bc[i];
        }
    std::vector<double> mid(kz * kz);
    for (size_t a = 0; a < kz; ++a)
        for (size_t c = a; c < kz; ++c)
            mid[a * kz + c] = mid[c * kz + a] =
                la::dot(BT.data() + a * static_cast<size_t>(rows), BT.data() + c * static_cast<size_t>(rows), rows);
    r.V.assign(kz * kz, 0.0);
    for (size_t a = 0; a < kz; ++a)
        for (size_t c = 0; c < kz; ++c) {
            double s = 0;
            for (size_t t = a; t < kz; ++t)
                for (size_t u = c; u < kz; ++u) s += Rci[a * kz + t] * mid[t * kz + u] * Rci[c * kz + u];
            r.V[a * kz + c] = s / Nw;
        }
    for (size_t a = 0; a < kz; ++a)
        for (size_t c = a + 1; c < kz; ++c) r.V[a * kz + c] = r.V[c * kz + a] = 0.5 * (r.V[a * kz + c] + r.V[c * kz + a]);
    r.Vmb = Ainv;
    for (double& v : r.Vmb) v /= Nw;
    r.beta = b;
    r.S = gram(Bv, rows);
    r.W = W;
    r.G = G;
    r.ok = true;
    return r;
}

}  // namespace cecon
