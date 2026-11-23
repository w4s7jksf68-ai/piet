// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include <map>

#include "common.h"
#include "commands.h"
#include "core/distributions.h"
#include "estimators/survival.h"

// P7 wave — stset, stcox, streg.
//
// `stset` is the data verb: it MATERIALIZES `_t0`, `_t`, `_d`, `_st` on the
// dataset (commit_frame, like every other mutator) and prints Stata's
// "Survival-time data settings" report as JSON on stdout. `stcox`/`streg` then
// read those four as ordinary columns — no session state, no hidden coupling.
//
// Oracle for every number: StataBE 19.5, internal capture
// p7_stata.log (the `preserve / sort id exit / stset exit,
// failure(fail) id(id)` block of p7.do). Assertions in tests/unit/test_survival.cpp.

namespace cecon {
namespace cli {

namespace {

// The four columns `stset` writes and every st command reads.
constexpr const char* kT0 = "_t0";
constexpr const char* kT = "_t";
constexpr const char* kD = "_d";
constexpr const char* kSt = "_st";

void require_st(const Dataset& ds, const char* cmd) {
    if (ds.has(kT0) && ds.has(kT) && ds.has(kD) && ds.has(kSt)) return;
    throw EngineError("bad_input",
                      std::string(cmd) + ": the data are not survival-time data — run `cobra stset` "
                                         "first (columns _t0/_t/_d/_st are missing)");
}

// The estimation sample shared by stcox and streg: load_glm does the factor
// expansion + listwise deletion + cluster column in ONE pass (exactly as every
// other MLE command does), and then the `_st == 1` restriction is applied on
// top. `_t` is passed as load_glm's y purely so the loader's listwise pass sees
// it; the analysis-time columns are re-read below at the kept row indices.
//
// Verified on the P7 oracle: `_st == 1` (1,207 records) intersected with
// listwise-complete x1 x2 leaves 1,173 records / 199 subjects / 194 failures /
// 263.5 time at risk — one subject's ENTIRE kept record set has x2 missing,
// which is why the subject count drops 200 -> 199.
struct StSample {
    int64_t n = 0;                // rows in the estimation sample
    int64_t n_st = 0;             // rows with _st == 1 before listwise deletion
    int k = 0;                    // design columns (intercept LAST, as load_glm builds it)
    std::vector<double> X;        // col-major n*k
    std::vector<double> t0, t, d;
    std::vector<std::string> names;
    std::vector<int32_t> cl_codes;  // dense 0..n_clusters-1, empty when no --cluster
    int n_clusters = 0;
    std::vector<int32_t> strata_codes;  // one code per stratum (strata vars combined), empty = none
    int n_strata = 0;
    double time_at_risk = 0;
    int64_t n_fail = 0;
    bool multi_record = false;  // any row enters after t = 0 (delayed entry)
    std::vector<int64_t> rows;  // the dataset row of each sample row
};

StSample load_st(const Dataset& ds, const char* cmd, const std::vector<std::string>& xs,
                 const std::string& cluster_var,
                 const std::vector<std::string>& strata_vars = {},
                 const std::vector<std::string>& listwise_extra = {}) {
    require_st(ds, cmd);
    std::vector<std::string> cl;
    if (!cluster_var.empty()) cl.push_back(cluster_var);
    // The strata variables ride load_glm's grouping-column pass (the same listwise
    // deletion Stata's `mark(STrata ...)` applies), after the cluster variable.
    const size_t strata_at = cl.size();
    for (const auto& v : strata_vars) cl.push_back(v);
    // streg's ancillary() variables: in the same listwise pass (marksample over
    // both equations), their design loaded on its own.
    for (const auto& v : listwise_extra) cl.push_back(v);
    GlmInputs g = load_glm(ds, cmd, kT, xs, cl);
    const double* raw_t0 = require_num(ds, cmd, kT0);
    const double* raw_t = require_num(ds, cmd, kT);
    const double* raw_d = require_num(ds, cmd, kD);
    const double* raw_st = require_num(ds, cmd, kSt);

    StSample s;
    s.k = g.k;
    s.names = g.names;
    for (int64_t i = 0; i < ds.n_rows(); ++i)
        if (raw_st[i] == 1.0) ++s.n_st;

    std::vector<int64_t> sel;
    sel.reserve(static_cast<size_t>(g.n));
    for (int64_t i = 0; i < g.n; ++i)
        if (raw_st[g.keep[static_cast<size_t>(i)]] == 1.0) sel.push_back(i);
    s.n = static_cast<int64_t>(sel.size());
    if (s.n == 0) return s;

    s.X.resize(static_cast<size_t>(s.n) * static_cast<size_t>(s.k));
    for (int c = 0; c < s.k; ++c)
        for (int64_t q = 0; q < s.n; ++q)
            s.X[static_cast<int64_t>(c) * s.n + q] = g.X[static_cast<int64_t>(c) * g.n + sel[static_cast<size_t>(q)]];
    s.t0.resize(static_cast<size_t>(s.n));
    s.t.resize(static_cast<size_t>(s.n));
    s.d.resize(static_cast<size_t>(s.n));
    s.rows.resize(static_cast<size_t>(s.n));
    for (int64_t q = 0; q < s.n; ++q) {
        const int64_t row = g.keep[static_cast<size_t>(sel[static_cast<size_t>(q)])];
        s.rows[static_cast<size_t>(q)] = row;
        s.t0[static_cast<size_t>(q)] = raw_t0[row];
        s.t[static_cast<size_t>(q)] = raw_t[row];
        s.d[static_cast<size_t>(q)] = raw_d[row] != 0.0 ? 1.0 : 0.0;
        s.time_at_risk += raw_t[row] - raw_t0[row];
        if (s.d[static_cast<size_t>(q)] != 0.0) ++s.n_fail;
        if (raw_t0[row] > 0.0) s.multi_record = true;
    }
    if (!strata_vars.empty()) {
        // One stratum per distinct combination of the strata variables' codes.
        std::map<std::vector<int32_t>, int32_t> combo;
        s.strata_codes.resize(static_cast<size_t>(s.n));
        for (int64_t q = 0; q < s.n; ++q) {
            std::vector<int32_t> key;
            for (size_t v = 0; v < strata_vars.size(); ++v) {
                const int32_t c = g.cluster[strata_at + v].codes[static_cast<size_t>(sel[static_cast<size_t>(q)])];
                if (c < 0) throw EngineError("bad_input", std::string(cmd) + ": strata variable has a missing value");
                key.push_back(c);
            }
            auto it = combo.find(key);
            if (it == combo.end()) it = combo.emplace(key, static_cast<int32_t>(combo.size())).first;
            s.strata_codes[static_cast<size_t>(q)] = it->second;
        }
        s.n_strata = static_cast<int>(combo.size());
    }
    if (!cluster_var.empty()) {
        // load_glm's cluster codes are dense over ITS kept rows; the `_st` filter
        // can leave a level empty, so re-densify over the rows that survived.
        const auto& F = g.cluster[0];
        std::vector<int32_t> remap(static_cast<size_t>(F.n_levels), -1);
        s.cl_codes.resize(static_cast<size_t>(s.n));
        for (int64_t q = 0; q < s.n; ++q) {
            const int32_t old = F.codes[static_cast<size_t>(sel[static_cast<size_t>(q)])];
            if (old < 0 || old >= F.n_levels)
                throw EngineError("bad_input", std::string(cmd) + ": cluster variable has a missing value");
            if (remap[static_cast<size_t>(old)] < 0) remap[static_cast<size_t>(old)] = s.n_clusters++;
            s.cl_codes[static_cast<size_t>(q)] = remap[static_cast<size_t>(old)];
        }
    }
    return s;
}

// The number of distinct subjects (the stset id() values) among the sample's rows
// (e(N_sub)); -1 when no id variable was named or it cannot be read.
int64_t count_subjects(const Dataset& ds, const StSample& s, const std::string& idvar) {
    if (idvar.empty() || !ds.has(idvar)) return -1;
    const double* id = require_num(ds, "st", idvar.c_str());
    std::vector<double> v;
    v.reserve(s.rows.size());
    for (const int64_t r : s.rows) v.push_back(id[r]);
    std::sort(v.begin(), v.end());
    return static_cast<int64_t>(std::unique(v.begin(), v.end()) - v.begin());
}

}  // namespace

// cobra stset --data f.hbin --time T [--failure F] [--id ID] [--keepif E] --out new.hbin
int cmd_stset(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), tvar = opt(a, "--time"), fvar = opt(a, "--failure"),
                idvar = opt(a, "--id"), out = opt(a, "--out");
    if (data_missing(data) || tvar.empty() || out_missing(out)) {
        std::fprintf(stderr,
                     "usage: cobra stset --data f.hbin --time T [--failure F] [--id ID] "
                     "[--keepif EXPR] --out new.hbin\n"
                     "  Declares survival-time data, Stata `stset T, failure(F) id(ID)`, by writing\n"
                     "  four real columns onto the dataset: _t0 (entry), _t (exit), _d (failure),\n"
                     "  _st (1 = in the analysis sample). With --id the records of one subject form\n"
                     "  the intervals (_t[_n-1], _t]; every record at or after the subject's FIRST\n"
                     "  failure is excluded (_st = 0) but KEPT in the data, exactly as Stata does.\n"
                     "  Without --failure the last record of each subject is the failure.\n"
                     "  The settings report is written to stdout as JSON; --out is the .hbin target.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* tp = require_num(ds, "stset", tvar);
    const double* fp = fvar.empty() ? nullptr : require_num(ds, "stset", fvar);
    const double* ip = idvar.empty() ? nullptr : require_num(ds, "stset", idvar);

    StsetResult r = stset_build(ip, tp, fp, ds.n_rows());
    if (!r.ok) throw EngineError("bad_input", "stset: " + r.err);

    Frame fr = frame_from_acquired(ds);
    fr.set_f64_column(kT0, r.t0);
    fr.set_f64_column(kT, r.t);
    fr.set_f64_column(kD, r.d);
    fr.set_f64_column(kSt, r.st);
    if (!commit_frame(std::move(fr), out))
        std::fprintf(stderr, "stset: wrote %s (%lld of %lld records in the analysis sample)\n",
                     out.c_str(), static_cast<long long>(r.n_kept),
                     static_cast<long long>(r.n_total));

    json report = {
        {"command", "stset"},
        {"time_var", tvar},
        {"id_var", idvar},
        {"failure_var", fvar},
        {"failure_event", fvar.empty() ? std::string("(last record per subject)")
                                       : fvar + "!=0 & " + fvar + "<."},
        {"time_interval", idvar.empty() ? std::string("(0, ") + tvar + "]"
                                        : std::string("(") + tvar + "[_n-1], " + tvar + "]"},
        {"exit_on_or_before", "failure"},
        {"n_total", r.n_total},
        {"n_excluded_missing", r.n_excl_missing},
        {"n_excluded_after_failure", r.n_excl_after_failure},
        {"n_excluded_negative_time", r.n_excl_negative},
        {"n_excluded_zero_length", r.n_excl_zero_length},
        {"n_excluded_same_instant", r.n_excl_same_instant},
        {"n_remaining", r.n_kept},
        {"n_subjects", r.n_subjects},
        {"n_failures", r.n_failures},
        {"single_failure_per_subject", r.single_failure},
        {"time_at_risk", r.time_at_risk},
        {"at_risk_from", r.origin},
        {"earliest_entry", r.first_entry},
        {"last_exit", r.last_exit}};
    emit(report, "");  // stdout, regardless of --out (which is the .hbin target here)
    return 0;
}

namespace {

// Shared coefficient-table rendering for stcox/streg.
//
// `b`/`se`/`ci_lo`/`ci_hi` stay in the RAW estimation metric, exactly like every
// other cobra estimator (so the shared tidy coeftable is internally consistent);
// the HAZARD-RATIO metric Stata prints by default rides alongside as `hr` /
// `se_hr` / `hr_ci_lo` / `hr_ci_hi`.
//
// The three rules the oracle pins:
//   se_HR = |HR| * se_b        (delta method on exp)
//   z, P>|z| come from the RAW b metric, NOT from HR/se_HR
//   the HR interval is exp(b +/- z* se_b), NOT HR +/- z* se_HR
// Cross-check: stcox x1 prints HR .9390707, std. err. .0550338, z -1.07. Then
// b = ln(.9390707) = -.0628617 and se_b = .0550338/.9390707 = .0586045, so
// -.0628617/.0586045 = -1.0726 -> the printed -1.07. (HR/se_HR would give
// -17.06, and HR - 1.96*se_HR would give .8312 against the printed .8371707.)
json coef_row(const std::string& name, double b, double se, double zc) {
    const double z = tstat(b, se);
    const double pv = (se > 0) ? z_twosided_pvalue(z) : 1.0;
    const double hr = std::exp(b);
    return json{{"name", name},
                {"b", b},
                {"se", se},
                {"z", z},
                {"p", pv},
                {"ci_lo", b - zc * se},
                {"ci_hi", b + zc * se},
                {"hr", hr},
                {"se_hr", hr * se},
                {"hr_ci_lo", std::exp(b - zc * se)},
                {"hr_ci_hi", std::exp(b + zc * se)}};
}

// The Wald chi2 Stata prints in the header, over `idx` of the k coefficients.
void add_wald(json& out, const std::vector<double>& beta, const std::vector<double>& V, int np,
              const std::vector<int>& idx) {
    const int j = static_cast<int>(idx.size());
    if (j == 0) return;
    std::vector<double> R(static_cast<size_t>(j) * static_cast<size_t>(np), 0.0), q(static_cast<size_t>(j), 0.0);
    for (int i = 0; i < j; ++i) R[static_cast<size_t>(i) * static_cast<size_t>(np) + static_cast<size_t>(idx[static_cast<size_t>(i)])] = 1.0;
    std::vector<double> b(static_cast<size_t>(np), 0.0);
    for (int i = 0; i < np && i < static_cast<int>(beta.size()); ++i) b[static_cast<size_t>(i)] = beta[static_cast<size_t>(i)];
    WaldResult w = wald_test(b.data(), V.data(), np, R.data(), q.data(), j);
    if (!w.ok) return;
    out["chi2"] = w.chi2;
    out["chi2_df"] = j;
    out["p_chi2"] = chi2_pvalue(w.chi2, static_cast<double>(j));
}

std::string parse_vce(const std::vector<std::string>& a, const char* cmd,
                      const std::string& cluster_var) {
    std::string vce = opt(a, "--vce", "iid");
    if (vce != "iid" && vce != "robust" && vce != "cluster")
        throw EngineError("bad_input",
                          std::string(cmd) + ": --vce must be one of iid|robust|cluster (got " + vce + ")");
    if (vce == "cluster" && cluster_var.empty())
        throw EngineError("bad_input", std::string(cmd) + ": --vce cluster requires --cluster C");
    return vce;
}

}  // namespace

// cobra stcox --data f.hbin --x X1,X2 [--vce iid|robust|cluster] [--cluster C] [--level P]
//             [--ties breslow|efron] [--strata S1,S2] ...
int cmd_stcox(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), xcsv = opt(a, "--x"), cluster_var = opt(a, "--cluster");
    if (data_missing(data) || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra stcox --data f.hbin --x X1,X2 [--vce iid|robust|cluster] "
                     "[--cluster C] [--level P] [--ties breslow|efron|exactp|exactm] [--strata S1,S2] "
                     "[--keepif EXPR] [--out f.json]\n"
                     "  Cox proportional hazards on stset data (needs _t0/_t/_d/_st): partial\n"
                     "  likelihood with delayed entry, risk set { i : _t0_i < t <= _t_i }, Breslow\n"
                     "  ties (Efron's approximation with --ties efron). Coefficients are reported\n"
                     "  as hazard ratios exp(b); z and P>|z| are computed in the raw b metric.\n"
                     "  --strata S1,S2   a separate baseline hazard per combination (risk sets\n"
                     "                   within stratum), Stata's strata().\n"
                     "  --vce cluster --cluster ID   Lin-Wei robust: efficient score residuals\n"
                     "                               summed within cluster (Stata vce(cluster ID)).\n");
        return 2;
    }
    const std::string ties_opt = opt(a, "--ties", "breslow");
    if (ties_opt != "breslow" && ties_opt != "efron" && ties_opt != "exactp" && ties_opt != "exactm")
        throw EngineError("bad_input",
                          "stcox: --ties must be breslow, efron, exactp or exactm (got " + ties_opt + ")");
    const std::vector<std::string> strata_vars = split_csv(opt(a, "--strata"));
    const std::string vce = parse_vce(a, "stcox", cluster_var);
    // The robust/cluster sandwich reads the Breslow score residuals of one risk-set
    // structure; with Efron ties or strata they are other quantities, not computed here.
    if ((vce == "robust" || vce == "cluster") && (ties_opt != "breslow" || !strata_vars.empty()))
        throw EngineError("bad_input",
                          "stcox: a robust/cluster VCE together with efron ties or strata is not "
                          "implemented");
    const double level = std::stod(opt(a, "--level", "95"));
    if (level < 10.0 || level > 99.99)
        throw EngineError("bad_input", "stcox: --level must be in [10, 99.99]");
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    StSample s = load_st(ds, "stcox", split_csv(xcsv), cluster_var, strata_vars);
    if (s.n == 0) { std::fprintf(stderr, "stcox: no rows in the analysis sample.\n"); return 1; }
    // The partial likelihood conditions the intercept away, so the Cox design is
    // load_glm's WITHOUT its trailing intercept column.
    const int k = s.k - 1;
    if (k <= 0) throw EngineError("bad_input", "stcox: at least one covariate is required");

