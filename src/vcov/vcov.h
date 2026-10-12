// cecon — variance estimation shared by OLS and 2SLS. The "design" passed in is
// X for OLS and the instrumented X-hat (= P_W X) for 2SLS; everything else
// (robust/cluster meats, small-sample factors, nested-dof, PSD repair) is common.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/model.h"

namespace cecon {

enum class Vcov { IID, HC1, Cluster, Multiway };

struct VcovResult {
    std::vector<double> V;  // k*k, column-major
    std::string type;
    int n_clusters = 0;
    int cluster_dof_k = 0;
    std::vector<std::string> nested_fe;
    bool npd_fix = false;
};

// Heteroskedasticity-robust meat: sum_i e_i^2 x_i x_i'  (k x k, full).
void hc_meat(const double* design, int64_t n, int k, const double* e, double* meat);

// Per-cluster score rows: S is G x k (column-major), row g = sum_{i in g} x_i e_i.
void cluster_scores(const double* design, int64_t n, int k, const double* e,
                    const Factor& cl, double* S);

// One-way cluster meat: sum_g sg sg', sg = sum_{i in g} x_i e_i. *n_clusters=G.
void cluster_meat(const double* design, int64_t n, int k, const double* e,
                  const Factor& cl, double* meat, int* n_clusters);

// Upper bound on multiway cluster dimensions: the meat sums 2^d - 1 subset meats, so
// the bound keeps that loop finite (255 subsets at 8). A design choice, not a Stata
// rule — reghdfe itself sets none; past it the engine refuses by name.
constexpr int kMaxMultiwayClusters = 8;

// Multiway (Cameron-Gelbach-Miller) meat over d = cls.size() dimensions:
// sum over non-empty subsets S of (-1)^(|S|+1) · meat(intersection of S). No
// small-sample factor is applied here (compute_vcov owns it).
void multiway_meat(const double* design, int64_t n, int k, const double* e,
                   const std::vector<const Factor*>& cls, double* meat);

// True iff every level of `fe` maps to a single cluster level (nested).
bool fe_nested_in_cluster(const Factor& fe, const Factor& cl);

// Assemble the requested vcov. bread = (design'design)^{-1} (k x k). sigma2 is
// used only for IID (= ssr/dof_resid). k_full = k + absorbed FE params.
// n_eff: the observation count the HC1 n/(n-k) and cluster (N-1)/(N-k) small-sample
// factors are built from. It equals `n` for every unweighted fit and for analytic/
// probability weights (where Stata's N is the row count); under frequency/importance
// weights Stata's N is Σw instead, so the caller passes that. <=0 (the default) means
// "use n" — which is what keeps every pre-weights caller byte-identical.
//
// ml_cluster_factor (BUG-175): selects which small-sample multiplier the CLUSTER /
// multiway branch applies.
//   false (default) — the REGRESSION (Cameron-Gelbach-Miller) factor
//                     G/(G-1) · (N-1)/(N-K). Correct for feols/reghdfe and 2SLS,
//                     and what Stata's own `regress, vce(cluster)` uses.
//   true            — the PURE ML factor G/(G-1), with no (N-1)/(N-K) term. This is
//                     what Stata's likelihood commands (`poisson`, `logit`, `nbreg`,
//                     `mlogit`, …) use; carrying the regression term there inflates
//                     every SE by sqrt((N-1)/(N-K)) and deflates the model Wald by
//                     (N-1)/(N-K). Measured against the P2 answerbook — see
//                     BUG-175 and the sweep recorded there.
// Only the cluster/multiway branch reads it; IID and HC1 are unaffected. Defaulting
// to false keeps every existing caller byte-identical.
//
// keep_nested_fe_dof (BUG-568): which K the cluster/multiway (N-1)/(N-K) factor uses.
//   false (default) — reghdfe / xtreg,fe: an FE nested in a cluster is EXEMPT, its
//                     (levels-1) parameters leave K and it is listed in nested_fe.
//                     w2b_estim.log reghdfe_cluster_ctl / xtreg_fe_cluster_ctl:
//                     se[x1] .0663182832 (nested cluster id).
//   true            — areg: every absorbed parameter is charged, nested or not
//                     (K = k_full). Same fit, areg_a_cl: se[x1] .0722686736,
//                     N_clust 10, df_r 9. A non-nested cluster is unaffected
//                     (areg_cluster_nonnested agrees with reghdfe).
// IID and HC1 never read it; defaulting to false keeps every caller byte-identical.
VcovResult compute_vcov(Vcov vt, const double* design, int64_t n, int k, const double* e,
                        const double* bread, int k_full, double sigma2,
                        const std::vector<Factor>& fe,
                        const std::vector<Factor>& cluster, int cluster_index,
                        double n_eff = -1.0, bool ml_cluster_factor = false,
                        bool keep_nested_fe_dof = false);

}  // namespace cecon
