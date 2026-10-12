// Sun & Abraham interaction-weighted (IW) event-study estimator — clean-room
// implementation from the published methodology paper:
//
//   [SA21] Sun, L. and S. Abraham (2021), "Estimating dynamic treatment
//          effects in event studies with heterogeneous treatment effects",
//          Journal of Econometrics 225(2), 175-199.
//
// The estimator is the two-step construction of SA21 §3 (see sunab.h for the
// contract). The saturated stage reuses the engine's own HDFE within-transform
// (src/hdfe/demean.h) rather than a second solver, exactly as feols does; the
// aggregation stage follows the influence-function propagation pattern csdid
// already uses for its event-study aggregation (src/estimators/csdid.cpp).
//
// Conventions the paper leaves open, pinned by this project's goldens
// (tests/unit/test_eventstudy.cpp) rather than by any prior implementation:
//
//   * WEIGHT NORMALIZATION. w_{l,g} is the share of cohort g among the
//     ESTIMATION-SAMPLE observations at relative period l that do not belong to
//     the control cohort. Measured against the answer key alongside two
//     alternatives — shares over all data rows including those dropped for a
//     missing outcome, and unit-level cohort shares (which ignore relative
//     period entirely). Those two agree with each other and disagree with the
//     oracle in the 3rd decimal; the estimation-sample observation share is the
//     one the oracle picks. See the sweep recorded in the test file.
//
//   * VARIANCE ASSEMBLY. Var(tau_l) = c_reg * Vb + Vw, block-diagonal in the
//     two influence functions (no delta/weight covariance term), where Vb is
//     the saturated stage's cluster-summed IF and Vw the cohort-share IF. Only
//     Vb carries the regression cluster small-sample factor.
//
//   * K IN THE SMALL-SAMPLE FACTOR. c_reg = G/(G-1) * (N-1)/(N-K) counts the
//     interactions, the NON-nested absorbed dof, and one intercept — an FE
//     nested inside the cluster variable is exempt (its parameters are already
//     accounted for by clustering). The same K drives the model F.
//
// Provenance: no eventstudyinteract.ado, Mata source, or fixest sunab() code
// was read; the clean-room wall is the one recorded for csdid
// (docs/legal/CSDID_CLEANROOM_WALL.md).

#include "estimators/sunab.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "core/distributions.h"
#include "core/linalg.h"
#include "core/model.h"
#include "hdfe/demean.h"
#include "vcov/vcov.h"

namespace cecon {
namespace {

// Dense-code a numeric column into a Factor (levels ascending by value).
Factor make_factor(const double* v, int64_t n, const char* name) {
    Factor f;
    f.name = name;
    std::map<double, int> lev;
    for (int64_t i = 0; i < n; ++i) lev.emplace(v[i], 0);
    int c = 0;
    for (auto& kv : lev) kv.second = c++;
    f.n_levels = c;
    f.codes.resize(static_cast<size_t>(n));
    f.count.assign(static_cast<size_t>(c), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        int code = lev.find(v[i])->second;
        f.codes[static_cast<size_t>(i)] = code;
        f.count[static_cast<size_t>(code)] += 1.0;
    }
    return f;
}

// Connected components of the bipartite (unit level, period level) graph — the
// number of separately-identified FE blocks, i.e. the redundancy count for the
// second absorbed dimension.
int fe_components(const Factor& a, const Factor& b, int64_t n) {
    const int na = a.n_levels;
    std::vector<int> parent(static_cast<size_t>(na + b.n_levels));
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
    std::function<int(int)> find = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] =
                parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    };
    for (int64_t i = 0; i < n; ++i) {
        int ra = find(a.codes[static_cast<size_t>(i)]);
        int rb = find(na + b.codes[static_cast<size_t>(i)]);
        if (ra != rb) parent[static_cast<size_t>(ra)] = rb;
    }
    std::vector<char> seen(parent.size(), 0);
    int comps = 0;
    for (int64_t i = 0; i < n; ++i) {
        int r = find(a.codes[static_cast<size_t>(i)]);
        if (!seen[static_cast<size_t>(r)]) { seen[static_cast<size_t>(r)] = 1; ++comps; }
    }
    return comps == 0 ? 1 : comps;
}

}  // namespace

