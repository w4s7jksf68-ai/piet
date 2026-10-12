// cecon — wild cluster bootstrap-t (WCR: impose the null), fast algorithm.
// Demeans + fits once, then each replicate is O(G^2) in the per-cluster pieces —
// no per-replicate refit. Bit-reproducible via the Philox counter RNG.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/model.h"

namespace cecon {

enum class BootWeights { Rademacher, Webb };

struct WildBootResult {
    int reps = 0;
    std::string weights;
    std::string param;
    double null_beta = 0;
    double beta = 0;   // unrestricted point estimate of the tested coef
    double se = 0;      // its cluster-robust SE
    double t0 = 0;      // (beta - null)/se
    double p_symmetric = 0;
    double p_equaltail = 0;
    int n_clusters = 0;
    uint64_t seed = 0;
    bool webb_fallback = false;
    std::vector<double> tstars;  // per-replicate bootstrap t (for tests/inspection)
};

// WCR wild cluster bootstrap-t for FE-OLS, testing H0: beta[param_index] = null_beta.
// Demeans md.dm in place. cluster_index selects the md.cluster dimension.
WildBootResult wild_cluster_bootstrap(ModelData& md, int param_index, double null_beta,
                                      int reps, uint64_t seed, int cluster_index = 0,
                                      BootWeights weights = BootWeights::Rademacher,
                                      bool webb_auto = true);

// WCR wild cluster bootstrap-t for 2SLS, testing the single endogenous regressor
// = null_beta (the common IV case). Currently supports exactly one endogenous
// regressor, which is the parameter tested. Demeans md in place.
WildBootResult wild_cluster_bootstrap_iv(IvModelData& md, double null_beta, int reps,
                                         uint64_t seed, int cluster_index = 0,
                                         BootWeights weights = BootWeights::Rademacher,
                                         bool webb_auto = true);

// Thread count the wild-cluster-bootstrap replicate loop uses for `reps`
// replicates over `G` clusters: threads_for_work with the loop's work model
// (each replicate is the G×G contraction) and its per-thread scratch (the
// transient v(G) plus allocator slack). Exposed for test_thread_determinism's
// anti-vacuity check.
int wildboot_fanout_threads(int reps, int G);

}  // namespace cecon
