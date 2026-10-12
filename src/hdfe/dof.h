// cecon — degrees-of-freedom and fixed-effect redundancy accounting.
// This is the most error-prone module for standard-error correctness, so the
// result echoes the full breakdown.
#pragma once
#include <cstdint>
#include <vector>

#include "core/model.h"

namespace cecon {

struct DofInfo {
    int64_t n = 0;
    int k_vars = 0;            // estimated slope coefficients (X)
    int k_fe = 0;              // absorbed FE parameters, redundancy removed
    int fe_redundant = 0;      // sum(levels) - k_fe
    int64_t dof_resid = 0;     // n - k_vars - k_fe
    bool fe_exact = true;      // false when k_fe is approximated (3+ FE dims)
    std::vector<int> fe_levels;
};

// k_vars = number of X slope coefficients (no intercept when FE present).
// 1 FE: k_fe = levels. 2 FE: k_fe = K1 + K2 - (#connected components) — exact.
// 3+ FE: k_fe = K1 + sum_{g>=2}(K_g - 1) — conservative, flagged inexact.
DofInfo compute_dof(int64_t n, int k_vars, const std::vector<Factor>& fe);

}  // namespace cecon
