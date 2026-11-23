// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// See rdlocal.h for method sources.
//
// Layout: (1) tiny dense linear algebra, (2) the shared local-polynomial core
// (kernel, Gamma/vartheta/Psi, weighted fits, the mass-point nearest-neighbor
// variance), (3) the CCFT19 mserd bandwidth chain, (4) the three drivers.

#include "estimators/rdlocal.h"

#include "core/distributions.h"
#include "core/stata_pctile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

namespace cecon {
namespace {

// ---------------------------------------------------------------- linear algebra
// Every matrix here is (p+1)x(p+1) with p <= 5, so a plain Gauss-Jordan with
// partial pivoting is both the simplest and the fastest thing available; no
// reason to reach for the project's blocked kernels.
bool mat_inverse(const std::vector<double>& A, int k, std::vector<double>& out) {
    std::vector<double> M(A);
    out.assign(static_cast<size_t>(k) * k, 0.0);
    for (int i = 0; i < k; ++i) out[static_cast<size_t>(i) * k + i] = 1.0;
    for (int col = 0; col < k; ++col) {
        int piv = col;
        double best = std::fabs(M[static_cast<size_t>(col) * k + col]);
        for (int r = col + 1; r < k; ++r) {
            double v = std::fabs(M[static_cast<size_t>(r) * k + col]);
            if (v > best) { best = v; piv = r; }
        }
        if (!(best > 0.0) || !std::isfinite(best)) return false;
        if (piv != col) {
            for (int j = 0; j < k; ++j) {
                std::swap(M[static_cast<size_t>(col) * k + j], M[static_cast<size_t>(piv) * k + j]);
                std::swap(out[static_cast<size_t>(col) * k + j], out[static_cast<size_t>(piv) * k + j]);
            }
        }
        double d = M[static_cast<size_t>(col) * k + col];
        for (int j = 0; j < k; ++j) {
            M[static_cast<size_t>(col) * k + j] /= d;
            out[static_cast<size_t>(col) * k + j] /= d;
        }
        for (int r = 0; r < k; ++r) {
            if (r == col) continue;
            double f = M[static_cast<size_t>(r) * k + col];
            if (f == 0.0) continue;
            for (int j = 0; j < k; ++j) {
                M[static_cast<size_t>(r) * k + j] -= f * M[static_cast<size_t>(col) * k + j];
                out[static_cast<size_t>(r) * k + j] -= f * out[static_cast<size_t>(col) * k + j];
            }
        }
    }
    return true;
}

// Ordinary least squares of y on (1, z, z^2, ..., z^deg) via the normal
// equations.  Used only for the GLOBAL polynomial fits (rdplot's line, the
// bandwidth chain's preliminary derivative estimates), where the design is
// well conditioned because z is already centred at the cutoff.
bool poly_ls(const std::vector<double>& z, const std::vector<double>& y, int deg,
             std::vector<double>& coef) {
    const int k = deg + 1;
    std::vector<double> A(static_cast<size_t>(k) * k, 0.0), r(static_cast<size_t>(k), 0.0);
    std::vector<double> pw(static_cast<size_t>(k));
    for (size_t i = 0; i < z.size(); ++i) {
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * z[i];
        for (int a = 0; a < k; ++a) {
            r[static_cast<size_t>(a)] += pw[static_cast<size_t>(a)] * y[i];
            for (int b = 0; b < k; ++b)
                A[static_cast<size_t>(a) * k + b] += pw[static_cast<size_t>(a)] * pw[static_cast<size_t>(b)];
        }
    }
    std::vector<double> Ai;
    if (!mat_inverse(A, k, Ai)) return false;
    coef.assign(static_cast<size_t>(k), 0.0);
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b) coef[static_cast<size_t>(a)] += Ai[static_cast<size_t>(a) * k + b] * r[static_cast<size_t>(b)];
    return true;
}

double factorial(int n) {
    double f = 1.0;
    for (int i = 2; i <= n; ++i) f *= i;
    return f;
}

// Standard normal two-sided tail, written without the catastrophic
// cancellation of 2*(1-Phi(|z|)) (same reason as cli::z_twosided_pvalue).
double z_pvalue(double z) { return std::erfc(std::fabs(z) * 0.7071067811865476); }

// Phi^{-1}((1+level/100)/2) for the handful of levels anybody asks for; a
// Beasley-Springer-Moro style rational approximation refined by one Newton
// step against erfc, which is exact to ~1e-15 over the useful range.
double normal_quantile(double p) {
    // p in (0,1).  Acklam's inverse-normal rational approximation.
    static const double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                -2.759285104469687e+02, 1.383577518672690e+02,
                                -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                -1.556989798598866e+02, 6.680131188771972e+01,
                                -1.328068155288572e+01};
    static const double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                -2.400758277161838e+00, -2.549732539343734e+00,
                                4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                                2.445134137142996e+00, 3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1.0 - plow;
    double x;
    if (p < plow) {
        double qq = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * qq + c[1]) * qq + c[2]) * qq + c[3]) * qq + c[4]) * qq + c[5]) /
            ((((d[0] * qq + d[1]) * qq + d[2]) * qq + d[3]) * qq + 1);
    } else if (p > phigh) {
        double qq = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * qq + c[1]) * qq + c[2]) * qq + c[3]) * qq + c[4]) * qq + c[5]) /
            ((((d[0] * qq + d[1]) * qq + d[2]) * qq + d[3]) * qq + 1);
    } else {
        double qq = p - 0.5, r2 = qq * qq;
        x = (((((a[0] * r2 + a[1]) * r2 + a[2]) * r2 + a[3]) * r2 + a[4]) * r2 + a[5]) * qq /
            (((((b[0] * r2 + b[1]) * r2 + b[2]) * r2 + b[3]) * r2 + b[4]) * r2 + 1);
    }
    for (int it = 0; it < 2; ++it) {  // Newton refinement on Phi(x) - p
        double e = 0.5 * std::erfc(-x * 0.7071067811865476) - p;
        double u = e * std::sqrt(2 * M_PI) * std::exp(x * x / 2);
        x -= u / (1 + x * u / 2);
    }
    return x;
}

// ---------------------------------------------------------------- kernel + side data
double kern_w(double u, RdKernel k) {
    double a = std::fabs(u);
    if (a > 1.0) return 0.0;
    return (k == RdKernel::Triangular) ? (1.0 - a) : 1.0;
}

// One side of the cutoff.  `x` holds the CENTRED running variable (x_i - c);
// on the left side these are negative, which is what CCT14's Gamma_{-,p}(h)
// wants (the sign of (x/h)^j is what makes the left-side constants carry the
// (-1)^{nu+r} factor automatically).  `n_all` is the FULL sample size, since
// every matrix below is normalised by n and not by the per-side count.
struct SideData {
    std::vector<double> x, y, s2;
    int64_t n_all = 0;
    bool empty() const { return x.empty(); }
};

