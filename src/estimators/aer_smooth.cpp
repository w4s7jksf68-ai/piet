#include "estimators/aer_smooth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>

namespace cecon {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

// ---------------------------------------------------------------------------
// Hodrick-Prescott

HpResult hp_filter(const std::vector<double>& y, double lambda) {
    const std::size_t T = y.size();
    HpResult r;
    r.cycle.assign(T, kNaN);
    r.trend.assign(T, kNaN);
    if (T < 3) return r;

    // M = I + lambda*K'K, accumulated row by row from K itself. Each row i of K
    // is (1, -2, 1) at columns i, i+1, i+2, so it contributes a 3x3 outer
    // product; summing those IS K'K, and at T=3 the two overlapping patterns add
    // to 1+4*lambda in the middle rather than the template's 1+5*lambda.
    std::vector<double> d(T, 1.0), u1(T > 1 ? T - 1 : 0, 0.0), u2(T > 2 ? T - 2 : 0, 0.0);
    for (std::size_t i = 0; i + 2 < T; ++i) {
        d[i] += lambda * 1.0;
        d[i + 1] += lambda * 4.0;
        d[i + 2] += lambda * 1.0;
        u1[i] += lambda * -2.0;
        u1[i + 1] += lambda * -2.0;
        u2[i] += lambda * 1.0;
    }

    // Banded LDL' with bandwidth 2. l1[i] = L[i+1][i], l2[i] = L[i+2][i].
    std::vector<double> D(T, 0.0), l1(T > 1 ? T - 1 : 0, 0.0), l2(T > 2 ? T - 2 : 0, 0.0);
    for (std::size_t i = 0; i < T; ++i) {
        double s = d[i];
        if (i >= 1) s -= l1[i - 1] * l1[i - 1] * D[i - 1];
        if (i >= 2) s -= l2[i - 2] * l2[i - 2] * D[i - 2];
        D[i] = s;
        if (i + 1 < T) {
            double t = u1[i];
            if (i >= 1) t -= l2[i - 1] * l1[i - 1] * D[i - 1];
            l1[i] = t / D[i];
        }
        if (i + 2 < T) {
            l2[i] = u2[i] / D[i];
        }
    }

    // L z = y, D w = z, L' tau = w.
    std::vector<double> z(T, 0.0);
    for (std::size_t i = 0; i < T; ++i) {
        double s = y[i];
        if (i >= 1) s -= l1[i - 1] * z[i - 1];
        if (i >= 2) s -= l2[i - 2] * z[i - 2];
        z[i] = s;
    }
    for (std::size_t i = 0; i < T; ++i) z[i] /= D[i];
    std::vector<double> tau(T, 0.0);
    for (std::size_t k = T; k-- > 0;) {
        double s = z[k];
        if (k + 1 < T) s -= l1[k] * tau[k + 1];
        if (k + 2 < T) s -= l2[k] * tau[k + 2];
        tau[k] = s;
    }
    for (std::size_t i = 0; i < T; ++i) {
        r.trend[i] = tau[i];
        r.cycle[i] = y[i] - tau[i];
    }
    return r;
}

double hp_lambda_for_format(const std::string& fmt) {
    // Match the LEADING format letters; a real format carries width/detail after
    // them (`%tq`, `%tqCCYY!qQ`, ...), so an exact compare would miss most of them.
    auto starts = [&fmt](const char* p) { return fmt.rfind(p, 0) == 0; };
    if (starts("%ty")) return 6.25;
    if (starts("%th")) return 100.0;
    if (starts("%tq")) return 1600.0;
    if (starts("%tm")) return 129600.0;
    if (starts("%tw")) return 1600.0 * 12.0 * 12.0 * 12.0 * 12.0;  // 33,177,600
    if (starts("%td")) {
        const double q = 365.0 / 4.0;  // 91.25, exactly representable
        return 1600.0 * q * q * q * q;  // 110,930,628,906.25
    }
    return -1.0;
}

// ---------------------------------------------------------------------------
// LOWESS

double lowess_effective_bwidth(double requested) {
    return (requested <= 0.0 || requested >= 1.0) ? 0.8 : requested;
}

std::vector<double> lowess_fit(const std::vector<double>& x, const std::vector<double>& y,
                               const LowessOpts& o) {
    const std::size_t n = x.size();
    std::vector<double> out(n, kNaN);
    if (n < 3) return out;  // lowess.ado's own guard: all-missing, not an error
    if (x.back() - x.front() < 1e-30) return out;

    const double bw = lowess_effective_bwidth(o.bwidth);
    const long long k =
        static_cast<long long>(std::floor((static_cast<double>(n) * bw - 0.5) / 2.0));
    const long long N = static_cast<long long>(n);

    for (long long i = 0; i < N; ++i) {
        const long long lo = std::max(0LL, i - k);
        const long long hi = std::min(N - 1, i + k);
        const double xi = x[static_cast<std::size_t>(i)];
        // Delta is 1.0001x the larger half-width of the window, so the point at
        // the far edge gets a weight just above zero rather than exactly zero.
        const double dl = xi - x[static_cast<std::size_t>(lo)];
        const double dr = x[static_cast<std::size_t>(hi)] - xi;
        const double delta = 1.0001 * std::max(dl, dr);

        double sw = 0, swx = 0, swy = 0, swxx = 0, swxy = 0;
        for (long long j = lo; j <= hi; ++j) {
            const double xj = x[static_cast<std::size_t>(j)];
            const double yj = y[static_cast<std::size_t>(j)];
            double w = 1.0;
            if (!o.noweight) {
                if (delta <= 0.0) {
                    w = 1.0;
                } else {
                    const double u = std::fabs(xj - xi) / delta;
                    if (u >= 1.0) continue;
                    const double t = 1.0 - u * u * u;
                    w = t * t * t;
                }
            }
            sw += w;
            swx += w * xj;
            swy += w * yj;
            swxx += w * xj * xj;
            swxy += w * xj * yj;
        }
        if (sw <= 0.0) continue;
        if (o.mean) {
            out[static_cast<std::size_t>(i)] = swy / sw;
            continue;
        }
        const double mx = swx / sw, my = swy / sw;
        const double sxx = swxx - sw * mx * mx;
        const double sxy = swxy - sw * mx * my;
        const double slope = (std::fabs(sxx) < 1e-300) ? 0.0 : sxy / sxx;
        out[static_cast<std::size_t>(i)] = my + slope * (xi - mx);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Local polynomial

namespace {

double kernel_value(LpolyKernel k, double z) {
    const double az = std::fabs(z);
    switch (k) {
        case LpolyKernel::Epanechnikov: {
            const double s = std::sqrt(5.0);
            if (az >= s) return 0.0;
            return 0.75 * (1.0 - z * z / 5.0) / s;
        }
        case LpolyKernel::Epan2:
            return az >= 1.0 ? 0.0 : 0.75 * (1.0 - z * z);
        case LpolyKernel::Biweight: {
            if (az >= 1.0) return 0.0;
            const double t = 1.0 - z * z;
            return (15.0 / 16.0) * t * t;
        }
        case LpolyKernel::Cosine:
            return az >= 0.5 ? 0.0 : 1.0 + std::cos(2.0 * M_PI * z);
        case LpolyKernel::Gaussian:
            return std::exp(-0.5 * z * z) / std::sqrt(2.0 * M_PI);
        case LpolyKernel::Parzen: {
            if (az <= 0.5) return 4.0 / 3.0 - 8.0 * z * z + 8.0 * az * az * az;
            if (az <= 1.0) {
                const double t = 1.0 - az;
                return 8.0 * t * t * t / 3.0;
            }
            return 0.0;
        }
        case LpolyKernel::Rectangle:
            return az >= 1.0 ? 0.0 : 0.5;
        case LpolyKernel::Triangle:
            return az >= 1.0 ? 0.0 : 1.0 - az;
    }
    return 0.0;
}

// C_{0,p}(K). STATA-EFFECTIVE values, backed out of live runs -- the same parity
// choice already made for `rectangle`, whose analytic 1.350960 is 1.27% away
// from what Stata uses and would trip lpoly's own 1e-3 STOP threshold by 13x.
// Only p=1 and p=3 are pinned (plus rectangle at p=5); anything else returns -1
// and the caller refuses by name rather than inventing a constant.
double rot_constant(LpolyKernel k, int p) {
    switch (p) {
        case 1:
            switch (k) {
                case LpolyKernel::Epanechnikov: return 0.768812;
                // MEASURED 2026-09-05, not analytic. The Fan-Gijbels definition
                // gives 1.718772; live StataBE's r(bwidth) on auto.dta (price~mpg)
                // is 2.15249845166361, and since h is linear in C with the rest of
                // the chain identical, Stata's effective constant is
                // 1.718772 * (2.15249845166361 / 2.151825823420829) = 1.719309 --
                // a 3.13e-4 gap, which is the "<=1.2e-3 band" the errata put epan2
                // in without pinning a value.
                case LpolyKernel::Epan2: return 1.719309;
                case LpolyKernel::Biweight: return 2.036168;
                case LpolyKernel::Cosine: return 4.261325;
                case LpolyKernel::Gaussian: return 0.776393;
                case LpolyKernel::Parzen: return 2.679241;
                case LpolyKernel::Rectangle: return 1.368148;
                case LpolyKernel::Triangle: return 1.888765;
            }
            return -1;
        case 3:
            switch (k) {
                case LpolyKernel::Epanechnikov: return 1.451098;
                case LpolyKernel::Epan2: return 3.243132;
                case LpolyKernel::Biweight: return 3.633345;
                case LpolyKernel::Cosine: return 7.465638;
                case LpolyKernel::Gaussian: return 1.160537;
                case LpolyKernel::Parzen: return 4.354117;
                case LpolyKernel::Rectangle: return 2.849599;
                case LpolyKernel::Triangle: return 3.420487;
            }
            return -1;
        case 5:
            return k == LpolyKernel::Rectangle ? 4.337764 : -1;
        default:
            return -1;
    }
}

// Solve the (m x m) SPD normal system in place by Cholesky. false = singular.
bool spd_solve(std::vector<double>& A, std::vector<double>& b, int m) {
    for (int i = 0; i < m; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = A[static_cast<std::size_t>(i * m + j)];
            for (int t = 0; t < j; ++t)
                s -= A[static_cast<std::size_t>(i * m + t)] * A[static_cast<std::size_t>(j * m + t)];
            if (i == j) {
                if (s <= 0.0) return false;
                A[static_cast<std::size_t>(i * m + j)] = std::sqrt(s);
            } else {
                A[static_cast<std::size_t>(i * m + j)] = s / A[static_cast<std::size_t>(j * m + j)];
            }
        }
    }
    for (int i = 0; i < m; ++i) {
        double s = b[static_cast<std::size_t>(i)];
        for (int t = 0; t < i; ++t) s -= A[static_cast<std::size_t>(i * m + t)] * b[static_cast<std::size_t>(t)];
        b[static_cast<std::size_t>(i)] = s / A[static_cast<std::size_t>(i * m + i)];
    }
    for (int i = m - 1; i >= 0; --i) {
        double s = b[static_cast<std::size_t>(i)];
        for (int t = i + 1; t < m; ++t) s -= A[static_cast<std::size_t>(t * m + i)] * b[static_cast<std::size_t>(t)];
        b[static_cast<std::size_t>(i)] = s / A[static_cast<std::size_t>(i * m + i)];
    }
    return true;
}

// Plain polynomial OLS of y on 1, x, ..., x^deg. Returns false if singular.
bool poly_ols(const std::vector<double>& x, const std::vector<double>& y, int deg,
              std::vector<double>* beta, double* rss) {
    const int m = deg + 1;
    const std::size_t n = x.size();
    std::vector<double> A(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), 0.0);
    std::vector<double> b(static_cast<std::size_t>(m), 0.0);
    std::vector<double> pw(static_cast<std::size_t>(m), 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        pw[0] = 1.0;
        for (int j = 1; j < m; ++j) pw[static_cast<std::size_t>(j)] = pw[static_cast<std::size_t>(j - 1)] * x[i];
        for (int r = 0; r < m; ++r) {
            b[static_cast<std::size_t>(r)] += pw[static_cast<std::size_t>(r)] * y[i];
            for (int c = 0; c < m; ++c)
                A[static_cast<std::size_t>(r * m + c)] += pw[static_cast<std::size_t>(r)] * pw[static_cast<std::size_t>(c)];
        }
    }
    std::vector<double> Acopy = A;
    if (!spd_solve(Acopy, b, m)) return false;
    *beta = b;
    double s = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double fit = 0, xp = 1.0;
        for (int j = 0; j < m; ++j) {
            fit += (*beta)[static_cast<std::size_t>(j)] * xp;
            xp *= x[i];
        }
        const double e = y[i] - fit;
        s += e * e;
    }
    *rss = s;
    return true;
}

}  // namespace

