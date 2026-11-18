// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include <algorithm>
#include "commands.h"

#include "estimators/csdid.h"
#include "estimators/xtpois.h"
#include "estimators/xtpoisre.h"
#include "estimators/xtreg.h"
#include "estimators/xtlogit.h"
#include "estimators/nbreg.h"
#include "core/collinear.h"
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
    std::string method = opt(a, "--method");
    std::string comparison = opt(a, "--comparison", "never");
    std::string clvar = opt(a, "--cluster");
    double pstrim = std::stod(opt(a, "--pscoretrim", "0.995"));
    bool long2 = has(a, "--long2");
    if (data_missing(data) || y.empty() || ivar.empty() || tvar.empty() || gvar.empty()) {
        std::fprintf(stderr,
            "usage: cobra csdid --data f.hbin --y Y --ivar ID --time T --gvar G "
            "[--x X1,X2,...] [--method dripw|reg] [--comparison never|notyet] "
            "[--cluster CL] [--pscoretrim 0.995] [--window LO,HI] [--long2] [--out f.json]\n");
        return 2;
    }
    // csdid's cell estimators: dripw (SZ20 §3.1) and reg (SZ20 §2). Stata's
    // drimp, ipw and stdipw are not built; they are refused by name.
    if (!method.empty() && method != "dripw" && method != "reg") {
        throw cecon::EngineError(
            "unsupported",
            "csdid: method(" + method + ") is not implemented in this version; "
            "the built cell estimators are dripw and reg",
            method);
    }
    // BUG-040 (input-validation parity): black-box probing of Stata csdid
    // (observed behaviour only) confirmed
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
    // `if`: csdid.ado's touse restricts every cell sample, the unit universe
    // and the time grid alike, so the fit runs on the rows the condition keeps
    // (vs-stata bank 7a3c76ca `csdid y if sid<=40, ivar(sid) time(year) gvar(gvar)`).
    const EsampleRequest esreq = esample_request(a, "csdid");
    const std::string keepif = opt(a, "--keepif");
    const std::string data_all = data;
    data = maybe_keepif(data, keepif);
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
    // BUG-043 (input-validation parity): black-box probing of Stata csdid
    // (observed behaviour only; 4 arms + xtset on/off) found Stata's real rule is NOT "cluster()==time()" as the
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
    // Stata's default (csdid-drdid LAYOUT.md 0): dripw, or reg when there is
    // no covariate.
    if (method.empty()) method = p > 0 ? "dripw" : "reg";
    CsdidConfig cfg;
    cfg.method = method == "reg" ? CsdidMethod::Reg : CsdidMethod::Dripw;
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
            {"att", c.att}, {"se", c.se}, {"n_cell", c.n_cell}, {"n_trt", c.n_trt}, {"ok", c.ok},
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
    // csdid's own e(b)/e(V) per agg() form (csdid-drdid LAYOUT.md 1.1-1.6):
    // {names (Stata's column full names), b, V (rows)}.
    auto posting_j = [](const CsdidPosting& P) {
        const size_t m = P.b.size();
        json rows = json::array();
        for (size_t r = 0; r < m; ++r) {
            json row = json::array();
            for (size_t c = 0; c < m; ++c) row.push_back(P.V[r * m + c]);
            rows.push_back(std::move(row));
        }
        return json{{"names", P.names}, {"b", P.b}, {"V", std::move(rows)}};
    };
    json out = {
        {"estimator", "csdid"},
        {"method", method},
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
        {"attgt", posting_j(R.attgt)},
        {"agg_simple", posting_j(R.agg_simple)},
        {"agg_event", posting_j(R.agg_event)},
        {"agg_group", posting_j(R.agg_group)},
        {"agg_calendar", posting_j(R.agg_calendar)},
        {"n_clusters", clv != nullptr ? json(R.n_clusters) : json(nullptr)},
        {"calendar", calendar_j},
        {"groups", groups_j},
        {"pretrend", {{"chi2", R.pretrend_chi2}, {"df", R.pretrend_df}, {"p", R.pretrend_p}}},
        {"window", {wlo, whi}},
        {"pscoretrim", pstrim},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        {"ok", R.ok},
        {"error", R.error},
    };
    // e(sample): every row some estimated cell used (csdid.ado's cs_sample, the
    // union of each cell's drdid e(sample)).
    if (esreq.active() && R.ok) {
        EsampleRows rows;
        rows.keep = R.sample_rows;
        rows.loaded = &ds;
        emit_esample(esreq, "csdid", data_all, keepif, false, rows, out);
    }
    emit(out, opt(a, "--out"));
    return R.ok ? 0 : 1;
}