// Gamma_{s,p}(h) = X_p(h)' W_s(h) X_p(h) / n, W = diag(1(side) K(x/h)/h).
std::vector<double> gamma_mat(const SideData& s, double h, int p, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> G(static_cast<size_t>(k) * k, 0.0), pw(static_cast<size_t>(k));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double u = s.x[i] / h;
        double w = kern_w(u, kk) / h;
        if (w == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * u;
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b)
                G[static_cast<size_t>(a) * k + b] += w * pw[static_cast<size_t>(a)] * pw[static_cast<size_t>(b)];
    }
    for (auto& v : G) v /= static_cast<double>(s.n_all);
    return G;
}

// vartheta_{s,p,r}(h) = X_p(h)' W_s(h) S_r(h) / n, S_r = ((x/h)^r).
std::vector<double> theta_vec(const SideData& s, double h, int p, int r, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> t(static_cast<size_t>(k), 0.0), pw(static_cast<size_t>(k));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double u = s.x[i] / h;
        double w = kern_w(u, kk) / h;
        if (w == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * u;
        double ur = std::pow(u, r);
        for (int a = 0; a < k; ++a) t[static_cast<size_t>(a)] += w * pw[static_cast<size_t>(a)] * ur;
    }
    for (auto& v : t) v /= static_cast<double>(s.n_all);
    return t;
}

// Psi_{s,p,q}(h,b) = X_p(h)' W_s(h) Sigma W_s(b) X_q(b) / n, with Sigma the
// diagonal of nearest-neighbor residual variances (CCT14 Section 5).
std::vector<double> psi_mat(const SideData& s, double h, double b, int p, int q, RdKernel kk) {
    const int kp = p + 1, kq = q + 1;
    std::vector<double> P(static_cast<size_t>(kp) * kq, 0.0);
    std::vector<double> ph(static_cast<size_t>(kp)), pb(static_cast<size_t>(kq));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double uh = s.x[i] / h, ub = s.x[i] / b;
        double wh = kern_w(uh, kk) / h, wb = kern_w(ub, kk) / b;
        if (wh == 0.0 || wb == 0.0) continue;
        ph[0] = 1.0;
        for (int j = 1; j < kp; ++j) ph[static_cast<size_t>(j)] = ph[static_cast<size_t>(j - 1)] * uh;
        pb[0] = 1.0;
        for (int j = 1; j < kq; ++j) pb[static_cast<size_t>(j)] = pb[static_cast<size_t>(j - 1)] * ub;
        double c = wh * wb * s.s2[i];
        for (int a = 0; a < kp; ++a)
            for (int bb = 0; bb < kq; ++bb)
                P[static_cast<size_t>(a) * kq + bb] += c * ph[static_cast<size_t>(a)] * pb[static_cast<size_t>(bb)];
    }
    for (auto& v : P) v /= static_cast<double>(s.n_all);
    return P;
}

// beta_{s,p}(h): the weighted local polynomial coefficients in RAW x units, so
// beta[j] estimates mu^{(j)}(c)/j! on that side.
bool beta_fit(const SideData& s, double h, int p, RdKernel kk, std::vector<double>& out) {
    const int k = p + 1;
    std::vector<double> G = gamma_mat(s, h, p, kk), rhs(static_cast<size_t>(k), 0.0);
    std::vector<double> pw(static_cast<size_t>(k));
    for (size_t i = 0; i < s.x.size(); ++i) {
        double u = s.x[i] / h;
        double w = kern_w(u, kk) / h;
        if (w == 0.0) continue;
        pw[0] = 1.0;
        for (int j = 1; j < k; ++j) pw[static_cast<size_t>(j)] = pw[static_cast<size_t>(j - 1)] * u;
        for (int a = 0; a < k; ++a) rhs[static_cast<size_t>(a)] += w * pw[static_cast<size_t>(a)] * s.y[i];
    }
    for (auto& v : rhs) v /= static_cast<double>(s.n_all);
    std::vector<double> Gi;
    if (!mat_inverse(G, k, Gi)) return false;
    out.assign(static_cast<size_t>(k), 0.0);
    for (int a = 0; a < k; ++a) {
        double acc = 0.0;
        for (int b = 0; b < k; ++b) acc += Gi[static_cast<size_t>(a) * k + b] * rhs[static_cast<size_t>(b)];
        out[static_cast<size_t>(a)] = acc * std::pow(h, -a);  // H_p(h) = diag(1, h^-1, ...)
    }
    return true;
}

// V_{s,nu,p}(h) = n^-1 h^-2nu nu!^2 e_nu' G^-1 Psi G^-1 e_nu.  This IS the
// exact finite-sample variance of nu! e_nu' beta_{s,p}(h) under Sigma.
double v_nu(const SideData& s, double h, int nu, int p, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> G = gamma_mat(s, h, p, kk), Gi;
    if (!mat_inverse(G, k, Gi)) return std::nan("");
    std::vector<double> P = psi_mat(s, h, h, p, p, kk);
    // row = e_nu' G^-1
    std::vector<double> row(static_cast<size_t>(k), 0.0);
    for (int b = 0; b < k; ++b) row[static_cast<size_t>(b)] = Gi[static_cast<size_t>(nu) * k + b];
    double acc = 0.0;
    for (int a = 0; a < k; ++a)
        for (int b = 0; b < k; ++b)
            acc += row[static_cast<size_t>(a)] * P[static_cast<size_t>(a) * k + b] * row[static_cast<size_t>(b)];
    double f = factorial(nu);
    return f * f * acc / (static_cast<double>(s.n_all) * std::pow(h, 2 * nu));
}

// O_{s,nu,p}(c) = nu! e_nu' Gamma_{s,p}(c)^-1 vartheta_{s,p,p+1}(c)
// (CCFT19 supplement Section 12: the pre-asymptotic bias constant, evaluated
// at the preliminary bandwidth rather than at the population kernel integrals
// CCT14 used).
double o_const(const SideData& s, double c, int nu, int p, RdKernel kk) {
    const int k = p + 1;
    std::vector<double> G = gamma_mat(s, c, p, kk), Gi;
    if (!mat_inverse(G, k, Gi)) return std::nan("");
    std::vector<double> t = theta_vec(s, c, p, p + 1, kk);
    double acc = 0.0;
    for (int b = 0; b < k; ++b) acc += Gi[static_cast<size_t>(nu) * k + b] * t[static_cast<size_t>(b)];
    return factorial(nu) * acc;
}