bool lpoly_kernel_from_name(const std::string& name, LpolyKernel* out) {
    if (name == "epanechnikov" || name.empty()) { *out = LpolyKernel::Epanechnikov; return true; }
    if (name == "epan2") { *out = LpolyKernel::Epan2; return true; }
    if (name == "biweight") { *out = LpolyKernel::Biweight; return true; }
    if (name == "cosine") { *out = LpolyKernel::Cosine; return true; }
    if (name == "gaussian") { *out = LpolyKernel::Gaussian; return true; }
    if (name == "parzen") { *out = LpolyKernel::Parzen; return true; }
    if (name == "rectangle") { *out = LpolyKernel::Rectangle; return true; }
    if (name == "triangle") { *out = LpolyKernel::Triangle; return true; }
    return false;
}

const char* lpoly_kernel_name(LpolyKernel k) {
    switch (k) {
        case LpolyKernel::Epanechnikov: return "epanechnikov";
        case LpolyKernel::Epan2: return "epan2";
        case LpolyKernel::Biweight: return "biweight";
        case LpolyKernel::Cosine: return "cosine";
        case LpolyKernel::Gaussian: return "gaussian";
        case LpolyKernel::Parzen: return "parzen";
        case LpolyKernel::Rectangle: return "rectangle";
        case LpolyKernel::Triangle: return "triangle";
    }
    return "epanechnikov";
}

