// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "survival.h"

#include "core/threads.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

#include "core/linalg.h"

namespace cecon {

namespace {

inline bool miss(double v) { return std::isnan(v); }

// Sort key that keeps a strict weak ordering in the presence of Stata missings:
// a missing sorts LAST (Stata's own convention) and all missings compare equal,
// so std::stable_sort's comparator stays valid instead of tripping the NaN trap.
inline double sort_key(double v) { return std::isnan(v) ? HUGE_VAL : v; }

}  // namespace

// ---------------------------------------------------------------------------
// stset
// ---------------------------------------------------------------------------

StsetResult stset_build(const double* idv, const double* timev, const double* failv, int64_t n) {
    StsetResult r;
    r.n_total = n;
    const double kNaN = std::nan("");
    r.t0.assign(static_cast<size_t>(n), kNaN);
    r.t.assign(static_cast<size_t>(n), kNaN);
    r.d.assign(static_cast<size_t>(n), kNaN);
    r.st.assign(static_cast<size_t>(n), 0.0);
    if (!timev) {
        r.err = "stset requires a time variable";
        return r;
    }
    if (n == 0) {
        r.ok = true;
        r.single_failure = true;
        return r;
    }

    // (id, time, original row) ordering — the sequence each subject's records are
    // walked in. The original-row tiebreak makes the result deterministic when a
    // subject has two records at the same exit time.
    std::vector<int64_t> ord(static_cast<size_t>(n));
    std::iota(ord.begin(), ord.end(), static_cast<int64_t>(0));
    std::stable_sort(ord.begin(), ord.end(), [&](int64_t a, int64_t b) {
        const double ia = idv ? sort_key(idv[a]) : static_cast<double>(a);
        const double ib = idv ? sort_key(idv[b]) : static_cast<double>(b);
        if (ia != ib) return ia < ib;
        const double ta = sort_key(timev[a]), tb = sort_key(timev[b]);
        if (ta != tb) return ta < tb;
        return a < b;
    });

    // id() data: Stata's own rules (st_set.ado's id branch, the analysis time being
    // the time variable itself). Within a subject, records in time order:
    //  * a record sharing its time with the record before or after it is "multiple
    //    records at same instant" and excluded -- every record of the tie;
    //  * _t0 is the previous record's time (the first record enters at 0), the
    //    previous record counted whether or not it is excluded;
    //  * the subject's exit is its first failure time over all its records, tied
    //    ones included: a record entering on or after it is excluded ("begin on or
    //    after (first) failure"), one straddling it is censored there;
    //  * a record that does not end after it enters is excluded.
    // vs-stata bank cf5b2145 `stset studytime, failure(died) id(drug)` on cancer.dta:
    // 20 at the same instant, 27 after the first failure, 1 record remaining.
    if (idv) {
        std::vector<int64_t> grp;
        auto flush = [&]() {
            const size_t m = grp.size();
            if (m == 0) return;
            std::vector<double> tt(m), dd(m), tz(m);
            for (size_t j = 0; j < m; ++j) {
                const int64_t i = grp[j];
                tt[j] = timev[i];
                dd[j] = failv ? (failv[i] != 0.0 ? 1.0 : 0.0) : (j + 1 == m ? 1.0 : 0.0);
            }
            double tex = HUGE_VAL;
            for (size_t j = 0; j < m; ++j)
                if (dd[j] != 0.0 && tt[j] < tex) tex = tt[j];
            int64_t kept_fail = 0;
            for (size_t j = 0; j < m; ++j) {
                const int64_t i = grp[j];
                double t0j = j == 0 ? ((m > 1 && tt[1] <= 0) ? 2.0 * tt[0] : 0.0) : tt[j - 1];
                double tj = tt[j], dj = dd[j];
                const bool same = (j > 0 && tt[j] == tt[j - 1]) || (j + 1 < m && tt[j] == tt[j + 1]);
                bool keep = false;
                if (same) {
                    ++r.n_excl_same_instant;
                } else if (t0j >= tex) {
                    ++r.n_excl_after_failure;
                } else {
                    if (t0j < tex && tj > tex) {
                        dj = 0.0;
                        tj = tex;
                    }
                    if (t0j < 0) t0j = 0.0;
                    if (t0j > tj) ++r.n_excl_negative;
                    else if (t0j == tj) ++r.n_excl_zero_length;
                    else keep = true;
                }
                r.t0[static_cast<size_t>(i)] = t0j;
                r.t[static_cast<size_t>(i)] = tj;
                r.d[static_cast<size_t>(i)] = dj;
                r.st[static_cast<size_t>(i)] = keep ? 1.0 : 0.0;
                if (!keep) continue;
                if (r.n_kept == 0) {
                    r.first_entry = t0j;
                    r.last_exit = tj;
                } else {
                    if (t0j < r.first_entry) r.first_entry = t0j;
                    if (tj > r.last_exit) r.last_exit = tj;
                }
                ++r.n_kept;
                r.time_at_risk += tj - t0j;
                if (dj != 0.0) {
                    ++r.n_failures;
                    ++kept_fail;
                }
            }
            bool anyKept = false;
            for (size_t j = 0; j < m; ++j) anyKept = anyKept || r.st[static_cast<size_t>(grp[j])] == 1.0;
            if (anyKept) ++r.n_subjects;
            if (kept_fail > 1) r.single_failure = false;
            grp.clear();
        };
        r.single_failure = true;
        double gid = 0;
        for (int64_t q = 0; q < n; ++q) {
            const int64_t i = ord[static_cast<size_t>(q)];
            const bool bad = miss(timev[i]) || miss(idv[i]) || (failv && miss(failv[i]));
            if (bad) {
                ++r.n_excl_missing;
                r.st[static_cast<size_t>(i)] = 0.0;
                continue;
            }
            if (!grp.empty() && !(idv[i] == gid)) flush();
            gid = idv[i];
            grp.push_back(i);
        }
        flush();
        r.origin = 0.0;
        r.ok = true;
        return r;
    }

    // failure()-less default (Stata): the LAST record of each subject fails.
    std::vector<char> last_of_subject(static_cast<size_t>(n), 0);
    if (!failv) {
        for (int64_t q = 0; q < n; ++q) {
            const int64_t i = ord[static_cast<size_t>(q)];
            const bool last =
                (q + 1 == n) ||
                (idv ? !(sort_key(idv[ord[static_cast<size_t>(q + 1)]]) == sort_key(idv[i])) : true);
            last_of_subject[static_cast<size_t>(i)] = last ? 1 : 0;
        }
    }

    bool have_subject = false;
    double cur_id = 0, prev_exit = 0;
    bool seen_failure = false;
    int64_t kept_fail_this_subject = 0;
    bool any_multi_failure = false;
    bool have_kept = false;
    double kept_id = 0;

    for (int64_t q = 0; q < n; ++q) {
        const int64_t i = ord[static_cast<size_t>(q)];
        const double this_id = idv ? idv[i] : static_cast<double>(i);
        if (!have_subject || !(this_id == cur_id)) {
            // New subject run (ord groups equal ids together; a missing id is its
            // own run because `this_id == cur_id` is false for NaN).
            if (kept_fail_this_subject > 1) any_multi_failure = true;
            have_subject = true;
            cur_id = this_id;
            prev_exit = 0;  // origin(time 0): the first record enters at t = 0
            seen_failure = false;
            kept_fail_this_subject = 0;
        }

        const double ti = timev[i];
        const bool bad = miss(ti) || (idv && miss(this_id)) || (failv && miss(failv[i]));
        if (bad) {
            ++r.n_excl_missing;
            r.st[static_cast<size_t>(i)] = 0.0;
            continue;  // a dropped record does not advance the subject's clock
        }

        const double t0i = prev_exit;
        const double di =
            failv ? (failv[i] != 0.0 ? 1.0 : 0.0) : (last_of_subject[static_cast<size_t>(i)] ? 1.0 : 0.0);
        prev_exit = ti;

        r.t0[static_cast<size_t>(i)] = t0i;
        r.t[static_cast<size_t>(i)] = ti;
        r.d[static_cast<size_t>(i)] = di;

        bool keep;
        if (seen_failure) {
            ++r.n_excl_after_failure;
            keep = false;
        } else if (ti < t0i) {
            ++r.n_excl_negative;
            keep = false;
        } else if (ti == t0i) {
            ++r.n_excl_zero_length;
            keep = false;
        } else {
            keep = true;
        }
        if (di != 0.0) seen_failure = true;

        r.st[static_cast<size_t>(i)] = keep ? 1.0 : 0.0;
        if (!keep) continue;

        if (!have_kept) {
            have_kept = true;
            kept_id = this_id;
            ++r.n_subjects;
            r.first_entry = t0i;
            r.last_exit = ti;
        } else {
            if (!(this_id == kept_id)) {
                ++r.n_subjects;
                kept_id = this_id;
            }
            if (t0i < r.first_entry) r.first_entry = t0i;
            if (ti > r.last_exit) r.last_exit = ti;
        }
        ++r.n_kept;
        r.time_at_risk += ti - t0i;
        if (di != 0.0) {
            ++r.n_failures;
            ++kept_fail_this_subject;
        }
    }
    if (kept_fail_this_subject > 1) any_multi_failure = true;

    r.origin = 0.0;
    r.single_failure = !any_multi_failure;
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// stcox
// ---------------------------------------------------------------------------

namespace {

// Everything about the sample that does not depend on beta: the two sweep orders
// and the failure-time grouping. One per stratum: the rows are the stratum's
// records (all n when the fit has no strata), as indices into the full sample.
struct CoxWork {
    int64_t n = 0;                   // records in this stratum
    int k = 0;
    std::vector<int64_t> ord_exit;   // rows by t  DESCENDING
    std::vector<int64_t> ord_entry;  // rows by t0 DESCENDING
    std::vector<double> ftime;       // DISTINCT failure times, DESCENDING
    std::vector<int64_t> fstart;     // ftime[m] covers frow[fstart[m] .. fstart[m+1])
    std::vector<int64_t> frow;       // failure row indices, grouped by ftime
    bool ok = false;
};

// `rows` (ascending) restricts the work to one stratum; nullptr = every row 0..n-1.
CoxWork cox_prepare(int64_t n, const double* t0, const double* t, const double* d,
                    const std::vector<int64_t>* rows = nullptr) {
    CoxWork w;
    if (rows) {
        w.ord_exit = *rows;
    } else {
        w.ord_exit.resize(static_cast<size_t>(n));
        std::iota(w.ord_exit.begin(), w.ord_exit.end(), static_cast<int64_t>(0));
    }
    w.n = static_cast<int64_t>(w.ord_exit.size());
    w.ord_entry = w.ord_exit;
    std::stable_sort(w.ord_exit.begin(), w.ord_exit.end(),
                     [&](int64_t a, int64_t b) { return t[a] > t[b]; });
    std::stable_sort(w.ord_entry.begin(), w.ord_entry.end(),
                     [&](int64_t a, int64_t b) { return t0[a] > t0[b]; });
    std::vector<int64_t> f;
    if (rows) {
        for (int64_t i : *rows)
            if (d[i] != 0.0) f.push_back(i);
    } else {
        for (int64_t i = 0; i < n; ++i)
            if (d[i] != 0.0) f.push_back(i);
    }
    std::stable_sort(f.begin(), f.end(), [&](int64_t a, int64_t b) { return t[a] > t[b]; });
    w.frow = std::move(f);
    for (size_t q = 0; q < w.frow.size(); ++q) {
        const double tq = t[w.frow[q]];
        if (w.ftime.empty() || tq != w.ftime.back()) {
            w.ftime.push_back(tq);
            w.fstart.push_back(static_cast<int64_t>(q));
        }
    }
    w.fstart.push_back(static_cast<int64_t>(w.frow.size()));
    w.ok = true;
    return w;
}

// The linear predictor and the risk weights exp(x b) of every row.
void cox_linear(const double* X, int64_t n, const std::vector<double>& beta,
                std::vector<double>& eta, std::vector<double>& wt) {
    const int k = static_cast<int>(beta.size());
    eta.assign(static_cast<size_t>(n), 0.0);
    wt.assign(static_cast<size_t>(n), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * beta[static_cast<size_t>(c)];
        eta[static_cast<size_t>(i)] = xb;
        wt[static_cast<size_t>(i)] = std::exp(xb);
    }
}

// One pass over one stratum's failure times, sweeping the risk-set accumulators.
//
// Risk set at t_j is R = { i : t0_i < t_j <= t_i }. Written as a SET DIFFERENCE,
// R = { t_i >= t_j } \ { t0_i >= t_j } (the second set is contained in the first
// because t0_i < t_i for every sample row), each side is a prefix of a
// descending sort — so both accumulators are built by ADDITION ONLY and the one
// subtraction happens at query time. That is strictly better conditioned than a
// single running accumulator that adds on entry and removes on exit, and it is
// still O(n log n): two monotone pointers, no risk set ever rebuilt.
//
// Ties: Breslow (every failure of a tie set sees the full risk set), or Efron
// (the r-th of D tied failures, r = 0..D-1, sees the risk set less r/D of the
// tie set's own weight: S0 - (r/D) F0, with F0 = SUM_{j in D} exp(x_j b), and
// the same downweighting of the first and second moments). The two coincide on
// a singleton tie set, which takes the Breslow arithmetic either way.
//
// `n` is X's row stride (the whole sample). Returns this stratum's LL; ADDS its
// gradient (k) and information (k*k, = -Hessian) into grad/info when non-null,
// and fills s0_out/xbar_out (per distinct failure time) when non-null.
double cox_eval_stratum(const CoxWork& w, const double* X, int64_t n, const double* t0,
                        const double* t, const std::vector<double>& eta,
                        const std::vector<double>& wt, int k, CoxTies ties,
                        std::vector<double>* grad, std::vector<double>* info,
                        std::vector<double>* s0_out, std::vector<double>* xbar_out) {
    const size_t kk = static_cast<size_t>(k) * static_cast<size_t>(k);
    const size_t M = w.ftime.size();
    if (s0_out) s0_out->assign(M, 0.0);
    if (xbar_out) xbar_out->assign(M * static_cast<size_t>(k), 0.0);

    double A0 = 0, B0 = 0;
    std::vector<double> A1(static_cast<size_t>(k), 0.0), B1(static_cast<size_t>(k), 0.0);
    std::vector<double> A2(info ? kk : 0, 0.0), B2(info ? kk : 0, 0.0);
    std::vector<double> S1(static_cast<size_t>(k), 0.0), xbar(static_cast<size_t>(k), 0.0);
    int64_t pa = 0, pb = 0;
    const int64_t nw = w.n;
    double LL = 0;

    auto accumulate = [&](int64_t i, double& s0, std::vector<double>& s1, std::vector<double>& s2) {
        const double wi = wt[static_cast<size_t>(i)];
        s0 += wi;
        for (int c = 0; c < k; ++c) {
            const double xc = X[static_cast<int64_t>(c) * n + i];
            s1[static_cast<size_t>(c)] += wi * xc;
            if (info)
                for (int e = 0; e < k; ++e)
                    s2[static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(e)] +=
                        wi * xc * X[static_cast<int64_t>(e) * n + i];
        }
    };

    const bool efron = ties == CoxTies::Efron;
    const bool exactp = ties == CoxTies::Exactp;
    const bool exactm = ties == CoxTies::Exactm;
    std::vector<double> F1, F2, s1r;
    if (exactp) s1r.assign(static_cast<size_t>(k), 0.0);
    if (efron || exactm) {
        F1.assign(static_cast<size_t>(k), 0.0);
        F2.assign(info ? kk : 0, 0.0);
        s1r.assign(static_cast<size_t>(k), 0.0);
    }

    for (size_t m = 0; m < M; ++m) {
        const double tj = w.ftime[m];
        while (pa < nw && t[w.ord_exit[static_cast<size_t>(pa)]] >= tj) {
            accumulate(w.ord_exit[static_cast<size_t>(pa)], A0, A1, A2);
            ++pa;
        }
        while (pb < nw && t0[w.ord_entry[static_cast<size_t>(pb)]] >= tj) {
            accumulate(w.ord_entry[static_cast<size_t>(pb)], B0, B1, B2);
            ++pb;
        }
        const double S0 = A0 - B0;
        if (!(S0 > 0)) return -HUGE_VAL;
        for (int c = 0; c < k; ++c) {
            S1[static_cast<size_t>(c)] = A1[static_cast<size_t>(c)] - B1[static_cast<size_t>(c)];
            xbar[static_cast<size_t>(c)] = S1[static_cast<size_t>(c)] / S0;
        }
        const int64_t cnt = w.fstart[m + 1] - w.fstart[m];
        const double dcnt = static_cast<double>(cnt);
        // exactm's tie-set term carries the failures' own weights inside it.
        if (!(exactm && cnt > 1))
            for (int64_t q = w.fstart[m]; q < w.fstart[m + 1]; ++q) {
                const int64_t j = w.frow[static_cast<size_t>(q)];
                LL += eta[static_cast<size_t>(j)];
                if (grad)
                    for (int c = 0; c < k; ++c)
                        (*grad)[static_cast<size_t>(c)] += X[static_cast<int64_t>(c) * n + j];
            }
        if (exactm && cnt > 1 && pa - pb <= cnt) {
            // The tie set is the whole risk set: nobody is left to outlast, so the
            // probability that D fails before the rest is 1 and the term is log 1 = 0
            // (the integral below with c = 0). Counted on rows, not on the weight
            // S0 - F0, which rounding leaves at +-1 ulp.
        } else if (exactm && cnt > 1) {
            // The probability that every member of the tie set D fails before every
            // other member of the risk set, P = INT_0^inf PROD_{l in D} (1 -
            // exp(-a_l u)) exp(-u) du with a_l = w_l / c (c = the weight of the risk
            // set less D), evaluated as Stata evaluates it -- 15-point Gauss-Laguerre
            // quadrature of the likelihood and of its derivatives ([ST] stcox Methods
            // and formulas; bank 2a3864a7 `stcox age i.drug, exactm`: z[3.drug]
            // -4.556319362212903, which the closed form misses by 1.5e-9). A singleton
            // keeps the exact w_j / S0 above (no ties = every method agrees). Per
            // node, log h = SUM_l log(1 - u_l), u_l = exp(-a_l x); d log h = SUM_l
            // g_a a_l', d2 = SUM_l (g_aa a_l' a_l'^T + g_a a_l''), with g_a = x u /
            // (1 - u), g_aa = -x^2 u / (1 - u)^2, a_l' = a_l (x_l - m), a_l'' = a_l
            // ((x_l - m)(x_l - m)^T - (C2/c - m m^T)), m = C1 / c. The node terms are
            // summed relative to the largest so a long tie set cannot underflow.
            static constexpr double kGLx[15] = {
                0.093307812017281805, 0.4926917403018839, 1.2155954120709496,
                2.2699495262037432,   3.6676227217514374, 5.4253366274135528,
                7.5659162266130675,   10.120228568019114, 13.130282482175723,
                16.654407708329959,   20.776478899448765, 25.62389422672878,
                31.40751916975394,    38.530683306486011, 48.026085572685794};
            static constexpr double kGLw[15] = {
                0.21823488594008689,    0.34221017792288333,    0.26302757794168008,
                0.12642581810593054,    0.040206864921000916,   0.0085638778036118378,
                0.001212436147214252,   0.00011167439234425194, 6.4599267620229013e-06,
                2.2263169070962725e-07, 4.2274303849793649e-09, 3.9218972670410896e-11,
                1.4565152640731265e-13, 1.4830270511133013e-16, 1.6005949062111332e-20};
            double F0 = 0;
            std::fill(F1.begin(), F1.end(), 0.0);
            std::fill(F2.begin(), F2.end(), 0.0);
            for (int64_t q = w.fstart[m]; q < w.fstart[m + 1]; ++q)
                accumulate(w.frow[static_cast<size_t>(q)], F0, F1, F2);
            const double cN = S0 - F0;
            if (!(cN > 0)) return -HUGE_VAL;
            const size_t K = static_cast<size_t>(k);
            const size_t dd = static_cast<size_t>(cnt);
            std::vector<double> mbar(K), dm(info ? kk : 0);
            for (size_t a = 0; a < K; ++a) mbar[a] = (S1[a] - F1[a]) / cN;
            if (info)
                for (size_t a = 0; a < K; ++a)
                    for (size_t c2 = 0; c2 < K; ++c2)
                        dm[a * K + c2] = (A2[a * K + c2] - B2[a * K + c2] - F2[a * K + c2]) / cN -
                                         mbar[a] * mbar[c2];
            std::vector<double> al(dd), ap(dd * K), app(info ? dd * kk : 0);
            for (size_t l = 0; l < dd; ++l) {
                const int64_t j = w.frow[static_cast<size_t>(w.fstart[m]) + l];
                al[l] = wt[static_cast<size_t>(j)] / cN;
                for (size_t a = 0; a < K; ++a)
                    ap[l * K + a] = al[l] * (X[static_cast<int64_t>(a) * n + j] - mbar[a]);
                if (info)
                    for (size_t a = 0; a < K; ++a)
                        for (size_t c2 = 0; c2 < K; ++c2)
                            app[l * kk + a * K + c2] =
                                al[l] * ((X[static_cast<int64_t>(a) * n + j] - mbar[a]) *
                                             (X[static_cast<int64_t>(c2) * n + j] - mbar[c2]) -
                                         dm[a * K + c2]);
            }
            double lw[15];
            std::vector<double> g1(15 * K, 0.0), g2(info ? 15 * kk : 0, 0.0);
            double lwMax = -HUGE_VAL;
            for (size_t qn = 0; qn < 15; ++qn) {
                const double x = kGLx[qn];
                double lh = 0;
                for (size_t l = 0; l < dd; ++l) {
                    const double ax = al[l] * x;
                    const double om = -std::expm1(-ax), u = std::exp(-ax);
                    if (!(om > 0)) return -HUGE_VAL;
                    lh += std::log(om);
                    const double ga = x * u / om, gaa = -x * x * u / (om * om);
                    for (size_t a = 0; a < K; ++a) g1[qn * K + a] += ga * ap[l * K + a];
                    if (info)
                        for (size_t a = 0; a < K; ++a)
                            for (size_t c2 = 0; c2 < K; ++c2)
                                g2[qn * kk + a * K + c2] += gaa * ap[l * K + a] * ap[l * K + c2] +
                                                            ga * app[l * kk + a * K + c2];
                }
                lw[qn] = std::log(kGLw[qn]) + lh;
                lwMax = std::max(lwMax, lw[qn]);
            }
            double P = 0;
            std::vector<double> P1(K, 0.0), P2(info ? kk : 0, 0.0);
            for (size_t qn = 0; qn < 15; ++qn) {
                const double Wq = std::exp(lw[qn] - lwMax);
                P += Wq;
                for (size_t a = 0; a < K; ++a) P1[a] += Wq * g1[qn * K + a];
                if (info)
                    for (size_t a = 0; a < K; ++a)
                        for (size_t c2 = 0; c2 < K; ++c2)
                            P2[a * K + c2] += Wq * (g1[qn * K + a] * g1[qn * K + c2] + g2[qn * kk + a * K + c2]);
            }
            if (!(P > 0) || !std::isfinite(P) || !std::isfinite(lwMax)) return -HUGE_VAL;
            LL += lwMax + std::log(P);
            if (grad)
                for (size_t a = 0; a < K; ++a) (*grad)[a] += P1[a] / P;
            if (info)
                for (size_t a = 0; a < K; ++a)
                    for (size_t c2 = 0; c2 < K; ++c2)
                        (*info)[a * K + c2] -= P2[a * K + c2] / P - (P1[a] / P) * (P1[c2] / P);
        } else if (exactp && cnt > 1) {
            // The exact partial likelihood's denominator, the elementary symmetric
            // polynomial of degree d in the risk weights, by the recursion over the
            // risk set E_s += w_i E_{s-1} (s = d..1), with its first and second
            // derivatives carried the same way; the weights are scaled by the risk
            // set's largest so the sums cannot overflow (the scale cancels in the
            // derivative ratios and is added back to the log).
            const int dd = static_cast<int>(cnt);
            double emax = -HUGE_VAL;
            for (int64_t q = 0; q < pa; ++q) {
                const int64_t i = w.ord_exit[static_cast<size_t>(q)];
                if (t0[i] >= tj) continue;
                emax = std::max(emax, eta[static_cast<size_t>(i)]);
            }
            const size_t K = static_cast<size_t>(k);
            std::vector<double> E(static_cast<size_t>(dd) + 1, 0.0), G((static_cast<size_t>(dd) + 1) * K, 0.0);
            std::vector<double> HH(info ? (static_cast<size_t>(dd) + 1) * kk : 0, 0.0);
            E[0] = 1.0;
            for (int64_t q = 0; q < pa; ++q) {
                const int64_t i = w.ord_exit[static_cast<size_t>(q)];
                if (t0[i] >= tj) continue;
                const double wi = std::exp(eta[static_cast<size_t>(i)] - emax);
                for (int sidx = dd; sidx >= 1; --sidx) {
                    const size_t s1 = static_cast<size_t>(sidx), s0 = s1 - 1;
                    if (info)
                        for (int a = 0; a < k; ++a) {
                            const double xa = X[static_cast<int64_t>(a) * n + i];
                            for (int c = 0; c < k; ++c) {
                                const double xc = X[static_cast<int64_t>(c) * n + i];
                                const size_t o = static_cast<size_t>(a) * K + static_cast<size_t>(c);
                                HH[s1 * kk + o] += wi * (HH[s0 * kk + o] + xa * G[s0 * K + static_cast<size_t>(c)] +
                                                         G[s0 * K + static_cast<size_t>(a)] * xc + xa * xc * E[s0]);
                            }
                        }
                    for (int a = 0; a < k; ++a)
                        G[s1 * K + static_cast<size_t>(a)] +=
                            wi * (G[s0 * K + static_cast<size_t>(a)] + X[static_cast<int64_t>(a) * n + i] * E[s0]);
                    E[s1] += wi * E[s0];
                }
            }
            const size_t D = static_cast<size_t>(dd);
            const double Q = E[D];
            if (!(Q > 0) || !std::isfinite(Q)) return -HUGE_VAL;
            LL -= std::log(Q) + dcnt * emax;
            for (int c = 0; c < k; ++c) s1r[static_cast<size_t>(c)] = G[D * K + static_cast<size_t>(c)] / Q;
            if (grad)
                for (int c = 0; c < k; ++c) (*grad)[static_cast<size_t>(c)] -= s1r[static_cast<size_t>(c)];
            if (info)
                for (int a = 0; a < k; ++a)
                    for (int c = 0; c < k; ++c) {
                        const size_t o = static_cast<size_t>(a) * K + static_cast<size_t>(c);
                        (*info)[o] += HH[D * kk + o] / Q - s1r[static_cast<size_t>(a)] * s1r[static_cast<size_t>(c)];
                    }
        } else if (efron && cnt > 1) {
            double F0 = 0;
            std::fill(F1.begin(), F1.end(), 0.0);
            std::fill(F2.begin(), F2.end(), 0.0);
            for (int64_t q = w.fstart[m]; q < w.fstart[m + 1]; ++q)
                accumulate(w.frow[static_cast<size_t>(q)], F0, F1, F2);
            for (int64_t r = 0; r < cnt; ++r) {
                const double c0 = static_cast<double>(r) / dcnt;
                const double s0r = S0 - c0 * F0;
                if (!(s0r > 0)) return -HUGE_VAL;
                LL -= std::log(s0r);
                for (int c = 0; c < k; ++c)
                    s1r[static_cast<size_t>(c)] =
                        (S1[static_cast<size_t>(c)] - c0 * F1[static_cast<size_t>(c)]) / s0r;
                if (grad)
                    for (int c = 0; c < k; ++c) (*grad)[static_cast<size_t>(c)] -= s1r[static_cast<size_t>(c)];
                if (info)
                    for (int c = 0; c < k; ++c)
                        for (int e = 0; e < k; ++e) {
                            const size_t o = static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(e);
                            (*info)[o] += (A2[o] - B2[o] - c0 * F2[o]) / s0r -
                                          s1r[static_cast<size_t>(c)] * s1r[static_cast<size_t>(e)];
                        }
            }
        } else {
            LL -= dcnt * std::log(S0);
            if (grad)
                for (int c = 0; c < k; ++c) (*grad)[static_cast<size_t>(c)] -= dcnt * xbar[static_cast<size_t>(c)];
            if (info)
                for (int c = 0; c < k; ++c)
                    for (int e = 0; e < k; ++e) {
                        const size_t o = static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(e);
                        (*info)[o] += dcnt * ((A2[o] - B2[o]) / S0 -
                                              xbar[static_cast<size_t>(c)] * xbar[static_cast<size_t>(e)]);
                    }
        }
        if (s0_out) (*s0_out)[m] = S0;
        if (xbar_out)
            for (int c = 0; c < k; ++c)
                (*xbar_out)[m * static_cast<size_t>(k) + static_cast<size_t>(c)] = xbar[static_cast<size_t>(c)];
    }
    return LL;
}

// The whole fit's log partial likelihood: the sum over strata. grad/info (when
// non-null) are set to the summed gradient / information.
double cox_eval(const std::vector<CoxWork>& ws, const double* X, int64_t n, const double* t0,
                const double* t, const std::vector<double>& beta, CoxTies ties,
                std::vector<double>* grad, std::vector<double>* info) {
    const int k = static_cast<int>(beta.size());
    std::vector<double> eta, wt;
    cox_linear(X, n, beta, eta, wt);
    if (grad) grad->assign(static_cast<size_t>(k), 0.0);
    if (info) info->assign(static_cast<size_t>(k) * static_cast<size_t>(k), 0.0);
    double LL = 0;
    for (const CoxWork& w : ws) {
        const double l = cox_eval_stratum(w, X, n, t0, t, eta, wt, k, ties, grad, info, nullptr, nullptr);
        if (!std::isfinite(l)) return -HUGE_VAL;
        LL += l;
    }
    return LL;
}

}  // namespace

CoxResult cox_fit(const double* X, int64_t n, int k, const double* t0, const double* t,
                  const double* d, const int32_t* strata, CoxTies ties) {
    CoxResult r;
    r.n = n;
    if (n <= 0 || k <= 0) {
        r.err = "stcox: empty sample";
        return r;
    }
    for (int64_t i = 0; i < n; ++i) {
        if (!(t[i] > t0[i])) {
            r.err = "stcox: every analysis record must satisfy _t > _t0";
            return r;
        }
        if (d[i] != 0.0) ++r.n_fail;
    }
    if (r.n_fail == 0) {
        r.err = "stcox: no failures in the estimation sample";
        return r;
    }
    // One risk-set structure per stratum (Stata's strata(): a separate baseline
    // hazard per stratum, so a failure's risk set holds its own stratum's records
    // only); the whole sample when there are no strata.
    std::vector<CoxWork> ws;
    if (strata == nullptr) {
        ws.push_back(cox_prepare(n, t0, t, d));
    } else {
        std::map<int32_t, std::vector<int64_t>> groups;
        for (int64_t i = 0; i < n; ++i) groups[strata[i]].push_back(i);
        for (const auto& g : groups) ws.push_back(cox_prepare(n, t0, t, d, &g.second));
    }
    // Ties are DETECTED (not just tolerated) so the caller can pick Stata's
    // header honestly: Breslow coincides with exact/efron/etc. only while every
    // tie set is a singleton, which is what "Cox regression with no ties" claims.
    for (const CoxWork& w : ws)
        for (size_t m = 0; m < w.ftime.size(); ++m) {
            const int64_t cnt = w.fstart[m + 1] - w.fstart[m];
            if (cnt > 1) r.n_tied_failures += cnt;
        }

    std::vector<double> beta(static_cast<size_t>(k), 0.0), grad, info, step(static_cast<size_t>(k));
    r.ll_null = cox_eval(ws, X, n, t0, t, beta, ties, nullptr, nullptr);
    double prev = r.ll_null;
    int it = 0;
    for (; it < 100; ++it) {
        const double LL = cox_eval(ws, X, n, t0, t, beta, ties, &grad, &info);
        std::vector<double> inv = info;
        if (la::spd_inverse(inv.data(), k) != 0) {
            r.err = "stcox: information matrix is not invertible (collinear covariates?)";
            return r;
        }
        la::matvec(inv.data(), k, grad.data(), step.data());
        double s = 1.0;
        std::vector<double> cand(static_cast<size_t>(k));
        double candLL = LL;
        for (int bt = 0; bt < 40; ++bt) {
            for (int c = 0; c < k; ++c) cand[static_cast<size_t>(c)] = beta[static_cast<size_t>(c)] + s * step[static_cast<size_t>(c)];
            candLL = cox_eval(ws, X, n, t0, t, cand, ties, nullptr, nullptr);
            // A step is kept unless it lowers the log likelihood by more than its
            // rounding: at the optimum the Newton step changes LL by ~1e-16 relative,
            // and a strict `>=` halved that last step on noise, leaving the fit half a
            // step short (bank b5c81a48 `stcox age, strata(drug)`: _b[age] 3.2e-9 off
            // Stata's fully converged .1127670478155866, z 3.02526010474 there).
            if (std::isfinite(candLL) && candLL >= LL - 1e-13 * (std::fabs(LL) + 1.0)) break;
            s *= 0.5;
        }
        beta = cand;
        if (std::fabs(candLL - prev) < 1e-12 * (std::fabs(prev) + 1e-12)) {
            prev = candLL;
            ++it;
            r.converged = true;
            break;
        }
        prev = candLL;
    }
    r.iters = it;
    r.ll = cox_eval(ws, X, n, t0, t, beta, ties, &grad, &info);
    std::vector<double> V = info;
    if (la::spd_inverse(V.data(), k) != 0) {
        r.err = "stcox: information matrix is not invertible (collinear covariates?)";
        return r;
    }
    r.vcov = V;
    r.beta = beta;
    r.se.resize(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c)
        r.se[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(c)]));
    r.ok = true;
    return r;
}