// ---------------------------------------------------------------- NN variance
// CCT14 Section 5's fixed-J nearest-neighbor variance, with the mass-point
// adjustment described in rdlocal.h: grow the neighbour set by whole ties
// until it holds at least J observations.  With no ties the first J singleton
// values are taken and this is exactly the Abadie-Imbens estimator; with ties
// the observation's OWN mass point already supplies >= J neighbours, so the
// estimate becomes the within-tie dispersion and no arbitrary tie-break is
// needed.  Neighbours never cross the cutoff (they are drawn from `s` only).
void nn_sigma2(SideData& s, int J) {
    const size_t m = s.x.size();
    s.s2.assign(m, 0.0);
    if (m == 0) return;
    std::vector<size_t> ord(m);
    std::iota(ord.begin(), ord.end(), size_t{0});
    std::stable_sort(ord.begin(), ord.end(),
                     [&](size_t a, size_t b) { return s.x[a] < s.x[b]; });
    // distinct value groups, in ascending x
    std::vector<double> val;
    std::vector<double> gsum;
    std::vector<double> gcnt;
    std::vector<size_t> grp(m);  // group index per ORIGINAL row
    for (size_t k = 0; k < m; ++k) {
        size_t i = ord[k];
        if (val.empty() || s.x[i] != val.back()) {
            val.push_back(s.x[i]);
            gsum.push_back(0.0);
            gcnt.push_back(0.0);
        }
        grp[i] = val.size() - 1;
        gsum.back() += s.y[i];
        gcnt.back() += 1.0;
    }
    const size_t nv = val.size();
    // per-group neighbour aggregate (sum and count INCLUDING the group itself)
    std::vector<double> nbsum(nv, 0.0), nbcnt(nv, 0.0);
    for (size_t v = 0; v < nv; ++v) {
        double sm = gsum[v], ct = gcnt[v];
        // the observation itself is removed later, so it needs ct-1 partners
        size_t lo = v, hi = v;
        while (ct - 1.0 < static_cast<double>(J)) {
            bool has_lo = lo > 0, has_hi = hi + 1 < nv;
            if (!has_lo && !has_hi) break;
            double dl = has_lo ? std::fabs(val[lo - 1] - val[v]) : 0.0;
            double dr = has_hi ? std::fabs(val[hi + 1] - val[v]) : 0.0;
            // Equidistant neighbours on both sides enter together (rdrobust's
            // nearest-neighbour residual grows both ends on a distance tie).
            if (has_lo && has_hi && dl == dr) {
                --lo; sm += gsum[lo]; ct += gcnt[lo];
                ++hi; sm += gsum[hi]; ct += gcnt[hi];
                continue;
            }
            bool take_lo = has_lo && (!has_hi || dl < dr);
            if (take_lo) { --lo; sm += gsum[lo]; ct += gcnt[lo]; }
            else { ++hi; sm += gsum[hi]; ct += gcnt[hi]; }
        }
        nbsum[v] = sm;
        nbcnt[v] = ct;
    }
    for (size_t i = 0; i < m; ++i) {
        size_t v = grp[i];
        double tot = nbsum[v] - s.y[i], cnt = nbcnt[v] - 1.0;
        if (cnt <= 0.0) { s.s2[i] = 0.0; continue; }
        double d = s.y[i] - tot / cnt;
        s.s2[i] = cnt / (cnt + 1.0) * d * d;
    }
}

// ---------------------------------------------------------------- misc helpers
int64_t count_within(const SideData& s, double h) {
    int64_t c = 0;
    for (double v : s.x) if (std::fabs(v) <= h) ++c;
    return c;
}
// rdrobust's "Unique obs" row counts the DISTINCT running values on each side
// of the cutoff over the whole side, not just inside the bandwidth: it is a
// property of the design (how coarse the running variable is), which is why it
// sits next to the raw per-side N rather than under Eff. Number of obs.
int64_t count_unique(const SideData& s) {
    std::vector<double> v(s.x);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return static_cast<int64_t>(v.size());
}
bool has_ties(const std::vector<double>& x) {
    std::vector<double> v(x);
    std::sort(v.begin(), v.end());
    return std::unique(v.begin(), v.end()) != v.end();
}
// ---------------------------------------------------------------- bandwidth chain

double side_max_abs(const SideData& s) {
    double m = 0.0;
    for (double v : s.x) m = std::max(m, std::fabs(v));
    return m;
}

SideData positive_window(const SideData& side, double bandwidth, RdKernel kernel, int nnmatch) {
    SideData out;
    out.n_all = side.n_all;
    for (size_t i = 0; i < side.x.size(); ++i) {
        if (kern_w(side.x[i] / bandwidth, kernel) <= 0.0) continue;
        out.x.push_back(side.x[i]);
        out.y.push_back(side.y[i]);
    }
    nn_sigma2(out, nnmatch);
    return out;
}

struct BwComponents {
    double variance = 0.0;
    double bias = 0.0;
    double regularization = 0.0;
};

// The CCFT19 selector uses the same preliminary window for the variance and
// kernel moment in every stage. The bias fit uses the previous stage's window.
BwComponents bw_components(const SideData& side, const SideData& variance_side,
                           double v, double bias_h, int order, int derivative,
                           int bias_order, RdKernel kernel, int nnmatch, bool regularize) {
    const double moment = o_const(variance_side, v, derivative, order, kernel) / factorial(derivative);
    const double variance = v_nu(variance_side, v, derivative, order, kernel) /
        std::pow(factorial(derivative), 2);
    const auto bias_side = positive_window(side, bias_h, kernel, nnmatch);
    std::vector<double> beta;
    if (!beta_fit(bias_side, bias_h, bias_order, kernel, beta))
        return {std::nan(""), std::nan(""), std::nan("")};
    const double scale = 2 * (order + 1 - derivative);
    double reg = 0.0;
    if (regularize) {
        const double bias_var = v_nu(bias_side, bias_h, order + 1, bias_order, kernel) /
            std::pow(factorial(order + 1), 2);
        reg = 3 * scale * moment * moment * bias_var;
    }
    return {(2 * derivative + 1) * std::pow(v, 2 * derivative + 1) * variance,
            std::sqrt(scale) * moment * beta[order + 1], reg};
}

struct BwChain {
    double b = 0.0;
    double h = 0.0;
    bool ok = false;
};