double lpoly_rot_bandwidth(const std::vector<double>& x, const std::vector<double>& y,
                           LpolyKernel k, int degree) {
    int p = degree;
    if (p % 2 == 0) ++p;  // formula (3) is stated for odd p
    const double C = rot_constant(k, p);
    if (C < 0) return -1;

    const std::size_t n = x.size();
    if (n < static_cast<std::size_t>(p + 4) + 1) return -1;
    const double xmin = *std::min_element(x.begin(), x.end());
    const double xmax = *std::max_element(x.begin(), x.end());
    const double range = xmax - xmin;
    if (range <= 0) return -1;

    // Global polynomial of degree p+3, then its (p+1)th derivative -- a
    // QUADRATIC in x, because the surviving terms are j = p+1..p+3.
    std::vector<double> beta;
    double rss = 0;
    if (!poly_ols(x, y, p + 3, &beta, &rss)) return -1;
    const double sigma2 = rss / static_cast<double>(n);  // RSS/n, not RSS/(n-p-4)

    // coefficient of x^(j-p-1) is b_j * j!/(j-p-1)!
    double c0 = 0, c1 = 0, c2 = 0;
    for (int j = p + 1; j <= p + 3; ++j) {
        double fact = 1.0;
        for (int t = j - p - 1 + 1; t <= j; ++t) fact *= static_cast<double>(t);
        const double coef = beta[static_cast<std::size_t>(j)] * fact;
        const int e = j - p - 1;
        if (e == 0) c0 = coef;
        else if (e == 1) c1 = coef;
        else c2 = coef;
    }

    // n * integral over the w0 interval of (c0 + c1 x + c2 x^2)^2 -- exact,
    // because the integrand is a quartic. NOT a sum over observations, and no
    // design-density factor.
    const double lo = xmin + 0.05 * range, hi = xmax - 0.05 * range;
    const double q0 = c0 * c0;
    const double q1 = 2 * c0 * c1;
    const double q2 = c1 * c1 + 2 * c0 * c2;
    const double q3 = 2 * c1 * c2;
    const double q4 = c2 * c2;
    auto anti = [&](double t) {
        return q0 * t + q1 * t * t / 2.0 + q2 * t * t * t / 3.0 + q3 * std::pow(t, 4) / 4.0 +
               q4 * std::pow(t, 5) / 5.0;
    };
    const double integral = anti(hi) - anti(lo);
    const double denom = static_cast<double>(n) * integral;
    if (!(denom > 0)) return -1;

    const double num = sigma2 * 0.9 * range;
    return C * std::pow(num / denom, 1.0 / (2.0 * p + 3.0));
}

