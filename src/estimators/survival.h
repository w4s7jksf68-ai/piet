// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — survival analysis: `stset`, `stcox`, `streg, distribution(weibull)`.
//
// THE THREE VERBS AND HOW THEY CONNECT
//
//   stset  is an ENGINE verb, not PIET state. It materializes four real f64
//          columns on the dataset — `_t0`, `_t`, `_d`, `_st` — from the raw
//          (id, time, failure) triple, exactly as Stata's `stset` does. That is
//          data computation (a per-subject scan with an exclusion rule), so it
//          belongs here; everything downstream then reads _t0/_t/_d/_st as
//          ordinary variables with no hidden session state.
//   stcox  Cox proportional-hazards partial likelihood with DELAYED ENTRY
//          (risk set R(t) = { i : _t0_i < t <= _t_i }), Breslow tie handling.
//   streg  Weibull proportional-hazards MLE in (beta, ln p), also with the
//          delayed-entry correction.
//
// Every formula below is pinned against StataBE 19.5 on the P7 pangram wave
// (internal capture p7_stata.log); the assertions live in
// tests/unit/test_survival.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

// ---------------------------------------------------------------------------
// stset
// ---------------------------------------------------------------------------

// The four materialized columns plus the report Stata prints under
// "Survival-time data settings". Every vector is length n and is in the
// dataset's ORIGINAL row order — stset_build sorts internally to walk each
// subject's records in time order, then scatters the results back, so the verb
// never reorders the caller's rows. (Stata leaves st data sorted by `id _t`;
// nothing downstream depends on row order, and not reordering keeps the verb's
// side effect to "four columns appeared".)
struct StsetResult {
    std::vector<double> t0, t, d;  // _t0 (entry), _t (exit), _d (failure indicator)
    std::vector<double> st;        // _st: 1 = in the analysis sample, 0 = excluded

    int64_t n_total = 0;  // rows in

    // Exclusion counters — one per report line Stata prints. Their sum is
    // n_total - n_kept. Only n_excl_after_failure fires on the P7 fixture; the
    // other three are emitted anyway so the report shape never depends on which
    // exclusions a particular dataset happens to trigger.
    int64_t n_excl_missing = 0;        // "obs. with missing values"  (id/time/failure)
    int64_t n_excl_after_failure = 0;  // "obs. begin on or after (first) failure"
    int64_t n_excl_negative = 0;       // "obs. with negative time"   (_t < _t0)
    int64_t n_excl_zero_length = 0;    // "obs. end on or before enter()" (_t == _t0)
    int64_t n_excl_same_instant = 0;   // "multiple records at same instant" (id() data)

    int64_t n_kept = 0;       // "observations remaining"
    int64_t n_subjects = 0;   // distinct id among kept records
    int64_t n_failures = 0;   // kept records with _d != 0
    double time_at_risk = 0;  // Sum(_t - _t0) over kept records
    double origin = 0;        // "At risk from t =" (always 0 here: origin(time 0))
    double first_entry = 0;   // "Earliest observed entry t =" — min _t0 over kept
    double last_exit = 0;     // "Last observed exit t ="      — max _t  over kept
    bool single_failure = false;  // no subject has more than one kept failure

    bool ok = false;
    std::string err;
};

// `idv` may be null (single-record data: every row is its own subject).
// `failv` may be null — then, following Stata's failure()-less default, the LAST
// record of each subject fails (every record, when idv is null).
//
// The exclusion rule that produces Stata's "obs. begin on or after (first)
// failure" line: within a subject, once a record with _d != 0 has been seen in
// exit-time order, EVERY later record is excluded (_st = 0). Equivalently, a
// record whose _t0 is at or past the subject's first failure time is out.
// Excluded rows PERSIST in the data with their _t0/_t/_d still computed — Stata
// keeps them and every st command skips them on _st.
StsetResult stset_build(const double* idv, const double* timev, const double* failv, int64_t n);

// ---------------------------------------------------------------------------
// stcox — Cox proportional hazards, delayed entry, Breslow or Efron ties, strata
// ---------------------------------------------------------------------------

struct CoxResult {
    std::vector<double> beta, se;  // length k (NO intercept — it cancels out of the
                                   // partial likelihood, so the design must not carry one)
    std::vector<double> vcov;      // k*k, the inverse observed information I^-1
    double ll = 0;                 // log partial likelihood at the optimum
    double ll_null = 0;            // ... at beta = 0 (Stata's "Iteration 0")
    int64_t n = 0, n_fail = 0;
    int64_t n_tied_failures = 0;   // failure records sharing a failure time with another
                                   // (0 => Stata's header is "Cox regression with no ties")
    int iters = 0;
    bool converged = false, ok = false;
    std::string err;
};