BwChain select_bandwidths(const SideData& left, const SideData& right,
                          const std::vector<double>& xall, int p, int q, int nu,
                          RdKernel kernel, int nnmatch) {
    BwChain out;
    const double n = static_cast<double>(xall.size());
    if (n < 10.0) return out;
    const double mean = std::accumulate(xall.begin(), xall.end(), 0.0) / n;
    double ss = 0.0;
    for (double value : xall) ss += (value - mean) * (value - mean);
    const double sd = std::sqrt(ss / (n - 1.0));
    std::vector<double> sorted(xall);
    std::sort(sorted.begin(), sorted.end());
    const double iqr = stata_pctile_sorted(sorted, 75) - stata_pctile_sorted(sorted, 25);
    const double base = std::min(sd, iqr / 1.349);
    if (!(base > 0.0)) return out;
    auto distances = [](const SideData& side) {
        std::vector<double> unique;
        for (double x : side.x) unique.push_back(std::fabs(x));
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        return unique;
    };
    const auto dl = distances(left), dr = distances(right);
    const double cap = std::max(dl.back(), dr.back());
    const bool concentrated = dl.size() <= 0.8 * left.x.size() || dr.size() <= 0.8 * right.x.size();
    const double floor = concentrated
        ? std::max(dl[std::min(size_t{10}, dl.size()) - 1], dr[std::min(size_t{10}, dr.size()) - 1])
        : 0.0;
    const double ck = kernel == RdKernel::Triangular ? 2.576 : 1.843;
    const double v = std::clamp(ck * base * std::pow(dl.size() + dr.size(), -0.2), floor, cap);
    const auto vl = positive_window(left, v, kernel, nnmatch);
    const auto vr = positive_window(right, v, kernel, nnmatch);
    auto select = [&](int order, int derivative, int bias_order, double hl, double hr, bool regularize) {
        const auto l = bw_components(left, vl, v, hl, order, derivative, bias_order, kernel, nnmatch, regularize);
        const auto r = bw_components(right, vr, v, hr, order, derivative, bias_order, kernel, nnmatch, regularize);
        const double bias = r.bias - l.bias;
        const double ratio = (l.variance + r.variance) / (bias * bias + l.regularization + r.regularization);
        return ratio > 0 ? std::min(cap, std::pow(ratio, 1.0 / (2 * order + 3))) : 0.0;
    };
    const double d = std::max(floor, select(q + 1, q + 1, q + 2, dl.back(), dr.back(), false));
    if (!(d > 0.0)) return out;
    out.b = select(q, p + 1, q + 1, d, d, true);
    if (!(out.b > 0.0)) return out;
    out.h = select(p, nu, q, out.b, out.b, true);
    out.ok = out.h > 0.0;
    return out;
}

// ---------------------------------------------------------------- sample prep
struct Split {
    SideData L, R;
    std::vector<double> xall;
    int64_t n = 0;
    bool ties = false;
};

Split split_sample(const double* y, const double* x, int64_t n, double c) {
    Split s;
    std::vector<double> xs, ys;
    for (int64_t i = 0; i < n; ++i) {
        double xi = x[i];
        if (!std::isfinite(xi)) continue;
        double yi = y ? y[i] : 0.0;
        if (y && !std::isfinite(yi)) continue;
        xs.push_back(xi - c);
        ys.push_back(yi);
    }
    s.n = static_cast<int64_t>(xs.size());
    s.xall = xs;
    s.ties = has_ties(xs);
    s.L.n_all = s.R.n_all = s.n;
    for (size_t i = 0; i < xs.size(); ++i) {
        SideData& d = (xs[i] < 0.0) ? s.L : s.R;
        d.x.push_back(xs[i]);
        d.y.push_back(ys[i]);
    }
    return s;
}

}  // namespace

// ================================================================= rdrobust
RdRobustResult rd_robust_run(const double* y, const double* x, int64_t n,
                              const RdLocalConfig& cfg) {
    RdRobustResult R;
    R.p = cfg.p;
    R.q = cfg.q;
    R.kernel = (cfg.kernel == RdKernel::Triangular) ? "Triangular" : "Uniform";
    R.vce = "NN";
    if (cfg.masspoints != RdMasspoints::Adjust) {
        R.ok = false;
        R.error = "rdrobust: only masspoints(adjust) is implemented; "
                  "nomasspoints / masspoints(check) are not supported";
        return R;
    }
    if (cfg.vce != "nn") {
        R.ok = false;
        R.error = "rdrobust: only vce(nn) is implemented; vce(" + cfg.vce +
                  ") is not supported";
        return R;
    }
    if (cfg.p < 1 || cfg.q <= cfg.p || cfg.q > 5) {
        R.ok = false;
        R.error = "rdrobust: need 1 <= p < q <= 5";
        return R;
    }
    if (!y || !x || n <= 0) {
        R.ok = false;
        R.error = "rdrobust: no data";
        return R;
    }
    Split S = split_sample(y, x, n, cfg.c);
    R.n_obs = S.n;
    R.n_left = static_cast<int64_t>(S.L.x.size());
    R.n_right = static_cast<int64_t>(S.R.x.size());
    R.masspoints_detected = S.ties;
    if (R.n_left < cfg.q + 2 || R.n_right < cfg.q + 2) {
        R.ok = false;
        R.error = "rdrobust: too few observations on one side of the cutoff";
        return R;
    }
    const int p = cfg.p, q = cfg.q, nu = 0;
    const RdKernel kk = cfg.kernel;
    double h = cfg.h_left, b = cfg.b_left;
    if (h > 0 || b > 0) {
        R.bw_type = "manual";
        if (!(h > 0)) h = b;
        if (!(b > 0)) b = h;
    } else {
        BwChain bw = select_bandwidths(S.L, S.R, S.xall, p, q, nu, kk, cfg.nnmatch);
        if (!bw.ok) {
            R.ok = false;
            R.error = "rdrobust: bandwidth selection failed (degenerate design)";
            return R;
        }
        h = bw.h;
        b = bw.b;
    }
    R.h_left = R.h_right = h;
    R.b_left = R.b_right = b;
    R.rho = h / b;
    R.n_unique_left = count_unique(S.L);
    R.n_unique_right = count_unique(S.R);
    // The estimation sample of each side is the observations with a POSITIVE kernel
    // weight under the wider of h and b (rdrobust: the w > 0 index sets), and the
    // nearest-neighbour residuals are matched within that sample, not over the
    // whole side. Eff. Number of obs counts the positive weights under h, so a
    // triangular kernel leaves out the points exactly at |x - c| = h (bank e2e101d7
    // `rdrobust wage x, h(3)`: N_h_l 408 / N_h_r 560 where |x| <= 3 holds 409 / 563).
    {
        const double hw = std::max(h, b);
        S.L = positive_window(S.L, hw, kk, cfg.nnmatch);
        S.R = positive_window(S.R, hw, kk, cfg.nnmatch);
        auto positive = [&](const SideData& s) {
            int64_t c = 0;
            for (double v : s.x)
                if (kern_w(v / h, kk) > 0.0) ++c;
            return c;
        };
        R.n_eff_left = positive(S.L);
        R.n_eff_right = positive(S.R);
    }

    // Point estimate, bias estimate and the three variances.  CCT14 Theorem A.1:
    //   tau_bc  = tau_p(h) - h^{p+1} [ beta_{+,q}(b)[p+1] B_+ - beta_{-,q}(b)[p+1] B_- ]
    //   V_bc_s  = V_{s,nu,p}(h) - 2 h^{p+1-nu} C_s B_s/(p+1)!
    //                            + h^{2(p+1-nu)} V_{s,p+1,q}(b) B_s^2/(p+1)!^2
    double tau = 0.0, bias = 0.0, Vconv = 0.0, Vrb = 0.0;
    for (int side = 0; side < 2; ++side) {
        const SideData& s = side ? S.R : S.L;
        const double sgn = side ? 1.0 : -1.0;
        std::vector<double> bp, bq;
        if (!beta_fit(s, h, p, kk, bp) || !beta_fit(s, b, q, kk, bq)) {
            R.ok = false;
            R.error = "rdrobust: singular local-polynomial design (bandwidth too small?)";
            return R;
        }
        tau += sgn * bp[0];
        // B_{s,nu,p,p+1}(h) = nu! e_nu' Gamma_{s,p}(h)^-1 vartheta_{s,p,p+1}(h)
        double Bc = o_const(s, h, nu, p, kk);
        bias += sgn * bq[static_cast<size_t>(p + 1)] * Bc;

        double Vp = v_nu(s, h, nu, p, kk);
        double Vpq = v_nu(s, b, p + 1, q, kk);
        // C_{s,nu,p,q}(h,b) = n^-1 h^-nu b^-(p+1) nu!(p+1)! e_nu'G_p(h)^-1 Psi(h,b) G_q(b)^-1 e_{p+1}
        std::vector<double> Gp = gamma_mat(s, h, p, kk), Gq = gamma_mat(s, b, q, kk);
        std::vector<double> Gpi, Gqi;
        if (!mat_inverse(Gp, p + 1, Gpi) || !mat_inverse(Gq, q + 1, Gqi)) {
            R.ok = false;
            R.error = "rdrobust: singular local-polynomial design";
            return R;
        }
        std::vector<double> Psi = psi_mat(s, h, b, p, q, kk);
        double Cs = 0.0;
        for (int a = 0; a <= p; ++a)
            for (int bb = 0; bb <= q; ++bb)
                Cs += Gpi[static_cast<size_t>(nu) * (p + 1) + a] *
                      Psi[static_cast<size_t>(a) * (q + 1) + bb] *
                      Gqi[static_cast<size_t>(bb) * (q + 1) + (p + 1)];
        Cs *= factorial(nu) * factorial(p + 1) /
              (static_cast<double>(s.n_all) * std::pow(h, nu) * std::pow(b, p + 1));

        double fp = factorial(p + 1);
        Vconv += Vp;
        Vrb += Vp - 2.0 * std::pow(h, p + 1 - nu) * Cs * Bc / fp +
               std::pow(h, 2 * (p + 1 - nu)) * Vpq * Bc * Bc / (fp * fp);
    }
    double tau_bc = tau - std::pow(h, p + 1) * bias;
    if (!(Vconv > 0.0) || !(Vrb > 0.0) || !std::isfinite(tau)) {
        R.ok = false;
        R.error = "rdrobust: non-positive variance estimate";
        return R;
    }
    const double zq = normal_quantile(0.5 + cfg.level / 200.0);
    double se_c = std::sqrt(Vconv), se_r = std::sqrt(Vrb);

    R.coef_conventional = tau;
    R.se_conventional = se_c;
    R.z_conventional = tau / se_c;
    R.p_conventional = z_pvalue(R.z_conventional);
    R.ci_conventional_lo = tau - zq * se_c;
    R.ci_conventional_hi = tau + zq * se_c;

    R.coef_bc = tau_bc;
    R.se_bc = se_c;  // bias-corrected point estimate, conventional variance
    R.z_bc = tau_bc / se_c;
    R.p_bc = z_pvalue(R.z_bc);
    R.ci_bc_lo = tau_bc - zq * se_c;
    R.ci_bc_hi = tau_bc + zq * se_c;

    R.coef_robust = tau_bc;
    R.se_robust = se_r;
    R.z_robust = tau_bc / se_r;
    R.p_robust = z_pvalue(R.z_robust);
    R.ci_robust_lo = tau_bc - zq * se_r;
    R.ci_robust_hi = tau_bc + zq * se_r;
    return R;
}

