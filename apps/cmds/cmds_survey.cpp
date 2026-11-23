// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include <map>

#include "vcov/svy_vcov.h"

// P3 answerbook wave: `mean` and `proportion` (Stata survey estimation commands).
// Both share one estimator shape — a weighted (or unweighted, w≡1) mean of some
// 0/1 or continuous quantity, with the linearized-variance formula Stata uses for
// `mean`/`proportion` under pweights (the same formula collapses exactly onto the
// textbook unweighted sample-mean SE when every weight is 1, so one code path
// covers both). `proportion` layers a per-level indicator + Stata's default
// logit-transformed CI on top of that shared core.
//
// --wtype is intentionally narrow: only `pw`/`pweight` is accepted. aw/fw/iw carry
// DIFFERENT variance semantics for these two estimators (not `w_i` on both sides of
// the ratio the way pw is) and none of those have been checked against a Stata
// oracle here — guessing would risk a silently wrong SE, so anything else is an
// honest bad_input reject rather than a guess.

namespace cecon {
namespace cli {

namespace {

// Validate --weight/--wtype for mean/proportion. Absent --weight ⇒ unweighted
// (w≡1); present --weight with no --wtype defaults to pw; any --wtype without
// --weight is meaningless (bad_input); any --wtype other than pw/pweight is the
// deliberate honest reject documented above, except the ones `mean` serves
// (allow_aw: aw, and fw/iw, each measured against StataBE) and `proportion`
// serves (allow_fw_iw: fw/iw, BUG-1541).
void check_wtype(const std::vector<std::string>& a, const char* cmd, const std::string& weight,
                 bool allow_aw = false, bool allow_fw_iw = false) {
    bool wtype_set = has(a, "--wtype");
    std::string wtype = opt(a, "--wtype", "pw");
    if (weight.empty()) {
        if (wtype_set)
            throw EngineError("bad_input", std::string(cmd) + ": --wtype requires --weight");
        return;
    }
    if (allow_aw && (wtype == "aw" || wtype == "aweight" || wtype == "fw" || wtype == "fweight" ||
                     wtype == "iw" || wtype == "iweight"))
        return;
    if (allow_fw_iw && (wtype == "fw" || wtype == "fweight" || wtype == "iw" || wtype == "iweight"))
        return;
    if (wtype != "pw" && wtype != "pweight")
        throw EngineError("bad_input",
            std::string(cmd) + ": only probability weights (pw) are supported for " + cmd +
            " in this engine — aweight/fweight/iweight variance semantics for this estimator "
            "are not verified against a Stata oracle here");
}

// Casewise-common estimation sample: row i is kept iff every listed analysis
// column is non-missing AND (if weighted) the weight is non-missing — the same
// "listwise across the whole varlist" rule Stata's estimation commands use (mean/
// proportion behave like regress here, not like summarize's per-variable sample).
std::vector<int64_t> casewise_keep(const std::vector<const double*>& cols, const double* w, int64_t N) {
    std::vector<int64_t> keep;
    for (int64_t i = 0; i < N; ++i) {
        bool ok = true;
        for (const double* c : cols) if (std::isnan(c[i])) { ok = false; break; }
        if (ok && w && std::isnan(w[i])) ok = false;
        if (ok) keep.push_back(i);
    }
    return keep;
}

// NaN → JSON null (house convention — see cmds_summary.cpp's mat()/row["p"] guards);
// never let a raw NaN reach nlohmann's dump().
json nz(double v) { return std::isnan(v) ? json(nullptr) : json(v); }

// The full covariance of K estimates whose variances are scale * Sum_i a_i z_ij^2, with
// the cross terms that formula implies: V_jk = scale * Sum_i a_i z_ij z_ik. `z` holds the
// estimation sample's K centered columns (row r of column j at z[j][r]), `a` one factor
// per row. Emitted as `e_V` (add_e_v) in column order when every entry is finite.
void add_full_e_v(json& out, const std::vector<std::string>& names,
                  const std::vector<std::vector<double>>& z, const std::vector<double>& a,
                  double scale) {
    const size_t k = names.size();
    std::vector<double> V(k * k, 0.0);
    for (size_t j = 0; j < k; ++j) {
        for (size_t l = 0; l <= j; ++l) {
            double s = 0.0;
            for (size_t r = 0; r < a.size(); ++r) s += a[r] * z[j][r] * z[l][r];
            V[j + l * k] = V[l + j * k] = scale * s;
        }
    }
    for (double v : V)
        if (!std::isfinite(v)) return;
    add_e_v(out, names, V);
}

// Render a level value the way categorize() (cmds_summary.cpp) renders numeric
// labels: an integral value prints without a decimal point ("0", not "0.000000").
std::string level_name(double v) {
    char bb[32];
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        std::snprintf(bb, sizeof(bb), "%lld", static_cast<long long>(v));
    else
        std::snprintf(bb, sizeof(bb), "%g", v);
    return bb;
}

// vce(cluster c): the linearized covariance of K estimates whose scores are
// u_ij = w_i z_ij s_j (z the estimation sample's deviations, s_j each column's
// 1/W), V_jl = G/(G-1) Sum_g (Sum_{i in g} u_ij)(Sum_{i in g} u_il), G the number of
// clusters (StataBE 19.5, vs-stata bank bcfd65d0 `mean price mpg, vce(cluster rep78)`:
// V[1,1] 24309.779166264638, e(df_r) G-1 = 4). Row-major k*k; `G` set.
std::vector<double> cluster_v(const std::vector<std::vector<double>>& z, const std::vector<double>& scale,
                              const std::vector<int64_t>& keep, const double* w, const double* cl,
                              int& G) {
    std::map<double, size_t> ids;
    std::vector<size_t> cid(keep.size());
    for (size_t r = 0; r < keep.size(); ++r) {
        auto it = ids.find(cl[keep[r]]);
        if (it == ids.end()) it = ids.emplace(cl[keep[r]], ids.size()).first;
        cid[r] = it->second;
    }
    G = static_cast<int>(ids.size());
    const size_t k = z.size();
    std::vector<std::vector<double>> S(k, std::vector<double>(ids.size(), 0.0));
    for (size_t j = 0; j < k; ++j)
        for (size_t r = 0; r < keep.size(); ++r)
            S[j][cid[r]] += (w ? w[keep[r]] : 1.0) * z[j][r] * scale[j];
    std::vector<double> V(k * k, std::numeric_limits<double>::quiet_NaN());
    if (G < 2) return V;
    const double f = static_cast<double>(G) / (G - 1.0);
    for (size_t j = 0; j < k; ++j)
        for (size_t l = 0; l <= j; ++l) {
            double s = 0;
            for (size_t c = 0; c < ids.size(); ++c) s += S[j][c] * S[l][c];
            V[j * k + l] = V[l * k + j] = f * s;
        }
    return V;
}

// The inverse of the regularized incomplete beta I_x(a, b) in x, by bisection to the
// last representable step (I is monotone in x).
double inv_betai(double a, double b, double prob) {
    if (!(prob > 0.0)) return 0.0;
    if (!(prob < 1.0)) return 1.0;
    double lo = 0.0, hi = 1.0;
    for (int it = 0; it < 2000; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (mid <= lo || mid >= hi) break;
        if (betai(a, b, mid) < prob) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// proportion's citype(): the interval of a proportion p with standard error se on n
// observations at the given level (tc the t critical value of e(df_r)). logit (the
// default) and wald (r(citype) normal) are built on se and t; wilson and agresti on
// the normal critical value and n; exact (Clopper-Pearson) and jeffreys are beta
// quantiles of x = p n successes (StataBE 19.5, vs-stata bank 0c939704 wilson,
// ed38d44c agresti, 85c0ed1c jeffreys, e449fb7a exact, 42c6554f wald).
// The count-based four read n as the sample's observations only for a model-based
// variance. Under a design-based one (vce(cluster), as e(vcetype) Robust) n is the
// effective sample size the variance implies, p(1 - p)/se^2, scaled by (z/tc)^2 so the
// normal-based interval carries the t reference of e(df_r) (adj 2); citype(, unadjusted)
// keeps p(1 - p)/se^2 (adj 1). agresti's upper limit is at most 1 (`agresti_cut` set
// when it was cut); jeffreys' limits are the beta quantiles at every x, 0 and n too.
void prop_ci(const std::string& citype, double p, double se, double n, double level, double tc,
             double& lo, double& hi, int adj = 0, bool* agresti_cut = nullptr) {
    const double alpha = 1.0 - level / 100.0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (citype == "wald") {
        lo = p - tc * se;
        hi = p + tc * se;
        return;
    }
    if (citype != "logit" && adj > 0) {
        n = p * (1.0 - p) / (se * se);
        if (adj == 2) {
            const double r = z_critical(alpha) / tc;
            n *= r * r;
        }
        if (!std::isfinite(n)) {
            lo = hi = nan;
            return;
        }
    }
    if (citype == "wilson" || citype == "agresti") {
        const double z = z_critical(alpha);
        if (citype == "wilson") {
            const double c = (p + z * z / (2.0 * n)) / (1.0 + z * z / n);
            const double h = z / (1.0 + z * z / n) * std::sqrt(p * (1.0 - p) / n + z * z / (4.0 * n * n));
            lo = c - h;
            hi = c + h;
        } else {
            const double nt = n + z * z, pt = (p * n + z * z / 2.0) / nt;
            const double h = z * std::sqrt(pt * (1.0 - pt) / nt);
            lo = pt - h;
            hi = pt + h;
            if (hi > 1.0) {
                hi = 1.0;
                if (agresti_cut) *agresti_cut = true;
            }
        }
        return;
    }
    if (citype == "exact") {
        const double x = p * n;
        lo = x <= 0.0 ? 0.0 : inv_betai(x, n - x + 1.0, alpha / 2.0);
        hi = x >= n ? 1.0 : inv_betai(x + 1.0, n - x, 1.0 - alpha / 2.0);
        return;
    }
    if (citype == "jeffreys") {
        const double x = p * n;
        lo = inv_betai(x + 0.5, n - x + 0.5, alpha / 2.0);
        hi = inv_betai(x + 0.5, n - x + 0.5, 1.0 - alpha / 2.0);
        return;
    }
    // logit; the degenerate p = 0 / 1 (or se 0) collapses onto the estimate
    if (p <= 0.0 || p >= 1.0 || se == 0.0) {
        lo = p; hi = p;
        return;
    }
    const double g = std::log(p / (1.0 - p)), h = tc * se / (p * (1.0 - p));
    lo = 1.0 / (1.0 + std::exp(-(g - h)));
    hi = 1.0 / (1.0 + std::exp(-(g + h)));
}

// A matrix reply {names, matrix} from a k*k row-major array.
json square(const std::vector<std::string>& names, const std::vector<double>& M) {
    const size_t k = names.size();
    json rows = json::array();
    for (size_t i = 0; i < k; ++i) {
        json row = json::array();
        for (size_t j = 0; j < k; ++j) row.push_back(nz(M[i * k + j]));
        rows.push_back(std::move(row));
    }
    return json{{"names", names}, {"matrix", std::move(rows)}};
}

// `mean ..., over(g)` and `proportion ..., over(g)`: every estimate is computed within
// one group of the over() variable (a subpopulation), the groups being its distinct
// values in the estimation sample, ascending. A column is (variable, group) for mean
// and (variable, level, group) for proportion, variable-major. Unweighted, the
// variance of a group's estimates is the plain one computed on that group alone
// (mean: Sum (x-m)(z-mz)/(n_g-1)/n_g; proportion: p(1-p)/n_g and its cross terms);
// under [pw=] it is the linearized one over the whole sample, n/(n-1) Sum w^2 u u'
// with u = w-weighted deviations within the group over the group's weight total.
// Columns of different groups have no covariance. e(N) is the sample's rows and
// e(df_r) N - 1 (StataBE 19.5, vs-stata bank 440bd7fb `mean price mpg, over(foreign)`,
// 0b672af1 `proportion foreign, over(rep78)`). Only the unweighted and [pw=] forms
// are served: the other weights are refused by the caller.
struct OverFit {
    std::vector<std::string> names;  // var@group or var:level@group
    std::vector<double> b, V, nG, sd, freq, Vsrs;  // V, Vsrs row-major k*k
    std::vector<double> groups;
    int64_t n = 0;
    int nclust = 0;  // vce(cluster): the number of clusters, else 0
    // stdize(): the standard strata (ascending), and per stratum x group (row-major
    // S*G) the rows (e(_N_stdsum)) and the group's standard weight (e(_p_stdize))
    std::vector<double> strata, nStd, pStd;
};

// Direct standardization, `stdize(s) stdweight(w)` (unweighted fits): a group's estimate
// is Sum_s pi_s * (the estimate within stratum s of the group), pi_s the stratum's
// standard weight over the total of the strata the group has rows in; its linearized
// score on a row of stratum s is pi_s (v - est_s) / n_gs, and a group's variance
// n_g/(n_g - 1) Sum score score' for a mean, Sum score score' for a proportion (bank
// 6ca1baf8 `proportion foreign ..., over(rep78) stdize(rep78) stdweight(sw)` se
// .0547722557505166 = sqrt(p(1-p)/n_g)). The standard weight must be constant within a
// stratum (r(466)). StataBE 19.5, vs-stata bank 56f2dacb `mean price, over(foreign)
// stdize(rep78) stdweight(sw)`: Foreign 5538.734 over strata 3-5 only, se 402.471.
struct StdizeSpec {
    const double* strata = nullptr;
    const double* weight = nullptr;
};

OverFit over_fit(bool proportion, const std::vector<std::string>& vars,
                 const std::vector<const double*>& col, const double* w, const double* g,
                 int64_t N, const double* cl = nullptr, StdizeSpec sd_spec = {}) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    std::vector<const double*> kc = col;
    kc.push_back(g);
    if (cl) kc.push_back(cl);
    if (sd_spec.strata) {
        kc.push_back(sd_spec.strata);
        kc.push_back(sd_spec.weight);
    }
    const std::vector<int64_t> keep = casewise_keep(kc, w, N);
    OverFit F;
    F.n = static_cast<int64_t>(keep.size());
    for (int64_t i : keep) F.groups.push_back(g[i]);
    std::sort(F.groups.begin(), F.groups.end());
    F.groups.erase(std::unique(F.groups.begin(), F.groups.end()), F.groups.end());
    const size_t G = F.groups.size();
    auto gidx = [&](int64_t i) {
        return static_cast<size_t>(std::lower_bound(F.groups.begin(), F.groups.end(), g[i]) -
                                   F.groups.begin());
    };
    std::vector<double> Wg(G, 0.0), ng(G, 0.0);
    for (int64_t i : keep) {
        Wg[gidx(i)] += w ? w[i] : 1.0;
        ng[gidx(i)] += 1.0;
    }
    // stdize(): the strata and each one's standard weight
    std::vector<double> stdw;
    if (sd_spec.strata) {
        for (int64_t i : keep) F.strata.push_back(sd_spec.strata[i]);
        std::sort(F.strata.begin(), F.strata.end());
        F.strata.erase(std::unique(F.strata.begin(), F.strata.end()), F.strata.end());
        stdw.assign(F.strata.size(), NA);
        for (int64_t i : keep) {
            const size_t s = static_cast<size_t>(
                std::lower_bound(F.strata.begin(), F.strata.end(), sd_spec.strata[i]) - F.strata.begin());
            if (std::isnan(stdw[s])) stdw[s] = sd_spec.weight[i];
            else if (stdw[s] != sd_spec.weight[i])
                throw EngineError("bad_input",
                                  "standardization weights must be constant within standard strata",
                                  "r(466)");
        }
        F.nStd.assign(F.strata.size() * G, 0.0);
        F.pStd.assign(F.strata.size() * G, 0.0);
    }
    const size_t S = F.strata.size();
    auto sidx = [&](int64_t i) {
        return static_cast<size_t>(std::lower_bound(F.strata.begin(), F.strata.end(), sd_spec.strata[i]) -
                                   F.strata.begin());
    };
    if (sd_spec.strata) {
        for (int64_t i : keep) F.nStd[sidx(i) * G + gidx(i)] += 1.0;
        for (size_t h = 0; h < G; ++h) {
            double tot = 0;
            for (size_t s2 = 0; s2 < S; ++s2)
                if (F.nStd[s2 * G + h] > 0) tot += stdw[s2];
            for (size_t s2 = 0; s2 < S; ++s2)
                if (F.nStd[s2 * G + h] > 0) F.pStd[s2 * G + h] = stdw[s2] / tot;
        }
    }
    // per column: its group and its deviation over the sample's rows (0 outside the group)
    std::vector<size_t> cg;
    std::vector<std::vector<double>> z;
    const size_t n = keep.size();
    for (size_t j = 0; j < vars.size(); ++j) {
        std::vector<double> levels;
        if (proportion) {
            for (int64_t i : keep) levels.push_back(col[j][i]);
            std::sort(levels.begin(), levels.end());
            levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        } else {
            levels.push_back(NA);
        }
        for (double L : levels) {
            for (size_t h = 0; h < G; ++h) {
                if (sd_spec.strata) {
                    // the standardized estimate and its scores (pi_s (v - est_s) / n_gs)
                    std::vector<double> sum(S, 0.0);
                    for (int64_t i : keep)
                        if (gidx(i) == h)
                            sum[sidx(i)] += proportion ? (col[j][i] == L ? 1.0 : 0.0) : col[j][i];
                    double est = 0;
                    for (size_t s2 = 0; s2 < S; ++s2) {
                        const double ns = F.nStd[s2 * G + h];
                        if (ns > 0) est += F.pStd[s2 * G + h] * sum[s2] / ns;
                    }
                    std::vector<double> zz(n, 0.0);
                    for (size_t r = 0; r < n; ++r) {
                        const int64_t i = keep[r];
                        if (gidx(i) != h) continue;
                        const size_t s2 = sidx(i);
                        const double ns = F.nStd[s2 * G + h];
                        const double v = proportion ? (col[j][i] == L ? 1.0 : 0.0) : col[j][i];
                        zz[r] = F.pStd[s2 * G + h] * (v - sum[s2] / ns) / ns;
                    }
                    F.names.push_back(proportion ? vars[j] + ":" + level_name(L) + "@" + level_name(F.groups[h])
                                                 : vars[j] + "@" + level_name(F.groups[h]));
                    F.b.push_back(est);
                    F.nG.push_back(ng[h]);
                    F.sd.push_back(NA);
                    F.freq.push_back(est * Wg[h]);
                    cg.push_back(h);
                    z.push_back(std::move(zz));
                    continue;
                }
                double s = 0;
                for (int64_t i : keep)
                    if (gidx(i) == h)
                        s += (w ? w[i] : 1.0) * (proportion ? (col[j][i] == L ? 1.0 : 0.0) : col[j][i]);
                const double est = Wg[h] > 0 ? s / Wg[h] : NA;
                std::vector<double> zz(n, 0.0);
                double ssd = 0;
                for (size_t r = 0; r < n; ++r) {
                    const int64_t i = keep[r];
                    if (gidx(i) != h) continue;
                    const double v = proportion ? (col[j][i] == L ? 1.0 : 0.0) : col[j][i];
                    zz[r] = v - est;
                    const double wi = w ? w[i] : 1.0;
                    ssd += (w ? wi * ng[h] / Wg[h] : 1.0) * zz[r] * zz[r];
                }
                F.names.push_back(proportion ? vars[j] + ":" + level_name(L) + "@" + level_name(F.groups[h])
                                             : vars[j] + "@" + level_name(F.groups[h]));
                F.b.push_back(est);
                F.nG.push_back(ng[h]);
                F.sd.push_back(ng[h] > 1 ? std::sqrt(ssd / (ng[h] - 1.0)) : NA);
                F.freq.push_back(est * Wg[h]);  // p*W, as Stata forms e(freq)
                cg.push_back(h);
                z.push_back(std::move(zz));
            }
        }
    }
    const size_t k = F.names.size();
    F.V.assign(k * k, 0.0);
    F.Vsrs.assign(k * k, 0.0);
    const double nn = static_cast<double>(n);
    for (size_t c = 0; c < k; ++c) {
        for (size_t d = 0; d <= c; ++d) {
            if (cg[c] != cg[d]) continue;
            const size_t h = cg[c];
            double s = 0;
            for (size_t r = 0; r < n; ++r) {
                const double wi = w ? w[keep[r]] : 1.0;
                s += (w ? wi * wi : 1.0) * z[c][r] * z[d][r];
            }
            double v;
            if (sd_spec.strata)  // a proportion's as its plain p(1-p)/n: no n/(n-1)
                v = proportion ? s : ng[h] > 1 ? ng[h] / (ng[h] - 1.0) * s : NA;
            else if (w)
                v = nn > 1 ? nn / (nn - 1.0) * s / (Wg[h] * Wg[h]) : NA;
            else if (proportion)
                v = s / (Wg[h] * Wg[h]);
            else
                v = ng[h] > 1 ? s / (ng[h] - 1.0) / ng[h] : NA;
            if (!cl) F.V[c * k + d] = F.V[d * k + c] = v;
            // under pw, the group's simple-random-sampling covariance, Sum w z z'/W_g/(n_g-1)
            // (vs-stata bank a19f5e8f `mean wage [pw=hours], over(collgrad)`)
            if ((w || cl) && !sd_spec.strata) {
                double sw = 0;
                for (size_t r = 0; r < n; ++r) sw += (w ? w[keep[r]] : 1.0) * z[c][r] * z[d][r];
                F.Vsrs[c * k + d] = F.Vsrs[d * k + c] =
                    ng[h] > 1 ? sw / Wg[h] / (ng[h] - 1.0) : NA;
            }
        }
    }
    // vce(cluster): every pair of columns, groups crossed, over the clusters' totals
    if (cl) {
        std::vector<double> scale(k);
        for (size_t c = 0; c < k; ++c) scale[c] = 1.0 / Wg[cg[c]];
        F.V = cluster_v(z, scale, keep, w, cl, F.nclust);
    }
    return F;
}

// The reply of an over() fit, in the shape of the plain one.
json over_reply(const char* estimator, const OverFit& F, const double* w, const std::string& weight,
                double level, bool proportion, const std::string& citype = "logit",
                bool ci_unadjusted = false) {
    const double dfree = F.nclust > 0 ? F.nclust - 1.0 : static_cast<double>(F.n - 1);
    const double tc = t_critical(1.0 - level / 100.0, dfree);
    json coefs = json::array(), coef = json::object(), se_map = json::object(), t_map = json::object(),
         p_map = json::object(), lo_map = json::object(), hi_map = json::object();
    const size_t k = F.names.size();
    // vce(cluster) is a design-based variance: the count-based intervals read the
    // effective n (prop_ci)
    const int adj = F.nclust > 0 ? (ci_unadjusted ? 1 : 2) : 0;
    bool agresti_cut = false;
    for (size_t c = 0; c < k; ++c) {
        const double b = F.b[c], se = std::sqrt(F.V[c * k + c]);
        const double t = se > 0 ? b / se : std::numeric_limits<double>::quiet_NaN();
        const double p = std::isnan(t) ? t : t_twosided_pvalue(t, dfree);
        double lo = b - tc * se, hi = b + tc * se;
        if (proportion) prop_ci(citype, b, se, F.nG[c], level, tc, lo, hi, adj, &agresti_cut);
        const std::string& nm = F.names[c];
        coefs.push_back({{"name", nm}, {"b", nz(b)}, {"se", nz(se)}, {"t", nz(t)}, {"p", nz(p)},
                         {"ci_lo", nz(lo)}, {"ci_hi", nz(hi)}});
        coef[nm] = nz(b); se_map[nm] = nz(se); t_map[nm] = nz(t); p_map[nm] = nz(p);
        lo_map[nm] = nz(lo); hi_map[nm] = nz(hi);
    }
    json out = {{"estimator", estimator}, {"n_obs", F.n}, {"n_rows", F.n}, {"level", level},
                {"n_clusters", F.nclust},
                {"coefs", coefs}, {"df", dfree}, {"coef", coef}, {"se", se_map}, {"tstat", t_map},
                {"pvalue", p_map}, {"ci_lo", lo_map}, {"ci_hi", hi_map}};
    if (w) out["weight"] = {{"var", weight}, {"type", "pw"}};
    if (agresti_cut) out["agresti_truncated"] = true;
    json groups = json::array();
    for (double gv : F.groups) groups.push_back(level_name(gv));
    out["over_levels"] = groups;
    json nG = json::array(), extra = json::array();
    for (size_t c = 0; c < k; ++c) {
        nG.push_back(F.nG[c]);
        extra.push_back(nz(proportion ? F.freq[c] : F.sd[c]));
    }
    out["e__N"] = nG;
    if (!F.strata.empty()) {
        // stdize(): no e(sd); e(_N_stdsum) and e(_p_stdize), strata x groups
        json ns = json::array(), ps = json::array();
        const size_t G = F.groups.size();
        for (size_t s2 = 0; s2 < F.strata.size(); ++s2) {
            json rn = json::array(), rp = json::array();
            for (size_t h = 0; h < G; ++h) {
                rn.push_back(F.nStd[s2 * G + h]);
                rp.push_back(F.pStd[s2 * G + h]);
            }
            ns.push_back(std::move(rn));
            ps.push_back(std::move(rp));
        }
        out["e__N_stdsum"] = ns;
        out["e__p_stdize"] = ps;
        out["n_stdize"] = F.strata.size();
        if (proportion) out["e_freq"] = extra;
    } else {
        out[proportion ? "e_freq" : "e_sd"] = extra;
    }
    bool finite = true;
    for (double v : F.V) finite = finite && std::isfinite(v);
    if (finite) out["e_V"] = square(F.names, F.V);
    if ((w || F.nclust > 0) && !proportion) out["e_V_srs"] = square(F.names, F.Vsrs);
    return out;
}

}  // namespace

// ── mean ─────────────────────────────────────────────────────────────────────
int cmd_mean(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    if (data_missing(data) || vars.empty()) {
        std::fprintf(stderr, "usage: cobra mean --data f.hbin --vars Y[,Y2,...] "
                             "[--weight W --wtype pw|aw|fw|iw] [--level P] [--keepif EXPR] [--out f.json]\n"
                             "  --weight W   Stata [pw=W] (default), [aw=W], [fw=W] or [iw=W]\n"
                             "  --level P    confidence level for ci_lo/ci_hi, 10 < P <= 99.99 (default 95).\n");
        return 2;
    }
    std::string weight = opt(a, "--weight");
    check_wtype(a, "mean", weight, /*allow_aw=*/true);
    // Stata `mean [aw=]` (w2aj_qualifiers.log:1983 qf_mn_aw_sp): b = Sum wx/Sum w and
    // se = sqrt(Sum v(x-b)^2/(n-1)/n) with v = w*n/Sum w, e(N) the row count.
    // `mean [fw=]` / `[iw=]`: the replicated sample, W = Sum w observations:
    // se = sqrt(Sum w(x-b)^2/(W-1)/W), e(N) = W, e(df_r) = W - 1, an iweight's
    // fractional W included (StataBE 19.5, scratch-laneA/agents/eng-ml/x1.log E1-E3:
    // [fw=wi] e(N) 30, se .79196757741599555, e(df_r) 29; [iw=wf] e(N) 28.5,
    // b 6.1842105263157894, se .80066889479364045, e(df_r) 27.5).
    const std::string wtype_in = opt(a, "--wtype", "pw");
    const bool analytic_w = !weight.empty() && (wtype_in == "aw" || wtype_in == "aweight");
    const bool freq_fw = !weight.empty() && (wtype_in == "fw" || wtype_in == "fweight");
    const bool freq_w = freq_fw || (!weight.empty() && (wtype_in == "iw" || wtype_in == "iweight"));
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "mean: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    const EsampleRequest esreq = esample_request(a, "mean");
    const std::string data0 = data, keepif = opt(a, "--keepif");
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    std::vector<const double*> col(vars.size());
    for (size_t j = 0; j < vars.size(); ++j) col[j] = require_num(ds, "mean", vars[j]);
    const double* w = weight.empty() ? nullptr : require_num(ds, "mean", weight);
    const std::string over = opt(a, "--over");
    if (!over.empty()) {
        if (analytic_w || freq_w || has(a, "--svy"))
            throw EngineError("bad_input", "mean: --over is served unweighted or with pw weights only");
        const double* g = require_num(ds, "mean", over);
        const std::string clv = opt(a, "--cluster");
        const double* cl = clv.empty() ? nullptr : require_num(ds, "mean", clv);
        // stdize(s) stdweight(w): direct standardization (unweighted, no vce(cluster))
        StdizeSpec sds;
        const std::string szv = opt(a, "--stdize"), swv = opt(a, "--stdweight");
        if (!szv.empty()) {
            if (w || cl || swv.empty())
                throw EngineError("bad_input",
                                  "mean: --stdize is served with --stdweight, unweighted and without --cluster");
            sds.strata = require_num(ds, "mean", szv);
            sds.weight = require_num(ds, "mean", swv);
        }
        OverFit F = over_fit(false, vars, col, w, g, N, cl, sds);
        if (F.n == 0) throw EngineError("no_obs", "no observations", "r(2000)");
        json out = over_reply("mean", F, w, weight, level, false);
        std::vector<const double*> kc = col;
        kc.push_back(g);
        if (cl) kc.push_back(cl);
        if (sds.strata) {
            kc.push_back(sds.strata);
            kc.push_back(sds.weight);
        }
        EsampleRows esrows;
        esrows.loaded = &ds;
        esrows.keep = casewise_keep(kc, w, N);
        emit_esample(esreq, "mean", data0, keepif, false, esrows, out);
        emit(out, opt(a, "--out"));
        return 0;
    }

    // P7 `svy: mean`. The single-stage formula below is exactly the degenerate case
    // of the survey one (one stratum whose PSUs are the observations), so svy does
    // not replace it — it generalizes the variance to PSU totals centered within
    // stratum, and the reference dof from n−1 to #PSU − #strata.
    const bool svy = has(a, "--svy");
    std::string psu_var = opt(a, "--psu"), strata_var = opt(a, "--strata");
    const double *psu_col = nullptr, *strata_col = nullptr;
    if (svy && (analytic_w || freq_w))
        throw EngineError("bad_input", "mean: --svy takes probability weights only");
    if (svy) {
        // --strata is optional: a design without strata() is one stratum, as
        // run_feols_svy reads it (BUG-1424: StataBE `svyset id [pweight=w]`, `svy:
        // mean y` e(N_strata) 1, e(df_r) = #PSU - 1 = 19, _se[y] .023028889911831222).
        if (psu_var.empty())
            throw EngineError("bad_input", "mean: --svy requires --psu");
        if (weight.empty())
            throw EngineError("bad_input",
                "mean: --svy requires --weight W --wtype pw (a survey design is a "
                "probability-weighted sample)");
        psu_col = require_num(ds, "mean", psu_var);
        if (!strata_var.empty()) strata_col = require_num(ds, "mean", strata_var);
    } else if (!psu_var.empty() || !strata_var.empty()) {
        throw EngineError("bad_input", "mean: --psu/--strata are only meaningful with --svy");
    }

    std::vector<const double*> keep_cols = col;
    if (svy) {
        keep_cols.push_back(psu_col);
        if (strata_col) keep_cols.push_back(strata_col);
    }
    // vce(cluster c): unweighted or pw only; the rows with the cluster present
    const std::string clv = opt(a, "--cluster");
    const double* cl = nullptr;
    if (!clv.empty()) {
        if (svy || analytic_w || freq_w)
            throw EngineError("bad_input", "mean: --cluster is served unweighted or with pw weights only");
        cl = require_num(ds, "mean", clv);
        keep_cols.push_back(cl);
    }
    std::vector<int64_t> keep = casewise_keep(keep_cols, w, N);
    // A zero aweight or pweight leaves the sample: `mean price [aw=foreign]` is
    // e(N) 22, e(sample) 22 rows (vs-stata bank bce4d4a312c17223). A zero
    // fweight stays in e(sample) but counts no row of e(_N) (da06b6d418f07568
    // `[fw=foreign]`: e(sample) 74 rows, e(N) 22, e(_N) 22); an iweight stays.
    {
        const bool keep0 = !weight.empty() && (wtype_in == "iw" || wtype_in == "iweight" ||
                                               wtype_in == "fw" || wtype_in == "fweight");
        if (w && !keep0) {
            std::vector<int64_t> kept;
            kept.reserve(keep.size());
            for (int64_t i : keep)
                if (w[i] != 0.0) kept.push_back(i);
            keep.swap(kept);
        }
    }
    const int64_t n = static_cast<int64_t>(keep.size());
    // No row with every variable present: r(2000) "no observations" (vs-stata
    // bank 3d0ad29226d8345e `mean x` over an all-missing x, 79a62c511d36edff).
    if (n == 0) throw EngineError("no_obs", "no observations", "r(2000)");
    double W = 0; for (int64_t i : keep) W += w ? w[i] : 1.0;
    // A negative aweight, fweight or pweight is Stata's r(402) "negative weights
    // encountered", ahead of a frequency weight's integrality (vs-stata bank
    // 0364c489f733be11 `mean price [aw=w]`, bc4516167c8c8a50 `[fw=w]`, w = +-0.5).
    // An iweight may be negative.
    const bool importance_w = !weight.empty() && (wtype_in == "iw" || wtype_in == "iweight");
    if (w && !importance_w)
        for (int64_t i : keep)
            if (w[i] < 0.0) throw EngineError("bad_input", "negative weights encountered", "r(402)");
    if (freq_fw)
        for (int64_t i : keep)
            if (w[i] != std::floor(w[i]))
                throw EngineError("bad_input", "mean: fweight '" + weight + "' has a non-integer value");

    cecon::Factor f_psu, f_strata;
    int design_df = 0;
    if (svy) {
        auto densify = [&](const double* src, const std::string& nm) {
            cecon::Factor f;
            f.name = nm;
            f.codes.resize(static_cast<size_t>(n));
            std::map<double, int> lev;
            for (int64_t r = 0; r < n; ++r) {
                double v = src[keep[static_cast<size_t>(r)]];
                auto it = lev.find(v);
                if (it == lev.end()) it = lev.emplace(v, static_cast<int>(lev.size())).first;
                f.codes[static_cast<size_t>(r)] = it->second;
            }
            f.n_levels = static_cast<int>(lev.size());
            f.count.assign(static_cast<size_t>(f.n_levels), 0.0);
            for (int c : f.codes) f.count[static_cast<size_t>(c)] += 1.0;
            return f;
        };
        f_psu = densify(psu_col, psu_var);
        if (strata_col) {
            f_strata = densify(strata_col, strata_var);
        } else {
            f_strata.n_levels = 1;
            f_strata.codes.assign(static_cast<size_t>(n), 0);
            f_strata.count.assign(1, static_cast<double>(n));
        }
        design_df = f_psu.n_levels - f_strata.n_levels;
        if (design_df <= 0)
            throw EngineError("bad_input",
                "mean: svy design degrees of freedom (#PSU − #strata) is not positive");
    }

    std::vector<double> Vcl;
    int nclust = 0;
    if (cl) {
        std::vector<std::vector<double>> z(vars.size(), std::vector<double>(static_cast<size_t>(n)));
        std::vector<double> scale(vars.size(), 1.0 / W);
        for (size_t j = 0; j < vars.size(); ++j) {
            double sw_y = 0;
            for (int64_t i : keep) sw_y += (w ? w[i] : 1.0) * col[j][i];
            const double mu = sw_y / W;
            for (int64_t r = 0; r < n; ++r)
                z[j][static_cast<size_t>(r)] = col[j][keep[static_cast<size_t>(r)]] - mu;
        }
        Vcl = cluster_v(z, scale, keep, w, cl, nclust);
    }
    const double dfree = svy ? static_cast<double>(design_df)
                         : cl ? nclust - 1.0
                         : freq_w ? W - 1.0 : static_cast<double>(n - 1);
    const double alpha = 1.0 - level / 100.0;
    const double tc = t_critical(alpha, dfree);

    // coef/se are not enough. `--tidy`'s coeftable builder (tidy_coeftable) takes the
    // coef{} branch whenever coef{} exists, and reads t/p/CI from the SIBLING maps
    // tstat/pvalue/ci_lo/ci_hi — never from coefs[]. Emitting coef/se alone therefore
    // renders a table with the estimate and SE filled in and t, p and the confidence
    // interval blank, which for `mean`/`proportion` is exactly the column Stata puts
    // the CI in. Every map below is filled from the SAME per-row values as coefs[], so
    // the two representations cannot drift apart.
    json coef = json::object(), se_map = json::object(), coefs = json::array();
    json t_map = json::object(), p_map = json::object(),
         lo_map = json::object(), hi_map = json::object();
    for (size_t j = 0; j < vars.size(); ++j) {
        double sw_y = 0; for (int64_t i : keep) sw_y += (w ? w[i] : 1.0) * col[j][i];
        double mu = W > 0 ? sw_y / W : std::numeric_limits<double>::quiet_NaN();
        double V;
        if (svy) {
            // Linearized variance of the ratio estimator: u_i = w_i (y_i − µ̂)/Ŵ,
            // then the same stratified PSU-total meat every svy estimator uses
            // (p = 1 here, and there is no bread because dµ/dµ = 1).
            std::vector<double> u(static_cast<size_t>(n));
            for (int64_t r = 0; r < n; ++r) {
                int64_t i = keep[static_cast<size_t>(r)];
                u[static_cast<size_t>(r)] = (w ? w[i] : 1.0) * (col[j][i] - mu) / W;
            }
            cecon::SvyMeatResult sm =
                cecon::svy_linearized_meat(u.data(), n, 1, f_psu, f_strata, nullptr);
            if (!sm.singleton_stratum.empty())
                throw EngineError("bad_input",
                    "mean: svy stratum variable '" + sm.singleton_stratum +
                        "' has a stratum with a single sampling unit; the linearized "
                        "variance is undefined there (Stata reports missing)");
            if (!sm.ok)
                throw EngineError("bad_input",
                    "mean: svy sampling units are not nested within strata");
            V = sm.meat[0];
        } else if (cl) {
            V = Vcl[j * vars.size() + j];
        } else if (analytic_w) {
            double ss = 0;
            for (int64_t i : keep) {
                const double v = w[i] * static_cast<double>(n) / W;
                ss += v * (col[j][i] - mu) * (col[j][i] - mu);
            }
            V = (n > 1 && W > 0) ? ss / static_cast<double>(n - 1) / static_cast<double>(n)
                                 : std::numeric_limits<double>::quiet_NaN();
        } else if (freq_w) {
            double ss = 0;
            for (int64_t i : keep) ss += w[i] * (col[j][i] - mu) * (col[j][i] - mu);
            V = (W > 1) ? ss / (W - 1.0) / W : std::numeric_limits<double>::quiet_NaN();
        } else {
            double ss = 0;
            for (int64_t i : keep) { double t = (w ? w[i] : 1.0) * (col[j][i] - mu); ss += t * t; }
            V = (n > 1 && W > 0) ? (static_cast<double>(n) / (n - 1)) * ss / (W * W)
                                 : std::numeric_limits<double>::quiet_NaN();
        }
        double se = std::sqrt(V);
        double t = se > 0 ? mu / se : std::numeric_limits<double>::quiet_NaN();
        double p = std::isnan(t) ? std::numeric_limits<double>::quiet_NaN() : t_twosided_pvalue(t, dfree);
        double ci_lo = mu - tc * se, ci_hi = mu + tc * se;
        coefs.push_back({{"name", vars[j]}, {"b", nz(mu)}, {"se", nz(se)}, {"t", nz(t)}, {"p", nz(p)},
                         {"ci_lo", nz(ci_lo)}, {"ci_hi", nz(ci_hi)}});
        coef[vars[j]] = nz(mu); se_map[vars[j]] = nz(se);
        t_map[vars[j]] = nz(t); p_map[vars[j]] = nz(p);
        lo_map[vars[j]] = nz(ci_lo); hi_map[vars[j]] = nz(ci_hi);
    }
    // e(N): the row count, or the weight sum under fw/iw.
    json out = {{"estimator", "mean"}, {"n_obs", freq_w ? json(W) : json(n)}, {"sum_w", W}, {"level", level},
                {"coefs", coefs}, {"df", dfree}, {"coef", coef}, {"se", se_map},
                {"tstat", t_map}, {"pvalue", p_map}, {"ci_lo", lo_map}, {"ci_hi", hi_map}};
    if (w)  // normalize the long spellings ("pweight" ...) to the two-letter ones
        out["weight"] = {{"var", weight},
                         {"type", analytic_w ? "aw" : freq_fw ? "fw" : freq_w ? "iw" : "pw"}};
    // e(V): each variance above with its cross-variable terms, Sum a_i z_ij z_ik with z
    // the centered values (StataBE 19.5, scratch-laneA2/agents/epost/s11.log, `mean x x2`
    // V[2,1] .013257575757575754; [aw=] -.087530428943171312, [fw=] -.015517241379310339,
    // [iw=] -.035012171577268526, [pw=] -.11170982699362622). The svy design's is not
    // built here.
    if (cl) {
        bool finite = true;
        for (double v : Vcl) finite = finite && std::isfinite(v);
        if (finite) out["e_V"] = square(vars, Vcl);
        out["n_clusters"] = nclust;
    } else if (!svy && n > 1 && W > 0 && (!freq_w || W > 1)) {
        std::vector<std::vector<double>> z(vars.size(),
                                           std::vector<double>(static_cast<size_t>(n)));
        std::vector<double> a(static_cast<size_t>(n));
        for (size_t j = 0; j < vars.size(); ++j) {
            double sw_y = 0;
            for (int64_t i : keep) sw_y += (w ? w[i] : 1.0) * col[j][i];
            const double mu = sw_y / W;
            for (int64_t r = 0; r < n; ++r)
                z[j][static_cast<size_t>(r)] = col[j][keep[static_cast<size_t>(r)]] - mu;
        }
        for (int64_t r = 0; r < n; ++r) {
            const double wi = w ? w[keep[static_cast<size_t>(r)]] : 1.0;
            a[static_cast<size_t>(r)] = analytic_w ? wi * static_cast<double>(n) / W
                                        : freq_w   ? wi
                                                   : wi * wi;
        }
        const double nn = static_cast<double>(n);
        const double scale = analytic_w ? 1.0 / (nn - 1.0) / nn
                             : freq_w   ? 1.0 / (W - 1.0) / W
                                        : nn / (nn - 1.0) / (W * W);
        add_full_e_v(out, vars, z, a, scale);
    }
    if (svy)
        out["svy"] = {{"strata_var", strata_var}, {"psu_var", psu_var},
                      {"n_strata", f_strata.n_levels}, {"n_psu", f_psu.n_levels},
                      {"design_df", design_df}, {"pop_size", W}};
    // mean's per-variable e() matrices (StataBE 19.5, vs-stata bank 85536cc0, 0f5dfbc8,
    // 3979e702, 3d498c4d, 7df7f233): e(_N) the sample's rows; e(sd) the standard
    // deviation, sqrt(Sum (x-m)^2/(n-1)) unweighted, sqrt(Sum w(x-m)^2/(W-1)) under fw/iw,
    // sqrt(Sum v(x-m)^2/(n-1)) with v = w n/W under aw/pw; e(_N_subp) the weight total
    // under fw/iw; e(V_srs) under pw, the simple-random-sampling covariance
    // Sum w(x-mx)(z-mz)/W/(n-1).
    if (!svy) {
        const double nn = static_cast<double>(n);
        std::vector<double> mus(vars.size());
        for (size_t j = 0; j < vars.size(); ++j) {
            double sw_y = 0;
            for (int64_t i : keep) sw_y += (w ? w[i] : 1.0) * col[j][i];
            mus[j] = W > 0 ? sw_y / W : std::numeric_limits<double>::quiet_NaN();
        }
        json nvec = json::array(), sdv = json::array();
        for (size_t j = 0; j < vars.size(); ++j) {
            double ss = 0;
            for (int64_t i : keep) {
                const double d = col[j][i] - mus[j];
                const double a_i = !w ? 1.0 : freq_w ? w[i] : w[i] * nn / W;
                ss += a_i * d * d;
            }
            const double den = freq_w ? W - 1.0 : nn - 1.0;
            int64_t nrow = n;
            if (w && freq_fw) {
                nrow = 0;
                for (int64_t i : keep) nrow += w[i] != 0.0 ? 1 : 0;
            }
            nvec.push_back(nrow);
            sdv.push_back(nz(den > 0 ? std::sqrt(ss / den) : std::numeric_limits<double>::quiet_NaN()));
        }
        out["e__N"] = nvec;
        out["e_sd"] = sdv;
        out["n_rows"] = n;
        if (freq_w) out["e__N_subp"] = W;
        if ((cl || (w && !analytic_w && !freq_w)) && n > 1 && W > 0) {
            const size_t k = vars.size();
            std::vector<double> M(k * k);
            for (size_t j = 0; j < k; ++j)
                for (size_t l = 0; l < k; ++l) {
                    double s = 0;
                    for (int64_t i : keep) s += (w ? w[i] : 1.0) * (col[j][i] - mus[j]) * (col[l][i] - mus[l]);
                    M[j * k + l] = s / W / (nn - 1.0);
                }
            out["e_V_srs"] = square(vars, M);
        }
        EsampleRows esrows;
        esrows.loaded = &ds;
        esrows.keep = keep;
        emit_esample(esreq, "mean", data0, keepif, false, esrows, out);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

// ── proportion ───────────────────────────────────────────────────────────────
int cmd_proportion(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    if (data_missing(data) || vars.empty()) {
        std::fprintf(stderr, "usage: cobra proportion --data f.hbin --vars V[,V2,...] "
                             "[--weight W --wtype pw|fw|iw] [--level P] [--keepif EXPR] "
                             "[--out f.json]\n"
                             "  --weight W   Stata [pw=W] (default), [fw=W] or [iw=W]\n"
                             "  --level P    confidence level for ci_lo/ci_hi, 10 < P <= 99.99 (default 95);\n"
                             "               CI uses Stata's default logit transform.\n");
        return 2;
    }
    std::string weight = opt(a, "--weight");
    check_wtype(a, "proportion", weight, /*allow_aw=*/false, /*allow_fw_iw=*/true);
    // `proportion [fw=]` / `[iw=]` (BUG-1541): the replicated sample of W = Sum w
    // observations, as `mean`'s (e(N) = W, e(df_r) = W - 1), with Stata's analytic
    // variance of a proportion, Sum w(ind - p)^2/W^2 = p(1 - p)/W -- no small-sample
    // factor (_svy_summarize.ado's AdjV4prop scales the SRS variance by (n-1)/W)
    // (StataBE 19.5, scratch-laneA/agents/file-new-2/m1.log: [fw=wi] e(N) 30, e(df_r)
    // 29, V[1,1] .0074074074074074051, V[1,2] -.0074074074074074051; [iw=wf] e(N)
    // 28.5, e(df_r) 27.5, V[1,1] .0077972709551656898).
    const std::string wtype_in = opt(a, "--wtype", "pw");
    const bool freq_fw = !weight.empty() && (wtype_in == "fw" || wtype_in == "fweight");
    const bool freq_w = freq_fw || (!weight.empty() && (wtype_in == "iw" || wtype_in == "iweight"));
    const std::string citype = opt(a, "--citype", "logit");
    if (citype != "logit" && citype != "wald" && citype != "wilson" && citype != "agresti" &&
        citype != "exact" && citype != "jeffreys")
        throw EngineError("bad_input", "proportion: unknown --citype " + citype);
    // citype(, unadjusted): under a design-based variance the count-based intervals
    // read p(1 - p)/se^2 as n without the t-to-normal scaling (prop_ci)
    const bool ci_unadjusted = has(a, "--ci-unadjusted");
    // the count-based intervals read n as the sample's observations: unweighted only
    if (!weight.empty() && citype != "logit" && citype != "wald")
        throw EngineError("bad_input",
                          "proportion: --citype " + citype + " is served for unweighted fits only");
    // An unweighted proportion takes that same analytic variance with w = 1, p(1 - p)/n
    // (AdjV4prop applies to it too): StataBE 19.5 (scratch-laneA2/agents/wml/x2.log)
    // `proportion d` on 12 rows, p = 2/3: V[1,1] .018518518518518514 = (2/9)/12,
    // _se[1.d] .13608276348795431, where the linearized n/(n-1) form gave (2/9)/11.
    // Only a pweight keeps the linearized variance.
    const bool analytic = freq_w || weight.empty();
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "proportion: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    const EsampleRequest esreq = esample_request(a, "proportion");
    const std::string data0 = data, keepif = opt(a, "--keepif");
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    std::vector<const double*> col(vars.size());
    for (size_t j = 0; j < vars.size(); ++j) col[j] = require_num(ds, "proportion", vars[j]);
    const double* w = weight.empty() ? nullptr : require_num(ds, "proportion", weight);
    const std::string over = opt(a, "--over");
    if (!over.empty()) {
        if (freq_w)
            throw EngineError("bad_input",
                              "proportion: --over is served unweighted or with pw weights only");
        const double* g = require_num(ds, "proportion", over);
        const std::string clv = opt(a, "--cluster");
        const double* cl = clv.empty() ? nullptr : require_num(ds, "proportion", clv);
        // stdize(s) stdweight(w): direct standardization (unweighted, no vce(cluster))
        StdizeSpec sds;
        const std::string szv = opt(a, "--stdize"), swv = opt(a, "--stdweight");
        if (!szv.empty()) {
            if (w || cl || swv.empty())
                throw EngineError("bad_input",
                                  "proportion: --stdize is served with --stdweight, unweighted and without --cluster");
            sds.strata = require_num(ds, "proportion", szv);
            sds.weight = require_num(ds, "proportion", swv);
        }
        OverFit F = over_fit(true, vars, col, w, g, N, cl, sds);
        if (F.n == 0) throw EngineError("no_obs", "no observations", "r(2000)");
        json out = over_reply("proportion", F, w, weight, level, true, citype, ci_unadjusted);
        std::vector<const double*> kc = col;
        kc.push_back(g);
        if (cl) kc.push_back(cl);
        if (sds.strata) {
            kc.push_back(sds.strata);
            kc.push_back(sds.weight);
        }
        EsampleRows esrows;
        esrows.loaded = &ds;
        esrows.keep = casewise_keep(kc, w, N);
        emit_esample(esreq, "proportion", data0, keepif, false, esrows, out);
        emit(out, opt(a, "--out"));
        return 0;
    }

    // vce(cluster c): unweighted or pw only; the rows with the cluster present
    const std::string clv = opt(a, "--cluster");
    const double* cl = nullptr;
    std::vector<const double*> keep_cols = col;
    if (!clv.empty()) {
        if (freq_w)
            throw EngineError("bad_input", "proportion: --cluster is served unweighted or with pw weights only");
        cl = require_num(ds, "proportion", clv);
        keep_cols.push_back(cl);
    }
    std::vector<int64_t> keep = casewise_keep(keep_cols, w, N);
    const int64_t n = static_cast<int64_t>(keep.size());
    double W = 0; for (int64_t i : keep) W += w ? w[i] : 1.0;
    if (freq_fw)
        for (int64_t i : keep)
            if (w[i] != std::floor(w[i]))
                throw EngineError("bad_input",
                                  "proportion: fweight '" + weight + "' has a non-integer value");
    // under vce(cluster) every level's centred indicator first, for the clusters' totals
    std::vector<double> Vcl;
    int nclust = 0;
    if (cl) {
        std::vector<std::vector<double>> z;
        for (size_t j = 0; j < vars.size(); ++j) {
            std::vector<double> levels;
            for (int64_t i : keep) levels.push_back(col[j][i]);
            std::sort(levels.begin(), levels.end());
            levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
            for (double L : levels) {
                double sw_ind = 0;
                for (int64_t i : keep) if (col[j][i] == L) sw_ind += w ? w[i] : 1.0;
                const double p = sw_ind / W;
                z.emplace_back(static_cast<size_t>(n));
                for (int64_t r = 0; r < n; ++r)
                    z.back()[static_cast<size_t>(r)] = (col[j][keep[static_cast<size_t>(r)]] == L ? 1.0 : 0.0) - p;
            }
        }
        Vcl = cluster_v(z, std::vector<double>(z.size(), 1.0 / W), keep, w, cl, nclust);
    }
    const double dfree = cl ? nclust - 1.0 : freq_w ? W - 1.0 : static_cast<double>(n - 1);
    const double alpha = 1.0 - level / 100.0;
    const double tc = t_critical(alpha, dfree);

    // coef/se are not enough. `--tidy`'s coeftable builder (tidy_coeftable) takes the
    // coef{} branch whenever coef{} exists, and reads t/p/CI from the SIBLING maps
    // tstat/pvalue/ci_lo/ci_hi — never from coefs[]. Emitting coef/se alone therefore
    // renders a table with the estimate and SE filled in and t, p and the confidence
    // interval blank, which for `mean`/`proportion` is exactly the column Stata puts
    // the CI in. Every map below is filled from the SAME per-row values as coefs[], so
    // the two representations cannot drift apart.
    json coef = json::object(), se_map = json::object(), coefs = json::array();
    json t_map = json::object(), p_map = json::object(),
         lo_map = json::object(), hi_map = json::object();
    // vce(cluster) is a design-based variance: the count-based intervals read the
    // effective n (prop_ci)
    const int ci_adj = cl ? (ci_unadjusted ? 1 : 2) : 0;
    bool agresti_cut = false;
    // every level's centered indicator over the sample, for e(V) below
    std::vector<std::string> v_names;
    std::vector<std::vector<double>> v_z;
    for (size_t j = 0; j < vars.size(); ++j) {
        std::vector<double> levels; for (int64_t i : keep) levels.push_back(col[j][i]);
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        for (double L : levels) {
            double sw_ind = 0;
            for (int64_t i : keep) if (col[j][i] == L) sw_ind += w ? w[i] : 1.0;
            double p = W > 0 ? sw_ind / W : std::numeric_limits<double>::quiet_NaN();
            v_names.push_back(vars[j] + ":" + level_name(L));
            v_z.emplace_back(static_cast<size_t>(n));
            for (int64_t r = 0; r < n; ++r) {
                const int64_t i = keep[static_cast<size_t>(r)];
                v_z.back()[static_cast<size_t>(r)] = (col[j][i] == L ? 1.0 : 0.0) - p;
            }
            double ss = 0;
            for (int64_t i : keep) {
                double ind = (col[j][i] == L) ? 1.0 : 0.0;
                if (analytic) {
                    ss += (w ? w[i] : 1.0) * (ind - p) * (ind - p);
                    continue;
                }
                double t = (w ? w[i] : 1.0) * (ind - p);
                ss += t * t;
            }
            const size_t kcl = v_names.size() - 1;  // this level's column
            double V = cl ? Vcl[kcl * static_cast<size_t>(std::sqrt(static_cast<double>(Vcl.size())) + 0.5) + kcl]
                     : analytic ? ((W > 0 && (w || n > 1)) ? ss / (W * W)
                                                           : std::numeric_limits<double>::quiet_NaN())
                     : (n > 1 && W > 0) ? (static_cast<double>(n) / (n - 1)) * ss / (W * W)
                                        : std::numeric_limits<double>::quiet_NaN();
            double se = std::sqrt(V);
            // Stata's default CI for proportion is the logit transform; guard the
            // degenerate p==0/p==1 boundary (ln(p/(1-p)) blows up) by collapsing the
            // CI onto the point estimate instead of emitting NaN/inf. se is already
            // exactly 0 in the degenerate case (every indicator deviation is 0).
            double ci_lo, ci_hi;
            prop_ci(citype, p, se, static_cast<double>(n), level, tc, ci_lo, ci_hi, ci_adj, &agresti_cut);
            double t = se > 0 ? p / se : std::numeric_limits<double>::quiet_NaN();
            double pval = std::isnan(t) ? std::numeric_limits<double>::quiet_NaN() : t_twosided_pvalue(t, dfree);
            std::string name = vars[j] + ":" + level_name(L);
            coefs.push_back({{"name", name}, {"b", nz(p)}, {"se", nz(se)}, {"t", nz(t)}, {"p", nz(pval)},
                             {"ci_lo", nz(ci_lo)}, {"ci_hi", nz(ci_hi)}});
            coef[name] = nz(p); se_map[name] = nz(se);
            t_map[name] = nz(t); p_map[name] = nz(pval);
            lo_map[name] = nz(ci_lo); hi_map[name] = nz(ci_hi);
        }
    }
    // e(N): the row count, or the weight sum under fw/iw.
    json out = {{"estimator", "proportion"}, {"n_obs", freq_w ? json(W) : json(n)}, {"sum_w", W},
                {"level", level}, {"coefs", coefs}, {"df", dfree}, {"coef", coef}, {"se", se_map},
                {"tstat", t_map}, {"pvalue", p_map}, {"ci_lo", lo_map}, {"ci_hi", hi_map}};
    if (agresti_cut) out["agresti_truncated"] = true;
    if (w)  // normalize the long spellings ("pweight" ...) to the two-letter ones
        out["weight"] = {{"var", weight}, {"type", freq_fw ? "fw" : freq_w ? "iw" : "pw"}};
    // e(V): each level's variance above with the cross terms between levels and between
    // variables, Sum a_i z_ia z_ib over the centered indicators (StataBE 19.5,
    // scratch-laneA2/agents/epost/s11.log, `proportion d d3` V[3,1] .009259259259259257,
    // V[5,1] -.018518518518518514; [fw=] .0037037037037037025, [iw=] .0026674874320303673,
    // [pw=] .0065356878303371975).
    if (W > 0 && (analytic ? (w != nullptr || n > 1) : n > 1)) {
        std::vector<double> av(static_cast<size_t>(n));
        for (int64_t r = 0; r < n; ++r) {
            const double wi = w ? w[keep[static_cast<size_t>(r)]] : 1.0;
            av[static_cast<size_t>(r)] = analytic ? wi : wi * wi;
        }
        const double nn = static_cast<double>(n);
        const double scale = analytic ? 1.0 / (W * W) : nn / (nn - 1.0) / (W * W);
        if (!cl) add_full_e_v(out, v_names, v_z, av, scale);
    }
    if (cl) {
        bool finite = !Vcl.empty();
        for (double v : Vcl) finite = finite && std::isfinite(v);
        if (finite) out["e_V"] = square(v_names, Vcl);
        out["n_clusters"] = nclust;
    }
    // proportion's per-level e() matrices (StataBE 19.5, vs-stata bank 4525943d, 187b786c,
    // 37332e76): e(_N) the sample's rows, e(freq) the level's weighted count p*W,
    // e(_N_subp) the weight total under fw/iw.
    {
        json nvec = json::array(), fvec = json::array();
        for (size_t c = 0; c < v_names.size(); ++c) {
            nvec.push_back(n);
            // the proportion times the weight total, as Stata forms it (bank 4525943d:
            // 5.rep78 10.999999999999998 = .15942028985507245 * 69)
            const json& b = coefs[c]["b"];
            fvec.push_back(b.is_number() ? json(b.get<double>() * W) : json(nullptr));
        }
        out["e__N"] = nvec;
        out["e_freq"] = fvec;
        out["n_rows"] = n;
        if (freq_w) out["e__N_subp"] = W;
    }
    EsampleRows esrows;
    esrows.loaded = &ds;
    esrows.keep = keep;
    emit_esample(esreq, "proportion", data0, keepif, false, esrows, out);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