// Tie handling: Stata's default `breslow`, `efron`, `exactp` (the exact partial
// likelihood: a tie set's term is over every subset of the risk set of its size) or
// `exactm` (the exact marginal likelihood: the probability that the tie set's
// members fail before the rest of the risk set, evaluated as Stata evaluates it:
// 15-point Gauss-Laguerre quadrature of its integral form).
enum class CoxTies { Breslow, Efron, Exactp, Exactm };

// X is col-major n*k with NO intercept column. t0/t/d are the stset columns
// restricted to the estimation sample (_st == 1 and listwise-complete on X).
// Requires t > t0 for every row (stset already guarantees that for _st == 1).
// `strata` (optional, length n): stratum codes -- Stata's strata(): each stratum
// has its own baseline hazard, so a failure's risk set holds its own stratum's
// records only and the log partial likelihood is the sum over strata.
CoxResult cox_fit(const double* X, int64_t n, int k, const double* t0, const double* t,
                  const double* d, const int32_t* strata = nullptr,
                  CoxTies ties = CoxTies::Breslow);

// Per-observation EFFICIENT SCORE RESIDUALS at `beta` — the same quantity Stata's
// `predict, esr` produces, and the meat ingredient for stcox's Lin-Wei robust /
// cluster-robust VCE:
//
//   U_i = d_i (x_i - xbar(t_i))
//         - exp(x_i b) * SUM_{j : failure, t0_i < t_j <= t_i} (x_i - xbar_j) / S0_j
//
// Written into `out` as an n*k COLUMN-major matrix (same layout convention as X),
// so summing rows within a cluster and forming Sum_g s_g s_g' gives the meat.
void cox_score_residuals(const double* X, int64_t n, int k, const double* t0, const double* t,
                         const double* d, const std::vector<double>& beta,
                         std::vector<double>* out);

// ---------------------------------------------------------------------------
// streg, distribution(weibull) — Weibull PH MLE in (beta, ln p)
// ---------------------------------------------------------------------------

namespace weib {

// Log likelihood and analytic gradient at theta = (beta[0..k-1], ln p).
// Per record, with the delayed-entry correction (the `t0^p` term = -ln S(t0)):
//
//   l_i = d_i [ ln p + p ln t_i + x_i b ] - (t_i^p - t0_i^p) exp(x_i b)
//                     ^^^^^^^^^
// NOTE THE `p ln t`, NOT `(p-1) ln t`. The hazard-metric Weibull log density is
// d_i[ln p + (p-1) ln t_i + x_i b] - ...; Stata's `streg` reports (and iterates
// on) the likelihood in the LOG-TIME metric it fits in, which differs by the
// change-of-variable Jacobian Sum_i d_i ln t_i. MEASURED, not assumed: on the P7
// sample the hazard-metric optimum is 99.118828 and Stata prints 156.73414, and
// Sum d ln t = 57.61531333 closes the gap to all eight printed digits for BOTH
// the constant-only prefit (98.765920 -> 156.38123) and the full model. The term
// carries no parameter, so gradient, Hessian, estimates and SEs are untouched —
// it shifts the reported LL only.
//
// `scores` (optional): n*np COLUMN-major output (np = k+1; column c, row i at
// scores[c*n+i]) filled with the SAME per-observation gradient contribution that
// would otherwise only be summed into *grad, so a robust/cluster meat can be
// built straight from it. Passing both grad and scores recomputes the shared
// coefficients in two independent branches, deliberately — see tobit.h:64-73 for
// why hoisting them into a common temp perturbs the last bits of the grad path.
double ll_grad(const double* X, int64_t n, int k, const double* t0, const double* t,
               const double* d, const std::vector<double>& theta, std::vector<double>* grad,
               std::vector<double>* scores = nullptr);

}  // namespace weib