// ================================================================= rddensity
namespace {

// Unrestricted CJM density fit on one side. See rddensity_fun.do in the
// authors' Stata package and rdbwdensity.ado for the selector below (v3.0).
// Sorted ties share the empirical CDF at the last index of each mass point.
// Only the CDF is computed on the full sample; the jackknife uses observations
// in the local window and divides by the full N-1.
struct DensityFit {
    double coefficient = 0.0;
    double variance = 0.0;
    bool ok = false;
};

DensityFit density_fit(const std::vector<double>& x, size_t first, size_t last,
                       double h, int order, int derivative, RdKernel kernel,
                       bool jackknife) {
    DensityFit out;
    if (!(h > 0.0) || first == last) return out;
    const int k = order + 1;
    struct Point { double u; double weight; double cdf; size_t count; };
    std::vector<Point> points;
    for (size_t i = first; i < last;) {
        size_t end = i + 1;
        while (end < last && x[i] == x[end]) ++end;
        const double u = x[i] / h;
        const double weight = kern_w(u, kernel);
        if (weight > 0.0)
            points.push_back({u, weight, static_cast<double>(end - 1) /
                                           static_cast<double>(x.size() - 1), end - i});
        i = end;
    }
    if (points.size() < static_cast<size_t>(k)) return out;

    std::vector<double> gram(static_cast<size_t>(k) * k, 0.0), rhs(k, 0.0);
    std::vector<double> powers(2 * order + 1);
    for (const auto& point : points) {
        powers[0] = 1.0;
        for (int j = 1; j <= 2 * order; ++j) powers[j] = powers[j - 1] * point.u;
        const double weight = point.count * point.weight;
        for (int j = 0; j < k; ++j) {
            rhs[j] += weight * powers[j] * point.cdf;
            for (int l = 0; l < k; ++l) gram[j * k + l] += weight * powers[j + l];
        }
    }
    std::vector<double> inverse;
    if (!mat_inverse(gram, k, inverse)) return out;
    const double* row = inverse.data() + derivative * k;
    const double scale = std::pow(h, derivative);
    out.coefficient = std::inner_product(rhs.begin(), rhs.end(), row, 0.0) / scale;
    if (jackknife) {
        double prefix = 0.0, sum_square = 0.0;
        for (const auto& point : points) {
            double power = 1.0, load = row[0];
            for (int j = 1; j < k; ++j) {
                power *= point.u;
                load += row[j] * power;
            }
            load *= point.weight;
            // Deleting one observation changes the CDF at its own mass point
            // by one count and at all subsequent mass points by the shared
            // prefix adjustment. All observations tied here share this load.
            const double influence = prefix + load;
            sum_square += point.count * influence * influence;
            prefix += point.count * load;
        }
        const double normalization = (x.size() - 1) * scale;
        out.variance = sum_square / (normalization * normalization);
    }
    out.ok = std::isfinite(out.coefficient) && std::isfinite(out.variance);
    return out;
}

double hermite(double x, int order) {
    double previous = 0.0, value = 1.0;
    for (int k = 0; k < order; ++k) {
        const double next = x * value - k * previous;
        previous = value;
        value = next;
    }
    return value;
}

// For the unrestricted fit the coefficient on u in the weighted projection
// of u^(p+1) supplies the bias in the derivative at the cutoff. Integrate
// the kernel moments analytically instead of approximating the integrals.
double density_bias_constant(int p, RdKernel kernel) {
    const int k = p + 1;
    auto moment = [kernel](int j) {
        if (kernel == RdKernel::Uniform) return 1.0 / (j + 1);
        return 1.0 / ((j + 1) * (j + 2));
    };
    std::vector<double> gram(k * k), rhs(k), inverse;
    for (int j = 0; j < k; ++j) {
        rhs[j] = moment(j + p + 1);
        for (int l = 0; l < k; ++l) gram[j * k + l] = moment(j + l);
    }
    if (!mat_inverse(gram, k, inverse)) return 0.0;
    return std::fabs(std::inner_product(rhs.begin(), rhs.end(), inverse.data() + k, 0.0));
}

double order_statistic(const std::vector<double>& values, size_t one_based) {
    return values[std::min(one_based, values.size()) - 1];
}

std::pair<double, double> density_bandwidths(const std::vector<double>& z, size_t split,
                                             int p, RdKernel kernel) {
    // Gaussian-reference constants for the two pilot fits, rdbwdensity v3.0.
    // Supported p is 1..4 because the final bias fit has p < q <= 5.
    constexpr double c_ref[] = {4.800000000000025, 548.5714285715546,
                                100800.00000020421, 29558225.45810061};
    constexpr double b_ref[] = {25884.44444449415, 3430865.455123618,
                                845007948.042626, 330631733667.0381};
    std::vector<double> left, right(z.begin() + split, z.end());
    left.reserve(split);
    for (size_t j = split; j > 0; --j) left.push_back(-z[j - 1]);
    auto unique = [](const std::vector<double>& v) {
        auto out = v;
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    };
    const auto left_unique = unique(left), right_unique = unique(right);
    const double cap_left = left.back(), cap_right = right.back();
    const double cap = std::max(cap_left, cap_right);
    const double mean = std::accumulate(z.begin(), z.end(), 0.0) / z.size();
    double ss = 0.0;
    for (double value : z) ss += (value - mean) * (value - mean);
    const double sd = std::sqrt(ss / (z.size() - 1));
    const double normal_z = -mean / sd;
    const double phi = std::exp(-normal_z * normal_z / 2) / std::sqrt(2 * M_PI);

    auto floor_side = [](const auto& raw, const auto& distinct, int order) {
        const size_t minimum = 20 + order + 1;
        return std::max(order_statistic(raw, minimum), order_statistic(distinct, minimum));
    };
    auto pilot = [&](int order, int power, double multiplier) {
        const double hpoly = hermite(normal_z, order);
        const double denominator = phi * hpoly * hpoly * z.size();
        const double unconstrained = denominator > 0.0
            ? std::pow(std::pow(sd, power) * multiplier / denominator, 1.0 / power)
            : cap;
        return std::max({std::min(unconstrained, cap),
                         floor_side(left, left_unique, order),
                         floor_side(right, right_unique, order)});
    };
    const double b = pilot(p + 2, 2 * p + 5, (2 * p + 1) * b_ref[p - 1] / 4);
    const double c = pilot(p, 2 * p + 1, c_ref[p - 1] / (2 * p));
    const auto bias_left = density_fit(z, 0, split, b, p + 2, p + 1, kernel, false);
    const auto bias_right = density_fit(z, split, z.size(), b, p + 2, p + 1, kernel, false);
    const auto variance_left = density_fit(z, 0, split, c, p, 1, kernel, true);
    const auto variance_right = density_fit(z, split, z.size(), c, p, 1, kernel, true);

    const double min_left = floor_side(left, left_unique, p);
    const double min_right = floor_side(right, right_unique, p);
    if (!bias_left.ok || !bias_right.ok || !variance_left.ok || !variance_right.ok) {
        // If a pilot is unidentified, only use the smallest regularized
        // window; the caller still checks identification of the final fit.
        return {min_left, min_right};
    }
    const double bias_const = density_bias_constant(p, kernel);
    const double bl = bias_const * bias_left.coefficient * (p % 2 ? -1.0 : 1.0);
    const double br = bias_const * bias_right.coefficient;
    const double vl = variance_left.variance, vr = variance_right.variance;
    auto select = [p, c](double bias, double variance, double hi, double lo) {
        double bandwidth = hi;
        if (bias != 0.0)
            bandwidth = std::pow(c * variance / (2 * p * bias * bias), 1.0 / (2 * p + 1));
        return std::max(lo, std::min(bandwidth, hi));
    };
    const double common_min = std::max(min_left, min_right);
    const double diff = select(br - bl, vl + vr, cap, common_min);
    const double sum = select(br + bl, vl + vr, cap, common_min);
    auto median = [diff, sum](double value) {
        return std::clamp(value, std::min(diff, sum), std::max(diff, sum));
    };
    return {median(select(bl, vl, cap_left, min_left)),
            median(select(br, vr, cap_right, min_right))};
}

}  // namespace

