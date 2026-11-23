// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Blinder-Oaxaca decomposition (Jann 2008; Ben Jann's oaxaca 4.1.1 method).
//
// The method, as the ado defines it:
//   1. A model per group (regress, or logit/probit), plus -- for pooled/omega --
//      a model on both groups (pooled adds the by variable as a regressor).
//   2. b0 = (b1, b2, [b_ref], x1, x2): the coefficient vectors and the group
//      means of the regressors (the constant's mean is 1). V0 is block diagonal
//      between the coefficient part and the means part. Without suest the
//      coefficient blocks are each model's own e(V) and the two mean blocks are
//      independent; with suest the coefficient part is the robust (or cluster)
//      sandwich of the stacked scores, factor n/(n-1) (G/(G-1)).
//   3. The decomposition b = g(b0) and V = G V0 G' with G the Jacobian of g.
// Every formula below was checked against StataBE 19.5 + oaxaca 4.1.1 answer
// keys (the vs-stata bank: default threefold, reverse, pooled, omega, weights
// aw/fw/iw/pw, robust, vce(cluster), all to <= 1e-13 relative).
#include "estimators/oaxaca.h"
#include "core/error.h"
#include "core/linalg.h"
#include "estimators/logit.h"  // binary_perfect_screen
#include "vcov/vcov.h"         // stata_post_vcov_screen
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace cecon {
namespace {

// Small dense row-major matrix.
struct Mat {
    int r = 0, c = 0;
    std::vector<double> a;
    Mat() = default;
    Mat(int rr, int cc, double v = 0) : r(rr), c(cc), a(static_cast<size_t>(rr) * cc, v) {}
    double& operator()(int i, int j) { return a[static_cast<size_t>(i) * c + j]; }
    double operator()(int i, int j) const { return a[static_cast<size_t>(i) * c + j]; }
};
Mat mul(const Mat& A, const Mat& B) {
    Mat C(A.r, B.c);
    for (int i = 0; i < A.r; ++i)
        for (int k = 0; k < A.c; ++k) {
            const double v = A(i, k);
            if (v == 0) continue;
            for (int j = 0; j < B.c; ++j) C(i, j) += v * B(k, j);
        }
    return C;
}
Mat tr(const Mat& A) {
    Mat T(A.c, A.r);
    for (int i = 0; i < A.r; ++i)
        for (int j = 0; j < A.c; ++j) T(j, i) = A(i, j);
    return T;
}
// D' V D
Mat sandwichT(const Mat& D, const Mat& V) { return mul(mul(tr(D), V), D); }

bool finite_all(const std::vector<const double*>& cols, int64_t i) {
    for (auto c : cols)
        if (!std::isfinite(c[i])) return false;
    return true;
}

struct ModelFit {
    std::vector<double> b;        // p, zero where dropped
    Mat V;                        // p x p
    Mat scores;                   // n_all x p (bread * x_i * u_i); zero rows outside the sample
    std::vector<char> rows;       // sample mask over all rows
    bool bad = false;             // dropped coefficient or zero variance (the ado's CheckCoefs)
    bool robust = false;
    double nobs = 0;              // rows (count)
};

// Which columns survive: the constant first, then the largest remaining
// pivot (the Stata 9.2 regression ordering the oracle duplicate fixtures pin:
// of two duplicates the LATER one stays).
std::vector<int> pivot_keep(const std::vector<double>& gram, int p, int intercept) {
    std::vector<double> work = gram, orig(p);
    for (int j = 0; j < p; ++j) orig[j] = gram[j + j * p];
    std::vector<int> kept;
    std::vector<char> used(p, 0);
    for (int step = 0; step < p; ++step) {
        int pivot = -1;
        if (step == 0 && intercept >= 0) pivot = intercept;
        else
            for (int j = 0; j < p; ++j) {
                if (used[j] || work[j + j * p] <= 1e-12 * orig[j]) continue;
                if (pivot < 0 || work[j + j * p] > work[pivot + pivot * p] * (1 + 1e-12)) pivot = j;
            }
        if (pivot < 0 || work[pivot + pivot * p] <= 0) break;
        kept.push_back(pivot);
        used[pivot] = 1;
        const double dd = work[pivot + pivot * p];
        for (int j = 0; j < p; ++j)
            if (!used[j])
                for (int i = 0; i < p; ++i)
                    if (!used[i]) work[i + j * p] -= work[i + pivot * p] * work[pivot + j * p] / dd;
    }
    return kept;
}

double Fn(int model, double z) {
    if (model == 0) return z;
    if (model == 1) return 1.0 / (1.0 + std::exp(-z));
    return 0.5 * std::erfc(-z / std::sqrt(2.0));
}
double fn(int model, double z) {
    if (model == 0) return 1.0;
    if (model == 1) {
        const double e = std::exp(z);
        return e / ((1 + e) * (1 + e));
    }
    return std::exp(-0.5 * z * z) / std::sqrt(2.0 * M_PI);
}

// One model on the rows of `mask` (and finite y / regressors / weight).
ModelFit fit_model(const std::vector<const double*>& cols, const double* y, const std::vector<char>& mask,
                   const OaxacaData& d, int model, bool robust, int intercept) {
    const int64_t nall = d.n;
    const int p = static_cast<int>(cols.size());
    ModelFit out;
    out.rows.assign(nall, 0);
    std::vector<int64_t> idx;
    for (int64_t i = 0; i < nall; ++i) {
        if (!mask[i] || !std::isfinite(y[i]) || !finite_all(cols, i)) continue;
        if (!d.w.empty() && !(d.w[i] > 0)) continue;
        idx.push_back(i);
    }
    int64_t n = static_cast<int64_t>(idx.size());
    if (n == 0) throw EngineError("no_obs", "no observations", "r(2000)");
    std::vector<double> X(static_cast<size_t>(n) * p), yy(n), ww(n, 1.0);
    for (int64_t r = 0; r < n; ++r) {
        const int64_t i = idx[r];
        out.rows[i] = 1;
        yy[r] = y[i];
        for (int j = 0; j < p; ++j) X[r + j * n] = cols[j][i];
        if (!d.w.empty()) ww[r] = d.w[i];
    }
    if (model != 0) {
        // logit / probit as Stata fits them: any nonzero outcome is a success, and
        // a regressor that predicts the outcome perfectly is omitted together with
        // the rows it determines (the binary perfect-prediction screen); the
        // dropped coefficient then makes the group model "bad" (r(499) without relax).
        for (double& v : yy) v = v != 0 ? 1.0 : 0.0;
        std::vector<int> scol;
        for (int j = 0; j < p; ++j)
            if (j != intercept) scol.push_back(j);
        const int ns = static_cast<int>(scol.size());
        const int ks = ns + (intercept >= 0 ? 1 : 0);
        std::vector<double> XS(static_cast<size_t>(n) * ks);
        for (int c = 0; c < ns; ++c) std::copy_n(X.data() + static_cast<size_t>(scol[c]) * n, n, XS.data() + static_cast<size_t>(c) * n);
        if (intercept >= 0) std::copy_n(X.data() + static_cast<size_t>(intercept) * n, n, XS.data() + static_cast<size_t>(ns) * n);
        const BinaryPerfectScreen scr = binary_perfect_screen(XS.data(), n, ks, ns, yy.data());
        if (scr.inestimable) throw EngineError("bad_input", "outcome predicts data perfectly", "r(2000)");
        for (int c = 0; c < ns; ++c)
            if (scr.omit_col[c]) std::fill_n(X.data() + static_cast<size_t>(scol[c]) * n, n, 0.0);
        std::vector<int64_t> live;
        for (int64_t r = 0; r < n; ++r) {
            if (scr.keep_row[r]) live.push_back(r);
            else out.rows[idx[r]] = 0;
        }
        const int64_t m = static_cast<int64_t>(live.size());
        if (m == 0) throw EngineError("no_obs", "no observations", "r(2000)");
        if (m < n) {
            std::vector<double> Xm(static_cast<size_t>(m) * p), ym(m), wm(m);
            std::vector<int64_t> im(m);
            for (int64_t q = 0; q < m; ++q) {
                const int64_t r = live[q];
                im[q] = idx[r];
                ym[q] = yy[r];
                wm[q] = ww[r];
                for (int j = 0; j < p; ++j) Xm[q + static_cast<size_t>(j) * m] = X[r + static_cast<size_t>(j) * n];
            }
            X.swap(Xm);
            idx.swap(im);
            yy.swap(ym);
            ww.swap(wm);
            n = m;
        }
    }
    if (d.wtype == "aw") {
        double sw = 0;
        for (double v : ww) sw += v;
        for (double& v : ww) v *= static_cast<double>(n) / sw;
    }
    double sumw = 0;
    for (double v : ww) sumw += v;
    out.nobs = static_cast<double>(n);
    // weighted gram
    std::vector<double> gram(static_cast<size_t>(p) * p, 0.0);
    for (int a = 0; a < p; ++a)
        for (int b = a; b < p; ++b) {
            double s = 0;
            for (int64_t r = 0; r < n; ++r) s += ww[r] * X[r + a * n] * X[r + b * n];
            gram[a + b * p] = gram[b + a * p] = s;
        }
    std::vector<int> kept = pivot_keep(gram, p, intercept);
    std::sort(kept.begin(), kept.end());
    const int k = static_cast<int>(kept.size());
    const double ndf = (d.wtype == "fw" || d.wtype == "iw") ? sumw : static_cast<double>(n);
    if (ndf <= k) throw EngineError("no_obs", "insufficient observations", "r(2001)");
    std::vector<double> xr(static_cast<size_t>(n) * k);
    for (int j = 0; j < k; ++j) std::copy_n(X.data() + kept[j] * n, n, xr.data() + j * n);
    std::vector<double> bk(k, 0.0), inv(static_cast<size_t>(k) * k), u(n);
    if (model == 0) {
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b) inv[a + b * k] = gram[kept[a] + kept[b] * p];
        if (la::spd_inverse(inv.data(), k)) throw EngineError("collinear", "singular design");
        std::vector<double> xy(k, 0.0);
        for (int j = 0; j < k; ++j)
            for (int64_t r = 0; r < n; ++r) xy[j] += xr[r + j * n] * ww[r] * yy[r];
        la::matvec(inv.data(), k, xy.data(), bk.data());
        for (int64_t r = 0; r < n; ++r) {
            double f = 0;
            for (int j = 0; j < k; ++j) f += xr[r + j * n] * bk[j];
            u[r] = yy[r] - f;
        }
    } else {
        // logit / probit by Newton-Raphson to the exact maximum (unweighted). A fit
        // that never settles is not reported: Stata's ml ends such a model with
        // "convergence not achieved", r(430).
        bool settled = false;
        for (int it = 0; it < 200; ++it) {
            std::vector<double> g(k, 0.0), H(static_cast<size_t>(k) * k, 0.0);
            for (int64_t r = 0; r < n; ++r) {
                double z = 0;
                for (int j = 0; j < k; ++j) z += xr[r + j * n] * bk[j];
                double sc, h;
                if (model == 1) {
                    const double pr = Fn(1, z);
                    sc = yy[r] - pr;
                    h = pr * (1 - pr);
                } else {
                    const double P = Fn(2, z), ph = fn(2, z);
                    if (yy[r] != 0) {
                        const double lam = ph / P;
                        sc = lam;
                        h = lam * (lam + z);
                    } else {
                        const double lam = ph / (1 - P);
                        sc = -lam;
                        h = lam * (lam - z);
                    }
                }
                for (int a = 0; a < k; ++a) {
                    g[a] += sc * xr[r + a * n];
                    for (int b = 0; b <= a; ++b) H[a + b * k] += h * xr[r + a * n] * xr[r + b * n];
                }
            }
            for (int a = 0; a < k; ++a)
                for (int b = 0; b < a; ++b) H[b + a * k] = H[a + b * k];
            inv = H;
            if (la::spd_inverse(inv.data(), k)) throw EngineError("collinear", "singular Hessian");
            std::vector<double> step(k);
            la::matvec(inv.data(), k, g.data(), step.data());
            double mx = 0;
            for (int j = 0; j < k; ++j) {
                bk[j] += step[j];
                mx = std::max(mx, std::fabs(step[j]) / (std::fabs(bk[j]) + 1));
            }
            if (!std::isfinite(mx)) break;
            if (mx < 1e-10) settled = true;
            if (mx < 1e-15 && it > 2) break;
        }
        for (double v : bk)
            if (!std::isfinite(v)) settled = false;
        if (!settled) throw EngineError("no_convergence","convergence not achieved", "r(430)");
        // the Hessian at the final point
        std::vector<double> H(static_cast<size_t>(k) * k, 0.0);
        for (int64_t r = 0; r < n; ++r) {
            double z = 0;
            for (int j = 0; j < k; ++j) z += xr[r + j * n] * bk[j];
            double sc, h;
            if (model == 1) {
                const double pr = Fn(1, z);
                sc = yy[r] - pr;
                h = pr * (1 - pr);
            } else {
                const double P = Fn(2, z), ph = fn(2, z);
                if (yy[r] != 0) {
                    const double lam = ph / P;
                    sc = lam;
                    h = lam * (lam + z);
                } else {
                    const double lam = ph / (1 - P);
                    sc = -lam;
                    h = lam * (lam - z);
                }
            }
            u[r] = sc;
            for (int a = 0; a < k; ++a)
                for (int b = 0; b < k; ++b) H[a + b * k] += h * xr[r + a * n] * xr[r + b * n];
        }
        inv = H;
        if (la::spd_inverse(inv.data(), k)) throw EngineError("collinear", "singular Hessian");
    }
    // variance
    Mat Vk(k, k);
    const bool sandwich = model == 0 && (robust || d.wtype == "pw");
    if (model != 0) {
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b) Vk(a, b) = inv[a + b * k];
    } else if (sandwich) {
        std::vector<double> meat(static_cast<size_t>(k) * k, 0.0);
        for (int64_t r = 0; r < n; ++r) {
            const double s = ww[r] * u[r];
            for (int a = 0; a < k; ++a)
                for (int b = 0; b < k; ++b) meat[a + b * k] += s * s * xr[r + a * n] * xr[r + b * n];
        }
        const double f = static_cast<double>(n) / static_cast<double>(n - k);
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b) {
                double v = 0;
                for (int c = 0; c < k; ++c)
                    for (int e = 0; e < k; ++e) v += inv[a + c * k] * meat[c + e * k] * inv[e + b * k];
                Vk(a, b) = v * f;
            }
        out.robust = true;
    } else {
        double rss = 0;
        for (int64_t r = 0; r < n; ++r) rss += ww[r] * u[r] * u[r];
        const double s2 = rss / (ndf - k);
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b) Vk(a, b) = s2 * inv[a + b * k];
    }
    out.b.assign(p, 0.0);
    out.V = Mat(p, p);
    out.scores = Mat(static_cast<int>(nall), p);
    for (int a = 0; a < k; ++a) {
        out.b[kept[a]] = bk[a];
        for (int b = 0; b < k; ++b) out.V(kept[a], kept[b]) = Vk(a, b);
    }
    // scores for suest: bread * x_i * u_i (unweighted)
    for (int64_t r = 0; r < n; ++r) {
        for (int a = 0; a < k; ++a) {
            double v = 0;
            for (int b = 0; b < k; ++b) v += inv[a + b * k] * xr[r + b * n];
            out.scores(static_cast<int>(idx[r]), kept[a]) = v * u[r];
        }
    }
    out.bad = k < p;
    for (int j = 0; j < p; ++j)
        if (out.V(j, j) == 0) out.bad = true;
    return out;
}

}  // namespace