void cox_score_residuals(const double* X, int64_t n, int k, const double* t0, const double* t,
                         const double* d, const std::vector<double>& beta,
                         std::vector<double>* out) {
    out->assign(static_cast<size_t>(n) * static_cast<size_t>(k), 0.0);
    if (n <= 0 || k <= 0) return;
    const CoxWork w = cox_prepare(n, t0, t, d);
    std::vector<double> S0, XB, eta, wt;
    cox_linear(X, n, beta, eta, wt);
    cox_eval_stratum(w, X, n, t0, t, eta, wt, k, CoxTies::Breslow, nullptr, nullptr, &S0, &XB);
    const size_t M = w.ftime.size();
    if (M == 0) return;

    // Cumulative-hazard prefix sums in ASCENDING failure-time order:
    //   F0(u) = SUM_{t_j <= u} m_j / S0_j          (m_j = # failures at t_j)
    //   F1(u) = SUM_{t_j <= u} m_j * xbar_j / S0_j
    // so the "SUM over j with t0_i < t_j <= t_i" in the score residual is just
    // F(t_i) - F(t0_i) — two binary searches per row, no O(n * #failures) loop.
    std::vector<double> at(M);                                 // failure times ASCENDING
    std::vector<double> F0(M + 1, 0.0), F1((M + 1) * static_cast<size_t>(k), 0.0);
    for (size_t m = 0; m < M; ++m) at[m] = w.ftime[M - 1 - m];
    for (size_t m = 0; m < M; ++m) {
        const size_t src = M - 1 - m;  // descending index of the m-th ascending time
        const double mult = static_cast<double>(w.fstart[src + 1] - w.fstart[src]) / S0[src];
        F0[m + 1] = F0[m] + mult;
        for (int c = 0; c < k; ++c)
            F1[(m + 1) * static_cast<size_t>(k) + static_cast<size_t>(c)] =
                F1[m * static_cast<size_t>(k) + static_cast<size_t>(c)] +
                mult * XB[src * static_cast<size_t>(k) + static_cast<size_t>(c)];
    }
    auto upto = [&](double u) -> size_t {
        return static_cast<size_t>(std::upper_bound(at.begin(), at.end(), u) - at.begin());
    };

    double* U = out->data();
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * beta[static_cast<size_t>(c)];
        const double wi = std::exp(xb);
        const size_t hi = upto(t[i]), lo = upto(t0[i]);
        const double dF0 = F0[hi] - F0[lo];
        if (d[i] != 0.0) {
            // t[i] is a failure time; its ascending index is hi-1.
            const size_t m = hi - 1;
            const size_t src = M - 1 - m;
            for (int c = 0; c < k; ++c)
                U[static_cast<int64_t>(c) * n + i] +=
                    X[static_cast<int64_t>(c) * n + i] - XB[src * static_cast<size_t>(k) + static_cast<size_t>(c)];
        }
        for (int c = 0; c < k; ++c) {
            const double dF1 = F1[hi * static_cast<size_t>(k) + static_cast<size_t>(c)] -
                               F1[lo * static_cast<size_t>(k) + static_cast<size_t>(c)];
            U[static_cast<int64_t>(c) * n + i] -= wi * (X[static_cast<int64_t>(c) * n + i] * dF0 - dF1);
        }
    }
}