    const CoxTies ties = ties_opt == "efron"    ? CoxTies::Efron
                         : ties_opt == "exactp" ? CoxTies::Exactp
                         : ties_opt == "exactm" ? CoxTies::Exactm
                                                : CoxTies::Breslow;
    CoxResult r = cox_fit(s.X.data(), s.n, k, s.t0.data(), s.t.data(), s.d.data(),
                          s.strata_codes.empty() ? nullptr : s.strata_codes.data(), ties);
    if (!r.ok) throw EngineError("bad_input", r.err.empty() ? "stcox: fit failed" : r.err);

    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S;
        cox_score_residuals(s.X.data(), s.n, k, s.t0.data(), s.t.data(), s.d.data(), r.beta, &S);
        const bool clustered = (vce == "cluster");
        if (clustered) {
            n_clusters = s.n_clusters;
            if (n_clusters < 2)
                throw EngineError("bad_input",
                                  "stcox: vce(cluster) requires at least 2 clusters, but the cluster "
                                  "variable has " + std::to_string(n_clusters));
        }
        // Small-sample multiplier. cluster: q_c = G/(G-1) — the same ML cluster
        // factor logit/tobit use, NOT the regress-style CGM product. VERIFIED,
        // not assumed: with G = 199 the two candidates differ by 0.25% in the SE,
        // and G/(G-1) reproduces the oracle's Haz. ratio std. err. .0550338 /
        // .1019729 exactly (q_c = 1 would give .0548954 / .1017164). robust:
        // N/(N-1), the G=N limit of the same rule — no P7 oracle pins that arm,
        // so it is the house convention rather than a measured fact.
        const double factor =
            clustered ? static_cast<double>(n_clusters) / static_cast<double>(n_clusters - 1)
                      : (s.n > 1 ? static_cast<double>(s.n) / static_cast<double>(s.n - 1) : 1.0);
        V = survival_sandwich(S, s.n, k, r.vcov, clustered ? s.cl_codes.data() : nullptr,
                              clustered ? n_clusters : 0, factor);
        if (V.empty()) throw EngineError("bad_input", "stcox: robust VCE failed (non-invertible sandwich)");
    }

