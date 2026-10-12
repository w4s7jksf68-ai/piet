// Callaway–Sant'Anna staggered difference-in-differences estimation.
//
// Implemented from the published methodology papers:
//   [CS21] Callaway, B. and P.H.C. Sant'Anna (2021), "Difference-in-Differences
//          with multiple time periods", Journal of Econometrics 225(2), 200-230.
//   [SZ20] Sant'Anna, P.H.C. and J. Zhao (2020), "Doubly robust difference-in-
//          differences estimators", Journal of Econometrics 219(1), 101-122.
// Clean-room provenance record: docs/legal/CSDID_CLEANROOM_WALL.md.
//
// drdid_dripw_cell estimates one 2x2 group-time cell with SZ20's doubly-robust
// DiD estimator for panel data (propensity score by ML logit, outcome
// regression by least squares on the comparison group), returning the ATT, its
// influence-function standard error, and the per-unit influence function.
//
// csdid_dripw_run is the CS21 driver: it enumerates the ATT(g,t) family over
// cohorts g and periods t (base period: g's preceding period for post-treatment
// cells, t's preceding period for pre-treatment cells — CS21 §2-3), estimates
// each cell by drdid_dripw_cell, and computes the paper's aggregations (simple
// group-size-weighted ATT, event-study bins, calendar-period effects) plus a
// joint pre-trends Wald test, all with influence-function inference (CS21 §4).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cecon {

struct DrdidCell {
    double att = 0;
    double se = 0;             // IID influence-function SE (== cluster SE for 1 row/unit)
    int64_t n = 0;
    std::vector<double> inf;   // per-unit recentered influence function (mean == att)
    bool ok = true;
};

// One 2x2 doubly-robust DiD cell (SZ20 §3.1, panel case). dy[n] = long
// difference Y_t - Y_base per unit; X = n*p covariates (column-major, no
// constant — a constant is appended internally as the LAST column); trt[n] in
// {0,1}. Comparison units whose fitted propensity exceeds pscoretrim receive
// zero weight (default trim 0.995).
DrdidCell drdid_dripw_cell(const double* dy, const double* X, int64_t n, int p,
                           const double* trt, double pscoretrim = 0.995);

// -----------------------------------------------------------------------------
// csdid_dripw_run: full Callaway-Sant'Anna driver.

enum class CsdidComparison { Never, Notyet };

struct CsdidConfig {
    CsdidComparison comparison = CsdidComparison::Never;
    double pscoretrim = 0.995;
    // long2: use a FIXED base period = g - gdelta (the period immediately
    // before each cohort's first treatment, on the regular gdelta-spaced grid)
    // for every (g,t) cell — the CS21 long-difference / standard event-study
    // parametrization. The default (false) is the varying base (the period
    // preceding t for pre-treatment cells, the period preceding g for post).
    // long2 also makes time0 an eligible target period (it is a valid
    // long-difference target vs g - gdelta, whereas under the varying base it
    // can only ever be a base).
    bool long2 = false;
    // cluster[n_obs]: cluster id per row (constant within unit). If non-null,
    // SEs cluster the influence functions by this variable:
    // VV = Σ_c (Σ_{u∈c} IF_u)² / N². If null, each unit is its own cluster
    // (the default — identical to unit-level IID inference).
    const double* cluster = nullptr;
};

struct CsdidCellEstimate {
    double g = 0;       // cohort
    double base = 0;    // base period (time1) used for this cell
    double t = 0;       // outcome period
    double att = 0;
    double se = 0;
    int64_t n_cell = 0; // # units used by the DR estimator for this cell
    bool ok = true;
};

struct CsdidEventBin {
    int e;              // event time = t - g
    double att;
    double se;
};

struct CsdidRunResult {
    std::vector<double> glev;        // sorted unique gvar > 0
    std::vector<double> tlev;        // sorted unique time minus min
    double time0 = 0;
    std::vector<double> pg;          // pg_i = #{u: gvar_u == g_i} / N_units, length G
    std::vector<CsdidCellEstimate> cells;  // K = G * T_minus1 cells, in (i, j) row-major

    // Aggregations
    double simple_att = 0;
    double simple_se = 0;
    double pre_avg = 0, pre_avg_se = 0;
    double post_avg = 0, post_avg_se = 0;
    std::vector<CsdidEventBin> events;   // one bin per realized event time in window

    // Event-study e(b)/e(V) (for `estat event, post`). event_names[j] is the
    // relative-period name for events[j]: "Tm#" for e=-#, "Tp#" for e=+#
    // (Tp0 = the e==0 contemporaneous period). event_b == events[j].att.
    // event_V is the full E×E covariance of the event vector (row-major), built
    // from the per-event influence functions (Σ_c IF_h[c]·IF_h'[c]/N²), so the
    // diagonal sqrt == events[j].se and off-diagonals support downstream
    // test/lincom on the posted event coefficients.
    std::vector<std::string> event_names;
    std::vector<double> event_b;            // length E
    std::vector<double> event_V;            // E*E, row-major

    // Pre-trends test: joint Wald that all pre-treatment att(g,t) cells (t<g) == 0.
    double pretrend_chi2 = 0, pretrend_p = 1;
    int pretrend_df = 0;
    // Calendar-period effects: group-weighted ATT per outcome period over post cells.
    struct CalendarBin { double t; double att; double se; };
    std::vector<CalendarBin> calendar;
    // Group (cohort) effects: ATT per cohort g over its post cells (t >= g).
    struct GroupBin { double g; double att; double se; };
    std::vector<GroupBin> groups;

    int64_t n_units = 0;
    int64_t n_obs_est = 0;   // # distinct raw rows used across all (g,t) cell
                             // estimation samples (base+target rows, unioned).
    bool ok = true;
    std::string error;
};

// Inputs (full panel, n_obs rows):
//   ivar[n_obs]: unit identifier (any double; equality used to group)
//   time[n_obs]: time variable
//   gvar[n_obs]: cohort var per row (must be constant within unit)
//   y[n_obs]:    outcome
//   X[n_obs * p] column-major (or nullptr if p==0)
//   p:           number of covariates (a constant is appended internally)
// Optional:
//   event_window = {lo, hi}: if {0, 0}, all realized event times are included;
//   otherwise only events e with lo <= e <= hi.
CsdidRunResult csdid_dripw_run(const double* ivar, const double* time,
                               const double* gvar, const double* y,
                               const double* X, int p, int64_t n_obs,
                               const CsdidConfig& cfg = {},
                               int event_lo = 0, int event_hi = 0);

}  // namespace cecon