// ---------------------------------------------------------------------------
// streg, distribution(weibull)
// ---------------------------------------------------------------------------

namespace weib {

double ll_grad(const double* X, int64_t n, int k, const double* t0, const double* t,
               const double* d, const std::vector<double>& theta, std::vector<double>* grad,
               std::vector<double>* scores) {
    const int np = k + 1;
    const double lnp = theta[static_cast<size_t>(k)];
    const double p = std::exp(lnp);
    double LL = 0;
    if (grad) std::fill(grad->begin(), grad->end(), 0.0);
    if (scores) scores->assign(static_cast<size_t>(n) * static_cast<size_t>(np), 0.0);
    double* S = scores ? scores->data() : nullptr;
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0;
        for (int c = 0; c < k; ++c) xb += X[static_cast<int64_t>(c) * n + i] * theta[static_cast<size_t>(c)];
        const double ti = t[i], t0i = t0[i], di = d[i];
        const double lt = std::log(ti);
        const double tp = std::exp(p * lt);
        double t0p = 0.0, lt0 = 0.0;
        if (t0i > 0.0) {
            lt0 = std::log(t0i);
            t0p = std::exp(p * lt0);
        }
        const double e = std::exp(xb);
        const double Lam = (tp - t0p) * e;
        LL += di * (lnp + p * lt + xb) - Lam;
        // grad and S are computed by two INDEPENDENT branches, deliberately not
        // sharing an intermediate — same reasoning as tobit.h:64-73 (a hoisted
        // temp forces an extra rounding step that moves the last bits of every
        // downstream SE). Only one of the two is ever requested at a call site
        // here (the Newton loop asks for grad, the sandwich asks for scores).
        if (grad) {
            const double cb = di - Lam;
            for (int c = 0; c < k; ++c)
                (*grad)[static_cast<size_t>(c)] += cb * X[static_cast<int64_t>(c) * n + i];
            (*grad)[static_cast<size_t>(k)] += di * (1.0 + p * lt) - p * e * (tp * lt - t0p * lt0);
        }
        if (S) {
            const double cb = di - Lam;
            for (int c = 0; c < k; ++c)
                S[static_cast<int64_t>(c) * n + i] = cb * X[static_cast<int64_t>(c) * n + i];
            S[static_cast<int64_t>(k) * n + i] = di * (1.0 + p * lt) - p * e * (tp * lt - t0p * lt0);
        }
    }
    return LL;
}

}  // namespace weib