    json coefs = json::array();
    std::vector<double> se(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) {
        se[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(k) + static_cast<size_t>(c)]));
        coefs.push_back(coef_row(s.names[static_cast<size_t>(c)], r.beta[static_cast<size_t>(c)],
                                 se[static_cast<size_t>(c)], zc));
    }
    json out = {{"estimator", "stcox"},
                {"vce", vce},
                // Breslow == every exact method while each tie set is a singleton,
                // which is what Stata's "Cox regression with no ties" header asserts.
                // Reported from a real tie SCAN so the header can never over-claim.
                // Stata's e(ties) words: breslow, efron, partial (exactp).
                {"ties", r.n_tied_failures == 0 ? std::string("none")
                         : ties_opt == "exactp"  ? std::string("partial")
                         : ties_opt == "exactm"  ? std::string("marginal")
                                                 : ties_opt},
                {"n_obs", s.n},
                {"n_dropped_missing", s.n_st - s.n},
                {"n_failures", s.n_fail},
                {"time_at_risk", s.time_at_risk},
                {"ll", r.ll},
                {"ll_null", r.ll_null},
                {"converged", r.converged},
                {"iters", r.iters},
                {"level", level}};
    // e(N_sub): the stset id()'s distinct subjects (--id), else one per record
    {
        const int64_t ns = count_subjects(ds, s, opt(a, "--id"));
        out["n_subjects"] = ns >= 0 ? ns : s.n;
    }
    if (!strata_vars.empty()) out["n_strata"] = s.n_strata;
    std::vector<int> idx(static_cast<size_t>(k));
    for (int c = 0; c < k; ++c) idx[static_cast<size_t>(c)] = c;
    add_wald(out, r.beta, V, k, idx);
    // BUG-383 #6 (off-pin sweep vs live Stata, 2026-09-01): with the default
    // (non-robust) VCE, Stata's stcox header and e(chi2) are the LIKELIHOOD-
    // RATIO statistic, not the Wald — verified against the capture to 1e-10:
    // e(chi2) = 10.4561647774 = 2*(e(ll) - e(ll_0)) exactly, where our Wald
    // said 12.4253. Under vce(robust)/vce(cluster) Stata itself switches the
    // header to Wald (a robust LR is not defined), which is what add_wald
    // already produced — so the override applies to the iid arm only. Both ll
    // terms were already computed and published above; this is a publication
    // fix, not an estimator change.
    if (vce == "iid" && r.converged && k > 0) {
        const double lr = 2.0 * (r.ll - r.ll_null);
        out["chi2"] = lr;
        out["chi2_df"] = k;
        out["p_chi2"] = chi2_pvalue(lr, static_cast<double>(k));
        out["chi2type"] = "LR";
    }
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    out["coefs"] = coefs;
    // --emit-v: the fit's full e(V) on the covariates' names (and, under a sandwich,
    // the observed-information variance as e_Vmb: Stata's e(V_modelbased)).
    if (has(a, "--emit-v")) {
        const std::vector<std::string> vnames(s.names.begin(), s.names.begin() + k);
        add_e_v(out, vnames, V);
        if (vce == "robust" || vce == "cluster") {
            json tmp;
            add_e_v(tmp, vnames, r.vcov);
            if (tmp.contains("e_V")) out["e_Vmb"] = tmp["e_V"];
        }
    }
    emit(out, opt(a, "--out"));
    return 0;
}

