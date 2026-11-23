// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata-default glm lifecycle -- see glm.h for the method and its receipts.
#include "estimators/glm.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/rng_normal.h"  // normal_icdf (AS241): the probit start g(mu0)

namespace cecon {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Neumaier-compensated sum. The log likelihood is the quantity the forward-step
// and halving rules compare, and those comparisons are where a last-bit
// difference in accumulation turns into a different path (glm.h, "Precision").
struct CSum {
    double s = 0.0, c = 0.0;
    void add(double x) {
        const double t = s + x;
        if (std::fabs(s) >= std::fabs(x)) c += (s - t) + x;
        else c += (x - t) + s;
        s = t;
    }
    double value() const { return s + c; }
};

// The link at one observation. `m` is the binomial denominator (1 for every
// other family), `p` the power of power()/opower(), `alpha` the negative
// binomial's. Each formula is glm's own glim_l* program: g(mu) (todo 0),
// g^-1(eta) (todo 1), dmu/deta (todo 2) and d2mu/deta2 (todo 3).
struct LinkCtx {
    GlmLinkFn link = GlmLinkFn::Identity;
    double p = 1.0, alpha = 1.0;
};

// d and d2 from the mean, as the glim_l programs compute them (todo 2 and 3
// read `mu`, the clamped mean, not eta).
void derivsFromMu(const LinkCtx& L, double mu, double m, double& d, double& d2) {
    switch (L.link) {
        case GlmLinkFn::Identity: d = m; d2 = 0.0; return;
        case GlmLinkFn::Log: d = mu; d2 = mu; return;
        case GlmLinkFn::Logit: d = mu * (1.0 - mu / m); d2 = d * (1.0 - 2.0 * mu / m); return;
        case GlmLinkFn::Probit: {
            const double q = normal_icdf(mu / m);
            const double ph = std::exp(-0.5 * q * q) / std::sqrt(2.0 * kPi);
            d = m * ph;
            d2 = -m * q * ph;
            return;
        }
        case GlmLinkFn::Cloglog: {
            const double l1 = std::log1p(-mu / m);
            d = (mu - m) * l1;
            d2 = (mu - m) * l1 * (1.0 + l1);
            return;
        }
        case GlmLinkFn::Loglog: {
            const double l = std::log(mu / m);
            d = -mu * l;
            d2 = mu * l * (1.0 + l);
            return;
        }
        case GlmLinkFn::Logc: d = mu - m; d2 = mu - m; return;
        case GlmLinkFn::Reciprocal: d = -mu * mu / m; d2 = 2.0 * mu * mu * mu / (m * m); return;
        case GlmLinkFn::PowerM2:
            d = -(mu * mu * mu) / (2.0 * m * m);
            d2 = (3.0 * mu * mu * mu * mu * mu) / (4.0 * m * m * m * m);
            return;
        case GlmLinkFn::Power:
            d = std::pow(mu, 1.0 - L.p) * std::pow(m, L.p) / L.p;
            d2 = std::pow(mu, 1.0 - 2.0 * L.p) * (1.0 / L.p) * (1.0 / L.p - 1.0) * std::pow(m, L.p);
            return;
        case GlmLinkFn::OPower: {
            const double t = m * std::pow(mu / m, 1.0 - L.p) * std::pow(1.0 - mu / m, 1.0 + L.p);
            d = t;
            d2 = t * t * (m * (1.0 - L.p) / mu - m * (1.0 + L.p) / (m - mu));
            return;
        }
        case GlmLinkFn::NegBin:
            d = mu * (1.0 + mu * L.alpha);
            d2 = d * (1.0 + 2.0 * mu * L.alpha);
            return;
    }
}

// g^-1(eta), before glim_mu's clamp.
double meanOf(const LinkCtx& L, double eta, double m) {
    switch (L.link) {
        case GlmLinkFn::Identity: return m == 1.0 ? eta : eta * m;
        case GlmLinkFn::Log: return m == 1.0 ? std::exp(eta) : m * std::exp(eta);
        case GlmLinkFn::Logit: return m == 1.0 ? 1.0 / (1.0 + std::exp(-eta)) : m / (1.0 + std::exp(-eta));
        case GlmLinkFn::Probit: return m * (0.5 * std::erfc(-eta / std::sqrt(2.0)));
        case GlmLinkFn::Cloglog: return m * (-std::expm1(-std::exp(eta)));
        case GlmLinkFn::Loglog: return m * std::exp(-std::exp(-eta));
        case GlmLinkFn::Logc: return m * (-std::expm1(eta));
        case GlmLinkFn::Reciprocal: return m / eta;
        case GlmLinkFn::PowerM2: return m / std::sqrt(eta);
        case GlmLinkFn::Power: return m * std::pow(eta, 1.0 / L.p);
        case GlmLinkFn::OPower: {
            const double t = std::pow(1.0 + L.p * eta, 1.0 / L.p);
            return m * t / (1.0 + t);
        }
        case GlmLinkFn::NegBin: return (1.0 / L.alpha) / (std::exp(-eta) - 1.0);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// Inverse link at eta: mu, dmu/deta, d2mu/deta2, with glim_mu's clamp of the
// mean into (0 + 1e-8, m - 1e-8) for the binomial and above 1e-8 for every other
// family but the gaussian. The four links this fitter first covered keep their
// eta-form derivatives (the same numbers, and the paths their receipts pin)
// unless the clamp moved the mean; every other link reads them off the mean.
void linkParts(const LinkCtx& L, GlmFamily fam, double eta, double m, double& mu, double& d, double& d2) {
    mu = meanOf(L, eta, m);
    bool clamped = false;
    if (fam != GlmFamily::Gaussian) {
        const double eps = 1e-8;
        if (mu <= eps) { mu = eps; clamped = true; }
        if (fam == GlmFamily::Bernoulli && mu >= m - eps) { mu = m - eps; clamped = true; }
    }
    if (!clamped && m == 1.0) {
        switch (L.link) {
            case GlmLinkFn::Identity: d = 1.0; d2 = 0.0; return;
            case GlmLinkFn::Log: d = mu; d2 = mu; return;
            case GlmLinkFn::Logit: d = mu * (1.0 - mu); d2 = d * (1.0 - 2.0 * mu); return;
            case GlmLinkFn::Probit:
                d = std::exp(-0.5 * eta * eta) / std::sqrt(2.0 * kPi);
                d2 = -eta * d;
                return;
            default: break;
        }
    }
    derivsFromMu(L, mu, m, d, d2);
}

// The link itself, g(mu) -- the start's eta0 = g(mu0).
double linkOf(const LinkCtx& L, double mu, double m) {
    switch (L.link) {
        case GlmLinkFn::Identity: return m == 1.0 ? mu : mu / m;
        case GlmLinkFn::Log: return m == 1.0 ? std::log(mu) : std::log(mu / m);
        case GlmLinkFn::Logit: return m == 1.0 ? std::log(mu / (1.0 - mu)) : std::log(mu / (m - mu));
        case GlmLinkFn::Probit: return normal_icdf(m == 1.0 ? mu : mu / m);
        case GlmLinkFn::Cloglog: return std::log(-std::log1p(-mu / m));
        case GlmLinkFn::Loglog: return -std::log(-std::log(mu / m));
        case GlmLinkFn::Logc: return std::log1p(-mu / m);
        case GlmLinkFn::Reciprocal: return m / mu;
        case GlmLinkFn::PowerM2: return (m / mu) * (m / mu);
        case GlmLinkFn::Power: return std::pow(mu / m, L.p);
        case GlmLinkFn::OPower: return (std::pow(mu / (m - mu), L.p) - 1.0) / L.p;
        case GlmLinkFn::NegBin: return std::log(mu / (mu + 1.0 / L.alpha));
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// The start's dmu: glm computes it from mu0 (`$SGLM_L 2` on the start mean).
// The first four links keep the eta form they were pinned with.
double startDmu(const LinkCtx& L, double eta0, double mu0, double m) {
    if (m == 1.0) {
        switch (L.link) {
            case GlmLinkFn::Identity: return 1.0;
            case GlmLinkFn::Log: return std::exp(eta0);
            case GlmLinkFn::Logit: {
                const double mu = 1.0 / (1.0 + std::exp(-eta0));
                return mu * (1.0 - mu);
            }
            case GlmLinkFn::Probit: return std::exp(-0.5 * eta0 * eta0) / std::sqrt(2.0 * kPi);
            default: break;
        }
    }
    double d, d2;
    derivsFromMu(L, mu0, m, d, d2);
    return d;
}

// Variance function V(mu) and dV/dmu.
void varParts(GlmFamily fam, double mu, double alpha, double m, double& v, double& dv) {
    switch (fam) {
        case GlmFamily::Gaussian: v = 1.0; dv = 0.0; return;
        case GlmFamily::Bernoulli:
            if (m == 1.0) { v = mu * (1.0 - mu); dv = 1.0 - 2.0 * mu; }
            else { v = mu * (1.0 - mu / m); dv = 1.0 - 2.0 * mu / m; }
            return;
        case GlmFamily::Poisson: v = mu; dv = 1.0; return;
        case GlmFamily::Gamma: v = mu * mu; dv = 2.0 * mu; return;
        case GlmFamily::NegBinomial: v = mu + mu * mu * alpha; dv = 1.0 + 2.0 * mu * alpha; return;
        case GlmFamily::InverseGaussian: v = mu * mu * mu; dv = 3.0 * mu * mu; return;
    }
}

// Per-observation log likelihood (glim_v* todo 5). `sig2` is the gaussian
// scale, phi*(N-k)/N; the scale-one families multiply by 1.
double llObs(GlmFamily fam, double y, double mu, double alpha, double sig2, double m) {
    switch (fam) {
        case GlmFamily::Gaussian:
            return -0.5 * ((y - mu) * (y - mu) / sig2 + std::log(2.0 * kPi * sig2));
        case GlmFamily::Bernoulli:
            if (m == 1.0 && (y == 0.0 || y == 1.0)) return (y != 0.0) ? std::log(mu) : std::log1p(-mu);
            if (y == 0.0) return m * std::log1p(-mu / m);
            if (y == m) return std::lgamma(m + 1.0) - std::lgamma(y + 1.0) + m * std::log(mu / m);
            return std::lgamma(m + 1.0) - std::lgamma(y + 1.0) - std::lgamma(m - y + 1.0) +
                   y * std::log(mu / m) + (m - y) * std::log1p(-mu / m);
        case GlmFamily::Poisson:
            return -mu + y * std::log(mu) - std::lgamma(y + 1.0);
        case GlmFamily::Gamma:
            return -y / mu + std::log(1.0 / mu);
        case GlmFamily::NegBinomial: {
            const double mm = 1.0 / alpha;
            return std::lgamma(mm + y) - std::lgamma(y + 1.0) - std::lgamma(mm) -
                   mm * std::log1p(mu / mm) + y * std::log(mu / (mu + mm));
        }
        case GlmFamily::InverseGaussian:
            return -0.5 * ((y - mu) * (y - mu) / (y * mu * mu) + std::log(y * y * y) + std::log(2.0 * kPi));
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// Per-observation deviance (glim_v* todo 3). `binary`: the binomial with m = 1
// and a 0/1 outcome takes glim_v2's two-branch form.
double devObs(GlmFamily fam, double y, double mu, double alpha, double m, bool binary) {
    switch (fam) {
        case GlmFamily::Gaussian:
            return (y - mu) * (y - mu);
        case GlmFamily::Bernoulli:
            if (m == 1.0 && binary) return (y != 0.0) ? -2.0 * std::log(mu) : -2.0 * std::log1p(-mu);
            if (y > 0.0 && y < m) return 2.0 * y * std::log(y / mu) + 2.0 * (m - y) * std::log((m - y) / (m - mu));
            if (y == 0.0) return 2.0 * m * std::log(m / (m - mu));
            return 2.0 * y * std::log(y / mu);
        case GlmFamily::Poisson:
            return (y == 0.0) ? 2.0 * mu : 2.0 * (y * std::log(y / mu) - (y - mu));
        case GlmFamily::Gamma:
            return -2.0 * (std::log(y / mu) - (y - mu) / mu);
        case GlmFamily::NegBinomial:
            return (y == 0.0) ? 2.0 * std::log1p(mu * alpha) / alpha
                              : 2.0 * (y * std::log(y / mu) - (1.0 + y * alpha) / alpha *
                                                                  std::log((1.0 + y * alpha) /
                                                                           (1.0 + mu * alpha)));
        case GlmFamily::InverseGaussian:
            return (y - mu) * (y - mu) / (mu * mu * y);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// Solve A x = b for a k*k row-major A (copied), Gaussian elimination with
// partial pivoting. The Hessian of a non-canonical link need not be positive
// definite away from the optimum, so no Cholesky assumption is made.
bool solveInPlace(std::vector<double> A, std::vector<double>& b, int k) {
    for (int c = 0; c < k; ++c) {
        int piv = c;
        for (int r = c + 1; r < k; ++r)
            if (std::fabs(A[r * k + c]) > std::fabs(A[piv * k + c])) piv = r;
        if (!(std::fabs(A[piv * k + c]) > 0.0)) return false;
        if (piv != c) {
            for (int j = 0; j < k; ++j) std::swap(A[c * k + j], A[piv * k + j]);
            std::swap(b[c], b[piv]);
        }
        for (int r = c + 1; r < k; ++r) {
            const double f = A[r * k + c] / A[c * k + c];
            if (f == 0.0) continue;
            for (int j = c; j < k; ++j) A[r * k + j] -= f * A[c * k + j];
            b[r] -= f * b[c];
        }
    }
    for (int r = k - 1; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < k; ++j) s -= A[r * k + j] * b[j];
        b[r] = s / A[r * k + r];
    }
    return true;
}

bool invert(const std::vector<double>& A, std::vector<double>& out, int k) {
    out.assign(static_cast<size_t>(k) * k, 0.0);
    for (int j = 0; j < k; ++j) {
        std::vector<double> e(static_cast<size_t>(k), 0.0);
        e[j] = 1.0;
        if (!solveInPlace(A, e, k)) return false;
        for (int i = 0; i < k; ++i) out[i * k + j] = e[i];
    }
    // Symmetrize: the exact inverse of a symmetric matrix is symmetric, and a
    // posted e(V) that is not would fail Stata's own `matrix` symmetry checks.
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < i; ++j) {
            const double m = 0.5 * (out[i * k + j] + out[j * k + i]);
            out[i * k + j] = m;
            out[j * k + i] = m;
        }
    return true;
}

// Scale-invariant rank test on a symmetric positive semi-definite k*k matrix.
// An exactly collinear regressor leaves an elimination pivot at ROUNDOFF level,
// not at zero, so solveInPlace would return a finite, meaningless start. Unit-
// diagonal scaling makes each Cholesky pivot 1 - R^2 of that column on the ones
// before it (uncentred), which no legitimate regressor drives to 1e-13.
bool rankDeficient(const std::vector<double>& A, int k) {
    std::vector<double> S(static_cast<size_t>(k) * k);
    for (int i = 0; i < k; ++i) {
        if (!(A[i * k + i] > 0.0)) return true;
        for (int j = 0; j < k; ++j)
            S[i * k + j] = A[i * k + j] / std::sqrt(A[i * k + i] * A[j * k + j]);
    }
    for (int c = 0; c < k; ++c) {
        double d = S[c * k + c];
        for (int j = 0; j < c; ++j) d -= S[c * k + j] * S[c * k + j];
        if (!(d > 1e-13)) return true;
        const double r = std::sqrt(d);
        S[c * k + c] = r;
        for (int i = c + 1; i < k; ++i) {
            double s = S[i * k + c];
            for (int j = 0; j < c; ++j) s -= S[i * k + j] * S[c * k + j];
            S[i * k + c] = s / r;
        }
    }
    return false;
}

double mreldif(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0.0;
    for (size_t j = 0; j < a.size(); ++j)
        m = std::max(m, std::fabs(a[j] - b[j]) / (std::fabs(b[j]) + 1.0));
    return m;
}

// One evaluation of the ml objective at `b` under the phi snapshot `phiIn`.
struct Eval {
    double lnf = 0.0;
    std::vector<double> g;     // score / phl
    std::vector<double> H;     // k*k negative Hessian / phl
    std::vector<double> sc;    // per-observation score weight w*(y-mu)*d/v (NOT divided)
    double phl = 1.0;          // the snapshot the derivatives were divided by
    double phiNew = 0.0;       // the Pearson scale at `b`, for the NEXT evaluation
    bool finite = true;
};

}  // namespace

// margins' inverse link: the unclamped mean of a Bernoulli/scale-one fit
// (GlmFamily::Gaussian applies no clamp).
double glm_mean(GlmLinkFn link, double eta) {
    double mu, d, d2;
    linkParts(LinkCtx{link}, GlmFamily::Gaussian, eta, 1.0, mu, d, d2);
    return mu;
}
double glm_dmean(GlmLinkFn link, double eta) {
    double mu, d, d2;
    linkParts(LinkCtx{link}, GlmFamily::Gaussian, eta, 1.0, mu, d, d2);
    return d;
}
double glm_d2mean(GlmLinkFn link, double eta) {
    double mu, d, d2;
    linkParts(LinkCtx{link}, GlmFamily::Gaussian, eta, 1.0, mu, d, d2);
    return d2;
}

GlmResult glm_fit(const double* X, int64_t n, int k, const double* y, const double* aweight,
                  const double* offset, const int64_t* cluster, const GlmOptions& opt) {
    GlmResult r;
    r.n = n;
    r.k = k;
    if (n <= 0 || k <= 0) {
        r.error = "no observations";
        return r;
    }
    const size_t N = static_cast<size_t>(n);
    const GlmFamily fam = opt.family;
    const LinkCtx L{opt.link, opt.power, opt.alpha};
    const GlmLinkFn link = opt.link;
    const double alpha = opt.alpha;
    const bool scaleOne = (fam == GlmFamily::Bernoulli || fam == GlmFamily::Poisson ||
                           fam == GlmFamily::NegBinomial);
    // $SGLM_s1: a scale-one family's 1, or scale(#)'s fixed value
    const double fixedPhi = opt.fixed_phi > 0.0 ? opt.fixed_phi : (scaleOne ? 1.0 : 0.0);
    auto mOf = [&opt](size_t i) { return opt.binom_m != nullptr ? opt.binom_m[i] : 1.0; };

    // Analytic weights normalized to mean one (sum N), as ml does for [aw];
    // frequency weights as given, N their sum.
    std::vector<double> w(N, 1.0);
    double nEff = static_cast<double>(n);
    if (aweight != nullptr) {
        CSum s;
        for (size_t i = 0; i < N; ++i) s.add(aweight[i]);
        r.sum_w = s.value();
        const double mean = r.sum_w / static_cast<double>(n);
        for (size_t i = 0; i < N; ++i) w[i] = aweight[i] / mean;
    } else if (opt.fweight != nullptr) {
        CSum s;
        for (size_t i = 0; i < N; ++i) {
            w[i] = opt.fweight[i];
            s.add(w[i]);
        }
        r.sum_w = s.value();
        nEff = r.sum_w;
    }
    const double dfr = nEff - static_cast<double>(k);
    bool binary = true;
    for (size_t i = 0; i < N && binary; ++i) binary = (y[i] == 0.0 || y[i] == 1.0);
    auto off = [offset](size_t i) { return offset != nullptr ? offset[i] : 0.0; };
    auto eta = [&](const std::vector<double>& b, size_t i) {
        double e = 0.0;
        for (int j = 0; j < k; ++j) e += X[static_cast<size_t>(j) * N + i] * b[j];
        return e + off(i);
    };

    // ---- 1. start: mu0, one WLS step, the seeded Pearson scale ----------------
    std::vector<double> b;
    double phiSeed = 1.0;
    if (!opt.b_start.empty()) {
        // from(): ml starts at the given point; glm's start step and its Pearson
        // seed are skipped ($SGLM_ph unset reads as 1 in glim_lf).
        if (static_cast<int>(opt.b_start.size()) != k) {
            r.error = "from() has the wrong number of coefficients";
            return r;
        }
        b = opt.b_start;
    } else {
        double ybar;
        {
            CSum sy, sw;
            for (size_t i = 0; i < N; ++i) {
                sy.add(w[i] * y[i]);
                sw.add(w[i]);
            }
            ybar = sy.value() / sw.value();
        }
        std::vector<double> v0(N);
        std::vector<double> XtWX(static_cast<size_t>(k) * k, 0.0), XtWz(static_cast<size_t>(k), 0.0);
        for (size_t i = 0; i < N; ++i) {
            const double m = mOf(i);
            double mu0;
            if (opt.mu_start != nullptr) mu0 = opt.mu_start[i];
            else if (fam == GlmFamily::Bernoulli) mu0 = m == 1.0 ? (y[i] + 0.5) / 2.0 : m * (y[i] + 0.5) / (m + 1.0);
            else mu0 = (y[i] + ybar) / 2.0;
            const double e0 = linkOf(L, mu0, m);
            const double d = startDmu(L, e0, mu0, m);
            double v, dv;
            varParts(fam, mu0, alpha, m, v, dv);
            v0[i] = v;
            const double z = e0 + (y[i] - mu0) / d - off(i);
            const double W = d * d / v * w[i];
            for (int a = 0; a < k; ++a) {
                const double xa = X[static_cast<size_t>(a) * N + i];
                XtWz[a] += W * xa * z;
                for (int c = 0; c <= a; ++c) XtWX[a * k + c] += W * xa * X[static_cast<size_t>(c) * N + i];
            }
        }
        for (int a = 0; a < k; ++a)
            for (int c = 0; c < a; ++c) XtWX[c * k + a] = XtWX[a * k + c];
        b = XtWz;
        if (rankDeficient(XtWX, k) || !solveInPlace(XtWX, b, k)) {
            r.error = "the starting weighted least-squares step is singular (collinear regressors)";
            return r;
        }
        CSum s;
        for (size_t i = 0; i < N; ++i) {
            // the start's mean is g^-1 of the fitted index, before any clamp
            const double mu = meanOf(L, eta(b, i), mOf(i));
            s.add(w[i] * (y[i] - mu) * (y[i] - mu) / v0[i]);
        }
        phiSeed = s.value() / dfr;
    }

    // ---- the ml evaluator -------------------------------------------------------
    // `iter` is ml's $ML_ic at this evaluation: the expected information stands in
    // for the observed one while iter <= fisher.
    auto evaluate = [&](const std::vector<double>& bb, double phiIn, bool derivs, int iter) {
        Eval ev;
        ev.phl = fixedPhi > 0.0 ? fixedPhi : phiIn;
        const double sig2 = (fixedPhi > 0.0 ? fixedPhi : phiIn * dfr / nEff);
        const bool eim = opt.fisher >= 0 && iter <= opt.fisher;
        CSum lnf, pear;
        if (derivs) {
            ev.g.assign(static_cast<size_t>(k), 0.0);
            ev.H.assign(static_cast<size_t>(k) * k, 0.0);
            ev.sc.assign(N, 0.0);
        }
        for (size_t i = 0; i < N; ++i) {
            const double m = mOf(i);
            double mu, d, d2, v, dv;
            linkParts(L, fam, eta(bb, i), m, mu, d, d2);
            varParts(fam, mu, alpha, m, v, dv);
            const double li = llObs(fam, y[i], mu, alpha, sig2, m);
            if (!std::isfinite(li)) ev.finite = false;
            lnf.add(w[i] * li);
            const double res = y[i] - mu;
            pear.add(w[i] * res * res / v);
            if (!derivs) continue;
            const double sc = w[i] * res * d / v;
            const double h = eim ? w[i] * (d * d / v)
                                 : w[i] * (d * d / v - res * (d2 / v - d * d * dv / (v * v)));
            ev.sc[i] = sc;
            for (int a = 0; a < k; ++a) {
                const double xa = X[static_cast<size_t>(a) * N + i];
                ev.g[a] += sc * xa;
                for (int c = 0; c <= a; ++c) ev.H[a * k + c] += h * xa * X[static_cast<size_t>(c) * N + i];
            }
        }
        ev.lnf = lnf.value();
        if (!std::isfinite(ev.lnf)) ev.finite = false;
        ev.phiNew = pear.value() / dfr;
        if (derivs) {
            for (int a = 0; a < k; ++a) {
                ev.g[a] /= ev.phl;
                for (int c = 0; c <= a; ++c) {
                    ev.H[a * k + c] /= ev.phl;
                    ev.H[c * k + a] = ev.H[a * k + c];
                }
            }
        }
        return ev;
    };
    // -H not negative definite (ml's "(not concave)")
    auto notConcave = [k](const std::vector<double>& H) {
        std::vector<double> Lc(H);
        for (int c = 0; c < k; ++c) {
            double d = Lc[c * k + c];
            for (int j = 0; j < c; ++j) d -= Lc[c * k + j] * Lc[c * k + j];
            if (!(d > 0.0)) return true;
            const double rt = std::sqrt(d);
            Lc[c * k + c] = rt;
            for (int i = c + 1; i < k; ++i) {
                double s = Lc[i * k + c];
                for (int j = 0; j < c; ++j) s -= Lc[i * k + j] * Lc[c * k + j];
                Lc[i * k + c] = s / rt;
            }
        }
        return false;
    };

    Eval cur = evaluate(b, phiSeed, true, 0);
    if (!cur.finite) {
        r.error = "the log likelihood is not finite at the starting values";
        return r;
    }
    double phi = cur.phiNew;
    r.ilog.push_back(cur.lnf);
    r.grad_log.push_back(cur.g);
    r.hess_log.push_back(cur.H);
    if (notConcave(cur.H)) r.nonconcave = true;

    // ---- 2./3. Newton-Raphson with halving and forward steps --------------------
    const bool closedForm = (fam == GlmFamily::Gaussian && link == GlmLinkFn::Identity &&
                             offset == nullptr && opt.b_start.empty());
    const int maxiter = closedForm ? 0 : std::max(0, opt.maxiter);
    bool converged = closedForm;
    int ic = 0;
    while (ic < maxiter && !converged) {
        std::vector<double> dvec = cur.g;
        if (!solveInPlace(cur.H, dvec, k)) {
            r.error = "the Hessian is singular";
            return r;
        }
        std::vector<double> cand(static_cast<size_t>(k));
        for (int j = 0; j < k; ++j) cand[j] = b[j] + dvec[j];
        double f1 = evaluate(cand, phi, false, ic).lnf;
        double step = 1.0;
        std::vector<double> bn;
        if (!(f1 >= cur.lnf)) {
            int nh = 0;
            while (nh < 60 && !(f1 >= cur.lnf)) {
                for (int j = 0; j < k; ++j) cand[j] = (cand[j] + b[j]) / 2.0;
                f1 = evaluate(cand, phi, false, ic).lnf;
                ++nh;
                step *= 0.5;
            }
            bn = cand;
        } else {
            std::vector<double> dd(static_cast<size_t>(k));
            for (int j = 0; j < k; ++j) dd[j] = 0.125 * dvec[j];
            double f0 = f1, inc = 0.125;
            for (;;) {
                std::vector<double> tryb(static_cast<size_t>(k));
                for (int j = 0; j < k; ++j) tryb[j] = cand[j] + dd[j];
                const double fc = evaluate(tryb, phi, false, ic).lnf;
                if (!(fc > f0)) break;
                cand = tryb;
                f0 = fc;
                step += inc;
                inc *= 2.0;
                for (int j = 0; j < k; ++j) dd[j] *= 2.0;
            }
            bn = cand;
        }
        Eval next = evaluate(bn, phi, true, ic + 1);
        if (!next.finite) {
            r.error = "the log likelihood became non-finite during iteration";
            return r;
        }
        ++ic;
        std::vector<double> hg = next.g;
        double nr = std::numeric_limits<double>::infinity();
        if (solveInPlace(next.H, hg, k)) {
            nr = 0.0;
            for (int j = 0; j < k; ++j) nr += next.g[j] * hg[j];
        }
        const double rl = std::fabs(next.lnf - cur.lnf) / (std::fabs(cur.lnf) + 1.0);
        converged = (mreldif(bn, b) <= opt.tol || rl <= opt.ltol) && nr < opt.nrtol;
        b = bn;
        cur = next;
        phi = next.phiNew;
        r.ilog.push_back(cur.lnf);
        r.steps.push_back(step);
        r.grad_log.push_back(cur.g);
        r.hess_log.push_back(cur.H);
        if (notConcave(cur.H)) r.nonconcave = true;
    }

    // ---- 4. VCE: inverse of the final (stale-phi) Hessian, or its sandwich ------
    std::vector<double> bread;
    if (!invert(cur.H, bread, k)) {
        r.error = "the Hessian at the final estimates is singular";
        return r;
    }
    if (cluster != nullptr || opt.robust) {
        std::vector<double> meat(static_cast<size_t>(k) * k, 0.0);
        double adj = 1.0;
        int64_t G = 0;
        if (cluster != nullptr) {
            for (size_t i = 0; i < N; ++i) G = std::max(G, cluster[i] + 1);
            std::vector<double> S(static_cast<size_t>(G) * k, 0.0);
            for (size_t i = 0; i < N; ++i)
                for (int a = 0; a < k; ++a)
                    S[static_cast<size_t>(cluster[i]) * k + a] +=
                        X[static_cast<size_t>(a) * N + i] * cur.sc[i] / cur.phl;
            for (int64_t g = 0; g < G; ++g)
                for (int a = 0; a < k; ++a)
                    for (int c = 0; c < k; ++c)
                        meat[a * k + c] += S[static_cast<size_t>(g) * k + a] * S[static_cast<size_t>(g) * k + c];
            adj = (G > 1) ? static_cast<double>(G) / static_cast<double>(G - 1) : 1.0;
        } else {
            // cur.sc[i] = w_i s_i; a frequency weight counts the outer product
            // s_i s_i' w_i times, so the row contributes sc_i^2 / w_i.
            G = n;
            for (size_t i = 0; i < N; ++i) {
                if (w[i] == 0.0) continue;
                const double si = cur.sc[i] / cur.phl;
                const double q = opt.fweight != nullptr ? si * si / w[i] : si * si;
                for (int a = 0; a < k; ++a)
                    for (int c = 0; c < k; ++c)
                        meat[a * k + c] += q * X[static_cast<size_t>(a) * N + i] * X[static_cast<size_t>(c) * N + i];
            }
            adj = (nEff > 1.0) ? nEff / (nEff - 1.0) : 1.0;
        }
        std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
        for (int a = 0; a < k; ++a)
            for (int c = 0; c < k; ++c) {
                double s = 0.0;
                for (int j = 0; j < k; ++j) s += bread[a * k + j] * meat[j * k + c];
                tmp[a * k + c] = s;
            }
        r.V.assign(static_cast<size_t>(k) * k, 0.0);
        for (int a = 0; a < k; ++a)
            for (int c = 0; c < k; ++c) {
                double s = 0.0;
                for (int j = 0; j < k; ++j) s += tmp[a * k + j] * bread[j * k + c];
                r.V[a * k + c] = s * adj;
            }
        for (int a = 0; a < k; ++a)
            for (int c = 0; c < a; ++c) {
                const double m = 0.5 * (r.V[a * k + c] + r.V[c * k + a]);
                r.V[a * k + c] = m;
                r.V[c * k + a] = m;
            }
        r.n_clusters = G;
        r.V_oim = bread;
    } else {
        r.V = bread;
    }

    // ---- post-estimation quantities at the final point --------------------------
    CSum dev, pear;
    r.scores.assign(N, 0.0);
    for (size_t i = 0; i < N; ++i) {
        const double m = mOf(i);
        double mu, d, d2, v, dv;
        linkParts(L, fam, eta(b, i), m, mu, d, d2);
        varParts(fam, mu, alpha, m, v, dv);
        dev.add(w[i] * devObs(fam, y[i], mu, alpha, m, binary));
        pear.add(w[i] * (y[i] - mu) * (y[i] - mu) / v);
        r.scores[i] = (y[i] - mu) * d / v / cur.phl;
    }
    r.deviance = std::fabs(dev.value());
    r.deviance_p = std::fabs(pear.value());
    r.pearson_phi = phi;
    r.beta = b;
    r.gradient = cur.g;
    r.ll = cur.lnf;
    r.ic = ic;
    r.converged = converged;
    r.phi = fixedPhi > 0.0 ? fixedPhi : phi;
    r.ok = true;
    return r;
}

}  // namespace cecon