namespace {

// Central-difference Hessian of weib::ll_grad's ANALYTIC gradient == observed
// information (negated). Same recipe as tobit_fit.
void weib_hessian(const double* X, int64_t n, int k, const double* t0, const double* t,
                  const double* d, const std::vector<double>& theta, std::vector<double>* H) {
    const int np = k + 1;
    H->assign(static_cast<size_t>(np) * static_cast<size_t>(np), 0.0);
    // Column j reads only theta and j and writes only H[:, j] -- no accumulator is
    // shared between columns -- so evaluating them concurrently returns the identical
    // matrix, with no summation order to reorder. (PERFSWEEP H: same class as
    // ivml/zicount/tobit.)
    parallel_for(np, [&](int64_t jj) {
        const int j = static_cast<int>(jj);
        thread_local std::vector<double> gp, gm, tp, tm;
        gp.resize(static_cast<size_t>(np));
        gm.resize(static_cast<size_t>(np));
        const double h = 1e-6 * (std::fabs(theta[static_cast<size_t>(j)]) + 1.0);
        tp = theta;
        tm = theta;
        tp[static_cast<size_t>(j)] += h;
        tm[static_cast<size_t>(j)] -= h;
        weib::ll_grad(X, n, k, t0, t, d, tp, &gp);
        weib::ll_grad(X, n, k, t0, t, d, tm, &gm);
        for (int i = 0; i < np; ++i)
            (*H)[static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(j)] =
                (gp[static_cast<size_t>(i)] - gm[static_cast<size_t>(i)]) / (2 * h);
    }, threads_for_work(static_cast<int64_t>(np) * n));
    for (int i = 0; i < np; ++i)
        for (int j = i + 1; j < np; ++j) {
            const size_t a = static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(j);
            const size_t b = static_cast<size_t>(j) * static_cast<size_t>(np) + static_cast<size_t>(i);
            const double s = 0.5 * ((*H)[a] + (*H)[b]);
            (*H)[a] = (*H)[b] = s;
        }
}

// Newton-Raphson with backtracking line search on weib::ll_grad. Returns the
// converged theta; `iters`/`converged` report the path taken.
bool weib_newton(const double* X, int64_t n, int k, const double* t0, const double* t,
                 const double* d, std::vector<double>* theta, double* ll, int* iters,
                 bool* converged) {
    const int np = k + 1;
    std::vector<double> grad(static_cast<size_t>(np)), H, step(static_cast<size_t>(np));
    double prev = -HUGE_VAL;
    int it = 0;
    *converged = false;
    for (; it < 200; ++it) {
        const double LL = weib::ll_grad(X, n, k, t0, t, d, *theta, &grad);
        weib_hessian(X, n, k, t0, t, d, *theta, &H);
        std::vector<double> negH(H.size());
        for (size_t q = 0; q < H.size(); ++q) negH[q] = -H[q];
        if (la::spd_inverse(negH.data(), np) != 0) break;
        la::matvec(negH.data(), np, grad.data(), step.data());
        double s = 1.0;
        std::vector<double> cand(static_cast<size_t>(np));
        double candLL = LL;
        for (int bt = 0; bt < 60; ++bt) {
            for (int i = 0; i < np; ++i)
                cand[static_cast<size_t>(i)] = (*theta)[static_cast<size_t>(i)] + s * step[static_cast<size_t>(i)];
            candLL = weib::ll_grad(X, n, k, t0, t, d, cand, nullptr);
            if (std::isfinite(candLL) && candLL >= LL) break;
            s *= 0.5;
        }
        *theta = cand;
        if (std::fabs(candLL - prev) < 1e-11 * (std::fabs(prev) + 1e-11)) {
            prev = candLL;
            ++it;
            *converged = true;
            break;
        }
        prev = candLL;
    }
    *iters = it;
    *ll = weib::ll_grad(X, n, k, t0, t, d, *theta, nullptr);
    return std::isfinite(*ll);
}

}  // namespace