// cobra streg --data f.hbin --x X1,X2 --dist D [--time] [--anc Z1,Z2] [--vce iid|robust|cluster]
//             [--cluster C] [--level P] [--keepif EXPR] [--emit-v] [--out f.json]
int cmd_streg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), xcsv = opt(a, "--x"), cluster_var = opt(a, "--cluster"),
                dist = opt(a, "--dist", "weibull");
    if (data_missing(data) || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra streg --data f.hbin --x X1,X2 --dist D [--time] [--anc Z1,Z2]\n"
                     "                   [--vce iid|robust|cluster] [--cluster C] [--level P]\n"
                     "                   [--keepif EXPR] [--emit-v] [--out f.json]\n"
                     "  Parametric survival regression on stset data (needs _t0/_t/_d/_st), on\n"
                     "  Stata's ml path: D = exponential | weibull | gompertz (PH) | loglogistic |\n"
                     "  lognormal (AFT). --time: weibull's accelerated failure-time metric.\n"
                     "  --anc: the ancillary parameter's own covariates (Stata's ancillary()).\n"
                     "  Reports the main equation, the ancillary equation, e_V (--emit-v), the\n"
                     "  constant-only ll (ll_null), the iteration count and log.\n"
                     "  --vce robust on MULTI-RECORD st data requires --cluster ID: Stata clusters\n"
                     "  vce(robust) on the stset id() SUBJECT, not on the record.\n");
        return 2;
    }
    StregDist sd;
    const char* ancEq = nullptr;  // e(b)'s ancillary equation name
    if (dist == "exponential") {
        sd = StregDist::Exponential;
    } else if (dist == "weibull") {
        sd = StregDist::Weibull;
        ancEq = "ln_p";
    } else if (dist == "gompertz") {
        sd = StregDist::Gompertz;
        ancEq = "gamma";
    } else if (dist == "loglogistic") {
        sd = StregDist::Loglogistic;
        ancEq = "lngamma";
    } else if (dist == "lognormal") {
        sd = StregDist::Lognormal;
        ancEq = "lnsigma";
    } else {
        throw EngineError("bad_input", "streg: --dist " + dist + " is not implemented in this build");
    }
    const bool time_metric = has(a, "--time");
    const std::vector<std::string> anc_vars = split_csv(opt(a, "--anc"));
    if (sd == StregDist::Exponential && !anc_vars.empty())
        throw EngineError("bad_input", "streg: exponential has no ancillary parameter");
    const std::string vce = parse_vce(a, "streg", cluster_var);
    const double level = std::stod(opt(a, "--level", "95"));
    if (level < 10.0 || level > 99.99)
        throw EngineError("bad_input", "streg: --level must be in [10, 99.99]");
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    StSample s = load_st(ds, "streg", split_csv(xcsv), cluster_var, {}, anc_vars);
    if (s.n == 0) { std::fprintf(stderr, "streg: no rows in the analysis sample.\n"); return 1; }
    const int k = s.k;              // intercept INCLUDED (load_glm puts it last)
    const int cons_col = s.k - 1;
    // The ancillary design: its variables then a constant, on the sample's rows.
    int kz = sd == StregDist::Exponential ? 0 : 1;
    std::vector<double> Z(static_cast<size_t>(s.n), 1.0);
    std::vector<std::string> znames = {"(intercept)"};
    if (!anc_vars.empty()) {
        GlmInputs gz = load_glm(ds, "streg", kT, anc_vars, {});
        std::map<int64_t, int64_t> at;
        for (int64_t i = 0; i < gz.n; ++i) at[gz.keep[static_cast<size_t>(i)]] = i;
        kz = gz.k;
        znames = gz.names;
        Z.assign(static_cast<size_t>(s.n) * static_cast<size_t>(kz), 0.0);
        for (int64_t q = 0; q < s.n; ++q) {
            const auto it = at.find(s.rows[static_cast<size_t>(q)]);
            if (it == at.end()) throw EngineError("bad_input", "streg: ancillary() row not in the sample");
            for (int c = 0; c < kz; ++c)
                Z[static_cast<int64_t>(c) * s.n + q] = gz.X[static_cast<int64_t>(c) * gz.n + it->second];
        }
    }
    const int np = k + kz;

    const bool cluster_on_id = (vce == "cluster") || (vce == "robust" && !cluster_var.empty());
    if (vce == "robust" && cluster_var.empty() && s.multi_record)
        throw EngineError("bad_input",
                          "streg: --vce robust on multi-record survival data requires --cluster ID "
                          "(Stata clusters vce(robust) on the stset id() subject); pass the id() "
                          "variable used by stset");

    StregFit r = streg_fit(sd, time_metric, s.X.data(), s.n, k, cons_col, kz > 0 ? Z.data() : nullptr,
                           kz, s.t0.data(), s.t.data(), s.d.data());
    if (!r.ok) throw EngineError("bad_input", r.err.empty() ? "streg: MLE failed" : r.err);

    std::vector<double> V = r.V;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        if (cluster_on_id) {
            n_clusters = s.n_clusters;
            if (n_clusters < 2)
                throw EngineError("bad_input",
                                  "streg: clustering requires at least 2 clusters, but the cluster "
                                  "variable has " + std::to_string(n_clusters));
        }
        // q_c = G/(G-1) when clustered, N/(N-1) for the unclustered single-record arm
        // (the factor stcox and streg have carried since P7).
        const double factor =
            cluster_on_id ? static_cast<double>(n_clusters) / static_cast<double>(n_clusters - 1)
                          : (s.n > 1 ? static_cast<double>(s.n) / static_cast<double>(s.n - 1) : 1.0);
        const std::vector<double> Vf = survival_sandwich(r.scores, s.n, np, r.V_fit,
                                                         cluster_on_id ? s.cl_codes.data() : nullptr,
                                                         cluster_on_id ? n_clusters : 0, factor);
        if (Vf.empty()) throw EngineError("bad_input", "streg: robust VCE failed (non-invertible sandwich)");
        V = streg_remetric(r.J, Vf, np);
    }
    auto se_of = [&V, np](int c) {
        return std::sqrt(std::max(0.0, V[static_cast<size_t>(c) * static_cast<size_t>(np) + static_cast<size_t>(c)]));
    };
    json coefs = json::array();
    for (int c = 0; c < k; ++c)
        coefs.push_back(coef_row(s.names[static_cast<size_t>(c)], r.theta[static_cast<size_t>(c)], se_of(c), zc));
    json ancj = json::array();
    for (int c = 0; c < kz; ++c)
        ancj.push_back(coef_row(znames[static_cast<size_t>(c)], r.theta[static_cast<size_t>(k + c)], se_of(k + c), zc));

    const bool aft = time_metric || sd == StregDist::Loglogistic || sd == StregDist::Lognormal;
    json out = {{"estimator", "streg"},
                {"distribution", dist},
                {"metric", aft ? "aft" : "ph"},
                {"vce", vce},
                {"n_obs", s.n},
                {"n_dropped_missing", s.n_st - s.n},
                {"n_failures", s.n_fail},
                {"time_at_risk", s.time_at_risk},
                {"ll", r.ll},
                {"ll_null", r.ll_0},
                {"converged", r.converged},
                {"converged_cons", r.converged_cons},
                {"iters", r.ic},
                {"level", level}};
    if (!aft) out["cons_note"] = "_cons estimates baseline hazard";
    // e(N_sub): the stset id()'s distinct subjects (--id), else one per record
    {
        const int64_t ns = count_subjects(ds, s, opt(a, "--id"));
        out["n_subjects"] = ns >= 0 ? ns : s.n;
    }
    // The model test: the slopes of the main equation (_cons and the ancillary
    // equation excluded) -- Wald under a sandwich, else the LR against the
    // constant-only fit: 2(e(ll) - e(ll_0)) (bank ed95841f LR chi2(3) 37.0667007250652).
    std::vector<int> idx;
    for (int c = 0; c < k; ++c)
        if (c != cons_col) idx.push_back(c);
    add_wald(out, r.theta, V, np, idx);
    if (vce == "iid" && !idx.empty()) {
        const double lr = 2.0 * (r.ll - r.ll_0);
        const int df = static_cast<int>(idx.size());
        out["chi2"] = lr;
        out["chi2_df"] = df;
        out["p_chi2"] = chi2_pvalue(lr, static_cast<double>(df));
        out["chi2type"] = "LR";
    }
    if (cluster_on_id) { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    out["coefs"] = coefs;
    if (ancEq != nullptr) {
        out["anc_eq"] = ancEq;
        out["anc"] = ancj;
    }
    out["ilog"] = r.ilog;
    out["gradient"] = r.gradient;
    // weibull without ancillary(): /ln_p and its two transforms p and 1/p, their
    // intervals the /ln_p interval mapped through each (1/p's comes out reversed).
    if (sd == StregDist::Weibull && anc_vars.empty()) {
        const double lnp = r.theta[static_cast<size_t>(k)], se_lnp = se_of(k), p = std::exp(lnp);
        out["ln_p"] = {{"b", lnp}, {"se", se_lnp}, {"z", tstat(lnp, se_lnp)},
                       {"ci_lo", lnp - zc * se_lnp}, {"ci_hi", lnp + zc * se_lnp}};
        out["p"] = {{"b", p}, {"se", p * se_lnp},
                    {"ci_lo", std::exp(lnp - zc * se_lnp)}, {"ci_hi", std::exp(lnp + zc * se_lnp)}};
        out["inv_p"] = {{"b", 1.0 / p}, {"se", se_lnp / p},
                        {"ci_lo", std::exp(-(lnp + zc * se_lnp))}, {"ci_hi", std::exp(-(lnp - zc * se_lnp))}};
    }
    if (has(a, "--emit-v")) {
        std::vector<std::string> vnames(s.names.begin(), s.names.begin() + k);
        for (int c = 0; c < kz; ++c) vnames.push_back("anc:" + znames[static_cast<size_t>(c)]);
        add_e_v(out, vnames, V);
        if (vce == "robust" || vce == "cluster") {
            json tmp;
            add_e_v(tmp, vnames, r.V);
            if (tmp.contains("e_V")) out["e_Vmb"] = tmp["e_V"];
        }
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