// cobra xtpois --re: the random-effects (gamma) Poisson model, Stata's default
// xtpoisson (estimators/xtpoisre.h). --x as load_glm reads it (factor variables
// included; the constant last, dropped under --no-intercept); --offset V or
// --exposure V (offset ln V); --vce robust|cluster: the sandwich over the panel
// scores, G/(G-1) (xtpoisson's vce(robust) clusters on the panel).
static int run_xtpois_re(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    const std::string ivar = opt(a, "--ivar");
    const std::string offv = opt(a, "--offset"), expv = opt(a, "--exposure");
    const std::string vce = opt(a, "--vce", "iid");
    if (data_missing(data) || y.empty() || xcsv.empty() || ivar.empty())
        throw EngineError("bad_input", "xtpois --re: --data, --y, --x and --ivar are required");
    if (vce != "iid" && vce != "robust" && vce != "cluster")
        throw EngineError("bad_input", "xtpois --re: --vce must be iid, robust or cluster");
    if (!offv.empty() && !expv.empty())
        throw EngineError("bad_input", "xtpois --re: --offset and --exposure are exclusive");
    const bool level_set = has(a, "--level");
    const double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "xtpois: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    int max_iter = 300;
    if (has(a, "--iterate")) max_iter = std::stoi(opt(a, "--iterate"));
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    // the panel id rides as a cluster column: listwise with y and x
    GlmInputs g = load_glm(ds, "xtpois", y, split_csv(xcsv), {ivar}, "", cecon::WeightKind::None,
                           offv.empty() ? expv : offv);
    require_sample(g.n);
    int k = g.k;
    int cons_col = -1;
    if (k > 0 && !g.names.empty() && g.names.back() == "(intercept)") {
        if (has(a, "--no-intercept")) {
            g.k -= 1;
            g.X.resize(static_cast<size_t>(g.n) * g.k);
            g.names.pop_back();
            k = g.k;
        } else {
            cons_col = k - 1;
        }
    }
    if (k == 0) throw EngineError("bad_input", "xtpois --re: no regressors");
    for (int64_t i = 0; i < g.n; ++i)
        if (g.y[static_cast<size_t>(i)] < 0)
            throw EngineError("bad_input", "xtpois --re: " + y + " must be greater than or equal to zero");
    std::vector<double> off;
    if (!g.offset.empty()) {
        off = g.offset;
        if (!expv.empty())
            for (double& v : off) {
                if (!(v > 0)) throw EngineError("bad_input", "xtpois --re: exposure() must be positive");
                v = std::log(v);
            }
    }
    // panel ids as doubles (the cluster factor's dense codes keep the grouping)
    std::vector<double> idv(static_cast<size_t>(g.n));
    for (int64_t i = 0; i < g.n; ++i) idv[static_cast<size_t>(i)] = static_cast<double>(g.cluster[0].codes[static_cast<size_t>(i)]);
    const double* offp = off.empty() ? nullptr : off.data();
    XtpoisReResult r = xtpoisson_re_gamma(g.X.data(), g.n, k, g.y.data(), idv.data(), offp, cons_col, max_iter);
    if (!r.ok)
        throw EngineError("no_convergence", "xtpois --re: MLE failed (singular information matrix or non-convergent fit).");
    const int np = k + 1;
    std::vector<double> V = r.V;
    int n_clusters = 0;
    if (vce != "iid") {
        // xtpoisson.ado's own robust VCE, clustered on the panel (vce(cluster panelvar)
        // is the same); vs-stata bank 8c1f9870 `xtpoisson fatal beertax unemp, vce(robust)`
        V = xtpoisson_re_stata_robust(g.X.data(), g.n, k, g.y.data(), idv.data(), offp, r);
        n_clusters = static_cast<int>(r.n_groups);
    }
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };
    json coefs = json::array();
    for (int c = 0; c < k; ++c) {
        const double b = r.theta[static_cast<size_t>(c)], se = sd(c);
        const double z = se > 0 ? b / se : 0.0;
        coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                         {"p", se > 0 ? z_twosided_pvalue(z) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    const double la_b = r.theta[static_cast<size_t>(k)], la_se = sd(k);
    // Wald test of the regressors (the constant excluded)
    std::vector<int> idx;
    for (int c = 0; c < k; ++c)
        if (c != cons_col) idx.push_back(c);
    double chi2 = 0.0;
    {
        const int d = static_cast<int>(idx.size());
        std::vector<double> Vs(static_cast<size_t>(d) * d);
        for (int i = 0; i < d; ++i)
            for (int j = 0; j < d; ++j)
                Vs[static_cast<size_t>(i) * d + j] = V[static_cast<size_t>(idx[static_cast<size_t>(i)]) * np + idx[static_cast<size_t>(j)]];
        if (d > 0 && la::spd_inverse(Vs.data(), d) == 0)
            for (int i = 0; i < d; ++i)
                for (int j = 0; j < d; ++j)
                    chi2 += r.theta[static_cast<size_t>(idx[static_cast<size_t>(i)])] * Vs[static_cast<size_t>(i) * d + j] *
                            r.theta[static_cast<size_t>(idx[static_cast<size_t>(j)])];
    }
    // XTPOISSON_FE_ROBUST's chi2: e(b)*syminv(V)*e(b)' over every parameter, the
    // constant and /lnalpha included (bank 8c1f9870 e(chi2) 2630.2993)
    if (vce != "iid") {
        std::vector<double> Vi = V;
        chi2 = 0.0;
        if (la::spd_inverse(Vi.data(), np) == 0)
            for (int i = 0; i < np; ++i)
                for (int j = 0; j < np; ++j)
                    chi2 += r.theta[static_cast<size_t>(i)] * Vi[static_cast<size_t>(i) * np + j] * r.theta[static_cast<size_t>(j)];
    }
    const int df = static_cast<int>(idx.size());
    json out = {{"estimator", "xtpoisson_re"}, {"vce", vce}, {"n_obs", g.n}, {"n_groups", r.n_groups},
                {"n_dropped_missing", g.n_dropped},
                {"g_min", r.g_min}, {"g_avg", r.g_avg}, {"g_max", r.g_max},
                {"coefs", coefs},
                {"lnalpha", {{"b", la_b}, {"se", la_se}, {"ci_lo", la_b - zc * la_se}, {"ci_hi", la_b + zc * la_se}}},
                {"alpha", {{"b", std::exp(la_b)}, {"se", std::exp(la_b) * la_se},
                           {"ci_lo", std::exp(la_b - zc * la_se)}, {"ci_hi", std::exp(la_b + zc * la_se)}}},
                {"ll", r.ll}, {"ll_c", r.ll_c},
                // xtpoisson.ado: a fit below the comparison model (the boundary) is chibar2 0
                {"chi2_c", r.ll < r.ll_c ? 0.0 : 2.0 * (r.ll - r.ll_c)},
                {"chi2", chi2}, {"df_chi2", df}, {"p_chi2", chi2_pvalue(chi2, df)}, {"chi2type", "Wald"},
                {"iters", r.iters}, {"converged", r.converged}, {"ilog", r.ilog}, {"gradient", r.gradient},
                {"level", level}};
    if (vce != "iid") out["n_clusters"] = n_clusters;
    if (!offv.empty()) out["offset"] = offv;
    if (!expv.empty()) out["offset"] = "ln(" + expv + ")";
    if (has(a, "--emit-v")) {
        std::vector<std::string> names = g.names;
        names.push_back("/lnalpha");
        add_e_v(out, names, V);
        if (vce != "iid") {
            json tmp;
            add_e_v(tmp, names, r.V);
            out["e_Vmb"] = std::move(tmp["e_V"]);
        }
    }
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_xtpois(const std::vector<std::string>& a) {
    if (has(a, "--re")) return run_xtpois_re(a);
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
    require_sample(static_cast<int64_t>(keep.size()));  // BUG-1269: r(2000) / r(2001)
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
        {"g_min", R.g_min}, {"g_avg", R.g_avg}, {"g_max", R.g_max},
        {"k", R.k},
        {"omitted", omitted},
        {"loglik", R.loglik},
        {"iters", R.iters},
        {"converged", R.converged},
        {"coefs", coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    // --emit-v: e(V), the covariance the reported se come from (ME06: margins
    // after xtpoisson, fe answers from e(b)/e(V)).
    if (has(a, "--emit-v"))
        add_e_v(out, xk, vce == "robust" ? R.V_robust : R.V_iid, nullptr);
    emit(out, opt(a, "--out"));
    return R.converged ? 0 : 1;
}

int cmd_xtreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string ivar = opt(a, "--ivar");
    if (data_missing(data) || y.empty() || xcsv.empty() || ivar.empty()) {
        std::fprintf(stderr,
            "usage: cobra xtreg --data f.hbin --y Y --x X1,X2 --ivar ID [--keepif EXPR]\n"
            "                   [--be [--level P]]\n"
            "                   [--esample-out PATH [--esample-index COL --esample-n N]] [--out f.json]\n"
            "  (random-effects Swamy-Arora; FE absorbs time-invariant vars in the within step)\n"
            "  --be  between effects: OLS on the panel means (xtreg, be); --level P its t CIs.\n"
            "  --esample-out PATH  write e(sample), one 1/0 line per --data row (see feols).\n");
        return 2;
    }
    const bool be = has(a, "--be");
    double level = 95.0;
    if (be && has(a, "--level")) {
        level = std::stod(opt(a, "--level"));
        if (level < 10.0 || level > 99.99) {
            std::fprintf(stderr, "xtreg: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
            return 2;
        }
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
    // BUG-1269: Stata's `xtreg, re` refuses 0 rows r(2000) and 1 row r(2001).
    require_sample(static_cast<int64_t>(keep.size()));
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
    XtregReResult R;
    XtregBeResult B;
    if (be) B = xtreg_be(X.data(), n_obs, k, yv_v.data(), iv_v.data());
    else R = xtreg_re(X.data(), n_obs, k, yv_v.data(), iv_v.data());
    auto t1 = std::chrono::steady_clock::now();
    // xtreg, re starts from the within regression, which leaves out the regressors
    // without within-panel variation; its residual dof N - N_g - (time-varying k)
    // must be positive, else r(2001) "insufficient observations" (bank 8631427e:
    // one observation per panel).
    if (!be && !(R.df_e_within > 0))
        throw cecon::EngineError("no_obs", "insufficient observations", "r(2001)");
    if (be && !B.ok) {
        std::fprintf(stderr, "xtreg: the between regression produced no coefficients (collinear panel means, or no more panels than parameters).\n");
        return 1;
    }
    const std::vector<double>& beta = be ? B.beta : R.beta;
    const std::vector<double>& sev = be ? B.se : R.se;
    if (static_cast<int>(beta.size()) < k + 1 || static_cast<int>(sev.size()) < k + 1) {
        std::fprintf(stderr, "xtreg: estimation produced no coefficients (rank-deficient within design — collinear regressors, no within-panel variation, or too few observations).\n");
        return 1;
    }
    json coefs = json::array();
    // xtreg, be is a least-squares fit on N_g means: t statistics on df_r = N_g - k - 1.
    const double tc = be ? t_critical(1.0 - level / 100.0, B.df_r) : 0.0;
    for (int c = 0; c < k + 1; ++c) {
        std::string nm = (c < k) ? xs[static_cast<size_t>(c)] : std::string("_cons");
        const double t = tstat(beta[c], sev[c]);
        json row = {{"name", nm}, {"beta", beta[c]}, {"se", sev[c]}, {"t", t}};
        if (be) {
            row["p"] = t_twosided_pvalue(t, B.df_r);
            row["ci_lo"] = beta[c] - tc * sev[c];
            row["ci_hi"] = beta[c] + tc * sev[c];
        }
        coefs.push_back(std::move(row));
    }

    // --- model-fit scalars (Stata xtreg, re): three R²s + Wald χ² (since contract A 1.5.0) ---
    // R² are squared Pearson correlations between the slope-only linear predictor xb and y,
    // in the overall / between (group means) / within (group-demeaned) dimensions — the xtreg
    // convention (the intercept cancels in every correlation, hence slopes-only xb).
    std::vector<double> xb(static_cast<size_t>(n_obs), 0.0);
    for (int c = 0; c < k; ++c)
        for (int64_t i = 0; i < n_obs; ++i)
            xb[static_cast<size_t>(i)] += X[static_cast<int64_t>(c) * n_obs + i] * beta[c];
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
    // The panel sizes T_i over the estimation sample, Stata's e(g_min), e(g_avg),
    // e(g_max) after be and re (and e(Tcon) = whether they are all equal; BUG-1515,
    // BUG-1516).
    int64_t g_min = 0, g_max = 0;
    for (int g = 0; g < ng; ++g) {
        const int64_t n_g = g_n[static_cast<size_t>(g)];
        if (g == 0 || n_g < g_min) g_min = n_g;
        if (g == 0 || n_g > g_max) g_max = n_g;
    }
    const double g_avg = (ng > 0) ? static_cast<double>(n_obs) / static_cast<double>(ng) : 0.0;

    if (be) {
        // BUG-1515: xtreg, be posts the regression on the means
        // (one row per panel): e(r2_b) is
        // its R-squared, e(F) its model F, e(rmse) sqrt(rss / df_r); e(r2_w) and
        // e(r2_o) are the squared correlations computed above with the between b.
        // e(Tbar) is the mean panel size (not RE's harmonic mean).
        const double df_m = static_cast<double>(k);
        const double s2 = B.rss / B.df_r;
        const double F = (k > 0 && s2 > 0) ? (B.mss / df_m) / s2 : std::nan("");
        const double r2b = B.mss / (B.mss + B.rss);
        json out = {
            {"estimator", "xtreg_be"}, {"depvar", y},
            {"n_obs", B.n_obs}, {"n_groups", B.n_groups}, {"n_dropped_missing", n_dropped_missing},
            {"df_m", df_m}, {"df_r", B.df_r}, {"rss", B.rss}, {"mss", B.mss},
            {"rmse", std::sqrt(s2)}, {"r2", r2b},
            {"adj_r2", 1.0 - (1.0 - r2b) * (static_cast<double>(B.n_groups) - 1.0) / B.df_r},
            {"r2_within", r2_within}, {"r2_between", r2b},
            {"r2_overall", r2_overall},
            {"g_min", g_min}, {"g_avg", g_avg}, {"g_max", g_max},
            {"level", level},
            {"coefs", coefs},
            {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
        };
        if (std::isfinite(F)) {
            out["F"] = F;
            out["p_F"] = f_pvalue(F, df_m, B.df_r);
        } else {
            out["F"] = nullptr;
            out["p_F"] = nullptr;
        }
        if (esreq.active()) {
            EsampleRows rows;
            rows.keep = std::move(keep);
            rows.loaded = &ds;
            emit_esample(esreq, "xtreg", src_data, keepif, false, rows, out);
        }
        if (has(a, "--emit-v")) {
            std::vector<std::string> names = xs;
            names.push_back("_cons");
            add_e_v(out, names, B.vcov_matrix);
        }
        emit(out, opt(a, "--out"));
        return 0;
    }

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
        {"g_min", g_min}, {"g_avg", g_avg}, {"g_max", g_max},
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
    // --emit-v (BUG-1196): the FGLS covariance, slopes then `_cons`.
    if (has(a, "--emit-v")) {
        std::vector<std::string> names = xs;
        names.push_back("_cons");
        add_e_v(out, names, R.vcov_matrix);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_nbreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string off = opt(a, "--offset");
    // --exposure E: Stata's exposure(E), the offset ln(E) (E must be positive).
    const std::string expo = opt(a, "--exposure");
    if (!expo.empty()) {
        if (!off.empty())
            throw cecon::EngineError("bad_input", "nbreg: only one of offset() or exposure() can be specified");
        off = expo;
    }
    std::string cl  = opt(a, "--cluster");
    std::string vce = opt(a, "--vce", "iid");
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
            "usage: cobra nbreg --data f.hbin --y Y --x X1,X2 "
            "[--no-intercept] [--offset O] [--vce iid|robust|cluster] [--cluster C] [--level P] "
            "[--weight W --wtype fw|iw|pw] [--keepif EXPR] [--out f.json]\n"
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
    // Stata fweight / iweight / pweight (BUG-1537; nbreg's aweight is Stata's r(101)):
    // the weighted likelihood (nbreg.h), e(N) = Sum w under an fweight, and a pweight
    // forces the sandwich, as cmd_logit (StataBE 19.5, scratch-laneA2/agents/wml/x1.log:
    // `nbreg cnt x z [fw=wi]` e(N) 150, _se[x] .24856779361597398; [iw=wf] e(N) 60,
    // .27197794625746341; [pw=wf] vce robust, .4424215353885973).
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw cecon::EngineError("bad_input", "nbreg: --wtype requires --weight");
        if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else throw cecon::EngineError("bad_input", "nbreg: --wtype must be fw, iw or pw");
        if (wkind == cecon::WeightKind::Probability && vce == "iid")
            throw cecon::EngineError("bad_input",
                "nbreg: [pweight=] requires vce(robust) or vce(cluster ...); Stata has no "
                "iid VCE under pweight");
    }
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
    const double* wraw = weight_var.empty() ? nullptr : require_num(ds, "nbreg", weight_var);
    if (wraw) all.push_back(wraw);  // a missing weight drops the row
    auto keep = listwise_keep(all, n_raw);
    if (wraw) {
        // build_model's weight rules: a zero weight drops the row, a negative one or
        // a non-integer fweight is refused.
        std::vector<int64_t> kept;
        for (int64_t i : keep) {
            const double wv = wraw[i];
            if (wv < 0.0)
                throw cecon::EngineError("bad_input",
                    "weight '" + weight_var + "' has a negative value (" + std::to_string(wv) +
                    "); weights must be non-negative");
            if (wkind == cecon::WeightKind::Frequency && wv != std::floor(wv))
                throw cecon::EngineError("bad_input",
                    "fweight '" + weight_var + "' has a non-integer value (" + std::to_string(wv) +
                    "); Stata requires integer frequency weights");
            if (wv != 0.0) kept.push_back(i);
        }
        keep.swap(kept);
    }
    require_sample(static_cast<int64_t>(keep.size()));  // BUG-1269: r(2000) / r(2001)
    int64_t n_obs = static_cast<int64_t>(keep.size());
    int64_t n_dropped_missing = n_raw - n_obs;
    auto yv_v = gather_rows(yraw, keep);
    std::vector<double> ov_v; if (oraw) ov_v = gather_rows(oraw, keep);
    if (!expo.empty())
        for (double& v : ov_v) {
            if (!(v > 0.0)) throw cecon::EngineError("bad_input", "exposure() must be greater than zero");
            v = std::log(v);
        }
    std::vector<double> cv_v; if (craw) cv_v = gather_rows(craw, keep);
    std::vector<double> wv_v; if (wraw) wv_v = gather_rows(wraw, keep);
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
    // Collinear regressors are omitted, as Stata's nbreg does (the constant first,
    // then the regressors in order; the later member of a collinear set drops): bank
    // 673fe183 `nbreg rep78 mpg weight w2` with w2 = weight posts `o.w2` and fits
    // mpg/weight (e(ll) -114.45060708274941). The rank-deficient design used to come
    // back "converged" after one iteration with every slope 0 and the constant-only ll.
    // The fit runs on the kept columns; an omitted one reports b = se = 0 and zero rows
    // and columns of V.
    std::vector<char> keep_col(static_cast<size_t>(k), 1);
    {
        std::vector<int> order;
        if (intercept) order.push_back(kx);
        for (int c = 0; c < kx; ++c) order.push_back(c);
        std::vector<double> Xo;
        Xo.reserve(order.size() * static_cast<size_t>(n_obs));
        for (int c : order)
            Xo.insert(Xo.end(), X.begin() + static_cast<std::ptrdiff_t>(c) * n_obs,
                      X.begin() + static_cast<std::ptrdiff_t>(c + 1) * n_obs);
        const std::vector<char> km =
            cecon::collinear_keep_mask(Xo.data(), n_obs, static_cast<int>(order.size()));
        for (size_t j = 0; j < order.size(); ++j)
            if (!km[j]) keep_col[static_cast<size_t>(order[j])] = 0;
    }
    std::vector<int> fit_cols;
    for (int c = 0; c < k; ++c)
        if (keep_col[static_cast<size_t>(c)]) fit_cols.push_back(c);
    const int kf = static_cast<int>(fit_cols.size());
    std::vector<double> Xf;
    if (kf < k) {
        Xf.reserve(static_cast<size_t>(kf) * static_cast<size_t>(n_obs));
        for (int c : fit_cols)
            Xf.insert(Xf.end(), X.begin() + static_cast<std::ptrdiff_t>(c) * n_obs,
                      X.begin() + static_cast<std::ptrdiff_t>(c + 1) * n_obs);
    }
    auto t0 = std::chrono::steady_clock::now();
    auto R = nbreg(kf < k ? Xf.data() : X.data(), n_obs, kf, yv_v.data(),
                   ov_v.empty() ? nullptr : ov_v.data(),
                   cv_v.empty() ? nullptr : cv_v.data(),
                   has(a, "--iterate") ? std::stoi(opt(a, "--iterate")) : 100, 1e-10,
                   wv_v.empty() ? nullptr : wv_v.data(), wkind == cecon::WeightKind::Frequency,
                   // the trailing constant column (collinearity checks keep it first), or
                   // none under --no-intercept, whatever the regressors hold
                   intercept && keep_col[static_cast<size_t>(kx)] ? kf - 1 : -1);
    auto t1 = std::chrono::steady_clock::now();
    if (kf < k && static_cast<int>(R.beta.size()) >= kf && static_cast<int>(R.se_iid.size()) >= kf &&
        static_cast<int>(R.se_robust.size()) >= kf) {
        // Back onto the full design: omitted columns are structural zeros.
        auto widen = [&](const std::vector<double>& v) {
            std::vector<double> w(static_cast<size_t>(k), 0.0);
            for (int j = 0; j < kf; ++j) w[static_cast<size_t>(fit_cols[static_cast<size_t>(j)])] = v[static_cast<size_t>(j)];
            return w;
        };
        auto widenV = [&](const std::vector<double>& v) {
            if (v.size() != static_cast<size_t>(kf + 1) * static_cast<size_t>(kf + 1)) return v;
            std::vector<int> at(fit_cols);
            at.push_back(k);  // lnα stays last
            std::vector<double> w(static_cast<size_t>(k + 1) * static_cast<size_t>(k + 1), 0.0);
            for (int a2 = 0; a2 <= kf; ++a2)
                for (int b2 = 0; b2 <= kf; ++b2)
                    w[static_cast<size_t>(at[static_cast<size_t>(a2)]) * static_cast<size_t>(k + 1) +
                      static_cast<size_t>(at[static_cast<size_t>(b2)])] =
                        v[static_cast<size_t>(a2) * static_cast<size_t>(kf + 1) + static_cast<size_t>(b2)];
            return w;
        };
        R.beta = widen(R.beta);
        if (R.gradient.size() == static_cast<size_t>(kf + 1)) {
            const double glna = R.gradient.back();
            R.gradient.pop_back();
            R.gradient = widen(R.gradient);
            R.gradient.push_back(glna);
        }
        R.se_iid = widen(R.se_iid);
        R.se_robust = widen(R.se_robust);
        R.vcov_iid = widenV(R.vcov_iid);
        R.vcov_robust = widenV(R.vcov_robust);
        R.k = k;
    }
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
    // e(N) under an fweight is the observations the rows stand for.
    int64_t n_out = R.n_obs;
    if (wkind == cecon::WeightKind::Frequency) {
        double sw = 0;
        for (double wv : wv_v) sw += wv;
        n_out = static_cast<int64_t>(sw);
    }
    json out = {
        {"estimator", "nbreg"}, {"vce", vce},
        {"n_obs", n_out}, {"k", R.k}, {"n_dropped_missing", n_dropped_missing},
        {"alpha", R.alpha}, {"lnalpha", R.lnalpha},
        {"se_lnalpha_iid", R.se_lnalpha_iid},
        {"loglik", R.loglik}, {"iters", R.iters}, {"converged", R.converged},
        {"coefs", coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    out["level"] = level;  // always emitted, like Stata's e(level) (BUG-279)
    // e(sample)'s row count under an fweight / iweight, whose e(N) the handshake
    // cannot count rows from (bank 2669e294 `nbreg ... [fweight=w]`: 336 rows)
    if (wkind == cecon::WeightKind::Frequency || wkind == cecon::WeightKind::Importance)
        out["n_rows"] = R.n_obs;
    // --emit-v (BUG-1196): the fit's full covariance over (β, lnα), `/lnalpha` last
    // (StataBE 19.5 colfullnames e(b) `cnt:x1 cnt:x2 cnt:_cons /lnalpha`), the one
    // the reported SEs are read from. None at the α→0 boundary (lnα is not
    // estimated there).
    const std::vector<double>& Vfit = (vce == "iid") ? R.vcov_iid : R.vcov_robust;
    if (has(a, "--emit-v") && !Vfit.empty()) {
        std::vector<std::string> names;
        for (int c = 0; c < k; ++c)
            names.push_back((c < kx) ? xcols[static_cast<size_t>(c)].first : std::string("_cons"));
        names.push_back("/lnalpha");
        add_e_v(out, names, Vfit);
        // ml's record of the fit (none at the α→0 boundary): e(ic) and e(ilog) of the
        // full model, e(gradient) over (β, lnα), e(V_modelbased) under a sandwich VCE,
        // e(ll_0) of the constant-only model and e(ll_c) of the Poisson comparison.
        if (!R.boundary && !R.ilog.empty() && R.gradient.size() == names.size()) {
            out["ic"] = R.iters;
            out["ilog"] = R.ilog;
            out["e_gradient"] = {{"names", names}, {"values", R.gradient}};
            if (vce != "iid" && R.vcov_iid.size() == names.size() * names.size()) {
                json tmp;
                add_e_v(tmp, names, R.vcov_iid);
                out["e_Vmb"] = std::move(tmp["e_V"]);
            }
            if (R.has_ll_0) out["ll_0"] = R.ll_0;
            // The Poisson comparison (the LR test of alpha = 0) is a likelihood-ratio
            // test, so it exists only under the model-based VCE: vce(robust),
            // vce(cluster) and [pweight] (which this command runs as robust) post
            // no e(ll_c) / e(chi2_c).
            if (vce == "iid") out["ll_c"] = R.ll_c;
            if (!cv_v.empty()) {  // e(N_clust)
                std::vector<double> g(cv_v);
                std::sort(g.begin(), g.end());
                out["n_clusters"] = static_cast<int64_t>(std::unique(g.begin(), g.end()) - g.begin());
            }
        }
    }
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
            "[--keepif EXPR] [--vce iid|robust] [--out f.json]\n");
        return 2;
    }
    // BUG-1269: the fit's `if`, as xtpois reads it (PIET forwards it; this
    // command refused the flag, so `xtlogit ..., fe` with an `if` was rc 1).
    data = maybe_keepif(data, opt(a, "--keepif"));
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
    require_sample(static_cast<int64_t>(keep.size()));  // BUG-1269: r(2000) / r(2001)
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
        {"g_min", R.g_min}, {"g_avg", R.g_avg}, {"g_max", R.g_max},
        {"loglik", R.loglik}, {"iters", R.iters}, {"converged", R.converged},
        {"coefs", coefs},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    // ml's iteration log, the gradient at the estimates and the log likelihood at
    // beta = 0 (Stata's e(ilog), e(gradient), e(ll_0); vs-stata bank 1ce53089
    // `xtlogit hi beertax unemp, fe`: e(ll_0) -166.5701, iteration 0 -151.63393).
    out["ilog"] = R.ilog;
    out["gradient"] = R.gradient;
    out["ll_0"] = R.ll_0;
    out["n_dropped_obs"] = R.n_dropped_obs;  // e(N_drop): the rows of every dropped panel
    if (has(a, "--emit-v"))
        add_e_v(out, xk, vce == "robust" ? R.V_robust : R.V_iid);
    emit(out, opt(a, "--out"));
    return R.converged ? 0 : 1;
}

}  // namespace cli
}  // namespace cecon