WeibullResult weibull_fit(const double* X, int64_t n, int k, const double* t0, const double* t,
                          const double* d, int cons_col) {
    WeibullResult r;
    r.n = n;
    if (n <= 0 || k <= 0) {
        r.err = "streg: empty sample";
        return r;
    }
    double risk = 0;
    for (int64_t i = 0; i < n; ++i) {
        if (!(t[i] > 0.0) || !(t0[i] >= 0.0) || !(t[i] > t0[i])) {
            r.err = "streg: every analysis record must satisfy 0 <= _t0 < _t";
            return r;
        }
        if (d[i] != 0.0) ++r.n_fail;
        risk += t[i] - t0[i];
    }
    if (r.n_fail == 0) {
        r.err = "streg: no failures in the estimation sample";
        return r;
    }

    const int np = k + 1;
    std::vector<double> theta(static_cast<size_t>(np), 0.0);
    // Start from the exponential (p = 1) MLE of the baseline rate: D / time-at-risk.
    const double rate0 = std::log(static_cast<double>(r.n_fail) / risk);
    if (cons_col >= 0 && cons_col < k) {
        // Stage 1 — constant-only prefit, exactly like Stata's "Fitting
        // constant-only model:" block; its (b_cons, ln p) then seeds the full fit
        // with the slopes at 0 ("Fitting full model:" starts at the prefit LL).
        std::vector<double> ones(static_cast<size_t>(n), 1.0);
        std::vector<double> th0(2, 0.0);
        th0[0] = rate0;
        double llc = 0;
        bool convc = false;
        int itc = 0;
        if (!weib_newton(ones.data(), n, 1, t0, t, d, &th0, &llc, &itc, &convc)) {
            r.err = "streg: constant-only prefit failed";
            return r;
        }
        r.ll_null = llc;
        r.iters_null = itc;
        theta[static_cast<size_t>(cons_col)] = th0[0];
        theta[static_cast<size_t>(k)] = th0[1];
    } else {
        theta[static_cast<size_t>(k)] = 0.0;
    }

    if (!weib_newton(X, n, k, t0, t, d, &theta, &r.ll, &r.iters, &r.converged)) {
        r.err = "streg: MLE failed";
        return r;
    }
    std::vector<double> H;
    weib_hessian(X, n, k, t0, t, d, theta, &H);
    std::vector<double> V(H.size());
    for (size_t q = 0; q < H.size(); ++q) V[q] = -H[q];
    if (la::spd_inverse(V.data(), np) != 0) {
        r.err = "streg: observed information is not invertible";
        return r;
    }
    r.vcov = V;
    r.beta.assign(theta.begin(), theta.begin() + k);
    r.se.resize(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c)
        r.se[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(np) + static_cast<size_t>(c)]));
    r.ln_p = theta[static_cast<size_t>(k)];
    r.se_ln_p =
        std::sqrt(std::max(0.0, V[static_cast<size_t>(k) * static_cast<size_t>(np) + static_cast<size_t>(k)]));
    r.p = std::exp(r.ln_p);
    r.se_p = r.p * r.se_ln_p;          // delta method on exp(ln p)
    r.inv_p = 1.0 / r.p;
    r.se_inv_p = r.se_ln_p / r.p;      // delta method on exp(-ln p)
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// shared sandwich
// ---------------------------------------------------------------------------

