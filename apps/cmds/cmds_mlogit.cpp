// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/mlogit.h"
#include "estimators/ml_constraints.h"
#include "estimators/oglm.h"  // ord_categories — the outcome-recoding rule (sorted
                              // distinct values -> 0..K-1) is shared with the ordered
                              // models; one definition, not two spellings of it.

// P7 wave — mlogit (Stata `mlogit`).
//
// The design KEEPS load_glm's trailing intercept: every non-base equation has its
// own constant, exactly as Stata prints one _cons per equation. θ is those
// equations stacked (equation-major, non-base outcomes ascending, load_glm's column
// order within an equation) and the score blocks are emitted in the same order, so
// the sandwich meat lines up with the bread block-for-block.
//
// The base outcome is chosen BY VALUE via --base; with the flag absent it is the
// most frequent outcome, which is Stata's own default.

namespace cecon {
namespace cli {

int cmd_mlogit(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra mlogit --data f.hbin --y Y --x X1,X2 [--base V] "
                     "[--vce iid|robust|cluster] [--cluster C] [--level P] [--iterate N] [--keepif EXPR] "
                     "[--out f.json]\n"
                     "  multinomial logit; one equation per non-base outcome.\n"
                     "  --base V                 base outcome BY VALUE (default: most frequent,\n"
                     "                           matching Stata's baseoutcome() default).\n"
                     "  --vce cluster            needs --cluster C.\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "mlogit: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "mlogit: --vce cluster requires --cluster C.\n");
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "mlogit: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    bool base_set = has(a, "--base");
    double base_value = 0;
    if (base_set) {
        try {
            base_value = std::stod(opt(a, "--base"));
        } catch (const std::exception&) {
            throw EngineError("bad_input", "mlogit: --base must be a numeric outcome VALUE (got '" +
                                           opt(a, "--base") + "')");
        }
    }

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    // --weight / --wtype: fweight, iweight or pweight (mlogit.ado `[fw iw pw]`),
    // each row's contribution scaled by its weight; a pweight takes the sandwich.
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", "mlogit: --wtype requires --weight");
        if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else throw EngineError("bad_input", "mlogit: --wtype must be fw, iw or pw");
        if (wkind == cecon::WeightKind::Probability && vce == "iid")
            throw EngineError("bad_input", "mlogit: [pweight=] requires vce(robust)");
        if (wkind != cecon::WeightKind::Probability && vce != "iid")
            throw EngineError("unsupported", "mlogit: fweights and iweights are fitted with the default VCE only");
    }
    GlmInputs g = load_glm(ds, "mlogit", y, split_csv(xcsv), cluster_names, weight_var, wkind);
    const double* wt = g.weights.empty() ? nullptr : g.weights.data();
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "mlogit: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    // Outcomes recoded 0..K-1 by sorted distinct value (ord_categories, shared with
    // the ordered models — the recoding rule is identical, only the model differs).
    std::vector<int> cat;
    std::vector<double> cat_values = ord_categories(g.y.data(), g.n, cat);
    const int K = static_cast<int>(cat_values.size());
    if (K < 2)
        throw EngineError("bad_input",
            "mlogit: outcome '" + y + "' takes fewer than 2 distinct values.");
    // Outcome frequencies for the default base. An fweight row stands for w
    // replicated observations, so it counts w times (the fit equals the expanded
    // data's); other weights count rows.
    std::vector<int64_t> counts(static_cast<size_t>(K), 0);
    std::vector<double> freq(static_cast<size_t>(K), 0.0);
    for (int64_t i = 0; i < g.n; ++i) {
        const size_t m = static_cast<size_t>(cat[static_cast<size_t>(i)]);
        ++counts[m];
        freq[m] += (wkind == cecon::WeightKind::Frequency && wt) ? wt[i] : 1.0;
    }
    int base = 0;
    if (base_set) {
        base = -1;
        for (int m = 0; m < K; ++m)
            if (cat_values[static_cast<size_t>(m)] == base_value) { base = m; break; }
        if (base < 0)
            throw EngineError("bad_input", "mlogit: --base value " + opt(a, "--base") +
                                           " does not occur in outcome '" + y + "'");
    } else {
        // Stata's default baseoutcome() is the MOST FREQUENT outcome; ties go to the
        // lower value (the scan keeps the first strict maximum).
        for (int m = 1; m < K; ++m)
            if (freq[static_cast<size_t>(m)] > freq[static_cast<size_t>(base)]) base = m;
    }
    const int neq = K - 1, np = neq * g.k;

