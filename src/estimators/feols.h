// cecon — fixed-effects OLS: HDFE demean (FWL within transform) + OLS on the
// demeaned data, with iid / HC1 / one-way-cluster standard errors.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/model.h"
#include "hdfe/dof.h"
#include "vcov/vcov.h"

namespace cecon {

struct FeolsResult {
    int64_t n = 0;
    int k = 0;  // number of coefficients; under FE this includes the reconstructed
                // reghdfe "_cons" row (GAP B), which fill_fit_scalars treats as the
                // intercept so df_m/F stay over the slope set only.
    std::vector<std::string> names;
    std::vector<double> beta;
    std::vector<double> se;
    std::vector<double> vcov_matrix;  // k*k, column-major
    std::string vcov_type;

    double ssr = 0;
    double tss_within = 0;
    double tss_total = 0;  // centered Σ(y_raw-ȳ)² on the ORIGINAL y (pre-within); FE only, else 0
    double r2_within = 0;
    double sigma2 = 0;

    // Model-fit scalars (Stata `regress` e()), filled post-vcov by fill_fit_scalars.
    // For the no-FE OLS path these match Stata `regress` exactly (Doctrine ①); under
    // real FE r2/r2_a follow the within convention (feols is the OLS/no-FE path, §7).
    double mss = 0;        // model SS = tss_within - ssr (centered when no FE)
    double rss = 0;        // residual SS (== ssr; explicit Stata-named scalar)
    double r2 = 0;         // centered R² (no-FE: == r2_within; FE: reghdfe FULL-MODEL e(r2) via tss_total)
    double r2_a = 0;       // adjusted R² = 1 - (1-r2)*(n-1)/dof_resid
    double rmse = 0;       // root MSE = sqrt(sigma2)
    double f_stat = 0;     // model Wald F over the slopes (vce-dependent); NaN if undefined
    int df_m = 0;          // model dof = # non-intercept slopes (Stata e(df_m))
    int64_t df_r = 0;      // F reference resid dof (Stata e(df_r)): G-1 cluster else dof_resid
    double df_r_exact = 0; // df_r without the iw rounding; drives Prob>F and the t CIs

    // Stata weights ([aw=]/[fw=]/[iw=]/[pw=]). All four stay 0/None on the
    // unweighted path, which is how every consumer tells the two apart — an
    // unweighted fit is byte-identical to before weights existed.
    WeightKind weight_kind = WeightKind::None;
    double sum_w = 0;            // Σ of the RAW weights over the estimation sample
    double n_eff = 0;            // Stata e(N): row count under aw/pw, Σw under fw/iw
    double dof_resid_exact = 0;  // n_eff - k_full; FRACTIONAL under iw (dof.dof_resid is its rounding)

    DofInfo dof;
    int n_clusters = 0;
    int cluster_dof_k = 0;              // params counted in the cluster (N-1)/(N-k) factor
    std::vector<std::string> nested_fe;  // FE nested within a cluster (dof-exempt)
    bool vcov_npd_fix = false;           // true if multiway vcov needed PSD repair

    int demean_iters = 0;
    double demean_resid = 0;
    bool demean_converged = true;
    int64_t n_dropped = 0;
    std::vector<std::string> omitted;  // collinear columns dropped (Stata omits, coef 0)
    std::vector<std::string> full_names;  // pre-drop design-matrix column order (BUG-112(6))
};

// Demeans md.dm in place. cluster_index selects which md.cluster dimension to
// use for Vcov::Cluster.
// no_intercept (Stata `regress ..., noconstant`): drops the constant column
// before fitting. Only meaningful when md.fe is empty (FE already absorb any
// constant); the caller is responsible for rejecting --no-intercept with --fe.
// Default false leaves md/the fit byte-identical to before this parameter existed.
// keep_nested_fe_dof (BUG-568, Stata `areg`): the cluster/multiway small-sample
// factor charges every absorbed parameter, including an FE nested in the cluster
// (compute_vcov's switch of the same name). Default false = reghdfe/xtreg,fe.
FeolsResult feols_fit(ModelData& md, Vcov vt, int cluster_index = 0, bool no_intercept = false,
                      bool keep_nested_fe_dof = false);

// Multi-outcome batch: fit the SAME design (X/fe/cluster in md) against several
// outcomes, sharing the expensive HDFE absorption of X and the (X'X)⁻¹ factor —
// only the per-outcome demean(y)/solve/residuals/vcov are recomputed. `ys[o]`
// is outcome o's values aligned to md's kept rows (length md.dm.n), assumed
// jointly observed (the caller enforces a common sample). md.dm.X is demeaned in
// place once. Returns one FeolsResult per outcome. no_intercept: see feols_fit.
std::vector<FeolsResult> feols_fit_multi(ModelData& md, std::vector<std::vector<double>>& ys,
                                         Vcov vt, int cluster_index = 0,
                                         bool no_intercept = false);

}  // namespace cecon