std::vector<double> survival_sandwich(const std::vector<double>& S, int64_t n, int np,
                                      const std::vector<double>& bread, const int32_t* codes,
                                      int n_levels, double factor) {
    const size_t pp = static_cast<size_t>(np) * static_cast<size_t>(np);
    if (bread.size() != pp) return {};
    if (S.size() != static_cast<size_t>(n) * static_cast<size_t>(np)) return {};
    std::vector<double> meat(pp, 0.0);
    if (codes && n_levels > 0) {
        std::vector<double> g(static_cast<size_t>(n_levels) * static_cast<size_t>(np), 0.0);
        for (int64_t i = 0; i < n; ++i) {
            const int32_t cl = codes[i];
            if (cl < 0 || cl >= n_levels) return {};
            for (int c = 0; c < np; ++c)
                g[static_cast<size_t>(cl) * static_cast<size_t>(np) + static_cast<size_t>(c)] +=
                    S[static_cast<int64_t>(c) * n + i];
        }
        for (int l = 0; l < n_levels; ++l)
            for (int c = 0; c < np; ++c)
                for (int e = 0; e < np; ++e)
                    meat[static_cast<size_t>(c) * static_cast<size_t>(np) + static_cast<size_t>(e)] +=
                        g[static_cast<size_t>(l) * static_cast<size_t>(np) + static_cast<size_t>(c)] *
                        g[static_cast<size_t>(l) * static_cast<size_t>(np) + static_cast<size_t>(e)];
    } else {
        la::xtx(S.data(), n, np, meat.data());
    }
    std::vector<double> V(pp, 0.0), tmp(pp, 0.0);
    la::sandwich(bread.data(), meat.data(), np, factor, V.data(), tmp.data());
    return V;
}

}  // namespace cecon