LpolyResult lpoly_fit(const std::vector<double>& x, const std::vector<double>& y,
                      const LpolyOpts& o) {
    LpolyResult r;
    const std::size_t n = x.size();
    if (n == 0) return r;

    double h = o.bwidth;
    if (h <= 0) h = lpoly_rot_bandwidth(x, y, o.kernel, o.degree);
    if (!(h > 0)) return r;  // caller turns this into a named refusal
    r.bwidth = h;
    // BUG-417: the PILOT bandwidth is 1.5 x the RULE-OF-THUMB bandwidth of the
    // same degree and kernel -- NOT 1.5 x the bandwidth actually used. The two
    // coincide only when the ROT is what was used, which is why an explicit
    // bwidth() was the arm that showed the difference (Stata 236.97023 vs the
    // 1.5 x 500 = 750 this used to report). lpoly.ado computes the pilot from
    // the same `_lpoly_rot` call whether or not bwidth() was given.
    if (o.pwidth > 0) {
        r.pwidth = o.pwidth;
    } else {
        const double rot = lpoly_rot_bandwidth(x, y, o.kernel, o.degree);
        // A sample too small for the ROT chain has no pilot to report; falling
        // back to 1.5 x h keeps the field populated and is NAMED here rather
        // than silently passed off as Stata's number.
        r.pwidth = (rot > 0) ? 1.5 * rot : 1.5 * h;
    }

    const double xmin = *std::min_element(x.begin(), x.end());
    const double xmax = *std::max_element(x.begin(), x.end());
    int ng = (o.n > 0) ? o.n : static_cast<int>(std::min<std::size_t>(n, 50));
    if (ng < 1) ng = 1;
    r.grid.resize(static_cast<std::size_t>(ng));
    for (int g = 0; g < ng; ++g) {
        r.grid[static_cast<std::size_t>(g)] =
            (ng == 1) ? xmin
                      : xmin + (xmax - xmin) * static_cast<double>(g) / static_cast<double>(ng - 1);
    }
    r.smooth.assign(static_cast<std::size_t>(ng), kNaN);
    if (o.want_se) r.se.assign(static_cast<std::size_t>(ng), kNaN);

    const int m = o.degree + 1;
    for (int g = 0; g < ng; ++g) {
        const double x0 = r.grid[static_cast<std::size_t>(g)];
        std::vector<double> A(static_cast<std::size_t>(m) * static_cast<std::size_t>(m), 0.0);
        std::vector<double> b(static_cast<std::size_t>(m), 0.0);
        std::vector<double> pw(static_cast<std::size_t>(m), 0.0);
        double wsum = 0;
        int nw = 0;  // observations INSIDE the window (positive kernel weight)
        for (std::size_t i = 0; i < n; ++i) {
            const double w = kernel_value(o.kernel, (x[i] - x0) / h) / h;
            if (w <= 0) continue;
            wsum += w;
            ++nw;
            const double dx = x[i] - x0;
            pw[0] = 1.0;
            for (int j = 1; j < m; ++j) pw[static_cast<std::size_t>(j)] = pw[static_cast<std::size_t>(j - 1)] * dx;
            for (int rr = 0; rr < m; ++rr) {
                b[static_cast<std::size_t>(rr)] += w * pw[static_cast<std::size_t>(rr)] * y[i];
                for (int cc = 0; cc < m; ++cc)
                    A[static_cast<std::size_t>(rr * m + cc)] +=
                        w * pw[static_cast<std::size_t>(rr)] * pw[static_cast<std::size_t>(cc)];
            }
        }
        // BUG-416: a window holding ONE observation is left MISSING, not fitted.
        // At degree 0 that window gives a "solvable" 1x1 system whose answer is
        // that single y, so the old wsum-only gate published a phantom smooth
        // value (a flat 12-mpg shelf across a 400-lb gap Stata leaves blank) and
        // counted it in r(ngrid) -- 49 vs Stata's 46 on sysuse auto. The rule is
        // MEASURED, not derived: on StataBE 19.5 every zero- or one-observation
        // window is missing and every two-observation window is fitted, even two
        // at the SAME x (so the test is a COUNT, not distinct x), at degree 0
        // and degree 1 alike. Beyond degree 1 the threshold is unmeasured --
        // Stata's `_lpoly_work` is Mata source we do not have -- and the
        // singular-system gate below already covers the under-determined cases.
        if (wsum <= 0 || nw < 2) continue;
        std::vector<double> Ac = A, bc = b;
        // r(ngrid) counts SUCCESSFUL local regressions, so a singular window is
        // a missing smooth value AND is not counted -- not an error.
        if (!spd_solve(Ac, bc, m)) continue;
        r.smooth[static_cast<std::size_t>(g)] = bc[0];
        ++r.ngrid;
    }
    return r;
}

}  // namespace cecon