struct WeibullResult {
    std::vector<double> beta, se;  // length k, in the design's column order
    std::vector<double> vcov;      // (k+1)*(k+1) in the (beta, ln p) metric
    double ln_p = 0, se_ln_p = 0;  // the ancillary parameter as fitted
    double p = 0, se_p = 0;        // exp(ln_p), delta-method SE = p * se_ln_p
    double inv_p = 0, se_inv_p = 0;  // 1/p, delta-method SE = se_ln_p / p
    double ll = 0;                 // Stata-metric LL (see weib::ll_grad)
    double ll_null = 0;            // constant-only prefit LL (Stata's "Fitting constant-only model")
    int64_t n = 0, n_fail = 0;
    int iters = 0, iters_null = 0;
    bool converged = false, ok = false;
    std::string err;
};

// X col-major n*k. `cons_col` is the index of the design's constant column, or -1
// when the model has none; when it is >= 0 the fit mirrors Stata's two-stage log
// — a constant-only model is fitted first and its (b_cons, ln p) seeds the full
// fit (slopes at 0) — and its LL is returned as ll_null.
WeibullResult weibull_fit(const double* X, int64_t n, int k, const double* t0, const double* t,
                          const double* d, int cons_col);

// ---------------------------------------------------------------------------
// streg, the parametric families on Stata's ml path
// ---------------------------------------------------------------------------
//
// Every family is fitted as Stata's ado fits it: a constant-only model first
// (its log likelihood is e(ll_0)), then the full model by ml's modified
// Newton-Raphson (logit.h ml_newton_path: same step rule, same stopping rule)
// from the constant-only estimates with the slopes at 0, in the ado's own
// parametrization. The log likelihood is Stata's: the record's
// d ln f(t) + (1-d) ln S(t) - ln S(t0) plus d ln t (the log-time metric; see
// weib::ll_grad), for every family.
//
//   exponential  ln h = xb                                  (PH)
//   weibull      ln h = xb + ln p + (p-1) ln t, p = exp(/ln_p)  (PH; the ado fits
//                it with ln t centred on its sample mean and moves _cons back
//                after the fit, or into the AFT metric under `time`)
//   gompertz     ln h = xb + gamma t, gamma = /gamma         (PH)
//   loglogistic  S = 1 / (1 + exp(z)), z = (ln t - xb) / g, g = exp(/lngamma)  (AFT)
//   lognormal    S = 1 - Phi(z),       z = (ln t - xb) / s, s = exp(/lnsigma)  (AFT)
//
// The ancillary parameter is an equation of its own: `Z` (n*kz, col-major, the
// constant LAST) is its design -- one column of ones without ancillary().
enum class StregDist { Exponential, Weibull, Gompertz, Loglogistic, Lognormal };

struct StregFit {
    std::vector<double> theta;      // reported metric: k main coefficients, then kz ancillary
    std::vector<double> V;          // np*np, reported metric (row-major, symmetric)
    std::vector<double> V_fit;      // np*np, the fitted metric: inverse of -H at the optimum
    std::vector<double> J;          // np*np, d reported / d fitted (identity but for weibull's)
    std::vector<double> gradient;   // d ll / d theta at the last iterate (the fitted metric)
    std::vector<double> ilog;       // full model: ll at iteration 0..ic
    std::vector<double> scores;     // n*np col-major, the fitted metric (a sandwich's meat)
    double ll = 0, ll_0 = 0;
    int ic = 0;
    bool converged = false, converged_cons = false;
    bool ok = false;
    std::string err;
};

// `cons_col`: the constant's column of X (-1: none -- noconstant is not offered).
// `time_metric`: weibull's AFT form (the PH fit remetricked, as the ado does).
StregFit streg_fit(StregDist dist, bool time_metric, const double* X, int64_t n, int k, int cons_col,
                   const double* Z, int kz, const double* t0, const double* t, const double* d);
// J * Vf * J' (np*np row-major), symmetrized: a fitted-metric covariance in the reported metric.
std::vector<double> streg_remetric(const std::vector<double>& J, const std::vector<double>& Vf, int np);

// ---------------------------------------------------------------------------
// shared robust/cluster sandwich
// ---------------------------------------------------------------------------

// V = factor * bread * (Sum_g s_g s_g') * bread, where s_g sums the rows of the
// np-column score matrix `S` (n*np, col-major) belonging to cluster g. `codes`
// may be null, in which case every observation is its own cluster (plain
// observation-level robust). Returns {} when the shapes disagree.
std::vector<double> survival_sandwich(const std::vector<double>& S, int64_t n, int np,
                                      const std::vector<double>& bread, const int32_t* codes,
                                      int n_levels, double factor);

}  // namespace cecon