OaxacaOut oaxaca_decompose(const OaxacaData& d, const OaxacaSpec& s) {
    const int64_t nall = d.n;
    const int nv = static_cast<int>(d.vars.size());
    const int k = nv + 1;  // coefficients: vars, then _cons
    const int lm = s.model == 0 ? 1 : 0;
    std::map<std::string, int> pos;
    for (int j = 0; j < nv; ++j) pos[d.vars[j]] = j;
    pos["_cons"] = nv;
    std::vector<std::string> coefs = d.vars;
    coefs.push_back("_cons");
    OaxacaOut out;

    // ---- the models ------------------------------------------------------
    std::vector<double> ones(nall, 1.0);
    std::vector<const double*> cols;
    std::vector<int> colpos;
    for (int j = 0; j < nv; ++j)
        if (d.regressor[j]) {
            cols.push_back(d.X[j].data());
            colpos.push_back(j);
        }
    std::vector<const double*> cols_c = cols;
    cols_c.push_back(ones.data());
    colpos.push_back(nv);
    const int intercept = static_cast<int>(cols_c.size()) - 1;
    std::vector<ModelFit> fits;
    for (int g = 0; g < 2; ++g) {
        std::vector<char> m(nall, 0);
        for (int64_t i = 0; i < nall; ++i) m[i] = d.group[i] == g;
        fits.push_back(fit_model(cols_c, d.y.data(), m, d, s.model, (s.robust || s.robustG[g]) && !s.suest, intercept));
    }
    std::vector<char> touse(nall, 0);
    for (int64_t i = 0; i < nall; ++i) touse[i] = fits[0].rows[i] || fits[1].rows[i];
    const bool hasRef = s.tf == 0 && (s.ref == 1 || s.ref == 2);
    if (hasRef) {
        std::vector<const double*> pc = cols;
        if (s.ref == 1) pc.push_back(d.byv.data());
        pc.push_back(ones.data());
        ModelFit f = fit_model(pc, d.y.data(), touse, d, s.model, s.robust && !s.suest, static_cast<int>(pc.size()) - 1);
        if (s.ref == 1) {  // drop the by coefficient: b_ref is the regressors and the constant
            const int bypos = static_cast<int>(pc.size()) - 2;
            const int p = static_cast<int>(pc.size());
            ModelFit g = f;
            g.b.clear();
            g.V = Mat(p - 1, p - 1);
            g.scores = Mat(static_cast<int>(nall), p - 1);
            std::vector<int> keepc;
            for (int j = 0; j < p; ++j)
                if (j != bypos) keepc.push_back(j);
            for (int a = 0; a < p - 1; ++a) {
                g.b.push_back(f.b[keepc[a]]);
                for (int b = 0; b < p - 1; ++b) g.V(a, b) = f.V(keepc[a], keepc[b]);
                for (int64_t i = 0; i < nall; ++i) g.scores(static_cast<int>(i), a) = f.scores(static_cast<int>(i), keepc[a]);
            }
            f = g;
        }
        fits.push_back(f);
    }
    std::vector<char> suestRows = touse;
    if (s.tf == 0 && s.ref == 4) {
        // the stored reference regression on its own sample; b_ref is its
        // coefficients on the decomposition's regressors (a regressor it lacks is 0)
        std::vector<const double*> rc;
        for (const auto& c : d.refX) rc.push_back(c.data());
        rc.push_back(ones.data());
        OaxacaData dr = d;
        dr.w.clear();
        dr.wtype.clear();
        ModelFit f = fit_model(rc, d.refY.data(), d.refMask, dr, 0, false, static_cast<int>(rc.size()) - 1);
        const int p = static_cast<int>(colpos.size());
        ModelFit g = f;
        g.b.assign(p, 0.0);
        g.V = Mat(p, p);
        g.scores = Mat(static_cast<int>(nall), p);
        std::vector<int> src(p, -1);
        for (int c = 0; c < p; ++c) {
            const std::string& nm = coefs[colpos[c]];
            if (nm == "_cons") src[c] = static_cast<int>(rc.size()) - 1;
            else
                for (size_t r = 0; r < d.refNames.size(); ++r)
                    if (d.refNames[r] == nm) src[c] = static_cast<int>(r);
        }
        for (int a = 0; a < p; ++a) {
            if (src[a] < 0) continue;
            g.b[a] = f.b[src[a]];
            for (int b = 0; b < p; ++b)
                if (src[b] >= 0) g.V(a, b) = f.V(src[a], src[b]);
            for (int64_t i = 0; i < nall; ++i) g.scores(static_cast<int>(i), a) = f.scores(static_cast<int>(i), src[a]);
        }
        g.bad = false;
        fits.push_back(g);
        for (int64_t i = 0; i < nall; ++i) suestRows[i] = suestRows[i] || f.rows[i];
    }
    bool haserr = false;
    for (const auto& f : fits) haserr |= f.bad;
    if (!s.relax && haserr)
        throw EngineError("bad_input", "dropped coefficients or zero variances encountered", "r(499)");
    out.robustModels = s.se && !s.suest;
    for (int g = 0; g < 2; ++g) out.robustModels = out.robustModels && fits[g].robust;
    if (fits.size() > 2) out.robustModels = out.robustModels && fits[2].robust;

    // sample sizes
    auto wsum = [&](const std::vector<char>& m) {
        double t = 0;
        for (int64_t i = 0; i < nall; ++i)
            if (m[i]) t += d.wtype == "fw" ? d.w[i] : 1.0;
        return t;
    };
    out.N1 = wsum(fits[0].rows);
    out.N2 = wsum(fits[1].rows);
    out.N = wsum(touse);
    out.esample = touse;

    // ---- b and V of the coefficient part, in coefficient order -----------
    const int nm = static_cast<int>(fits.size());
    std::vector<double> bc(static_cast<size_t>(nm) * k, 0.0);
    for (int m = 0; m < nm; ++m)
        for (size_t c = 0; c < colpos.size(); ++c) bc[m * k + colpos[c]] = fits[m].b[c];
    Mat Vc(nm * k, nm * k);
    if (s.se) {
        if (s.suest) {
            // stacked scores; robust or cluster sandwich
            std::vector<int64_t> rows;
            for (int64_t i = 0; i < nall; ++i)
                if (suestRows[i]) rows.push_back(i);
            const int q = nm * k;
            std::vector<std::vector<double>> agg;
            if (!d.cl.empty()) {
                std::map<double, size_t> cid;
                for (int64_t i : rows) {
                    if (!std::isfinite(d.cl[i]))
                        throw EngineError("bad_input", "cluster variable is not allowed to be missing within the estimation sample", "r(459)");
                    if (!cid.count(d.cl[i])) {
                        cid[d.cl[i]] = agg.size();
                        agg.emplace_back(q, 0.0);
                    }
                }
                for (int64_t i : rows) {
                    auto& v = agg[cid[d.cl[i]]];
                    for (int m = 0; m < nm; ++m)
                        for (size_t c = 0; c < colpos.size(); ++c)
                            v[m * k + colpos[c]] += fits[m].scores(static_cast<int>(i), static_cast<int>(c));
                }
                out.nClust = static_cast<int64_t>(agg.size());
            } else {
                for (int64_t i : rows) {
                    std::vector<double> v(q, 0.0);
                    for (int m = 0; m < nm; ++m)
                        for (size_t c = 0; c < colpos.size(); ++c)
                            v[m * k + colpos[c]] = fits[m].scores(static_cast<int>(i), static_cast<int>(c));
                    agg.push_back(std::move(v));
                }
            }
            const double G = static_cast<double>(agg.size());
            const double f = G / (G - 1);
            for (const auto& v : agg)
                for (int a = 0; a < q; ++a) {
                    if (v[a] == 0) continue;
                    for (int b = 0; b < q; ++b) Vc(a, b) += v[a] * v[b];
                }
            for (double& x : Vc.a) x *= f;
        } else {
            for (int m = 0; m < nm; ++m)
                for (size_t a = 0; a < colpos.size(); ++a)
                    for (size_t b = 0; b < colpos.size(); ++b)
                        Vc(m * k + colpos[a], m * k + colpos[b]) = fits[m].V(static_cast<int>(a), static_cast<int>(b));
        }
    }

    // ---- normalize(): deviation contrast of each dummy set ---------------
    if (!s.normalize.empty()) {
        Mat D(k, k);
        for (int j = 0; j < k; ++j) D(j, j) = 1;
        std::string label;
        for (const auto& [cons, dums] : s.normalize) {
            const int pc = pos.at(cons);
            const int kk = static_cast<int>(dums.size());
            std::vector<int> pp;
            for (const auto& v : dums) pp.push_back(pos.at(v));
            for (int a : pp) {
                D(a, pc) += 1.0 / kk;
                for (int b : pp) D(a, b) -= 1.0 / kk;
            }
            if (s.check) {
                for (int g = 0; g < 2; ++g) {
                    bool err = false;
                    std::vector<double> xs, cs;
                    for (int64_t i = 0; i < nall; ++i) {
                        if (!fits[g].rows[i]) continue;
                        double sum = 0;
                        bool miss = false;
                        for (int a : pp) {
                            const double v = d.X[a][i];
                            if (!std::isfinite(v)) miss = true;
                            sum += v;
                        }
                        if (cons == "_cons") {
                            if (!miss && sum != 1) err = true;
                        } else {
                            xs.push_back(miss ? NAN : sum);
                            cs.push_back(d.X[pc][i]);
                        }
                    }
                    if (cons != "_cons") {
                        // mreldif(rowsum, cons) over the nonmissing rows > 1e-7
                        double mx = 0;
                        for (size_t t = 0; t < xs.size(); ++t) {
                            if (!std::isfinite(xs[t])) continue;
                            mx = std::max(mx, std::fabs(xs[t] - cs[t]) / (std::fabs(cs[t]) + 1));
                        }
                        err = mx > 1e-7;
                    }
                    if (err) {
                        std::string ds;
                        for (size_t t = 0; t < dums.size(); ++t) ds += (t ? " " : "") + dums[t];
                        throw EngineError("bad_input", "inconsistent dummy set: " + ds, "r(3498)");
                    }
                }
            }
        }
        Mat DD(nm * k, nm * k);
        for (int m = 0; m < nm; ++m)
            for (int a = 0; a < k; ++a)
                for (int b = 0; b < k; ++b) DD(m * k + a, m * k + b) = D(a, b);
        Mat bm(1, nm * k);
        bm.a = bc;
        bc = mul(bm, DD).a;
        if (s.se) Vc = sandwichT(DD, Vc);
        for (const auto& [cons, dums] : s.normalize) {
            std::string g;
            for (size_t t = 0; t < dums.size(); ++t) g += (t ? " " : "") + dums[t];
            if (cons != "_cons") g += " # " + cons;
            label += (label.empty() ? "" : ", ") + g;
        }
        out.notes.push_back("(normalized: " + label + ")");
    }

    // ---- weight(): b_ref = w b1 + (1-w) b2 --------------------------------
    int nmods = nm;
    if (s.tf == 0 && s.ref == 3) {
        std::vector<double> w(k);
        for (int j = 0; j < k; ++j) w[j] = s.weights[j % s.weights.size()];
        Mat Gw(2 * k, 3 * k);
        for (int j = 0; j < 2 * k; ++j) Gw(j, j) = 1;
        for (int j = 0; j < k; ++j) {
            Gw(j, 2 * k + j) = w[j];
            Gw(k + j, 2 * k + j) = 1 - w[j];
        }
        Mat bm(1, 2 * k);
        bm.a = bc;
        bc = mul(bm, Gw).a;
        if (s.se) Vc = sandwichT(Gw, Vc);
        nmods = 3;
    }

    // ---- means -------------------------------------------------------------
    std::vector<std::vector<double>> xm(2, std::vector<double>(k, 0.0));
    Mat Vx(2 * k, 2 * k);
    {
        std::vector<const double*> mc;
        for (int j = 0; j < nv; ++j) mc.push_back(d.X[j].data());
        std::vector<std::vector<int64_t>> mrows(2);
        for (int g = 0; g < 2; ++g)
            for (int64_t i = 0; i < nall; ++i)
                if (fits[g].rows[i] && finite_all(mc, i)) mrows[g].push_back(i);
        const double ntot = static_cast<double>(mrows[0].size() + mrows[1].size());
        std::vector<std::vector<double>> wv(2);
        std::vector<double> W(2, 0.0);
        auto xval = [&](int j, int64_t i) { return j < nv ? d.X[j][i] : 1.0; };
        for (int g = 0; g < 2; ++g) {
            const double ng = static_cast<double>(mrows[g].size());
            wv[g].assign(mrows[g].size(), 1.0);
            if (!d.w.empty())
                for (size_t r = 0; r < mrows[g].size(); ++r) wv[g][r] = d.w[mrows[g][r]];
            double sw = 0;
            for (double v : wv[g]) sw += v;
            if (d.wtype == "aw")
                for (double& v : wv[g]) v *= ng / sw;
            for (double v : wv[g]) W[g] += v;
            for (int j = 0; j < k; ++j) {
                double t = 0;
                for (size_t r = 0; r < mrows[g].size(); ++r) t += wv[g][r] * xval(j, mrows[g][r]);
                xm[g][j] = t / W[g];
            }
        }
        if (s.se) {
            if (s.clusterMeans && !d.cl.empty()) {
                std::map<double, std::vector<double>> agg;
                for (int g = 0; g < 2; ++g)
                    for (size_t r = 0; r < mrows[g].size(); ++r) {
                        const int64_t i = mrows[g][r];
                        if (!std::isfinite(d.cl[i])) continue;
                        auto& v = agg[d.cl[i]];
                        if (v.empty()) v.assign(2 * k, 0.0);
                        for (int j = 0; j < k; ++j) v[g * k + j] += wv[g][r] * (xval(j, i) - xm[g][j]) / W[g];
                    }
                const double G = static_cast<double>(agg.size());
                for (const auto& [c, v] : agg)
                    for (int a = 0; a < 2 * k; ++a)
                        for (int b = 0; b < 2 * k; ++b) Vx(a, b) += v[a] * v[b];
                for (double& x : Vx.a) x *= G / (G - 1);
                out.nClust = static_cast<int64_t>(agg.size());
            } else if (d.wtype == "pw") {
                for (int g = 0; g < 2; ++g)
                    for (size_t r = 0; r < mrows[g].size(); ++r) {
                        const int64_t i = mrows[g][r];
                        std::vector<double> z(k);
                        for (int j = 0; j < k; ++j) z[j] = wv[g][r] * (xval(j, i) - xm[g][j]) / W[g];
                        for (int a = 0; a < k; ++a)
                            for (int b = 0; b < k; ++b) Vx(g * k + a, g * k + b) += z[a] * z[b];
                    }
                for (double& x : Vx.a) x *= ntot / (ntot - 1);
            } else {
                for (int g = 0; g < 2; ++g) {
                    for (size_t r = 0; r < mrows[g].size(); ++r) {
                        const int64_t i = mrows[g][r];
                        for (int a = 0; a < k; ++a) {
                            const double da = xval(a, i) - xm[g][a];
                            if (da == 0) continue;
                            for (int b = 0; b < k; ++b)
                                Vx(g * k + a, g * k + b) += wv[g][r] * da * (xval(b, i) - xm[g][b]);
                        }
                    }
                    for (int a = 0; a < k; ++a)
                        for (int b = 0; b < k; ++b) Vx(g * k + a, g * k + b) /= W[g] * (W[g] - 1);
                }
            }
            if (!s.suest)  // the two groups' means are independent without suest
                for (int a = 0; a < k; ++a)
                    for (int b = 0; b < k; ++b) Vx(a, k + b) = Vx(k + b, a) = 0;
        }
        // x1()/x2(): fixed values, zero variance
        for (int g = 0; g < 2; ++g)
            for (const auto& [v, val] : (g == 0 ? s.x1 : s.x2)) {
                const int j = pos.at(v);
                xm[g][j] = val;
                for (int c = 0; c < 2 * k; ++c) Vx(g * k + j, c) = Vx(c, g * k + j) = 0;
            }
        if (s.fixedAll) std::fill(Vx.a.begin(), Vx.a.end(), 0.0);
        for (const auto& v : s.fixedVars) {
            const int j = pos.at(v);
            for (int g = 0; g < 2; ++g)
                for (int c = 0; c < 2 * k; ++c) Vx(g * k + j, c) = Vx(c, g * k + j) = 0;
        }
    }

    // ---- b0 / V0 -----------------------------------------------------------
    const int nb = nmods * k;
    std::vector<std::string> eq0 = {"b1", "b2"};
    if (nmods == 3) eq0.push_back("b_ref");
    eq0.push_back("x1");
    eq0.push_back("x2");
    out.b0 = bc;
    out.b0.insert(out.b0.end(), xm[0].begin(), xm[0].end());
    out.b0.insert(out.b0.end(), xm[1].begin(), xm[1].end());
    for (const auto& e : eq0)
        for (const auto& c : coefs) out.names0.push_back(e + ":" + c);
    const int q0 = nb + 2 * k;
    Mat V0(q0, q0);
    if (s.se) {
        for (int a = 0; a < nb; ++a)
            for (int b = 0; b < nb; ++b) V0(a, b) = Vc(a, b);
        for (int a = 0; a < 2 * k; ++a)
            for (int b = 0; b < 2 * k; ++b) V0(nb + a, nb + b) = Vx(a, b);
        out.V0 = V0.a;
    }

    // ---- the decomposition (the ado's oaxaca() Mata function) --------------
    std::vector<char> p(k, 1);
    std::vector<std::string> coefsadj;
    const bool adjust = !s.adjust.empty();
    {
        std::set<std::string> adj(s.adjust.begin(), s.adjust.end());
        for (int j = 0; j < k; ++j) {
            p[j] = !adj.count(coefs[j]);
            if (p[j]) coefsadj.push_back(coefs[j]);
        }
    }
    const int tf = s.tf;
    const bool split = s.split, detail = s.detail;
    if (tf && split && s.se)  // the ado's Jacobian rows do not conform (Mata r(3200))
        throw EngineError("bad_input", "conformability error", "r(3200)");
    std::vector<double> b1(bc.begin(), bc.begin() + k), b2(bc.begin() + k, bc.begin() + 2 * k);
    std::vector<double> B = tf == 0 ? std::vector<double>(bc.begin() + 2 * k, bc.begin() + 3 * k) : (tf == 1 ? b2 : b1);
    const std::vector<double>& x1 = xm[0];
    const std::vector<double>& x2 = xm[1];
    // data rows for the nonlinear models
    std::vector<std::vector<int64_t>> srows(2);
    for (int g = 0; g < 2; ++g)
        for (int64_t i = 0; i < nall; ++i)
            if (fits[g].rows[i]) srows[g].push_back(i);
    auto xv = [&](int j, int64_t i) { return j < nv ? d.X[j][i] : 1.0; };
    auto wgt = [&](int64_t i) { return (!d.w.empty() && lm == 0) ? d.w[i] : 1.0; };
    struct Pred { double F = 0, f = 0; std::vector<double> fx; };
    // mean of F(X*(c:*mask)) and f(.), f(.)*X:*pmask over group g (lm: the means row)
    auto pred = [&](int g, const std::vector<double>& c, const std::vector<char>& mask, bool usep) {
        Pred r;
        r.fx.assign(k, 0.0);
        if (lm) {
            const auto& xx = g == 0 ? x1 : x2;
            double z = 0;
            for (int j = 0; j < k; ++j) z += xx[j] * c[j] * mask[j];
            r.F = z;
            r.f = 1;
            for (int j = 0; j < k; ++j) r.fx[j] = xx[j] * (usep ? p[j] : 1);
            return r;
        }
        double sw = 0;
        for (int64_t i : srows[g]) {
            double z = 0;
            for (int j = 0; j < k; ++j) z += xv(j, i) * c[j] * mask[j];
            const double w = wgt(i);
            sw += w;
            r.F += w * Fn(s.model, z);
            const double fz = fn(s.model, z);
            r.f += w * fz;
            for (int j = 0; j < k; ++j) r.fx[j] += w * fz * xv(j, i) * (usep ? p[j] : 1);
        }
        r.F /= sw;
        r.f /= sw;
        for (double& v : r.fx) v /= sw;
        return r;
    };
    std::vector<char> all1(k, 1);
    Pred o1, o2;
    if (adjust) {
        o1 = pred(0, b1, all1, false);
        o2 = pred(1, b2, all1, false);
    }
    Pred P11 = pred(0, b1, p, true), P22 = pred(1, b2, p, true);
    Pred P1B = pred(0, tf ? b2 : B, p, true), P2B = pred(1, tf ? b1 : B, p, true);
    std::vector<double> WE, WU, WI, WEn, WUn, WIn;
    double WEd = 0, WUd = 0, WId = 0;
    auto finishW = [&](const std::vector<double>& n_, double& dd, std::vector<double>& W) {
        double sum = 0;
        for (double v : n_) sum += v;
        dd = lm ? 1.0 : sum;
        dd = dd == 0 ? dd : 1 / dd;
        W.assign(k, 0.0);
        for (int j = 0; j < k; ++j) W[j] = n_[j] * dd;
    };
    if (detail) {
        WEn.assign(k, 0);
        WUn.assign(k, 0);
        for (int j = 0; j < k; ++j) {
            WEn[j] = (x1[j] - x2[j]) * B[j] * p[j];
            if (tf == 0 && !split) WUn[j] = (x1[j] * (b1[j] - B[j]) + x2[j] * (B[j] - b2[j])) * p[j];
            else if (split) WUn[j] = x1[j] * (b1[j] - B[j]) * p[j];
            else if (tf == 1) WUn[j] = x2[j] * (b1[j] - b2[j]) * p[j];
            else WUn[j] = x1[j] * (b1[j] - b2[j]) * p[j];
        }
        finishW(WEn, WEd, WE);
        finishW(WUn, WUd, WU);
        if (tf || split) {
            WIn.assign(k, 0);
            for (int j = 0; j < k; ++j) {
                if (split) WIn[j] = x2[j] * (B[j] - b2[j]) * p[j];
                else if (tf == 1) WIn[j] = (x1[j] - x2[j]) * (b1[j] - b2[j]) * p[j];
                else WIn[j] = (x1[j] - x2[j]) * (b2[j] - b1[j]) * p[j];
            }
            finishW(WIn, WId, WI);
        }
    }
    auto sel = [&](const std::vector<double>& v) {
        std::vector<double> o;
        for (int j = 0; j < k; ++j)
            if (p[j]) o.push_back(v[j]);
        return o;
    };
    std::vector<double> b;
    if (adjust) b.insert(b.end(), {o1.F, o2.F, o1.F - o2.F});
    b.insert(b.end(), {P11.F, P22.F, P11.F - P22.F});
    if (tf == 0) b.push_back(P1B.F - P2B.F);
    else if (tf == 1) b.push_back(P1B.F - P22.F);
    else b.push_back(P11.F - P2B.F);
    if (tf == 0 && !split) b.push_back((P11.F - P1B.F) + (P2B.F - P22.F));
    else if (split) b.insert(b.end(), {P11.F - P1B.F, P2B.F - P22.F});
    else if (tf == 1) b.insert(b.end(), {P2B.F - P22.F, P11.F - P1B.F - P2B.F + P22.F});
    else b.insert(b.end(), {P11.F - P1B.F, P1B.F - P11.F - P22.F + P2B.F});
    const int jE = 3 + 3 * (adjust ? 1 : 0);  // 0-based index of the explained part
    std::vector<double> WEs, WUs, WIs;
    if (detail) {
        WEs = sel(WE);
        WUs = sel(WU);
        const double sE = lm ? 1.0 : b[jE], sU = lm ? 1.0 : b[jE + 1];
        for (double v : WEs) b.push_back(v * sE);
        for (double v : WUs) b.push_back(v * sU);
        if (split || tf) {
            WIs = sel(WI);
            const double sI = lm ? 1.0 : b[jE + 2];
            for (double v : WIs) b.push_back(v * sI);
        }
    }
    const int mb = static_cast<int>(b.size());
    Mat V;
    if (s.se) {
        const int nblk = tf == 0 ? 5 : 4;
        const int qc = nblk * k;
        Mat G(mb, qc);
        // block order: b1 b2 [B] x1 x2
        auto blk = [&](int which) {  // 0 b1 1 b2 2 B 3 x1 4 x2 -> column offset
            if (tf == 0) return which * k;
            if (which == 2) return -1;
            return (which < 2 ? which : which - 1) * k;
        };
        auto setrow = [&](int row, int which, const std::vector<double>& v) {
            const int off = blk(which);
            if (off < 0) return;
            for (int j = 0; j < k; ++j) G(row, off + j) += v[j];
        };
        auto scal = [&](const std::vector<double>& v, double f) {
            std::vector<double> o(v);
            for (double& x : o) x *= f;
            return o;
        };
        auto prodp = [&](double f, const std::vector<double>& c) {
            std::vector<double> o(k);
            for (int j = 0; j < k; ++j) o[j] = f * c[j] * p[j];
            return o;
        };
        auto sub = [&](const std::vector<double>& a, const std::vector<double>& c) {
            std::vector<double> o(k);
            for (int j = 0; j < k; ++j) o[j] = a[j] - c[j];
            return o;
        };
        int i = 0;
        if (adjust) {
            std::vector<double> a1(k), a2(k);
            for (int j = 0; j < k; ++j) {
                a1[j] = o1.f * b1[j];
                a2[j] = o2.f * b2[j];
            }
            // the unadjusted rows use the unmasked derivatives
            setrow(i, 0, o1.fx);
            setrow(i, 3, a1);
            ++i;
            setrow(i, 1, o2.fx);
            setrow(i, 4, a2);
            ++i;
            setrow(i, 0, o1.fx);
            setrow(i, 1, scal(o2.fx, -1));
            setrow(i, 3, a1);
            setrow(i, 4, scal(a2, -1));
            ++i;
        }
        setrow(i, 0, P11.fx);
        setrow(i, 3, prodp(P11.f, b1));
        ++i;
        setrow(i, 1, P22.fx);
        setrow(i, 4, prodp(P22.f, b2));
        ++i;
        setrow(i, 0, P11.fx);
        setrow(i, 1, scal(P22.fx, -1));
        setrow(i, 3, prodp(P11.f, b1));
        setrow(i, 4, prodp(-P22.f, b2));
        ++i;
        if (tf == 0) {
            setrow(i, 2, sub(P1B.fx, P2B.fx));
            setrow(i, 3, prodp(P1B.f, B));
            setrow(i, 4, prodp(-P2B.f, B));
        } else if (tf == 1) {
            setrow(i, 1, sub(P1B.fx, P22.fx));
            setrow(i, 3, prodp(P1B.f, b2));
            setrow(i, 4, prodp(-P22.f, b2));
        } else {
            setrow(i, 0, sub(P11.fx, P2B.fx));
            setrow(i, 3, prodp(P11.f, b1));
            setrow(i, 4, prodp(-P2B.f, b1));
        }
        ++i;
        auto comb = [&](double f1, const std::vector<double>& c1, double f2, const std::vector<double>& c2) {
            std::vector<double> o(k);
            for (int j = 0; j < k; ++j) o[j] = (f1 * c1[j] - f2 * c2[j]) * p[j];
            return o;
        };
        if (tf == 0 && !split) {
            setrow(i, 0, P11.fx);
            setrow(i, 1, scal(P22.fx, -1));
            setrow(i, 2, sub(P2B.fx, P1B.fx));
            setrow(i, 3, comb(P11.f, b1, P1B.f, B));
            setrow(i, 4, comb(P2B.f, B, P22.f, b2));
            ++i;
        } else if (split) {
            setrow(i, 0, P11.fx);
            setrow(i, 2, scal(P1B.fx, -1));
            setrow(i, 3, comb(P11.f, b1, P1B.f, B));
            ++i;
            setrow(i, 1, scal(P22.fx, -1));
            setrow(i, 2, P2B.fx);
            setrow(i, 4, comb(P2B.f, B, P22.f, b2));
            ++i;
        } else if (tf == 1) {
            setrow(i, 0, P2B.fx);
            setrow(i, 1, scal(P22.fx, -1));
            setrow(i, 4, comb(P2B.f, b1, P22.f, b2));
            ++i;
            setrow(i, 0, sub(P11.fx, P2B.fx));
            setrow(i, 1, sub(P22.fx, P1B.fx));
            setrow(i, 3, comb(P11.f, b1, P1B.f, b2));
            setrow(i, 4, comb(P22.f, b2, P2B.f, b1));
            ++i;
        } else {
            setrow(i, 0, P11.fx);
            setrow(i, 1, scal(P1B.fx, -1));
            setrow(i, 3, comb(P11.f, b1, P1B.f, b2));
            ++i;
            setrow(i, 0, sub(P2B.fx, P11.fx));
            setrow(i, 1, sub(P1B.fx, P22.fx));
            setrow(i, 3, comb(P1B.f, b2, P11.f, b1));
            setrow(i, 4, comb(P2B.f, b1, P22.f, b2));
            ++i;
        }
        if (detail) {
            // dW = diag(c*Wd) - (1-lm) * (Wn' * (c:*p)) * Wd^2, per block; rows selected by p
            struct Part { int which; std::vector<double> c; };
            auto dW = [&](const std::vector<Part>& parts, const std::vector<double>& Wn, double Wd) {
                Mat M(k, qc);
                for (const auto& pt : parts) {
                    const int off = blk(pt.which);
                    if (off < 0) continue;
                    for (int a = 0; a < k; ++a) {
                        M(a, off + a) += pt.c[a] * Wd;
                        if (!lm)
                            for (int c = 0; c < k; ++c) M(a, off + c) -= Wn[a] * pt.c[c] * p[c] * Wd * Wd;
                    }
                }
                return M;
            };
            auto neg = [&](const std::vector<double>& v) { return scal(v, -1); };
            std::vector<double> dx = sub(x1, x2);
            Mat dWE, dWU, dWI;
            if (tf == 0) dWE = dW({{2, dx}, {3, B}, {4, neg(B)}}, WEn, WEd);
            else if (tf == 1) dWE = dW({{1, dx}, {3, b2}, {4, neg(b2)}}, WEn, WEd);
            else dWE = dW({{0, dx}, {3, b1}, {4, neg(b1)}}, WEn, WEd);
            if (tf == 0 && !split)
                dWU = dW({{0, x1}, {1, neg(x2)}, {2, neg(dx)}, {3, sub(b1, B)}, {4, sub(B, b2)}}, WUn, WUd);
            else if (split) dWU = dW({{0, x1}, {2, neg(x1)}, {3, sub(b1, B)}}, WUn, WUd);
            else if (tf == 1) dWU = dW({{0, x2}, {1, neg(x2)}, {4, sub(b1, b2)}}, WUn, WUd);
            else dWU = dW({{0, x1}, {1, neg(x1)}, {3, sub(b1, b2)}}, WUn, WUd);
            if (tf || split) {
                if (split) dWI = dW({{1, neg(x2)}, {2, x2}, {4, sub(B, b2)}}, WIn, WId);
                else if (tf == 1) dWI = dW({{0, dx}, {1, neg(dx)}, {3, sub(b1, b2)}, {4, sub(b2, b1)}}, WIn, WId);
                else dWI = dW({{0, neg(dx)}, {1, dx}, {3, sub(b2, b1)}, {4, sub(b1, b2)}}, WIn, WId);
            }
            auto put = [&](const Mat& dM, const std::vector<double>& W, int jrow) {
                const double bj = lm ? 1.0 : b[jrow];
                for (int a = 0; a < k; ++a) {
                    if (!p[a]) continue;
                    for (int c = 0; c < qc; ++c)
                        G(i, c) = dM(a, c) * bj + (lm ? 0.0 : W[a] * G(jrow, c));
                    ++i;
                }
            };
            put(dWE, WE, jE);
            put(dWU, WU, jE + 1);
            if (tf || split) put(dWI, WI, jE + 2);
        }
        // V0's blocks are b1 b2 [b_ref] x1 x2: exactly G's columns
        V = mul(mul(G, V0), tr(G));
    }

    // ---- detail() groups -----------------------------------------------------
    std::vector<std::string> cf = coefsadj;
    if (detail && !s.vgroups.empty()) {
        const int kc = static_cast<int>(cf.size());
        Mat D(kc, kc);
        for (int j = 0; j < kc; ++j) D(j, j) = 1;
        std::vector<char> keepc(kc, 1);
        std::vector<std::string> stripe = cf;
        std::string legend;
        for (const auto& [gname, gvars] : s.vgroups) {
            int p1 = -1;
            std::vector<std::string> found;
            for (const auto& v : gvars) {
                auto it = std::find(cf.begin(), cf.end(), v);
                if (it == cf.end()) {
                    out.notes.push_back("(" + gname + ": " + v + " not found)");
                    continue;
                }
                const int pp = static_cast<int>(it - cf.begin());
                found.push_back(v);
                if (p1 < 0) {
                    p1 = pp;
                    stripe[pp] = gname;
                } else {
                    D(pp, p1) = 1;
                    keepc[pp] = 0;
                }
            }
            if (!found.empty()) {
                std::string g = gname + ":";
                for (const auto& v : found) g += " " + v;
                legend += (legend.empty() ? "" : " ") + std::string("\"") + g + "\"";
            }
        }
        out.legend = legend;
        Mat Ds(kc, 0);
        std::vector<int> kidx;
        for (int j = 0; j < kc; ++j)
            if (keepc[j]) kidx.push_back(j);
        Ds = Mat(kc, static_cast<int>(kidx.size()));
        for (int a = 0; a < kc; ++a)
            for (size_t c = 0; c < kidx.size(); ++c) Ds(a, static_cast<int>(c)) = D(a, kidx[c]);
        std::vector<std::string> ncf;
        for (int j : kidx) ncf.push_back(stripe[j]);
        const int head = (split == 0 && tf == 0) ? 5 + (adjust ? 3 : 0) : 6 + (adjust ? 3 : 0);
        const int nparts = (split == 0 && tf == 0) ? 2 : 3;
        const int kd = static_cast<int>(kidx.size());
        Mat DD(head + nparts * kc, head + nparts * kd);
        for (int j = 0; j < head; ++j) DD(j, j) = 1;
        for (int t = 0; t < nparts; ++t)
            for (int a = 0; a < kc; ++a)
                for (int c = 0; c < kd; ++c) DD(head + t * kc + a, head + t * kd + c) = Ds(a, c);
        Mat bm(1, mb);
        bm.a = b;
        b = mul(bm, DD).a;
        if (s.se) V = sandwichT(DD, V);
        cf = ncf;
    }

    // ---- column stripe and the keep rule --------------------------------------
    const std::string lblWE = tf ? "endowments" : "explained";
    const std::string lblWU = tf ? "coefficients" : (split ? "unexplained1" : "unexplained");
    const std::string lblWI = tf ? "interaction" : "unexplained2";
    std::vector<std::pair<std::string, std::string>> cs;
    if (adjust) {
        for (const char* n : {"group_1", "group_2", "difference"}) cs.push_back({"overall", n});
        for (const std::string n : {std::string("group_1"), std::string("group_2"), std::string("difference"), lblWE, lblWU})
            cs.push_back({"adjusted", n});
    } else {
        for (const std::string n : {std::string("group_1"), std::string("group_2"), std::string("difference"), lblWE, lblWU})
            cs.push_back({"overall", n});
    }
    if (split || tf) cs.push_back({cs.back().first, lblWI});
    if (detail) {
        for (const auto& c : cf) cs.push_back({lblWE, c});
        for (const auto& c : cf) cs.push_back({lblWU, c});
        if (split || tf)
            for (const auto& c : cf) cs.push_back({lblWI, c});
    }
    const int m2 = static_cast<int>(b.size());
    std::vector<int> keep;
    for (int j = 0; j < m2; ++j) {
        bool kp = (cs[j].first != lblWE && cs[j].first != "interaction") || cs[j].second != "_cons";
        kp = ((cs[j].first != lblWU && cs[j].first != lblWI) || cs[j].second != "_offset") && kp;
        kp = b[j] != 0 || kp;
        if (kp || !detail) keep.push_back(j);
    }
    std::set<std::string> eqs;
    for (int j : keep) {
        out.names.push_back(cs[j].first + ":" + cs[j].second);
        out.b.push_back(b[j]);
        eqs.insert(cs[j].first);
    }
    if (s.se) {
        const int mk = static_cast<int>(keep.size());
        out.V.assign(static_cast<size_t>(mk) * mk, 0.0);
        for (int a = 0; a < mk; ++a)
            for (int c = 0; c < mk; ++c) out.V[a * mk + c] = V(keep[a], keep[c]);
        // e(V) is posted with `ereturn post`, whose variance screen applies: a pair
        // whose correlation exceeds one (e.g. a decomposition term that is zero by
        // construction, such as unexplained1 under weight(1) split, whose variance
        // is rounding residue) makes the posted V zero, with Stata's warning.
        if (!stata_post_vcov_screen(out.V, mk)) {
            out.vWarning = true;
            out.notes.push_back("warning: variance matrix is nonsymmetric or highly singular.");
        }
    }
    out.kEq = static_cast<int>(eqs.size());
    return out;
}

OaxacaResult oaxaca_fit(const DesignMatrix& dm, const std::vector<int>& group, bool relax) {
    OaxacaData d;
    d.n = dm.n;
    d.y = dm.y;
    const int p = dm.k;
    for (int j = 0; j < p - 1; ++j) {
        d.vars.push_back(dm.xnames[j]);
        d.X.emplace_back(dm.X.begin() + static_cast<int64_t>(j) * dm.n, dm.X.begin() + static_cast<int64_t>(j + 1) * dm.n);
        d.regressor.push_back(1);
    }
    d.group = group;
    for (int g : group) d.byv.push_back(g);
    OaxacaSpec s;
    s.tf = 0;
    s.ref = 1;
    s.suest = true;
    s.relax = relax;
    OaxacaOut o = oaxaca_decompose(d, s);
    OaxacaResult r;
    r.names = o.names;
    r.beta = o.b;
    r.V = o.V;
    r.n = {static_cast<int64_t>(o.N1), static_cast<int64_t>(o.N2)};
    // the wrapper's callers read "anything dropped" the way relax reports it
    for (size_t j = 0; j < o.b.size(); ++j)
        if (!o.V.empty() && o.V[j * o.b.size() + j] == 0) r.omitted = true;
    return r;
}
}