    // --iterate N: ml's iterate(#) (default 300; 0 reports the constants-only start).
    int max_iter = 300;
    if (has(a, "--iterate")) {
        const std::string it_s = opt(a, "--iterate");
        char* end = nullptr;
        const long v = std::strtol(it_s.c_str(), &end, 10);
        if (it_s.empty() || end == nullptr || *end != '\0' || v < 0 || v > 16000) {
            std::fprintf(stderr, "mlogit: --iterate must be an integer in [0, 16000] (got %s).\n",
                         it_s.c_str());
            return 2;
        }
        max_iter = static_cast<int>(v);
    }
    int cons_col = -1;
    for (int c = 0; c < g.k; ++c)
        if (g.names[static_cast<size_t>(c)] == "(intercept)") cons_col = c;
    // --cns: constraints() (ml_constraints.h). A term's EQ names an outcome by
    // its value or its position among the sorted outcomes; the base outcome's coefficients are 0 by
    // construction, so a term on them drops out (mlogit's own handling of the base
    // equation), and a row left with no term holds only when its right side is 0.
    // A row naming an equation the model does not have (r(303)) or a coefficient
    // it does not have (r(111)) is dropped, as makecns drops a constraint that
    // causes an error; cns_dropped reports each such row and its return code.
    MlCns cns;
    std::vector<std::vector<double>> cns_rows;  // the kept rows, [R | r] in θ order
    json cns_dropped = json::array();
    if (has(a, "--cns")) {
        const std::vector<CnsRowSpec> spec = parse_cns_spec(opt(a, "--cns"));
        std::vector<double> R, rr;
        for (size_t ri = 0; ri < spec.size(); ++ri) {
            const CnsRowSpec& row = spec[ri];
            std::vector<double> line(static_cast<size_t>(np), 0.0);
            bool any = false;
            int bad = 0;
            for (const auto& t : row.terms) {
                int c = -1;
                const std::string nm = t.name == "_cons" ? std::string("(intercept)") : t.name;
                for (int j = 0; j < g.k; ++j)
                    if (g.names[static_cast<size_t>(j)] == nm) c = j;
                int m = -1;  // the outcome's position
                if (t.eq.empty()) {
                    m = 0;
                } else if (t.eq[0] == '#') {
                    m = std::atoi(t.eq.c_str() + 1) - 1;
                } else if (t.eq[0] == '=') {
                    const double v = std::strtod(t.eq.c_str() + 1, nullptr);
                    for (int j = 0; j < K; ++j)
                        if (cat_values[static_cast<size_t>(j)] == v) m = j;
                }
                if (m < 0 || m >= K) {
                    bad = 303;
                    break;
                }
                if (c < 0) {
                    bad = 111;
                    break;
                }
                if (m == base) continue;
                const int e = (m < base) ? m : m - 1;
                line[static_cast<size_t>(e) * g.k + c] += t.coef;
                any = true;
            }
            if (bad != 0) {
                cns_dropped.push_back({{"row", ri}, {"rc", bad}});
                continue;
            }
            if (!any) {
                if (row.rhs != 0.0) throw EngineError("bad_input", "mlogit: the constraints are inconsistent");
                continue;
            }
            R.insert(R.end(), line.begin(), line.end());
            rr.push_back(row.rhs);
        }
        if (!rr.empty() && !ml_cns_build(R, rr, static_cast<int>(rr.size()), np, cns))
            throw EngineError("bad_input", "mlogit: the constraints are inconsistent");
        for (size_t i = 0; i < rr.size(); ++i) {
            if (std::find(cns.dropped.begin(), cns.dropped.end(), static_cast<int>(i)) != cns.dropped.end())
                continue;
            std::vector<double> line(R.begin() + static_cast<std::ptrdiff_t>(i) * np,
                                     R.begin() + static_cast<std::ptrdiff_t>(i + 1) * np);
            line.push_back(rr[i]);
            cns_rows.push_back(std::move(line));
        }
    }
    MlogitResult r = mlogit_fit(g.X.data(), g.n, g.k, cat.data(), K, base, cons_col, max_iter, wt,
                                cns.active() ? &cns : nullptr);
    if (!r.ok)
        throw EngineError("no_convergence",
            "mlogit: MLE failed (singular information matrix or non-convergent fit).");

