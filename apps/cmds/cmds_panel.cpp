#include "common.h"
#include "commands.h"

#include "estimators/csdid.h"
#include "estimators/xtpois.h"
#include "estimators/xtreg.h"
#include "estimators/xtlogit.h"
#include "estimators/nbreg.h"
#include "estimators/posthest.h"  // wald_test/WaldResult (cmd_xtreg's df_chi2/Wald chi2)

namespace cecon {
namespace cli {

int cmd_csdid(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::string y = opt(a, "--y");
    std::string ivar = opt(a, "--ivar");
    std::string tvar = opt(a, "--time");
    std::string gvar = opt(a, "--gvar");
    std::string xcsv = opt(a, "--x");
    std::string winstr = opt(a, "--window");
    std::string method = opt(a, "--method", "dripw");
    std::string comparison = opt(a, "--comparison", "never");
    std::string clvar = opt(a, "--cluster");
    double pstrim = std::stod(opt(a, "--pscoretrim", "0.995"));
    bool long2 = has(a, "--long2");
    if (data_missing(data) || y.empty() || ivar.empty() || tvar.empty() || gvar.empty()) {
        std::fprintf(stderr,
            "usage: cobra csdid --data f.hbin --y Y --ivar ID --time T --gvar G "
            "[--x X1,X2,...] [--method dripw] [--comparison never|notyet] "
            "[--cluster CL] [--pscoretrim 0.995] [--window LO,HI] [--long2] [--out f.json]\n");
        return 2;
    }
    if (method != "dripw") {
        std::fprintf(stderr, "cobra csdid: only method=dripw is implemented\n");
        return 2;
    }
    // BUG-040 (input-validation parity): black-box probing of Stata csdid (clean-
    // room; no .ado/.mata read — docs/legal/CSDID_CLEANROOM_WALL.md) confirmed
    // Stata refuses `ivar(X) ... cluster(X)` — the SAME variable named as both the
    // panel identifier and the cluster variable — with r(198) "X may not be both
    // target and by()". The trigger is a literal name match between the --ivar and
    // --cluster tokens of THIS call, independent of xtset (present or absent, the
    // same rejection fires — probed both ways) and independent of method()/
    // comparison() (probed dripw/never and dripw/notyet, same rejection). A
    // value-identical but differently-named cluster column is accepted by Stata
    // (probed: a byte-for-byte copy of the ivar column under a new name runs
    // clustered normally) — the check is on the NAME token, not on the underlying
    // data, so we mirror that exactly: compare the option strings, not the columns.
    if (!clvar.empty() && clvar == ivar) {
        throw cecon::EngineError(
            "bad_input",
            "csdid: --cluster and --ivar name the same variable ('" + ivar +
                "') — a variable cannot be both the panel identifier and the "
                "cluster variable in the same csdid call",
            ivar);
    }
    int wlo = 0, whi = 0;
    if (!winstr.empty()) {
        auto v = split_csv(winstr);
        if (v.size() != 2) { std::fprintf(stderr, "--window LO,HI\n"); return 2; }
        wlo = std::stoi(v[0]); whi = std::stoi(v[1]);
    }
    const Dataset& ds = acquire_data(data);
    auto get_f64 = [&](const std::string& n) -> const double* {
        return require_num(ds, "csdid", n);
    };
    const double* yv = get_f64(y);
    const double* iv = get_f64(ivar);
    const double* tv = get_f64(tvar);
    const double* gv = get_f64(gvar);
    const double* clv = clvar.empty() ? nullptr : get_f64(clvar);
    auto xs = split_csv(xcsv);
    int64_t n_obs = ds.n_rows();
    // BUG-043 (input-validation parity): black-box probing of Stata csdid (clean-
    // room; no .ado/.mata read — docs/legal/CSDID_CLEANROOM_WALL.md, 4 arms +
    // xtset on/off) found Stata's real rule is NOT "cluster()==time()" as the
    // bug title suggests but the general panel-clustering nesting requirement:
    // --cluster must be CONSTANT WITHIN each --ivar unit (r(498) "panels are
    // not nested within clusters"). Confirmed: (a) cluster naming the literal
    // time variable fails ; (b) a byte-identical COPY of the time column under
    // a different name fails identically (value-based, persists under
    // renaming — unlike BUG-040's r(198) name-token check above); (c) a
    // genuinely different but unit-constant cluster var (e.g. a region code)
    // is accepted, unaffected; (d) a cluster var that varies within a unit but
    // is NOT the time column also fails the same way — the nesting property is
    // the trigger, not equality to --time specifically. xtset presence/absence
    // does not change the outcome (probed both ways, mirrors BUG-040).
    if (clv != nullptr) {
        std::unordered_map<double, double> unit_cluster;
        unit_cluster.reserve(static_cast<size_t>(n_obs));
        for (int64_t i = 0; i < n_obs; ++i) {
            auto it = unit_cluster.find(iv[i]);
            if (it == unit_cluster.end()) {
                unit_cluster.emplace(iv[i], clv[i]);
            } else if (it->second != clv[i]) {
                throw cecon::EngineError(
                    "bad_input",
                    "csdid: --cluster ('" + clvar + "') is not nested within --ivar ('" +
                        ivar + "') — its value changes within a panel unit; panel-robust "
                        "clustering requires the cluster variable to be constant within "
                        "each panel unit (panels not nested within clusters)",
                    clvar);
            }
        }
    }
    // Route covariates through the engine varlist expander so factor operators
    // (i./b#./i.x#i.y, …) and time-series operators (L./F./D./S., panel-aware via
    // ivar/time) work in csdid exactly as they do in feols/iv. (Fixes the live bug
    // `csdid … i.h_g6 …` → "column 'i(h_g6)' not found".)
    TsMeta tsm; tsm.timevar = tvar; tsm.panelvar = ivar;  // panel/time = csdid's ivar/time
    auto xcols = expand_varlist(ds, xs, tsm, /*bare_factor=*/false);
    int p = static_cast<int>(xcols.size());
    std::vector<double> X;
    if (p > 0) {
        X.assign(static_cast<size_t>(n_obs) * p, 0.0);
        for (int c = 0; c < p; ++c)
            std::copy(xcols[c].second.begin(), xcols[c].second.end(),
                      X.data() + static_cast<int64_t>(c) * n_obs);
    }
    CsdidConfig cfg;
    cfg.comparison = (comparison == "notyet" ? CsdidComparison::Notyet
                                              : CsdidComparison::Never);
    cfg.pscoretrim = pstrim;
    cfg.cluster = clv;
    cfg.long2 = long2;
    auto t0 = std::chrono::steady_clock::now();
    auto R = csdid_dripw_run(iv, tv, gv, yv, p > 0 ? X.data() : nullptr, p,
                             n_obs, cfg, wlo, whi);
    auto t1 = std::chrono::steady_clock::now();

    json cells_j = json::array();
    for (const auto& c : R.cells) {
        cells_j.push_back({
            {"g", c.g}, {"base", c.base}, {"t", c.t},
            {"att", c.att}, {"se", c.se}, {"n_cell", c.n_cell}, {"ok", c.ok},
        });
    }
    json events_j = json::array();
    for (const auto& e : R.events) {
        events_j.push_back({{"e", e.e}, {"att", e.att}, {"se", e.se}});
    }
    json calendar_j = json::array();
    for (const auto& c : R.calendar) {
        calendar_j.push_back({{"t", c.t}, {"att", c.att}, {"se", c.se}});
    }
    json groups_j = json::array();
    for (const auto& gb : R.groups) {
        groups_j.push_back({{"g", gb.g}, {"att", gb.att}, {"se", gb.se}});
    }
    // F3: event-study e(b)/e(V) for `estat event, post`. e_b is the relative-period
    // ATT vector (named Tm#/Tp#, Stata estat-event order), e_V its full E×E vcov
    // (cluster-folded when --cluster is set). The PIET kernel posts these to the
    // active e(b)/e(V) on `estat event, post`; matrix b/V = e(b)/e(V) then read them.
    const size_t E = R.event_b.size();
    json eV_rows = json::array();
    for (size_t r = 0; r < E; ++r) {
        json row = json::array();
        for (size_t c = 0; c < E; ++c) row.push_back(R.event_V[r * E + c]);
        eV_rows.push_back(row);
    }
    json estat_event_j = {
        {"e_b", {{"names", R.event_names}, {"values", R.event_b}}},
        {"e_V", {{"names", R.event_names}, {"matrix", eV_rows}}},
    };
    json out = {
        {"estimator", "csdid"},
        {"method", "dripw"},
        {"comparison", comparison},
        {"cluster", clvar},
        {"n_units", R.n_units},
        {"n_obs", R.n_obs_est},
        {"G", static_cast<int>(R.glev.size())},
        {"T_minus1", static_cast<int>(R.tlev.size())},
        {"time0", R.time0},
        {"glev", R.glev},
        {"tlev", R.tlev},
        {"pg", R.pg},
        {"cells", cells_j},
        {"simple_att", R.simple_att},
        {"simple_se", R.simple_se},
        {"pre_avg", R.pre_avg},
        {"pre_avg_se", R.pre_avg_se},
        {"post_avg", R.post_avg},
        {"post_avg_se", R.post_avg_se},
        {"events", events_j},
        {"estat_event", estat_event_j},
        {"calendar", calendar_j},
        {"groups", groups_j},
        {"pretrend", {{"chi2", R.pretrend_chi2}, {"df", R.pretrend_df}, {"p", R.pretrend_p}}},
        {"window", {wlo, whi}},
        {"pscoretrim", pstrim},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    emit(out, opt(a, "--out"));
    return R.ok ? 0 : 1;
}

int cmd_xtpois(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string ivar = opt(a, "--ivar");
    std::string off  = opt(a, "--offset");
    std::string vce  = opt(a, "--vce", "iid");
    if (data_missing(data) || y.empty() || xcsv.empty() || ivar.empty()) {
        std::fprintf(stderr,
            "usage: cobra xtpois --data f.hbin --y Y --x X1,X2 --ivar ID "
            "[--offset O] [--vce iid|robust] [--level P] [--keepif EXPR] [--out f.json]\n"
            "  --level P  coeftable ci_lo/ci_hi at P%% (asymptotic z, matching Stata's own\n"
            "             xtpoisson, fe table); default 95, matching Stata's default table\n"
            "             (BUG-202, BUG-279).\n");
        return 2;
    }
    // --level P (BUG-202): xtpois (conditional FE Poisson MLE) had no --level at
    // all. Stata's xtpoisson, fe reports z/P>|z|, the same asymptotic-normal
    // convention as logit/probit/nbreg. BUG-279: default level is 95, like
    // Stata's — the plain invocation emits the 95% interval unprompted.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "xtpois: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "xtpois", y);
    const double* iraw = require_num(ds, "xtpois", ivar);
    const double* oraw = off.empty() ? nullptr : require_num(ds, "xtpois", off);
    auto xs = split_csv(xcsv);
    // Route covariates through the shared varlist expander so factor operators
    // (i./b#./ib#./#/##, …) work in xtpoisson,fe exactly as in feols/csdid —
    // expanded names follow the engine's `level.varname` convention (BUG-101).
    TsMeta tsm; tsm.panelvar = ivar;
    auto xcols = expand_varlist(ds, xs, tsm, /*bare_factor=*/false);
    int k = static_cast<int>(xcols.size());
    std::vector<std::string> xnames(static_cast<size_t>(k));
    std::vector<const double*> xraw(k);
    for (int c = 0; c < k; ++c) {
        xnames[static_cast<size_t>(c)] = xcols[static_cast<size_t>(c)].first;
        xraw[c] = xcols[static_cast<size_t>(c)].second.data();
    }

    // Listwise-delete rows with any missing value, then compact.
    std::vector<const double*> all = {yraw, iraw};
    if (oraw) all.push_back(oraw);
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    if (keep.empty()) { std::fprintf(stderr, "xtpois: no rows left after dropping missing values.\n"); return 1; }
    int64_t n_obs = static_cast<int64_t>(keep.size());
    auto yv_v = gather_rows(yraw, keep);
    auto iv_v = gather_rows(iraw, keep);
    std::vector<double> ov_v; if (oraw) ov_v = gather_rows(oraw, keep);
    std::vector<double> X(static_cast<size_t>(n_obs) * k);
    for (int c = 0; c < k; ++c) {
        auto col = gather_rows(xraw[c], keep);
        std::copy(col.begin(), col.end(), X.data() + static_cast<int64_t>(c) * n_obs);
    }
    // Poisson requires non-negative outcomes.
    for (int64_t i = 0; i < n_obs; ++i)
        if (yv_v[static_cast<size_t>(i)] < 0) {
            std::fprintf(stderr, "xtpois: outcome '%s' has negative values; Poisson requires y >= 0.\n", y.c_str());
            return 2;
        }
    int64_t n_dropped_missing = n_raw - n_obs;
    // Drop covariates with no within-panel variation (collinear with the FE).
    auto kept = within_varying_cols(X, iv_v.data(), n_obs, k);
    std::vector<std::string> omitted;
    for (int c = 0; c < k; ++c)
        if (std::find(kept.begin(), kept.end(), c) == kept.end()) omitted.push_back(xnames[static_cast<size_t>(c)]);
    int kk = static_cast<int>(kept.size());
    if (kk == 0) { std::fprintf(stderr, "xtpois: all covariates are time-invariant within panels (collinear with the fixed effect).\n"); return 2; }
    std::vector<double> Xk(static_cast<size_t>(n_obs) * kk);
    std::vector<std::string> xk(kk);
    for (int j = 0; j < kk; ++j) {
        int c = kept[static_cast<size_t>(j)];
        xk[static_cast<size_t>(j)] = xnames[static_cast<size_t>(c)];
        std::copy(X.begin() + static_cast<int64_t>(c) * n_obs,
                  X.begin() + static_cast<int64_t>(c + 1) * n_obs,
                  Xk.begin() + static_cast<int64_t>(j) * n_obs);
    }
    auto t0 = std::chrono::steady_clock::now();
    auto R = xtpoisson_fe(Xk.data(), n_obs, kk, yv_v.data(), iv_v.data(),
                          ov_v.empty() ? nullptr : ov_v.data());
    auto t1 = std::chrono::steady_clock::now();
    if (static_cast<int>(R.beta.size()) < kk || static_cast<int>(R.se_iid.size()) < kk ||
        static_cast<int>(R.se_robust.size()) < kk) {
        std::fprintf(stderr, "xtpois: estimation produced no coefficients (degenerate fit — all panels dropped, collinear, or non-convergent).\n");
        return 1;
    }

    json coefs = json::array();
    for (int c = 0; c < kk; ++c) {
        bool robust = (vce == "robust");
        double se = robust ? R.se_robust[c] : R.se_iid[c];
        json row = {
            {"name", xk[c]}, {"beta", R.beta[c]},
            {"se", se}, {"t", tstat(R.beta[c], se)},
            {"se_iid", R.se_iid[c]}, {"se_robust", R.se_robust[c]},
        };
        if (se > 0) {
            row["ci_lo"] = R.beta[c] - zc * se;
            row["ci_hi"] = R.beta[c] + zc * se;
        }
        coefs.push_back(std::move(row));
    }
    json out = {
        {"estimator", "xtpoisson_fe"},
        {"vce", vce},
        {"n_obs", R.n_obs},
        {"n_groups", R.n_groups},
        {"n_dropped_missing", n_dropped_missing},
        {"n_dropped_zero", R.n_dropped_zero},
        {"n_dropped_size", R.n_dropped_size},
        {"k", R.k},
        {"omitted", omitted},
        {"loglik", R.loglik},
        {"iters", R.iters},
        {"converged", R.converged},
        {"coefs", coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    emit(out, opt(a, "--out"));
    return R.converged ? 0 : 1;
}

int cmd_xtreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string ivar = opt(a, "--ivar");
    if (data_missing(data) || y.empty() || xcsv.empty() || ivar.empty()) {
        std::fprintf(stderr,
            "usage: cobra xtreg --data f.hbin --y Y --x X1,X2 --ivar ID [--keepif EXPR]\n"
            "                   [--esample-out PATH [--esample-index COL --esample-n N]] [--out f.json]\n"
            "  (random-effects Swamy-Arora; FE absorbs time-invariant vars in the within step)\n"
            "  --esample-out PATH  write e(sample), one 1/0 line per --data row (see feols).\n");
        return 2;
    }
    const EsampleRequest esreq = esample_request(a, "xtreg");
    const std::string src_data = data, keepif = opt(a, "--keepif");
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "xtreg", y);
    const double* iraw = require_num(ds, "xtreg", ivar);
    auto xs = split_csv(xcsv);
    int k = static_cast<int>(xs.size());
    std::vector<const double*> xraw(k);
    for (int c = 0; c < k; ++c) xraw[c] = require_num(ds, "xtreg", xs[c]);
    std::vector<const double*> all = {yraw, iraw};
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    if (keep.empty()) { std::fprintf(stderr, "xtreg: no rows left after dropping missing values.\n"); return 1; }
    int64_t n_obs = static_cast<int64_t>(keep.size());
    int64_t n_dropped_missing = n_raw - n_obs;
    auto yv_v = gather_rows(yraw, keep);
    auto iv_v = gather_rows(iraw, keep);
    std::vector<double> X(static_cast<size_t>(n_obs) * k);
    for (int c = 0; c < k; ++c) {
        auto col = gather_rows(xraw[c], keep);
        std::copy(col.begin(), col.end(), X.data() + static_cast<int64_t>(c) * n_obs);
    }
    auto t0 = std::chrono::steady_clock::now();
    auto R = xtreg_re(X.data(), n_obs, k, yv_v.data(), iv_v.data());
    auto t1 = std::chrono::steady_clock::now();
    if (static_cast<int>(R.beta.size()) < k + 1 || static_cast<int>(R.se.size()) < k + 1) {
        std::fprintf(stderr, "xtreg: estimation produced no coefficients (rank-deficient within design — collinear regressors, no within-panel variation, or too few observations).\n");
        return 1;
    }
    json coefs = json::array();
    for (int c = 0; c < k + 1; ++c) {
        std::string nm = (c < k) ? xs[static_cast<size_t>(c)] : std::string("_cons");
        coefs.push_back({{"name", nm}, {"beta", R.beta[c]},
                         {"se", R.se[c]}, {"t", tstat(R.beta[c], R.se[c])}});
    }

    // --- model-fit scalars (Stata xtreg, re): three R²s + Wald χ² (since contract A 1.5.0) ---
    // R² are squared Pearson correlations between the slope-only linear predictor xb and y,
    // in the overall / between (group means) / within (group-demeaned) dimensions — the xtreg
    // convention (the intercept cancels in every correlation, hence slopes-only xb).
    std::vector<double> xb(static_cast<size_t>(n_obs), 0.0);
    for (int c = 0; c < k; ++c)
        for (int64_t i = 0; i < n_obs; ++i)
            xb[static_cast<size_t>(i)] += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[c];
    auto corr2 = [](const std::vector<double>& av, const std::vector<double>& bv) -> double {
        size_t m = av.size();
        if (m == 0) return 0.0;
        double ma = 0, mb = 0;
        for (size_t i = 0; i < m; ++i) { ma += av[i]; mb += bv[i]; }
        ma /= static_cast<double>(m); mb /= static_cast<double>(m);
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < m; ++i) {
            double da = av[i] - ma, db = bv[i] - mb;
            sab += da * db; saa += da * da; sbb += db * db;
        }
        double den = saa * sbb;
        return (den > 0.0) ? (sab * sab) / den : 0.0;
    };
    double r2_overall = corr2(xb, yv_v);
    // group means (between) + group-demeaned (within); map ivar value → contiguous group.
    std::unordered_map<int64_t, int> gidx;
    std::vector<double> g_xb, g_y; std::vector<int64_t> g_n;
    std::vector<int> grp(static_cast<size_t>(n_obs));
    for (int64_t i = 0; i < n_obs; ++i) {
        int64_t key = static_cast<int64_t>(std::llround(iv_v[static_cast<size_t>(i)]));
        auto it = gidx.find(key);
        int gi;
        if (it == gidx.end()) { gi = static_cast<int>(g_xb.size()); gidx[key] = gi;
                                g_xb.push_back(0); g_y.push_back(0); g_n.push_back(0); }
        else gi = it->second;
        grp[static_cast<size_t>(i)] = gi;
        g_xb[static_cast<size_t>(gi)] += xb[static_cast<size_t>(i)];
        g_y[static_cast<size_t>(gi)] += yv_v[static_cast<size_t>(i)];
        g_n[static_cast<size_t>(gi)] += 1;
    }
    int ng = static_cast<int>(g_xb.size());
    for (int g = 0; g < ng; ++g) { g_xb[static_cast<size_t>(g)] /= static_cast<double>(g_n[static_cast<size_t>(g)]);
                                   g_y[static_cast<size_t>(g)]  /= static_cast<double>(g_n[static_cast<size_t>(g)]); }
    double r2_between = corr2(g_xb, g_y);
    std::vector<double> xbw(static_cast<size_t>(n_obs)), yw(static_cast<size_t>(n_obs));
    for (int64_t i = 0; i < n_obs; ++i) {
        int gi = grp[static_cast<size_t>(i)];
        xbw[static_cast<size_t>(i)] = xb[static_cast<size_t>(i)] - g_xb[static_cast<size_t>(gi)];
        yw[static_cast<size_t>(i)]  = yv_v[static_cast<size_t>(i)] - g_y[static_cast<size_t>(gi)];
    }
    double r2_within = corr2(xbw, yw);
    // Wald χ²: H0 all k slopes = 0 (intercept = last coef, excluded). R selects the k slopes.
    int kk = k + 1;
    std::vector<double> Rsel(static_cast<size_t>(k) * kk, 0.0);  // k×(k+1) row-major selector
    for (int j = 0; j < k; ++j) Rsel[static_cast<size_t>(j) * kk + j] = 1.0;
    std::vector<double> qz(static_cast<size_t>(k), 0.0);
    WaldResult wr = wald_test(R.beta.data(), R.vcov_matrix.data(), kk, Rsel.data(), qz.data(), k);

    json out = {
        {"estimator", "xtreg_re"},
        {"n_obs", R.n_obs}, {"n_groups", R.n_groups}, {"n_dropped_missing", n_dropped_missing},
        {"sigma_e", R.sigma_e}, {"sigma_u", R.sigma_u}, {"rho", R.rho}, {"T_h", R.T_h},
        {"r2_within", r2_within}, {"r2_between", r2_between}, {"r2_overall", r2_overall},
        {"df_chi2", k},
        {"coefs", coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    if (wr.ok && k > 0) {
        out["chi2"] = wr.chi2;
        out["p_chi2"] = chi2_pvalue(wr.chi2, static_cast<double>(k));
    } else {
        out["chi2"] = nullptr;
        out["p_chi2"] = nullptr;
    }
    // e(sample) T1a: the RE fit uses every listwise-kept row (no singleton drop).
    if (esreq.active()) {
        EsampleRows rows;
        rows.keep = std::move(keep);
        rows.loaded = &ds;
        emit_esample(esreq, "xtreg", src_data, keepif, false, rows, out);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_nbreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string off = opt(a, "--offset");
    std::string cl  = opt(a, "--cluster");
    std::string vce = opt(a, "--vce", "iid");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
            "usage: cobra nbreg --data f.hbin --y Y --x X1,X2 "
            "[--no-intercept] [--offset O] [--vce iid|robust|cluster] [--cluster C] [--level P] "
            "[--keepif EXPR] [--out f.json]\n"
            "  --level P  coeftable ci_lo/ci_hi at P%% (asymptotic z, matching Stata's own\n"
            "             nbreg table); default 95, matching Stata's default table\n"
            "             (BUG-202, BUG-279).\n");
        return 2;
    }
    // --level P (BUG-202): nbreg had no --level at all, so ci_lo/ci_hi was
    // unconditionally absent. Stata's nbreg reports z/P>|z| (asymptotic ML), the
    // same convention as logit/probit — NOT the t(df_r) this file's tobit sibling
    // uses. BUG-279: default level is 95, like Stata's — the plain invocation
    // emits the 95% interval unprompted.
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "nbreg: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "nbreg", y);
    const double* oraw = off.empty() ? nullptr : require_num(ds, "nbreg", off);
    bool want_cluster = (vce == "cluster") && !cl.empty();
    const double* craw = want_cluster ? require_num(ds, "nbreg", cl) : nullptr;
    bool intercept = !has(a, "--no-intercept");
    auto xs = split_csv(xcsv);
    // Route covariates through the shared varlist expander so factor operators
    // (i./b#./ib#./#/##, …) work in nbreg exactly as in feols/csdid — expanded
    // names follow the engine's `level.varname` convention (BUG-101).
    TsMeta tsm;  // nbreg has no panel/time context of its own; TS ops unsupported here
    auto xcols = expand_varlist(ds, xs, tsm, /*bare_factor=*/false);
    int kx = static_cast<int>(xcols.size());         // user-supplied regressors (post-expansion)
    int k = kx + (intercept ? 1 : 0);               // Stata nbreg adds _cons by default (cf. ols)
    std::vector<const double*> xraw(kx);
    for (int c = 0; c < kx; ++c) xraw[c] = xcols[static_cast<size_t>(c)].second.data();
    std::vector<const double*> all = {yraw};
    if (oraw) all.push_back(oraw);
    if (craw) all.push_back(craw);
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    if (keep.empty())
        throw cecon::EngineError("no_obs", "nbreg: no rows left after dropping missing values.");
    int64_t n_obs = static_cast<int64_t>(keep.size());
    int64_t n_dropped_missing = n_raw - n_obs;
    auto yv_v = gather_rows(yraw, keep);
    std::vector<double> ov_v; if (oraw) ov_v = gather_rows(oraw, keep);
    std::vector<double> cv_v; if (craw) cv_v = gather_rows(craw, keep);
    std::vector<double> X(static_cast<size_t>(n_obs) * k);
    for (int c = 0; c < kx; ++c) {
        auto col = gather_rows(xraw[c], keep);
        std::copy(col.begin(), col.end(), X.data() + static_cast<int64_t>(c) * n_obs);
    }
    if (intercept)                                   // trailing constant column = _cons
        std::fill_n(X.data() + static_cast<int64_t>(kx) * n_obs, n_obs, 1.0);
    for (int64_t i = 0; i < n_obs; ++i)
        if (yv_v[static_cast<size_t>(i)] < 0) {
            throw cecon::EngineError("bad_input",
                "nbreg: outcome '" + y + "' has negative values; count models require y >= 0.");
        }
    auto t0 = std::chrono::steady_clock::now();
    auto R = nbreg(X.data(), n_obs, k, yv_v.data(),
                   ov_v.empty() ? nullptr : ov_v.data(),
                   cv_v.empty() ? nullptr : cv_v.data());
    auto t1 = std::chrono::steady_clock::now();
    if (static_cast<int>(R.beta.size()) < k || static_cast<int>(R.se_iid.size()) < k ||
        static_cast<int>(R.se_robust.size()) < k) {
        // Structured error → top-level catch emits {"error":{code,message}} as JSON,
        // honouring Boundary A (cobra is JSON-out) even under --tidy / --format json.
        throw cecon::EngineError("no_convergence",
            "nbreg: estimation produced no coefficients (degenerate fit — collinear, "
            "non-convergent, or all-zero outcome).");
    }
    json coefs = json::array();
    for (int c = 0; c < k; ++c) {
        double se = (vce == "iid") ? R.se_iid[c] : R.se_robust[c];
        std::string nm = (c < kx) ? xcols[static_cast<size_t>(c)].first : std::string("_cons");
        json row = {{"name", nm}, {"beta", R.beta[c]},
                    {"se", se}, {"t", tstat(R.beta[c], se)},
                    {"se_iid", R.se_iid[c]}, {"se_robust", R.se_robust[c]}};
        if (se > 0) {
            row["ci_lo"] = R.beta[c] - zc * se;
            row["ci_hi"] = R.beta[c] + zc * se;
        }
        coefs.push_back(std::move(row));
    }
    json out = {
        {"estimator", "nbreg"}, {"vce", vce},
        {"n_obs", R.n_obs}, {"k", R.k}, {"n_dropped_missing", n_dropped_missing},
        {"alpha", R.alpha}, {"lnalpha", R.lnalpha},
        {"se_lnalpha_iid", R.se_lnalpha_iid},
        {"loglik", R.loglik}, {"iters", R.iters}, {"converged", R.converged},
        {"coefs", coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    emit(out, opt(a, "--out"));
    return R.converged ? 0 : 1;
}

int cmd_xtlogit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string ivar = opt(a, "--ivar");
    std::string vce  = opt(a, "--vce", "iid");
    if (data_missing(data) || y.empty() || xcsv.empty() || ivar.empty()) {
        std::fprintf(stderr,
            "usage: cobra xtlogit --data f.hbin --y Y --x X1,X2 --ivar ID "
            "[--vce iid|robust] [--out f.json]\n");
        return 2;
    }
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "xtlogit", y);
    const double* iraw = require_num(ds, "xtlogit", ivar);
    auto xs = split_csv(xcsv);
    // Route covariates through the shared varlist expander so factor operators
    // (i./b#./ib#./#/##, …) work in xtlogit,fe exactly as in feols/csdid —
    // expanded names follow the engine's `level.varname` convention (BUG-101).
    TsMeta tsm; tsm.panelvar = ivar;
    auto xcols = expand_varlist(ds, xs, tsm, /*bare_factor=*/false);
    int k = static_cast<int>(xcols.size());
    std::vector<std::string> xnames(static_cast<size_t>(k));
    std::vector<const double*> xraw(k);
    for (int c = 0; c < k; ++c) {
        xnames[static_cast<size_t>(c)] = xcols[static_cast<size_t>(c)].first;
        xraw[c] = xcols[static_cast<size_t>(c)].second.data();
    }
    std::vector<const double*> all = {yraw, iraw};
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    if (keep.empty()) { std::fprintf(stderr, "xtlogit: no rows left after dropping missing values.\n"); return 1; }
    int64_t n_obs = static_cast<int64_t>(keep.size());
    int64_t n_dropped_missing = n_raw - n_obs;
    auto yv_v = gather_rows(yraw, keep);
    auto iv_v = gather_rows(iraw, keep);
    // Conditional logit needs a 0/1 outcome — reject non-binary up front
    // (otherwise the partition-function recursion indexes out of bounds -> crash).
    for (int64_t i = 0; i < n_obs; ++i) {
        double yi = yv_v[static_cast<size_t>(i)];
        if (yi != 0.0 && yi != 1.0) {
            std::fprintf(stderr,
                "xtlogit: outcome '%s' is not binary (found value %g). xtlogit, fe "
                "requires y in {0,1}. Recode first, e.g. cobra eval --gen \"%s_bin=(%s>0)\".\n",
                y.c_str(), yi, y.c_str(), y.c_str());
            return 2;
        }
    }
    std::vector<double> X(static_cast<size_t>(n_obs) * k);
    for (int c = 0; c < k; ++c) {
        auto col = gather_rows(xraw[c], keep);
        std::copy(col.begin(), col.end(), X.data() + static_cast<int64_t>(c) * n_obs);
    }
    // Drop covariates with no within-panel variation (collinear with the FE).
    auto kept = within_varying_cols(X, iv_v.data(), n_obs, k);
    std::vector<std::string> omitted;
    for (int c = 0; c < k; ++c)
        if (std::find(kept.begin(), kept.end(), c) == kept.end()) omitted.push_back(xnames[static_cast<size_t>(c)]);
    int kk = static_cast<int>(kept.size());
    if (kk == 0) { std::fprintf(stderr, "xtlogit: all covariates are time-invariant within panels (collinear with the fixed effect).\n"); return 2; }
    std::vector<double> Xk(static_cast<size_t>(n_obs) * kk);
    std::vector<std::string> xk(kk);
    for (int j = 0; j < kk; ++j) {
        int c = kept[static_cast<size_t>(j)];
        xk[static_cast<size_t>(j)] = xnames[static_cast<size_t>(c)];
        std::copy(X.begin() + static_cast<int64_t>(c) * n_obs,
                  X.begin() + static_cast<int64_t>(c + 1) * n_obs,
                  Xk.begin() + static_cast<int64_t>(j) * n_obs);
    }
    auto t0 = std::chrono::steady_clock::now();
    auto R = xtlogit_fe(Xk.data(), n_obs, kk, yv_v.data(), iv_v.data());
    auto t1 = std::chrono::steady_clock::now();
    if (static_cast<int>(R.beta.size()) < kk || static_cast<int>(R.se_iid.size()) < kk ||
        static_cast<int>(R.se_robust.size()) < kk) {
        std::fprintf(stderr, "xtlogit: estimation produced no coefficients (all panels dropped — no within-unit variation in y, or non-convergent).\n");
        return 1;
    }
    json coefs = json::array();
    for (int c = 0; c < kk; ++c) {
        double se = (vce == "robust") ? R.se_robust[c] : R.se_iid[c];
        coefs.push_back({{"name", xk[c]}, {"beta", R.beta[c]},
                         {"se", se}, {"t", tstat(R.beta[c], se)},
                         {"se_iid", R.se_iid[c]}, {"se_robust", R.se_robust[c]}});
    }
    json out = {
        {"estimator", "xtlogit_fe"}, {"vce", vce}, {"omitted", omitted},
        {"n_obs", R.n_obs}, {"n_groups", R.n_groups}, {"n_dropped_missing", n_dropped_missing},
        {"n_dropped_const", R.n_dropped_const}, {"n_dropped_size", R.n_dropped_size},
        {"loglik", R.loglik}, {"iters", R.iters}, {"converged", R.converged},
        {"coefs", coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    emit(out, opt(a, "--out"));
    return R.converged ? 0 : 1;
}

}  // namespace cli
}  // namespace cecon