RdDensityResult rd_density_run(const double* x, int64_t n, const RdLocalConfig& cfg) {
    RdDensityResult R;
    // rddensity's own defaults are p=2, q=3 — different from rdrobust's.
    // BUG-186: a (cfg.p==1 && cfg.q==2) sentinel can't distinguish "the
    // caller asked for p=1" from "the caller asked for nothing", so use the
    // explicit has_p/has_q flags instead. q's default follows rddensity's
    // own convention (q = p + 1) off of whichever p is actually in force.
    int p = cfg.has_p ? cfg.p : 2;
    int q = cfg.has_q ? cfg.q : (p + 1);
    R.p = p;
    R.q = q;
    R.kernel = (cfg.kernel == RdKernel::Triangular) ? "triangular" : "uniform";
    if (cfg.masspoints != RdMasspoints::Adjust) {
        R.ok = false;
        R.error = "rddensity: only masspoints(adjust) is implemented; "
                  "nomasspoints is not supported";
        return R;
    }
    if (p < 1 || q <= p || q > 5) {
        R.ok = false;
        R.error = "rddensity: need 1 <= p < q <= 5";
        return R;
    }
    if (!x || n <= 0) {
        R.ok = false;
        R.error = "rddensity: no data";
        return R;
    }
    std::vector<double> z;
    for (int64_t i = 0; i < n; ++i)
        if (std::isfinite(x[i])) z.push_back(x[i] - cfg.c);
    R.n_obs = static_cast<int64_t>(z.size());
    for (double v : z) (v < 0.0 ? R.n_left : R.n_right) += 1;
    R.masspoints_detected = has_ties(z);
    if (R.n_left < q + 2 || R.n_right < q + 2) {
        R.ok = false;
        R.error = "rddensity: too few observations on one side of the cutoff";
        return R;
    }
    std::sort(z.begin(), z.end());
    const size_t split = static_cast<size_t>(R.n_left);
    if (cfg.h_left > 0) {
        R.bw_method = "manual";
        R.h_left = cfg.h_left;
        R.h_right = cfg.h_right > 0 ? cfg.h_right : cfg.h_left;
    } else {
        const auto bandwidths = density_bandwidths(z, split, p, cfg.kernel);
        R.h_left = bandwidths.first;
        R.h_right = bandwidths.second;
    }
    for (double v : z) {
        if (v < 0.0 && v >= -R.h_left) ++R.n_eff_left;
        if (v >= 0.0 && v <= R.h_right) ++R.n_eff_right;
    }

    auto run_order = [&](int order, double& fL, double& fR, double& se, double& t, double& pv) {
        const auto left = density_fit(z, 0, split, R.h_left, order, 1, cfg.kernel, true);
        const auto right = density_fit(z, split, z.size(), R.h_right, order, 1, cfg.kernel, true);
        const double variance = left.variance + right.variance;
        if (!left.ok || !right.ok || !(variance > 0.0)) return false;
        fL = left.coefficient;
        fR = right.coefficient;
        if (order == q) {
            R.se_left_q = std::sqrt(left.variance);
            R.se_right_q = std::sqrt(right.variance);
        }
        se = std::sqrt(variance);
        t = (fR - fL) / se;
        pv = z_pvalue(t);
        return true;
    };
    if (!run_order(p, R.f_left, R.f_right, R.se_conventional, R.t_conventional, R.p_conventional) ||
        !run_order(q, R.f_left_q, R.f_right_q, R.se_robust, R.t_robust, R.p_robust)) {
        R.ok = false;
        R.error = "rddensity: singular local-polynomial design";
        return R;
    }
    R.diff = R.f_right - R.f_left;
    R.diff_q = R.f_right_q - R.f_left_q;

    // Start with enough observations on each side, then extend each window
    // toward its estimation bandwidth in at most ten equal increments.
    constexpr int kWindows = 10;
    constexpr int kInitialCount = 20;
    std::vector<double> left;
    left.reserve(split);
    for (size_t j = split; j > 0; --j) left.push_back(-z[j - 1]);
    const std::vector<double> right(z.begin() + split, z.end());
    const double first = std::max(order_statistic(left, kInitialCount),
                                  order_statistic(right, kInitialCount));
    const int windows = first >= std::min(R.h_left, R.h_right) ? 1 : kWindows;
    const double end_left = std::min(first * kWindows, R.h_left);
    const double end_right = std::min(first * kWindows, R.h_right);
    for (int j = 0; j < windows; ++j) {
        RdDensityBinomRow row;
        auto window = [j, first](double end) {
            return j == kWindows - 1 ? end : first + j * ((end - first) / (kWindows - 1));
        };
        row.window = window(end_left);
        row.window_right = window(end_right);
        row.n_left = std::upper_bound(left.begin(), left.end(), row.window) - left.begin();
        row.n_right = std::upper_bound(right.begin(), right.end(), row.window_right) - right.begin();
        row.p_lower = row.n_right == 0 ? 1.0 : betai(row.n_right, row.n_left + 1, 0.5);
        row.p_upper = row.n_left == 0 ? 1.0 : betai(row.n_left, row.n_right + 1, 0.5);
        row.p_value = std::min(1.0, 2 * std::min(row.p_lower, row.p_upper));
        R.binomial.push_back(row);
    }
    return R;
}