    // V = bread (iid) or a sandwich bread·meat·bread over the per-observation score
    // rows, whose block order matches r.vcov's exactly.
    //
    // SMALL-SAMPLE FACTOR. Stata's ML multiplier, NOT the regression-style
    // (N-1)/(N-K)·G/(G-1):
    //   vce(robust)  → N/(N-1)
    //   vce(cluster) → G/(G-1)
    // Same P5 precedent as logit/probit/tobit (cmds_mle.cpp:284-297), VERIFIED not
    // assumed — tests/unit/test_mlogit.cpp asserts all 14 standard errors of the P7
    // oracle `mlogit ynom x1 x2 i.region, baseoutcome(0) vce(cluster id)` (200
    // clusters, N=4,671) against G/(G-1) and reproduces every printed digit; the
    // regression-style factor would add (N-1)/(N-K) ≈ 1.0014 to each.
    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S = mlogit_scores(g.X.data(), g.n, g.k, cat.data(), r, wt);
        int64_t rows = g.n;
        const Factor* fac = nullptr;
        double factor;
        if (vce == "cluster") {
            fac = &g.cluster[0];
            n_clusters = fac->n_levels;
            rows = n_clusters;
            factor = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
        } else {
            factor = g.n > 1 ? static_cast<double>(g.n) / (static_cast<double>(g.n) - 1.0) : 1.0;
        }
        std::vector<double> M(static_cast<size_t>(rows) * np, 0.0);
        for (int64_t i = 0; i < g.n; ++i) {
            int64_t rr = fac ? fac->codes[static_cast<size_t>(i)] : i;
            for (int p = 0; p < np; ++p)
                M[static_cast<int64_t>(p) * rows + rr] += S[static_cast<int64_t>(p) * g.n + i];
        }
        std::vector<double> meat(static_cast<size_t>(np) * np, 0.0);
        la::xtx(M.data(), rows, np, meat.data());
        std::vector<double> tmp(static_cast<size_t>(np) * np, 0.0);
        V.assign(static_cast<size_t>(np) * np, 0.0);
        la::sandwich(r.vcov.data(), meat.data(), np, factor, V.data(), tmp.data());
    }
    auto sd = [&](int p) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * np + p])); };

    // equations[]: one entry per outcome IN OUTCOME ORDER. The base outcome carries
    // {outcome, base:true} and no coefs — rendering "(base outcome)" is PIET's job;
    // the engine emits the stub so the equation list is complete and ordered.
    json equations = json::array();
    for (int m = 0; m < K; ++m) {
        json eq = {{"outcome", cat_values[static_cast<size_t>(m)]},
                   {"n_obs_outcome", counts[static_cast<size_t>(m)]}};
        if (m == base) {
            eq["base"] = true;
            equations.push_back(eq);
            continue;
        }
        const int e = (m < base) ? m : m - 1;  // equation index (base excluded)
        json coefs = json::array();
        for (int c = 0; c < g.k; ++c) {
            const int p = e * g.k + c;
            double b = r.beta[static_cast<size_t>(p)], se = sd(p);
            double z = (se > 0) ? b / se : 0.0;
            coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                             {"p", (se > 0) ? z_twosided_pvalue(z) : 1.0},
                             {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
        }
        eq["base"] = false;
        eq["coefs"] = coefs;
        equations.push_back(eq);
    }

    // Wald χ² over ALL slopes of ALL equations — every equation's constant excluded
    // (load_glm puts it last in each block), so df = (K-1)·(k-1): Stata's chi2(12)
    // for 2 equations × 6 slopes here.
    double chi2 = 0;
    int df = 0;
    {
        std::vector<int> idx;
        for (int e = 0; e < neq; ++e)
            for (int c = 0; c < g.k; ++c)
                if (g.names[static_cast<size_t>(c)] != "(intercept)") idx.push_back(e * g.k + c);
        df = static_cast<int>(idx.size());
        if (df > 0) {
            std::vector<double> Vs(static_cast<size_t>(df) * df);
            for (int i = 0; i < df; ++i)
                for (int j = 0; j < df; ++j)
                    Vs[static_cast<size_t>(i) * df + j] =
                        V[static_cast<size_t>(idx[static_cast<size_t>(i)]) * np + idx[static_cast<size_t>(j)]];
            if (la::spd_inverse(Vs.data(), df) == 0)
                for (int i = 0; i < df; ++i)
                    for (int j = 0; j < df; ++j)
                        chi2 += r.beta[static_cast<size_t>(idx[static_cast<size_t>(i)])] *
                                Vs[static_cast<size_t>(i) * df + j] *
                                r.beta[static_cast<size_t>(idx[static_cast<size_t>(j)])];
        }
    }

    // With the default (observed-information) VCE Stata's model test is the
    // likelihood-ratio statistic 2(ll - ll_0) against the constants-only model
    // (bank e9b3681e `mlogit race age grade wage`: LR chi2(6) 70.59563857956209 =
    // 2(-1383.0285584 + 1418.3263777)); under vce(robust)/vce(cluster) it is the
    // Wald statistic above.
    // constraints(): the Wald test, a slope the constraints fix dropping out of it
    // (bank 908e6dce `mlogit race age grade wage, c(1)` with [2]age = 0: Wald chi2(5)).
    if (cns.active()) {
        std::vector<int> idx;
        for (int e = 0; e < neq; ++e)
            for (int c = 0; c < g.k; ++c)
                if (g.names[static_cast<size_t>(c)] != "(intercept)") idx.push_back(e * g.k + c);
        chi2 = ml_cns_wald(V, np, r.beta, idx, &df);
    }
    const bool lr = (vce == "iid") && !cns.active();
    if (lr && df > 0) chi2 = 2.0 * (r.ll - r.ll_0);
    int64_t n_obs = g.n;  // e(N): the weight sum under fweights
    if (wkind == cecon::WeightKind::Frequency) {
        double sw = 0.0;
        for (double w : g.weights) sw += w;
        n_obs = static_cast<int64_t>(std::llround(sw));
    }
    json out = {{"estimator", "mlogit"}, {"n_obs", n_obs}, {"n_dropped_missing", g.n_dropped},
                {"n_cats", K}, {"cat_values", cat_values},
                {"base_outcome", cat_values[static_cast<size_t>(base)]},
                {"vce", vce}, {"equations", equations},
                {"ll", r.ll}, {"ll_0", r.ll_0},
                {"r2_pseudo", (r.ll_0 != 0) ? 1.0 - r.ll / r.ll_0 : 0.0},
                {"chi2", chi2}, {"df_chi2", df}, {"p_chi2", chi2_pvalue(chi2, df)},
                {"chi2type", lr ? "LR" : "Wald"},
                {"iters", r.iters}, {"converged", r.converged}};
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (wkind != cecon::WeightKind::None) out["n_rows"] = g.n;  // the e(sample) handshake's rows
    if (level_set) out["level"] = level;
    // Under constraints() there is no constants-only comparison model; the kept
    // constraint rows [R | r] in θ's order (e(Cns)) and the coefficients they fix.
    if (cns.active()) {
        out.erase("ll_0");
        out.erase("r2_pseudo");
        out["cns"] = cns_rows;
        out["rank"] = cns.nfree;
        for (auto& eq : out["equations"])
            if (eq.contains("coefs"))
                for (auto& c : eq["coefs"])
                    if (c["se"].get<double>() == 0.0) c["constrained"] = true;
    }
    if (!cns_dropped.empty()) out["cns_dropped"] = cns_dropped;
    add_warnings(out, g.warnings);
    // --emit-v (BUG-1196): the fit's V over the non-base equations in θ's order,
    // each name `<outcome value>:<coefficient>`. Stata's e(V) also carries the
    // base outcome's equation as zero rows/columns (StataBE 19.5 colfullnames
    // e(b) `1:o.x1 1:o.x2 1:o._cons 2:x1 ...`); that stripe is PIET's to build.
    if (has(a, "--emit-v")) {
        std::vector<std::string> names;
        for (int m = 0; m < K; ++m) {
            if (m == base) continue;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", cat_values[static_cast<size_t>(m)]);
            for (int c = 0; c < g.k; ++c) names.push_back(std::string(buf) + ":" + g.names[static_cast<size_t>(c)]);
        }
        add_e_v(out, names, V);
        // e(V_modelbased): the observed-information variance under a sandwich.
        if (vce != "iid") {
            json tmp;
            add_e_v(tmp, names, r.vcov);
            out["e_Vmb"] = std::move(tmp["e_V"]);
        }
    }
    // ml's iteration log (iteration 0 is the constants-only start, e(ll_0)) and the
    // gradient at the estimates in θ's order (e(ilog), e(gradient)).
    out["ilog"] = r.ilog;
    out["gradient"] = r.gradient;
    // The largest fitted probability of an observation's own outcome: below
    // 1 - 1e-6 no observation is completely determined (e(N_cd) 0).
    {
        const std::vector<int> eq_of_cat = mlogit_eq_of_cat(r);
        double pmax = 0.0;
        std::vector<double> eta(static_cast<size_t>(K), 0.0);
        for (int64_t i = 0; i < g.n; ++i) {
            double mx = 0.0;
            for (int m = 0; m < K; ++m) {
                const int e = eq_of_cat[static_cast<size_t>(m)];
                double v = 0.0;
                if (e >= 0)
                    for (int c = 0; c < g.k; ++c)
                        v += g.X[static_cast<size_t>(c) * static_cast<size_t>(g.n) + static_cast<size_t>(i)] *
                             r.beta[static_cast<size_t>(e) * g.k + c];
                eta[static_cast<size_t>(m)] = v;
                mx = std::max(mx, v);
            }
            double den = 0.0;
            for (int m = 0; m < K; ++m) den += std::exp(eta[static_cast<size_t>(m)] - mx);
            const int own = cat[static_cast<size_t>(i)];
            pmax = std::max(pmax, std::exp(eta[static_cast<size_t>(own)] - mx) / den);
        }
        out["max_p_own"] = pmax;
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
