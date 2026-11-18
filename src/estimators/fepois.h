// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Poisson regression with high-dimensional fixed effects (PPML), via
// IRLS where each step is a weighted HDFE-demeaned WLS. Matches fepois/ppmlhdfe.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/model.h"
#include "hdfe/dof.h"
#include "vcov/vcov.h"

namespace cecon {

struct FepoisResult {
    int64_t n = 0;
    int k = 0;
    std::vector<std::string> names;
    std::vector<double> beta;
    std::vector<double> se;
    std::vector<double> vcov_matrix;
    std::vector<double> full_vcov_matrix;  // absorbed fits: slopes followed by recovered _cons
    std::string vcov_type;

    double deviance = 0;
    int irls_iters = 0;
    bool converged = true;
    bool separation = false;  // crude separation/non-convergence flag
    // ModelData::stata_ml: the ml path's iteration count, log likelihood at each
    // iteration 0..ic and gradient at the posted iterate; ml_ran false (and these
    // empty) when the IRLS answer stands.
    bool ml_ran = false;
    int ic = 0;
    std::vector<double> ilog;
    std::vector<double> gradient;
    // The inverse information (bread) of a pooled fit whose VCE is a sandwich:
    // Stata's e(V_modelbased). Empty otherwise.
    std::vector<double> vcov_model;

    // Stata `poisson` e(ll)/e(ll_0)/e(chi2)/e(df_m)/e(r2_p). Additive: every field
    // above keeps the value it had before these existed. ll is the Poisson
    // log-likelihood Σ[y·ln μ − μ − ln y!] at the converged μ; ll_0 is the same for
    // the PLAIN intercept-only model (Stata's null for `poisson` — with FE absorbed
    // the FE are NOT in the null); r2_p = 1 − ll/ll_0 (McFadden).
    double ll = 0;
    double ll_0 = 0;
    // The intercept-only null WITHOUT the offset: ppmlhdfe's e(ll_0) (bank 2f9a91bc
    // `ppmlhdfe ..., absorb(sid) offset(lpop)`: -119412.988171009, the ll of
    // `poisson fatal`), where `poisson`'s keeps it. Equal to ll_0 when there is none.
    double ll_0_nooffset = 0;
    double r2_p = 0;
    // Model test over the non-intercept coefficient block. Stata's rule: LR = 2(ll−ll_0)
    // under iid VCE, a Wald quadratic form b'V⁻¹b under robust/cluster (the likelihood
    // is no longer the estimating criterion once the VCE is a sandwich).
    double chi2 = 0;
    int chi2_df = 0;
    bool chi2_is_wald = false;
    bool ll_valid = false;  // false only if the fit never reached a usable μ

    DofInfo dof;
    int n_clusters = 0;
    int cluster_dof_k = 0;
    std::vector<std::string> nested_fe;

    int demean_iters = 0;
    bool demean_converged = true;
    int64_t n_dropped = 0;
    bool has_offset = false;

    // ppmlhdfe-style constant recovered under absorbed FE (there is no _cons column in
    // the design — the FE absorb it — so it is emitted from these fields, not beta/se).
    double cons_b = 0;
    double cons_se = 0;
    bool has_cons = false;       // a recovered _cons row is available (absorb case)
    bool cons_se_valid = false;  // recovered intercept has a full fitted covariance row

    // Regressors dropped before the fit as collinear (with the absorbed effects, or
    // with earlier regressors), in design order, and the design's names before the
    // drop (add_omitted's placement order).
    std::vector<std::string> omitted;
    std::vector<std::string> full_names;
};

// Fit Poisson with HDFE. Uses md.offset if present (e.g. log population exposure).
// fe_sum_out (opt-in, absorbed fits only): per kept row, ppmlhdfe's `d` -- the sum
// of the absorbed effects net of _cons, eta - offset - x'b - _cons.
FepoisResult fepois_fit(ModelData& md, Vcov vt, int cluster_index = 0,
                        std::vector<double>* fe_sum_out = nullptr);

// Stata `poisson`'s ml path on a plain design (no absorbed effects): poisson's own
// starting values, then ml's modified Newton-Raphson stopped by ml's rule or after
// `maxiter` iterations (iterate(#); 0 posts the start). X is n x k column-major,
// `cons` the constant's column (-1 under noconstant), w the row weights (null = 1),
// off the offset (null = none). False (b, ll untouched) when the path cannot run.
bool stata_poisson_ml(const double* X, int64_t n, int k, const double* y, const double* w,
                      const double* off, int cons, int maxiter, std::vector<double>& b,
                      double& ll);

}  // namespace cecon
