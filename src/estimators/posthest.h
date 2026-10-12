// Post-estimation utilities: Hausman, predict, margins.
//
// All work on pre-computed (β, V) tuples plus the data — no command coupling.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/distributions.h"

namespace cecon {

struct HausmanResult {
    int dof = 0;             // # parameters used in the test
    double chi2 = 0;
    double pvalue = 0;       // approximate (chi² right-tail)
    bool ok = true;
    std::string error;
};

// Hausman test: H = (b_FE - b_RE)' (V_FE - V_RE)^{-1} (b_FE - b_RE).
//   b_FE, b_RE: coefficient vectors (length k)
//   V_FE, V_RE: covariance matrices (k*k, column-major)
// If V_FE - V_RE is not PD (can happen in finite samples), we PSD-project
// the difference before inverting (Stata "Hausman test inconsistent" workaround).
HausmanResult hausman_test(const double* b_FE, const double* V_FE,
                           const double* b_RE, const double* V_RE,
                           int k);

// predict_xb: xb_i = X_i β + offset_i. X is column-major n*k. Returns nullptr-
// safe; writes into out (length n).
void predict_xb(const double* X, int64_t n, int k,
                const double* beta, const double* offset,
                double* out);

// Average marginal effect for Poisson (or any log-link, exp-mean model):
//   AME_c = (1/n) Σ_i β_c · exp(X_i β + off_i) = β_c · ymean
// with delta-method SE:
//   var(AME_c) = J V J'   where J_d = β_c · (1/n) Σ_i X_i,d · exp(X_i β + off_i)
//                                     + δ_{cd} · (1/n) Σ_i exp(X_i β + off_i)
// V is k*k.
struct MarginsResult {
    std::vector<double> dydx;
    std::vector<double> se;
    bool ok = true;
};

MarginsResult margins_poisson(const double* X, int64_t n, int k,
                              const double* beta, const double* V,
                              const double* offset = nullptr);

// Logit marginal effects (binary):
//   p_i  = 1/(1 + e^{-X_i β})
//   AME_c = (1/n) Σ_i β_c · p_i (1 - p_i)
MarginsResult margins_logit(const double* X, int64_t n, int k,
                            const double* beta, const double* V);

// Probit marginal effects (binary): AME_c = (1/n) Σ_i β_c · φ(X_i β),
// φ = standard normal pdf; delta-method SE. V is k*k.
MarginsResult margins_probit(const double* X, int64_t n, int k,
                             const double* beta, const double* V);

// --- P5 (BUG-158/159): weighted, interaction-aware, factor-discrete-change AME --
//
// margins_logit/probit/poisson above are correct ONLY when every design column
// is a single continuous main effect (no `#`/`##` interactions, no `i.`/`level.var`
// factor columns) and the fit is unweighted. Three independent failures otherwise:
//   1. No weights — under [pweight=] the AME must be the pweight-weighted mean over
//      the estimation sample, not a plain arithmetic mean.
//   2. No interaction chain rule — with e.g. `c.x1##i.treated`, d(xb)/d(x1) is
//      beta_x1 + beta_{x1#1.treated}*treated_i, not just beta_x1.
//   3. Factor levels need the Stata "discrete change from base" contrast
//      (mean_i[Pr(y|level)] - mean_i[Pr(y|base)]), not a pdf-scaled derivative.
//
// margins_ame implements all three, generalized over the logit/probit/poisson
// link, driven ENTIRELY off the design's column NAMES (BUG-101 convention:
// `level.var` for a factor level, `a#b` for an interaction of parts `a`,`b`,
// each itself either `var` (continuous) or `level.var`). It never re-derives the
// design — it only reads which named columns exist and what they multiply.
enum class GlmLink { Logit, Probit, Poisson };

// One requested/derived AME row: a continuous variable's chain-rule slope, or one
// non-base level of a requested factor variable's discrete change. `name` is the
// BUG-101 column-naming convention ("x1", "1.treated") so callers can key off it
// the same way coeftables already do.
struct AmeRow {
    std::string name;
    double dydx = 0;
    double se = 0;
};

struct AmeResult {
    std::vector<AmeRow> rows;
    bool ok = true;
    std::string error;
};

// X: column-major n*k design (as fit — including any intercept column, named
// "(intercept)", which is silently skipped for both requesting and reporting).
// names: length k, BUG-101 column names aligned with X/beta/V.
// beta/V: the fitted coefficients and the model's OWN (possibly sandwich, possibly
// pweight-reweighted) covariance — whatever VCE the caller wants the delta-method
// SEs built from.
// weights: length n raw estimation weights, or nullptr for an unweighted (mean)
// AME.
// request_vars: base variable names to report (Stata `dydx()` — e.g. {"x1",
// "treated"}, NOT design-column names); empty means "every column with no `#`",
// i.e. auto-report every main-effect/level column, matching the shape of the
// simple per-column functions above when there are no interactions.
// offset: optional GLM offset (Poisson), length n, or nullptr.
AmeResult margins_ame(GlmLink link, const double* X, int64_t n, int k,
                      const std::vector<std::string>& names,
                      const double* beta, const double* V,
                      const double* weights,
                      const std::vector<std::string>& request_vars,
                      const double* offset = nullptr);

// --- Distribution tail probabilities -----------------------------------------
// betai / t_twosided_pvalue / t_critical / z_critical moved to
// core/distributions.h at the P6 wave so PIET's resampling driver can share the
// SAME implementation instead of growing a second copy of the same mathematics.
// Included here so every existing consumer of this header still sees them.
// Upper-tail p-value of an F(df1, df2) statistic: P(F > f).
double f_pvalue(double f, double df1, double df2);
// Upper-tail p-value of a chi-square(dof) statistic at x.
double chi2_pvalue(double x, double dof);
// Lower-tail χ² CDF P(X²_dof ≤ x), accurate in the left tail.
double chi2_cdf(double x, double dof);
// Regularized lower incomplete gamma P(a, x).
double gammp(double a, double x);

// --- General linear (Wald) test of H0: R β = q -------------------------------
//   chi2 = (Rβ−q)' (R V R')⁻¹ (Rβ−q) ~ χ²(J);  F = chi2/J.
// R is J*k row-major, q is length J, V is k*k column-major. If R V R' is not PD
// it is PSD-projected before inversion (like hausman_test); ok=false if it still
// cannot be inverted. Distribution-free — caller forms the p-values.
struct WaldResult {
    int J = 0;
    double chi2 = 0;
    double F = 0;
    bool ok = true;
    std::string error;
};
WaldResult wald_test(const double* beta, const double* V, int k,
                     const double* R, const double* q, int J);

}  // namespace cecon
