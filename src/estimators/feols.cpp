#include "estimators/feols.h"
#include "core/error.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "core/collinear.h"
#include "core/linalg.h"
#include "hdfe/demean.h"
#include "vcov/vcov.h"

namespace cecon {

namespace {
// TSS for the reported R². When fixed effects are absorbed, y is already mean-zero
// from the within-transform, so the within-TSS is the plain Σy² (centering would
// subtract a ~0 mean and is a no-op). When there are NO fixed effects AND a
// constant is in the model, the correct TSS is the CENTERED Σ(y-ȳ)² — identical to
// ols_fit, giving the Stata `regress` R² (Doctrine ①). `intercept` is md.has_intercept
// (false under FE, and also false for --no-intercept): Stata `regress ..., noconstant`
// reports the UNCENTERED R² (Σy²) exactly like the FE case, so gating on has_intercept
// rather than fe.empty() covers both without a separate branch.
inline double centered_when_no_fe(const double* y, int64_t n, bool intercept) {
    // COBRA-DD-ACCUM: compensated (T1 tier — see core/linalg.h).
    if (!intercept) return la::sumsq(y, n);
    const double ybar = la::sum(y, n) / static_cast<double>(n);
    return la::sumsq_dev(y, n, ybar);
}

// Fill the Stata `regress` model-fit scalars (mss/rss/r2/r2_a/rmse/f_stat/df_m/df_r)
// from an already-fitted FeolsResult (ssr, tss_within, sigma2, dof, beta, vcov_matrix,
// names, n_clusters all set). The F is the Wald statistic over the SLOPE coefficients
// (intercept excluded), built from the actual (iid/HC1/cluster) vcov — so for robust
// and cluster it is Stata's robust regress F, while under iid it reduces to the classic
// (mss/df_m)/(rss/df_r). df_r follows Stata's e(df_r): G-1 under cluster, else the true
// residual dof. r2_a and rmse always use the TRUE residual dof (n-k), matching Stata even
// under cluster (where e(df_r)=G-1 but RMSE/adj-R² still divide by n-k). prob_f (Prob>F)
// is computed by the CLI via f_pvalue() to keep the F distribution out of the estimator
// layer. f_stat is NaN when undefined (no slopes / non-PD slope vcov).
void fill_fit_scalars(FeolsResult& r) {
    r.rss = r.ssr;
    r.mss = r.tss_within - r.ssr;
    if (r.mss < 0 && r.mss > -1e-9 * (std::abs(r.tss_within) + 1.0)) r.mss = 0.0;  // clamp fp noise
    // Header R²: under FE, reghdfe reports the FULL-MODEL e(r2) = 1 - RSS/TSS_total
    // (TSS_total = centered Σ(y_raw-ȳ)² on the ORIGINAL outcome, FE variation INCLUDED),
    // NOT the within-transformed r2_within. tss_total is populated only on the FE path;
    // no-FE keeps r2==r2_within byte-for-byte (Doctrine ③). r2_a below then inherits the
    // full-model r2 and the existing (1-r2)*(n-1)/dof_resid formula reproduces e(r2_a).
    r.r2 = (r.tss_total > 0.0) ? 1.0 - r.ssr / r.tss_total : r.r2_within;
    r.rmse = std::sqrt(r.sigma2);
    // True residual dof = n - k_vars - k_fe, except under weights, where it is
    // n_eff - k_full and may be fractional (iw). Both the adjusted R² and the F
    // reference dof below must use the SAME real value Stata does — using the
    // rounded one moves e(rmse)/F/adj-R² in the 5th significant digit.
    const double dof_resid = (r.dof_resid_exact > 0.0) ? r.dof_resid_exact
                                                       : static_cast<double>(r.dof.dof_resid);
    const double n_obs = (r.n_eff > 0.0) ? r.n_eff : static_cast<double>(r.n);

    // df_m = number of non-intercept slopes; locate the intercept column by name.
    // Under FE, reghdfe's reconstructed intercept is named "_cons" (GAP B) — treat
    // it as the intercept too, else it is miscounted as a slope and F/df_m regress.
    int icpt = -1, q = 0;
    for (int j = 0; j < r.k; ++j) {
        if (r.names[static_cast<size_t>(j)] == "(intercept)" ||
            r.names[static_cast<size_t>(j)] == "_cons") icpt = j;
        else ++q;
    }
    r.df_m = q;
    r.df_r = (r.n_clusters > 0) ? static_cast<int64_t>(r.n_clusters - 1)
                                : static_cast<int64_t>(std::llround(dof_resid));
    r.df_r_exact = (r.n_clusters > 0) ? static_cast<double>(r.n_clusters - 1) : dof_resid;
    r.r2_a = (dof_resid > 0) ? 1.0 - (1.0 - r.r2) * (n_obs - 1.0) / dof_resid : 0.0;

    // Wald F over the q slopes: F = (1/q) b_s' (V_ss)^-1 b_s, V_ss = slope sub-block of vcov.
    // Undefined (→ NaN) when there are no slopes or the F reference dof is degenerate:
    // a single cluster (G=1) gives df_r=G-1=0 AND a Gmin/(Gmin-1)=+inf cluster factor, so the
    // slope vcov is +inf and spd_inverse(+inf) succeeds with a 0 inverse → a meaningless finite
    // F=0. Stata reports F/Prob>F as MISSING there; gating on df_r>0 matches that (Doctrine ①).
    r.f_stat = std::numeric_limits<double>::quiet_NaN();
    if (q > 0 && r.df_r > 0 && static_cast<int>(r.vcov_matrix.size()) == r.k * r.k) {
        std::vector<int> S;
        S.reserve(static_cast<size_t>(q));
        for (int j = 0; j < r.k; ++j)
            if (j != icpt) S.push_back(j);
        std::vector<double> Vss(static_cast<size_t>(q) * q), bs(static_cast<size_t>(q));
        for (int a = 0; a < q; ++a) {
            bs[static_cast<size_t>(a)] = r.beta[static_cast<size_t>(S[a])];
            for (int b = 0; b < q; ++b)
                Vss[static_cast<size_t>(a) + static_cast<size_t>(b) * q] =
                    r.vcov_matrix[static_cast<size_t>(S[a]) + static_cast<size_t>(S[b]) * r.k];
        }
        if (la::spd_inverse(Vss.data(), q) == 0) {  // V_ss^-1 in place
            std::vector<double> tmp(static_cast<size_t>(q));
            la::matvec(Vss.data(), q, bs.data(), tmp.data());  // tmp = V_ss^-1 b_s
            double w = 0.0;
            for (int a = 0; a < q; ++a) w += bs[static_cast<size_t>(a)] * tmp[static_cast<size_t>(a)];
            r.f_stat = w / static_cast<double>(q);
        }
    }
}
}  // namespace

FeolsResult feols_fit(ModelData& md, Vcov vt, int cluster_index, bool no_intercept,
                      bool keep_nested_fe_dof) {
    DesignMatrix& dm = md.dm;
    // --no-intercept (Stata `regress ..., noconstant`): only meaningful with no FE
    // (the caller rejects --no-intercept + --fe); when present the intercept is
    // always build_model's column 0 (dm.xnames[0]=="(intercept)") — drop it before
    // anything else so demean/collinearity/OLS/dof/fill_fit_scalars all naturally
    // see a constant-free design. Default no_intercept=false: dm untouched.
    if (no_intercept && !dm.xnames.empty() && dm.xnames[0] == "(intercept)") {
        dm.X.erase(dm.X.begin(), dm.X.begin() + dm.n);
        dm.xnames.erase(dm.xnames.begin());
        --dm.k;
        md.has_intercept = false;
    }
    const int64_t n = dm.n;
    int k = dm.k;
    if (n <= k) throw EngineError("no_obs", "not enough observations: n <= k");

    FeolsResult r;
    r.n = n;
    r.n_dropped = dm.n_dropped;

    // --- Stata estimation weights: WLS by √w̃ row scaling ---------------------
    // Scaling every row of y and X by √w̃ turns the unweighted machinery below
    // into the weighted one exactly, with no second code path:
    //     X̃'X̃ = X'W̃X  and  X̃'ỹ = X'W̃y   → the WLS β
    //     Σẽ²  = Σ w̃ e²                   → the weighted RSS
    //     Σ(x̃ẽ)(x̃ẽ)' = Σ(w̃ e x)(w̃ e x)'  → Stata's pweight-correct HC1/cluster
    //                                        meat, which is what makes
    //                                        `[pw=], vce(cluster)` come out right
    //                                        without touching vcov.cpp at all.
    //   EXCEPT fw + HC1: a frequency weight is w replicated rows, whose HC1 meat
    //   is Σ w e² xx' (one power of w), not the Σ w² e² xx' above — see the
    //   e_meat de-scaling at the compute_vcov call (BUG-644). Cluster meat is
    //   right as is: the replicas share a cluster, so its score is Σ w e x.
    // w̃ = w · c, where the per-kind normalizer c and the reported N are:
    //   aw/pw  c = n_rows/Σw  (Stata normalizes analytic/probability weights to
    //          sum to the row count), N = n_rows. c CANCELS out of the vcov
    //          (bread ∝ 1/c, σ² ∝ c), so it only sets the reported RSS/TSS/RMSE
    //          — which is precisely where Stata's numbers need it.
    //   fw/iw  c = 1 (weights used raw), N = Σw. The residual dof is then Σw - k,
    //          FRACTIONAL under iw: Stata's own iweight regress reports e(rmse),
    //          F and every SE off that fractional dof while printing the rounded
    //          integer in the ANOVA df column (measured against Stata 19.5 —
    //          integer dof there misses se, rmse AND F).
    // Weighted absorption (md.fe non-empty) keeps w̃ in `wt` and defers the √w̃
    // scaling until AFTER the weighted within transform below: the FE cell means
    // must be the WEIGHTED means Σw̃v/Σw̃ of the ORIGINAL data. Scaling first and
    // then demeaning unweighted subtracts plain means of √w̃·v instead, which is
    // right only when w̃ is constant within every FE cell (oracle w2_pweight
    // A3_areg_pw_VARYING_w_clusterid, wv varying within id, tells them apart).
    double w_tss = -1.0;  // <0 ⇒ unweighted/FE; ≥0 ⇒ the no-FE weighted centered TSS
    std::vector<double> wt;  // w̃ = w·c, set only on the weighted-FE path
    if (!md.weights.empty()) {
        double wsum = 0.0;
        for (int64_t i = 0; i < n; ++i) wsum += md.weights[static_cast<size_t>(i)];
        if (!(wsum > 0.0)) throw EngineError("bad_input", "sum of weights must be positive");
        const bool normalize = (md.weight_kind == WeightKind::Analytic ||
                                md.weight_kind == WeightKind::Probability);
        const double c = normalize ? static_cast<double>(n) / wsum : 1.0;
        r.weight_kind = md.weight_kind;
        r.sum_w = wsum;
        r.n_eff = normalize ? static_cast<double>(n) : wsum;

        // Weighted-FE path: keep w̃; the full-model TSS, the GAP-B means and the
        // scaling are all taken below, around the weighted demean.
        if (!md.fe.empty()) {
            wt.resize(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i)
                wt[static_cast<size_t>(i)] = md.weights[static_cast<size_t>(i)] * c;
        }
        // The reported TSS is the weighted centered Σw̃(y-ȳ_w)² on the ORIGINAL y,
        // so it must be taken BEFORE the scaling below (afterwards the plain mean
        // of ỹ is not ȳ_w). --no-intercept keeps Stata's UNcentered convention.
        else if (md.has_intercept) {
            double sw = 0.0, sy = 0.0;
            for (int64_t i = 0; i < n; ++i) {
                const double wi = md.weights[static_cast<size_t>(i)] * c;
                sw += wi;
                sy += wi * dm.y[static_cast<size_t>(i)];
            }
            const double ybar_w = sy / sw;
            double t = 0.0;
            for (int64_t i = 0; i < n; ++i) {
                const double d = dm.y[static_cast<size_t>(i)] - ybar_w;
                t += md.weights[static_cast<size_t>(i)] * c * d * d;
            }
            w_tss = t;
        } else {
            double t = 0.0;
            for (int64_t i = 0; i < n; ++i) {
                const double yi = dm.y[static_cast<size_t>(i)];
                t += md.weights[static_cast<size_t>(i)] * c * yi * yi;
            }
            w_tss = t;
        }

        if (md.fe.empty()) {
            for (int64_t i = 0; i < n; ++i) {
                const double s = std::sqrt(md.weights[static_cast<size_t>(i)] * c);
                dm.y[static_cast<size_t>(i)] *= s;
                for (int j = 0; j < k; ++j) dm.X[static_cast<size_t>(j) * n + i] *= s;
            }
        }
    }
    const bool weighted_fe = !wt.empty();

    // GAP B (reghdfe-style _cons): capture the raw ORIGINAL-scale means of y and each X
    // column BEFORE demean() mutates the buffers. Used only under FE to
    // reconstruct the intercept via "regression through means": b0 = ybar - xbar·beta.
    // No effect on any slope math (pure read). Gated on !md.fe.empty() so the no-FE
    // path is byte-identical. Weighted: the means are Σw̃v/Σw̃ and the TSS is the
    // weighted centered Σw̃(y-ȳ_w)², all on the still-unscaled data.
    double ybar = 0.0;
    std::vector<double> xbar;
    double sum_wt = 0.0;  // Σw̃ (= n under aw/pw, Σw under fw/iw); weighted-FE only
    if (weighted_fe) {
        sum_wt = la::sum(wt.data(), n);
        ybar = la::dot(wt.data(), dm.y.data(), n) / sum_wt;
        xbar.assign(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            xbar[static_cast<size_t>(c)] =
                la::dot(wt.data(), dm.X.data() + static_cast<int64_t>(c) * n, n) / sum_wt;
        std::vector<double> d(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i)
            d[static_cast<size_t>(i)] = std::sqrt(wt[static_cast<size_t>(i)]) *
                                        (dm.y[static_cast<size_t>(i)] - ybar);
        r.tss_total = la::sumsq(d.data(), n);
    } else if (!md.fe.empty()) {
        // COBRA-DD-ACCUM: compensated means (T1 tier — see core/linalg.h).
        ybar = la::sum(dm.y.data(), n) / static_cast<double>(n);
        xbar.assign(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            xbar[static_cast<size_t>(c)] =
                la::sum(dm.X.data() + static_cast<int64_t>(c) * n, n) / static_cast<double>(n);
        // Full-model TSS for reghdfe's e(r2): centered Σ(y_raw-ȳ)² on the ORIGINAL y,
        // captured BEFORE demean() strips the FE variation (r2_within uses tss_within).
        r.tss_total = la::sumsq_dev(dm.y.data(), n, ybar);
    }

    // --- HDFE demean (within transform) of y and X ---
    // Weighted: subtract the weighted cell means, THEN scale by √w̃ (see above).
    const double* wdm = weighted_fe ? wt.data() : nullptr;
    DemeanReport dy = demean(dm.y.data(), n, 1, md.fe, wdm);
    DemeanReport dx = demean(dm.X.data(), n, k, md.fe, wdm);
    r.demean_iters = std::max(dy.iters, dx.iters);
    r.demean_resid = std::max(dy.resid, dx.resid);
    r.demean_converged = dy.converged && dx.converged;
    // sw[i] = √w̃_i: the row scale, reused by GAP B's augmented robust design.
    std::vector<double> sw;
    if (weighted_fe) {
        sw.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            const double s = std::sqrt(wt[static_cast<size_t>(i)]);
            sw[static_cast<size_t>(i)] = s;
            dm.y[static_cast<size_t>(i)] *= s;
            for (int j = 0; j < k; ++j) dm.X[static_cast<size_t>(j) * n + i] *= s;
        }
    }

    // --- omit collinear regressors (Stata `regress` parity) ---
    // Detect on the demeaned design (the matrix actually solved); drop the LATER
    // column of each collinear set so surviving coefs match Stata coef-for-coef.
    // Snapshot dm.xnames BEFORE compact_columns mutates it in place — this is the
    // only point where the pre-drop design-matrix order still exists (BUG-112(6)).
    std::vector<std::string> full_names = dm.xnames;
    {
        std::vector<char> keep = collinear_keep_mask(dm.X.data(), n, k);
        int nkeep = 0;
        for (char c : keep) nkeep += c;
        if (nkeep < k) {
            r.omitted = compact_columns(dm.X, n, k, dm.xnames, keep);
            dm.k = k;
        }
    }
    r.k = k;
    r.names = dm.xnames;
    r.full_names = full_names;
    if (n <= k) throw EngineError("no_obs", "not enough observations after omitting collinear terms: n <= k");

    // --- OLS on demeaned data ---
    std::vector<double> bread(static_cast<size_t>(k) * k);
    la::xtx(dm.X.data(), n, k, bread.data());
    if (la::spd_inverse(bread.data(), k) != 0)
        throw EngineError("collinear", "X'X not positive-definite (collinear regressors?)");

    std::vector<double> Xty(k);
    la::xtv(dm.X.data(), n, k, dm.y.data(), Xty.data());
    r.beta.resize(k);
    la::matvec(bread.data(), k, Xty.data(), r.beta.data());

    std::vector<double> e(n);
    la::residual(dm.X.data(), n, k, r.beta.data(), dm.y.data(), e.data());
    double ssr = la::sumsq(e.data(), n), tssw = 0;  // COBRA-DD-ACCUM (T1)
    // TSS for R²: with FE absorbed, the within-transform leaves a mean-zero y, so
    // the within-TSS is Σ(demeaned y)² (uncentered sum == centered sum). With NO
    // FE and an intercept present, the reported R² must be the CENTERED Σ(y-ȳ)²,
    // matching `cobra ols` and Stata `regress` (Doctrine ①); with --no-intercept
    // (md.has_intercept false, no FE) Stata's noconstant R² is UNCENTERED (Σy²),
    // same branch as the FE case.
    // (weighted, no FE: the centered TSS was taken above, on the un-scaled y;
    //  weighted FE: w_tss stays <0 and Σ(√w̃·demeaned y)² = Σw̃(y-ȳ_w,cell)² is the
    //  within TSS, uncentered because it is already mean-zero within cells)
    tssw = (w_tss >= 0.0) ? w_tss : centered_when_no_fe(dm.y.data(), n, md.has_intercept);
    r.ssr = ssr;
    r.tss_within = tssw;
    r.r2_within = (tssw > 0) ? 1.0 - ssr / tssw : 0.0;

    // --- degrees of freedom (counts absorbed FE) ---
    r.dof = compute_dof(n, k, md.fe);
    // Unweighted: the residual dof IS r.dof.dof_resid (n - k_vars - k_fe), integral.
    // Weighted: it is n_eff - k_full instead, and under iw that is fractional — so the
    // real value drives σ²/F/SEs while dof.dof_resid keeps the rounded one Stata prints
    // in the ANOVA df column and reports as e(df_r).
    double dof_resid = static_cast<double>(r.dof.dof_resid);
    if (r.n_eff > 0.0) {
        dof_resid = r.n_eff - static_cast<double>(k + r.dof.k_fe);
        r.dof_resid_exact = dof_resid;
        r.dof.dof_resid = static_cast<int64_t>(std::llround(dof_resid));
    }
    if (dof_resid <= 0) throw EngineError("no_obs", "non-positive residual degrees of freedom");
    r.sigma2 = ssr / dof_resid;
    const int k_full = k + r.dof.k_fe;  // total estimated params incl. FE

    // --- vcov (shared with 2SLS) ---
    // n_eff differs from n only under fw/iw (Σw); aw/pw and unweighted pass -1/n.
    // BUG-644: under fw the HC1 meat must be the replicated-data Σ w e² xx', so the
    // residual handed to it is ẽ/√w = e (the √w̃-scaled design supplies the one
    // power of w). StataBE: `regress y x1 x2 [fw=w], vce(robust)` se[x1]
    // 0x1.ae5e0af6def2fp-4 and `areg ..., absorb(id) vce(robust)` 0x1.e603349ddddfep-4,
    // each equal to the same fit on `expand w` data. aw/pw/iw and every cluster
    // meat keep ẽ.
    std::vector<double> e_fw;
    if (vt == Vcov::HC1 && md.weight_kind == WeightKind::Frequency && !md.weights.empty()) {
        e_fw.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i)  // fw: c = 1, weights > 0 (zero rows are dropped)
            e_fw[static_cast<size_t>(i)] = e[static_cast<size_t>(i)] /
                                           std::sqrt(md.weights[static_cast<size_t>(i)]);
    }
    const double* e_meat = e_fw.empty() ? e.data() : e_fw.data();
    VcovResult vc = compute_vcov(vt, dm.X.data(), n, k, e_meat, bread.data(), k_full,
                                 r.sigma2, md.fe, md.cluster, cluster_index,
                                 r.n_eff > 0.0 ? r.n_eff : -1.0, false, keep_nested_fe_dof);
    r.vcov_type = vc.type;
    r.n_clusters = vc.n_clusters;
    r.cluster_dof_k = vc.cluster_dof_k;
    r.nested_fe = vc.nested_fe;
    r.vcov_npd_fix = vc.npd_fix;

    r.se.resize(k);
    for (int j = 0; j < k; ++j) r.se[j] = std::sqrt(vc.V[j + j * k]);
    r.vcov_matrix = std::move(vc.V);

    // GAP B — reghdfe reconstructed intercept (_cons) for absorb() models.
    // reghdfe reports a "regression through means" intercept when the absorbed FE
    // set carries an intercept (standard absorb). By Frisch-Waugh the slope
    // betas/SEs/F/R² are UNCHANGED; we only append one row. Gated on
    // !md.fe.empty() (no-FE OLS already carries a real _cons column) and
    // r.omitted.empty() (collinear renumbering of xbar is deferred; ppda has none).
    if (!md.fe.empty() && r.omitted.empty()) {
        // Point: b0 = ybar - xbar·beta   (regression-through-means intercept)
        double b0 = ybar;
        for (int j = 0; j < k; ++j) b0 -= xbar[static_cast<size_t>(j)] * r.beta[static_cast<size_t>(j)];

        // side_j = -(xbar · inv_xx)_j ;  corner = 1/N + xbar·inv_xx·xbar'
        // bread is column-major (X_dm'X_dm)^-1 (symmetric); these are the extra
        // row/col of the extended (K+1)×(K+1) inverse.
        std::vector<double> side(static_cast<size_t>(k), 0.0);
        for (int j = 0; j < k; ++j) {
            double s = 0;
            for (int m = 0; m < k; ++m)
                s += xbar[static_cast<size_t>(m)] * bread[static_cast<size_t>(m) + static_cast<size_t>(j) * k];
            side[static_cast<size_t>(j)] = -s;
        }
        // 1/N is 1/Σw̃ under weights (= 1/n for aw/pw, 1/Σw for fw/iw).
        double corner = weighted_fe ? 1.0 / sum_wt : 1.0 / static_cast<double>(n);
        for (int j = 0; j < k; ++j) corner += xbar[static_cast<size_t>(j)] * (-side[static_cast<size_t>(j)]);

        double var_cons = 0.0;
        std::vector<double> cov_cons(static_cast<size_t>(k), 0.0);

        if (vt == Vcov::IID) {
            // Under IID the slope V = scale · bread with a shared scalar; recover
            // it from any diagonal and scale the extended corner/side.
            double scale = r.vcov_matrix[0] / bread[0];
            var_cons = scale * corner;
            for (int j = 0; j < k; ++j) cov_cons[static_cast<size_t>(j)] = scale * side[static_cast<size_t>(j)];
        } else {
            // HC1 / Cluster / Multiway: the corner-from-bread shortcut is NOT
            // valid — the robust meat needs the intercept column of 1s and the
            // slope columns re-centered to level (reghdfe's absorbed-FE convention).
            // Reuse compute_vcov verbatim on the augmented (k+1) design:
            //   Xa[:,j] = dm.X[:,j] + xbar[j]  (re-centered slopes), last col = 1.
            //   Weighted: dm.X is √w̃-scaled, so the level and the 1s are too
            //   (Xa[:,j] = dm.X[:,j] + √w̃·xbar[j], last col = √w̃).
            //   extended_inv_xx = [[bread, side'],[side, corner]]  (col-major).
            //   k_full stays k_full (NOT k_full+1) — reghdfe excludes _cons from
            //   the cluster dof multiplier; passing k_full+1 would rescale every
            //   slope SE and break the slope-block-identity below (ADDENDUM B4).
            const int ka = k + 1;
            std::vector<double> Xa(static_cast<size_t>(n) * ka);
            for (int j = 0; j < k; ++j) {
                const double* src = dm.X.data() + static_cast<int64_t>(j) * n;
                double* dst = Xa.data() + static_cast<int64_t>(j) * n;
                const double xb = xbar[static_cast<size_t>(j)];
                if (weighted_fe)
                    for (int64_t i = 0; i < n; ++i) dst[i] = src[i] + sw[static_cast<size_t>(i)] * xb;
                else
                    for (int64_t i = 0; i < n; ++i) dst[i] = src[i] + xb;
            }
            double* ones = Xa.data() + static_cast<int64_t>(k) * n;
            if (weighted_fe)
                for (int64_t i = 0; i < n; ++i) ones[i] = sw[static_cast<size_t>(i)];
            else
                for (int64_t i = 0; i < n; ++i) ones[i] = 1.0;

            std::vector<double> ext(static_cast<size_t>(ka) * ka, 0.0);
            for (int a = 0; a < k; ++a)
                for (int b = 0; b < k; ++b)
                    ext[static_cast<size_t>(a) + static_cast<size_t>(b) * ka] =
                        bread[static_cast<size_t>(a) + static_cast<size_t>(b) * k];
            for (int j = 0; j < k; ++j) {
                ext[static_cast<size_t>(j) + static_cast<size_t>(k) * ka] = side[static_cast<size_t>(j)];
                ext[static_cast<size_t>(k) + static_cast<size_t>(j) * ka] = side[static_cast<size_t>(j)];
            }
            ext[static_cast<size_t>(k) + static_cast<size_t>(k) * ka] = corner;

            //   The same keep_nested_fe_dof as the slope call: areg's _cons SE
            //   carries areg's factor (w2b_estim.log areg_vce_cluster
            //   se[_cons] .4823829775414377).
            VcovResult vca = compute_vcov(vt, Xa.data(), n, ka, e_meat, ext.data(),
                                          k_full, r.sigma2, md.fe, md.cluster, cluster_index,
                                          r.n_eff > 0.0 ? r.n_eff : -1.0, false,
                                          keep_nested_fe_dof);
            var_cons = vca.V[static_cast<size_t>(k) + static_cast<size_t>(k) * ka];
            for (int j = 0; j < k; ++j)
                cov_cons[static_cast<size_t>(j)] = vca.V[static_cast<size_t>(j) + static_cast<size_t>(k) * ka];
#ifndef NDEBUG
            // Debug guard (debug builds only): the augmented slope block must equal the
            // original vc.V. If it doesn't, the re-centering / ones-append is wrong. This
            // is an invariant check, never hit in correct operation, so gating it out of
            // release changes no output value — it just stops paying the k*k compare on
            // every _cons feols call (GAP-B slice 1; the algebraic-extension rewrite that
            // would drop the second compute_vcov stays deferred).
            for (int a = 0; a < k; ++a)
                for (int b = 0; b < k; ++b) {
                    double got = vca.V[static_cast<size_t>(a) + static_cast<size_t>(b) * ka];
                    double want = r.vcov_matrix[static_cast<size_t>(a) + static_cast<size_t>(b) * k];
                    double tol = 1e-8 * (1.0 + std::abs(want));
                    if (std::abs(got - want) > tol)
                        throw EngineError("internal",
                            "reghdfe _cons: augmented slope vcov block diverged from slope vcov");
                }
#endif
        }

        // Append the _cons row and grow the vcov k×k -> (k+1)×(k+1) column-major.
        r.beta.push_back(b0);
        r.se.push_back(std::sqrt(var_cons));
        r.names.push_back("_cons");
        const int kn = k + 1;
        std::vector<double> Vn(static_cast<size_t>(kn) * kn, 0.0);
        for (int a = 0; a < k; ++a)
            for (int b = 0; b < k; ++b)
                Vn[static_cast<size_t>(a) + static_cast<size_t>(b) * kn] =
                    r.vcov_matrix[static_cast<size_t>(a) + static_cast<size_t>(b) * k];
        for (int j = 0; j < k; ++j) {
            Vn[static_cast<size_t>(j) + static_cast<size_t>(k) * kn] = cov_cons[static_cast<size_t>(j)];
            Vn[static_cast<size_t>(k) + static_cast<size_t>(j) * kn] = cov_cons[static_cast<size_t>(j)];
        }
        Vn[static_cast<size_t>(k) + static_cast<size_t>(k) * kn] = var_cons;
        r.vcov_matrix = std::move(Vn);
        ++r.k;
    }

    fill_fit_scalars(r);  // mss/rss/r2/r2_a/rmse/f_stat/df_m/df_r (Stata regress e())
    return r;
}

std::vector<FeolsResult> feols_fit_multi(ModelData& md, std::vector<std::vector<double>>& ys,
                                         Vcov vt, int cluster_index, bool no_intercept) {
    DesignMatrix& dm = md.dm;
    // See feols_fit: drop the intercept column (col 0) before anything else.
    if (no_intercept && !dm.xnames.empty() && dm.xnames[0] == "(intercept)") {
        dm.X.erase(dm.X.begin(), dm.X.begin() + dm.n);
        dm.xnames.erase(dm.xnames.begin());
        --dm.k;
        md.has_intercept = false;
    }
    const int64_t n = dm.n;
    int k = dm.k;
    if (n <= k) throw EngineError("no_obs", "not enough observations: n <= k");

    // --- shared, y-independent work: demean X once, factor (X'X)⁻¹ once, dof ---
    DemeanReport dx = demean(dm.X.data(), n, k, md.fe);

    // Collinearity is a property of X alone (not y), so detect & omit once on the
    // shared demeaned design — every outcome then uses the same reduced columns.
    // Snapshot dm.xnames BEFORE compact_columns mutates it in place — this is the
    // only point where the pre-drop design-matrix order still exists (BUG-112(6)).
    std::vector<std::string> full_names = dm.xnames;
    std::vector<std::string> omitted;
    {
        std::vector<char> keep = collinear_keep_mask(dm.X.data(), n, k);
        int nkeep = 0;
        for (char c : keep) nkeep += c;
        if (nkeep < k) {
            omitted = compact_columns(dm.X, n, k, dm.xnames, keep);
            dm.k = k;
            if (n <= k) throw EngineError("no_obs", "not enough observations after omitting collinear terms: n <= k");
        }
    }

    std::vector<double> bread(static_cast<size_t>(k) * k);
    la::xtx(dm.X.data(), n, k, bread.data());
    if (la::spd_inverse(bread.data(), k) != 0)
        throw EngineError("collinear", "X'X not positive-definite (collinear regressors?)");
    DofInfo dof = compute_dof(n, k, md.fe);
    const int64_t dof_resid = dof.dof_resid;
    if (dof_resid <= 0) throw EngineError("no_obs", "non-positive residual degrees of freedom");
    const int k_full = k + dof.k_fe;

    std::vector<FeolsResult> outs;
    outs.reserve(ys.size());
    std::vector<double> Xty(k), e(n);
    for (auto& yraw : ys) {
        FeolsResult r;
        r.n = n; r.k = k; r.names = dm.xnames; r.n_dropped = dm.n_dropped;
        r.omitted = omitted;
        r.full_names = full_names;
        r.dof = dof;
        r.demean_iters = dx.iters; r.demean_resid = dx.resid; r.demean_converged = dx.converged;
        // per-outcome: demean y, solve, residuals
        std::vector<double> y = yraw;  // copy (demean mutates)
        DemeanReport dyr = demean(y.data(), n, 1, md.fe);
        r.demean_converged = r.demean_converged && dyr.converged;
        la::xtv(dm.X.data(), n, k, y.data(), Xty.data());
        r.beta.resize(k);
        la::matvec(bread.data(), k, Xty.data(), r.beta.data());
        la::residual(dm.X.data(), n, k, r.beta.data(), y.data(), e.data());
        double ssr = la::sumsq(e.data(), n), tssw = 0;  // COBRA-DD-ACCUM (T1)
        // No-FE + intercept → center y so R² matches `ols`/Stata; with FE the
        // demeaned y is mean-zero (within-TSS); with --no-intercept (no FE, no
        // constant) Stata's noconstant R² is uncentered, same branch as FE. See
        // feols_fit for the full rationale.
        tssw = centered_when_no_fe(y.data(), n, md.has_intercept);
        r.ssr = ssr; r.tss_within = tssw;
        r.r2_within = (tssw > 0) ? 1.0 - ssr / tssw : 0.0;
        // Full-model TSS for reghdfe's e(r2): centered Σ(y_raw-ȳ)² on the ORIGINAL
        // outcome (pre-within), FE only. Mirrors feols_fit; no-FE leaves tss_total=0.
        if (!md.fe.empty()) {
            // COBRA-DD-ACCUM (T1): compensated mean + centred sum of squares.
            const double yb = la::sum(yraw.data(), n) / static_cast<double>(n);
            r.tss_total = la::sumsq_dev(yraw.data(), n, yb);
        }
        r.sigma2 = ssr / static_cast<double>(dof_resid);
        VcovResult vc = compute_vcov(vt, dm.X.data(), n, k, e.data(), bread.data(), k_full,
                                     r.sigma2, md.fe, md.cluster, cluster_index);
        r.vcov_type = vc.type; r.n_clusters = vc.n_clusters;
        r.cluster_dof_k = vc.cluster_dof_k; r.nested_fe = vc.nested_fe; r.vcov_npd_fix = vc.npd_fix;
        r.se.resize(k);
        for (int j = 0; j < k; ++j) r.se[j] = std::sqrt(vc.V[j + j * k]);
        r.vcov_matrix = std::move(vc.V);
        fill_fit_scalars(r);  // per-outcome Stata regress e() scalars
        outs.push_back(std::move(r));
    }
    return outs;
}

}  // namespace cecon