SunabResult sunab_run(const double* y, const double* rel_ind, int n_rel,
                      const double* cohort, const double* control,
                      const double* fe_unit, const double* fe_time,
                      const double* cluster, int64_t n_obs,
                      const SunabConfig& cfg) {
    SunabResult R;
    R.level = cfg.level;
    if (n_obs <= 0 || n_rel <= 0 || y == nullptr || rel_ind == nullptr ||
        cohort == nullptr || fe_unit == nullptr || fe_time == nullptr) {
        R.ok = false;
        R.error = "eventstudyinteract: empty or incomplete estimation sample";
        return R;
    }
    if (cluster == nullptr) {
        R.ok = false;
        R.error = "eventstudyinteract: vce(cluster) requires a cluster variable";
        return R;
    }
    auto is_control = [&](int64_t i) { return control != nullptr && control[i] != 0.0; };

    // --- cohort levels actually interacted (control cohort excluded) ---------
    std::map<double, int> cohort_index;
    for (int64_t i = 0; i < n_obs; ++i)
        if (!is_control(i)) cohort_index.emplace(cohort[i], 0);
    int C = 0;
    for (auto& kv : cohort_index) kv.second = C++;
    R.cohorts.resize(static_cast<size_t>(C));
    for (const auto& kv : cohort_index) R.cohorts[static_cast<size_t>(kv.second)] = kv.first;
    if (C == 0) {
        R.ok = false;
        R.error = "eventstudyinteract: control_cohort() leaves no treated cohort to interact";
        return R;
    }

    // --- cohort shares within each relative period (SA21 eq. 6 weights) ------
    // Counted over the ESTIMATION sample, excluding the control cohort.
    std::vector<double> cnt(static_cast<size_t>(n_rel) * C, 0.0);
    std::vector<double> n_l(static_cast<size_t>(n_rel), 0.0);
    int64_t n_weight_rows = 0;
    for (int64_t i = 0; i < n_obs; ++i) {
        if (is_control(i)) continue;
        ++n_weight_rows;
        const int gi = cohort_index.find(cohort[i])->second;
        for (int l = 0; l < n_rel; ++l) {
            if (rel_ind[static_cast<size_t>(l) * n_obs + i] == 0.0) continue;
            cnt[static_cast<size_t>(l) * C + gi] += 1.0;
            n_l[static_cast<size_t>(l)] += 1.0;
        }
    }
    R.n_obs_weights = n_weight_rows;
    std::vector<double> W(static_cast<size_t>(n_rel) * C, 0.0);
    for (int l = 0; l < n_rel; ++l)
        if (n_l[static_cast<size_t>(l)] > 0)
            for (int g = 0; g < C; ++g)
                W[static_cast<size_t>(l) * C + g] =
                    cnt[static_cast<size_t>(l) * C + g] / n_l[static_cast<size_t>(l)];

    // --- saturated design: one column per NON-EMPTY (relative period, cohort) --
    std::vector<int> col_of(static_cast<size_t>(n_rel) * C, -1);
    std::vector<int> col_l, col_g;
    for (int l = 0; l < n_rel; ++l)
        for (int g = 0; g < C; ++g)
            if (cnt[static_cast<size_t>(l) * C + g] > 0) {
                col_of[static_cast<size_t>(l) * C + g] = static_cast<int>(col_l.size());
                col_l.push_back(l);
                col_g.push_back(g);
            }
    const int K = static_cast<int>(col_l.size());
    if (K == 0) {
        R.ok = false;
        R.error = "eventstudyinteract: the relative-period varlist selects no observation";
        return R;
    }
    R.n_interactions = K;

    std::vector<double> Z(static_cast<size_t>(n_obs) * K, 0.0);
    for (int64_t i = 0; i < n_obs; ++i) {
        if (is_control(i)) continue;
        const int gi = cohort_index.find(cohort[i])->second;
        for (int l = 0; l < n_rel; ++l) {
            if (rel_ind[static_cast<size_t>(l) * n_obs + i] == 0.0) continue;
            const int j = col_of[static_cast<size_t>(l) * C + gi];
            if (j >= 0) Z[static_cast<size_t>(j) * n_obs + i] = 1.0;
        }
    }

    // --- HDFE absorption (absorb(unit period)) ------------------------------
    Factor fu = make_factor(fe_unit, n_obs, "absorb1");
    Factor ft = make_factor(fe_time, n_obs, "absorb2");
    Factor fc = make_factor(cluster, n_obs, "cluster");
    const int ncomp = fe_components(fu, ft, n_obs);
    const int64_t df_a = static_cast<int64_t>(fu.n_levels) + ft.n_levels - ncomp;

    // COBRA-DD-ACCUM (T1): compensated mean + centred sum of squares.
    const double ybar = la::sum(y, n_obs) / static_cast<double>(n_obs);
    const double tss = la::sumsq_dev(y, n_obs, ybar);

    std::vector<double> yd(y, y + n_obs);
    std::vector<Factor> fes{fu, ft};
    demean(yd.data(), n_obs, 1, fes);
    demean(Z.data(), n_obs, K, fes);

    // --- OLS on the within-transformed design -------------------------------
    std::vector<double> A(static_cast<size_t>(K) * K);
    la::xtx(Z.data(), n_obs, K, A.data());
    std::vector<double> Zty(static_cast<size_t>(K));
    la::xtv(Z.data(), n_obs, K, yd.data(), Zty.data());
    if (la::spd_inverse(A.data(), K) != 0) {
        R.ok = false;
        R.error = "eventstudyinteract: the saturated cohort x relative-period design is "
                  "collinear after absorbing absorb() — no unique IW decomposition";
        return R;
    }
    std::vector<double> b(static_cast<size_t>(K));
    la::matvec(A.data(), K, Zty.data(), b.data());
    std::vector<double> e(static_cast<size_t>(n_obs));
    la::residual(Z.data(), n_obs, K, b.data(), yd.data(), e.data());

    const double rss = la::sumsq(e.data(), n_obs);  // COBRA-DD-ACCUM (T1)
    const int64_t dof = n_obs - K - df_a;
    R.n_obs = n_obs;
    R.df_absorb = df_a;
    R.dof_resid = dof;
    R.r2 = tss > 0 ? 1.0 - rss / tss : std::nan("");
    R.r2_a = dof > 0 ? 1.0 - (1.0 - R.r2) * static_cast<double>(n_obs - 1) /
                                 static_cast<double>(dof)
                     : std::nan("");
    R.rmse = dof > 0 ? std::sqrt(rss / static_cast<double>(dof)) : std::nan("");
    R.n_clusters = fc.n_levels;

    // Cluster small-sample factor. An absorbed FE nested inside the cluster
    // variable is exempt from K (clustering already accounts for it); the
    // intercept that FE block carried is added back.
    int64_t k_cluster = K + df_a;
    bool any_nested = false;
    if (fe_nested_in_cluster(fu, fc)) { k_cluster -= fu.n_levels; any_nested = true; }
    if (fe_nested_in_cluster(ft, fc)) { k_cluster -= (ft.n_levels - ncomp); any_nested = true; }
    if (any_nested) k_cluster += 1;
    const double G = static_cast<double>(fc.n_levels);
    const double c_reg =
        (G > 1 && n_obs > k_cluster)
            ? (G / (G - 1.0)) *
                  (static_cast<double>(n_obs - 1) / static_cast<double>(n_obs - k_cluster))
            : 1.0;

    // --- model F over the K interactions ------------------------------------
    {
        std::vector<double> S(static_cast<size_t>(fc.n_levels) * K, 0.0);
        for (int64_t i = 0; i < n_obs; ++i) {
            const int c = fc.codes[static_cast<size_t>(i)];
            for (int j = 0; j < K; ++j)
                S[static_cast<size_t>(j) * fc.n_levels + c] +=
                    Z[static_cast<size_t>(j) * n_obs + i] * e[static_cast<size_t>(i)];
        }
        std::vector<double> meat(static_cast<size_t>(K) * K);
        la::xtx(S.data(), fc.n_levels, K, meat.data());
        std::vector<double> V(static_cast<size_t>(K) * K);
        la::sandwich(A.data(), meat.data(), K, c_reg, V.data());
        if (la::spd_inverse(V.data(), K) == 0) {
            std::vector<double> Vb(static_cast<size_t>(K));
            la::matvec(V.data(), K, b.data(), Vb.data());
            double w = 0;
            for (int j = 0; j < K; ++j) w += b[static_cast<size_t>(j)] * Vb[static_cast<size_t>(j)];
            R.F = w / K;
        } else {
            R.F = std::nan("");
        }
        R.df1 = K;
        R.df2 = fc.n_levels - 1;
    }

    // --- IW aggregation with influence-function inference --------------------
    const double alpha = cfg.level == 95 ? 0.05 : 1.0 - cfg.level / 100.0;
    const double tcrit = t_critical(alpha, static_cast<double>(R.df2));
    std::vector<double> Sb(static_cast<size_t>(fc.n_levels)), Sw(static_cast<size_t>(fc.n_levels));
    std::vector<double> what(static_cast<size_t>(K)), u(static_cast<size_t>(K));
    for (int l = 0; l < n_rel; ++l) {
        SunabCoef row;
        row.rel_index = l;
        row.cohort_weight.assign(static_cast<size_t>(C), 0.0);
        row.cohort_coef.assign(static_cast<size_t>(C), 0.0);
        std::fill(what.begin(), what.end(), 0.0);
        double tau = 0;
        for (int g = 0; g < C; ++g) {
            const int j = col_of[static_cast<size_t>(l) * C + g];
            const double w = W[static_cast<size_t>(l) * C + g];
            row.cohort_weight[static_cast<size_t>(g)] = w;
            if (j < 0) continue;
            row.cohort_coef[static_cast<size_t>(g)] = b[static_cast<size_t>(j)];
            what[static_cast<size_t>(j)] = w;
            tau += w * b[static_cast<size_t>(j)];
        }
        row.coef = tau;
        if (n_l[static_cast<size_t>(l)] <= 0) {
            row.coef = std::nan("");
            row.se = std::nan(""); row.t = std::nan(""); row.p = std::nan("");
            row.ci_lo = std::nan(""); row.ci_hi = std::nan("");
            R.coefs.push_back(row);
            continue;
        }
        la::matvec(A.data(), K, what.data(), u.data());
        std::fill(Sb.begin(), Sb.end(), 0.0);
        std::fill(Sw.begin(), Sw.end(), 0.0);
        for (int64_t i = 0; i < n_obs; ++i) {
            const int c = fc.codes[static_cast<size_t>(i)];
            double zu = 0;
            for (int j = 0; j < K; ++j)
                zu += Z[static_cast<size_t>(j) * n_obs + i] * u[static_cast<size_t>(j)];
            Sb[static_cast<size_t>(c)] += zu * e[static_cast<size_t>(i)];
            if (is_control(i)) continue;
            if (rel_ind[static_cast<size_t>(l) * n_obs + i] == 0.0) continue;
            const int gi = cohort_index.find(cohort[i])->second;
            const int j = col_of[static_cast<size_t>(l) * C + gi];
            if (j < 0) continue;
            Sw[static_cast<size_t>(c)] +=
                (b[static_cast<size_t>(j)] - tau) / n_l[static_cast<size_t>(l)];
        }
        double vb = 0, vw = 0;
        for (int c = 0; c < fc.n_levels; ++c) {
            vb += Sb[static_cast<size_t>(c)] * Sb[static_cast<size_t>(c)];
            vw += Sw[static_cast<size_t>(c)] * Sw[static_cast<size_t>(c)];
        }
        const double var = c_reg * vb + vw;
        row.se = var > 0 ? std::sqrt(var) : 0.0;
        row.t = row.se > 0 ? row.coef / row.se : std::nan("");
        row.p = row.se > 0 ? t_twosided_pvalue(row.t, static_cast<double>(R.df2)) : std::nan("");
        row.ci_lo = row.coef - tcrit * row.se;
        row.ci_hi = row.coef + tcrit * row.se;
        R.coefs.push_back(row);
    }
    return R;
}

}  // namespace cecon