// ================================================================= rdplot
RdPlotResult rd_plot_run(const double* y, const double* x, int64_t n,
                          const RdLocalConfig& cfg) {
    RdPlotResult R;
    // rdplot's own default fit order is 4, not rdrobust's 1.  BUG-186: a
    // (cfg.p==1) sentinel can't distinguish "the caller asked for p=1" from
    // "the caller asked for nothing", so use the explicit has_p flag instead.
    const int pfit = cfg.has_p ? cfg.p : 4;
    R.poly_order_left = R.poly_order_right = pfit;
    R.kernel = (cfg.kernel == RdKernel::Triangular) ? "Triangular" : "Uniform";
    if (cfg.nbins_left < 0 || cfg.nbins_right < 0) {
        R.ok = false;
        R.error = "rdplot: nbins must be positive";
        return R;
    }
    if (!y || !x || n <= 0) {
        R.ok = false;
        R.error = "rdplot: no data";
        return R;
    }
    Split S = split_sample(y, x, n, cfg.c);
    R.n_obs = S.n;
    R.n_left = static_cast<int64_t>(S.L.x.size());
    R.n_right = static_cast<int64_t>(S.R.x.size());
    // BUG-184: same Split, same running variable, same two helpers rd_robust_run
    // uses at :594/:625 — set before the too-few-obs early return below so the
    // design facts are reported even when the fit itself cannot run.
    R.masspoints_detected = S.ties;
    R.n_unique_left = count_unique(S.L);
    R.n_unique_right = count_unique(S.R);
    if (R.n_left < pfit + 2 || R.n_right < pfit + 2) {
        R.ok = false;
        R.error = "rdplot: too few observations on one side of the cutoff";
        return R;
    }
    // The polynomial fit spans the whole side by default (h = max|x-c|) with a
    // uniform kernel, i.e. a global fit.
    double capL = side_max_abs(S.L), capR = side_max_abs(S.R);
    double cap = std::max(capL, capR);
    R.h_left = (cfg.h_left > 0) ? cfg.h_left : cap;
    R.h_right = (cfg.h_right > 0) ? cfg.h_right : cap;
    R.n_eff_left = count_within(S.L, R.h_left);
    R.n_eff_right = count_within(S.R, R.h_right);

    // --- bin-count selection (CCT15) ---------------------------------------
    // J_IMSE,s = ceil( (2 B_ES,s / V_ES,s)^{1/3} n^{1/3} )    [eq. (1)]
    // J_MV,s   = ceil( (V_s / V_ES,s) * n / log(n)^2 )        [eq. (3)/(14)]
    //
    // B_ES,s = (range^2 / (12 n)) * sum_{i on side s} (mu'_k(X_i))^2  [eq. (8)]
    //   NOTE the divisor: 12 times the FULL sample size n, with the sum taken
    //   over one side only.  That is what makes B a density-weighted (w = f)
    //   average rather than a plain integral, matching V_ES's own weighting.
    //   Getting this wrong is a factor of ~20 and moves the bin count.
    //
    // V_ES,s = (1/range) * sum_i (x_(i) - x_(i-1)) sigma2-hat_k(midpoint)
    //   This is CCT15 eq. (11), the series/"concomitant" form, NOT the spacings
    //   estimator of eq. (7).  Eq. (7) weights each consecutive pair by its X
    //   spacing, so under mass points every within-tie pair contributes exactly
    //   zero and the estimator collapses onto the handful of tie boundaries
    //   (49 of 2390 pairs on the oracle fixture, giving 0.362 where the true
    //   sigma^2 is ~1.01).  CCT15's own Remark 1 flags the concomitant route as
    //   invalid for non-continuous data and offers eq. (11) as the substitute;
    //   a mass-point running variable is that case, and eq. (11) is what
    //   reproduces the oracle's bin counts on BOTH sides.
    //
    // k is the plot's own polynomial order p — the selector reuses the fit the
    // plot already draws rather than introducing a second tuning parameter.
    const int kfit = pfit;
    const double N = static_cast<double>(S.n);
    auto side_bins = [&](const SideData& s, double range, int& jimse, int& jmv,
                         double& vesOut) {
        jimse = 1;
        jmv = 1;
        vesOut = 0.0;
        std::vector<double> y2(s.y.size());
        for (size_t i = 0; i < s.y.size(); ++i) y2[i] = s.y[i] * s.y[i];
        std::vector<double> c1, c2;
        if (!poly_ls(s.x, s.y, kfit, c1) || !poly_ls(s.x, y2, kfit, c2)) return;
        std::vector<double> sx(s.x);
        std::sort(sx.begin(), sx.end());
        auto evalp = [](const std::vector<double>& cc, double t) {
            double acc = 0.0, pw = 1.0;
            for (double v : cc) { acc += v * pw; pw *= t; }
            return acc;
        };
        double ves = 0.0;
        for (size_t i = 1; i < sx.size(); ++i) {
            double dx = sx[i] - sx[i - 1];
            if (dx <= 0.0) continue;
            double mid = 0.5 * (sx[i] + sx[i - 1]);
            double s2 = evalp(c2, mid) - evalp(c1, mid) * evalp(c1, mid);
            ves += dx * s2;
        }
        ves /= range;
        vesOut = ves;
        // eq. (8): sum of the squared fitted first derivative AT THE DATA
        // POINTS of this side, divided by 12 * the full sample size.
        double acc = 0.0;
        for (double t : s.x) {
            double d1 = 0.0, pw = 1.0;
            for (int j = 1; j <= kfit; ++j) { d1 += c1[static_cast<size_t>(j)] * j * pw; pw *= t; }
            acc += d1 * d1;
        }
        double bes = range * range / (12.0 * N) * acc;
        double vy = 0.0, my = 0.0;
        for (double v : s.y) my += v;
        my /= static_cast<double>(s.y.size());
        for (double v : s.y) vy += (v - my) * (v - my);
        vy /= (static_cast<double>(s.y.size()) - 1.0);
        if (ves > 0.0 && bes > 0.0)
            jimse = std::max(1, static_cast<int>(std::ceil(std::cbrt(2.0 * bes / ves) * std::cbrt(N))));
        if (ves > 0.0)
            jmv = std::max(1, static_cast<int>(std::ceil(vy / ves * N / (std::log(N) * std::log(N)))));
    };
    double vesL = 0, vesR = 0;
    side_bins(S.L, capL, R.bins_imse_left, R.bins_mv_left, vesL);
    side_bins(S.R, capR, R.bins_imse_right, R.bins_mv_right, vesR);
    // binselect(esmv) — the default — plots the mimicking-variance bins.
    R.bins_left = (cfg.nbins_left > 0) ? cfg.nbins_left : R.bins_mv_left;
    R.bins_right = (cfg.nbins_right > 0) ? cfg.nbins_right : R.bins_mv_right;
    if (cfg.nbins_left > 0 || cfg.nbins_right > 0) R.binselect = "manual";
    R.implied_scale_left = R.bins_imse_left > 0
        ? static_cast<double>(R.bins_left) / R.bins_imse_left : 0.0;
    R.implied_scale_right = R.bins_imse_right > 0
        ? static_cast<double>(R.bins_right) / R.bins_imse_right : 0.0;

    // --- the binned scatter -------------------------------------------------
    // Evenly spaced bins over [min(x)-c, 0] and [0, max(x)-c].
    auto build_bins = [](const SideData& s, double lo, double hi, int J,
                         std::vector<double>& bx, std::vector<double>& by) {
        if (J <= 0 || hi <= lo) return;
        std::vector<double> sx(static_cast<size_t>(J), 0.0), sy(static_cast<size_t>(J), 0.0);
        std::vector<double> cnt(static_cast<size_t>(J), 0.0);
        double w = (hi - lo) / J;
        for (size_t i = 0; i < s.x.size(); ++i) {
            int j = static_cast<int>(std::floor((s.x[i] - lo) / w));
            j = std::min(std::max(j, 0), J - 1);
            sx[static_cast<size_t>(j)] += s.x[i];
            sy[static_cast<size_t>(j)] += s.y[i];
            cnt[static_cast<size_t>(j)] += 1.0;
        }
        for (int j = 0; j < J; ++j) {
            if (cnt[static_cast<size_t>(j)] <= 0.0) continue;
            bx.push_back(sx[static_cast<size_t>(j)] / cnt[static_cast<size_t>(j)]);
            by.push_back(sy[static_cast<size_t>(j)] / cnt[static_cast<size_t>(j)]);
        }
    };
    build_bins(S.L, -capL, 0.0, R.bins_left, R.bin_x_left, R.bin_mean_y_left);
    build_bins(S.R, 0.0, capR, R.bins_right, R.bin_x_right, R.bin_mean_y_right);
    R.avg_bin_len_left = R.med_bin_len_left = R.bins_left > 0 ? capL / R.bins_left : 0.0;
    R.avg_bin_len_right = R.med_bin_len_right = R.bins_right > 0 ? capR / R.bins_right : 0.0;

    // --- the fitted line ----------------------------------------------------
    auto fit_side = [&](const SideData& s, double h, double lo, double hi,
                        std::vector<double>& coef, std::vector<double>& fx,
                        std::vector<double>& fy) {
        std::vector<double> zx, zy;
        for (size_t i = 0; i < s.x.size(); ++i)
            if (kern_w(s.x[i] / h, cfg.kernel) > 0.0 || std::fabs(s.x[i]) <= h) {
                zx.push_back(s.x[i]);
                zy.push_back(s.y[i]);
            }
        if (!poly_ls(zx, zy, pfit, coef)) return;
        const int M = 50;
        for (int i = 0; i <= M; ++i) {
            double t = lo + (hi - lo) * i / M;
            double acc = 0.0, pw = 1.0;
            for (double v : coef) { acc += v * pw; pw *= t; }
            fx.push_back(t);
            fy.push_back(acc);
        }
    };
    fit_side(S.L, R.h_left, -capL, 0.0, R.coef_left, R.fit_x_left, R.fit_y_left);
    fit_side(S.R, R.h_right, 0.0, capR, R.coef_right, R.fit_x_right, R.fit_y_right);
    return R;
}

}  // namespace cecon
