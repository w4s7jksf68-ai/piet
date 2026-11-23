// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/oglm.h"
#include "estimators/ml_constraints.h"

// P7 wave — ologit, oprobit (Stata `ologit` / `oprobit`).
//
// Both verbs are the SAME model with a different link, so they share run_oglm()
// below and differ only in the OrdLink they pass and the name they report.
//
// NO INTERCEPT. load_glm returns [expanded x…, intercept] with the constant LAST,
// so dropping it is a pure truncation (g.k -= 1, resize X, pop the name). It has to
// go: with a constant column in the design the information matrix is singular
// against the cutpoints (the model is identified only up to a common shift of xβ
// and every κ), and Stata reports the cutpoints instead of a constant for exactly
// that reason.

namespace cecon {
namespace cli {

namespace {

int run_oglm(const std::vector<std::string>& a, OrdLink link, const char* cmd) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    // --vce default "iid": opt() with a default leaves an omitted flag unconsumed,
    // so a plain invocation keeps the model-based (observed-information) SEs.
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra %s --data f.hbin --y Y --x X1,X2 [--vce iid|robust|cluster] "
                     "[--cluster C] [--level P] [--iterate N] [--keepif EXPR] [--out f.json]\n"
                     "  ordered %s: outcome recoded 0..J-1 by sorted distinct value;\n"
                     "  reports J-1 cutpoints (/cut1 …) instead of a constant.\n"
                     "  --vce cluster            needs --cluster C.\n",
                     cmd, link == OrdLink::Logit ? "logit" : "probit");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "%s: --vce must be one of iid|robust|cluster (got %s).\n", cmd, vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "%s: --vce cluster requires --cluster C.\n", cmd);
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "%s: --level must be in [10, 99.99] (got %s).\n", cmd,
                     opt(a, "--level").c_str());
        return 2;
    }
    // Asymptotic z inference throughout — Stata's ordered models report z, not t.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    // --weight / --wtype: fweight, iweight or pweight (ologit.ado / oprobit.ado
    // `[fw iw pw]`), each row's log-likelihood contribution scaled by its weight;
    // a pweight is fitted with the sandwich, Stata's rule. vs-stata bank aa1bb3ea /
    // 1198712b / e6cc710e `ologit rep78 mpg weight [fw|iw|pw=w]`: e(ll) -175.42837
    // under all three, e(N) the weight sum under fw.
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", std::string(cmd) + ": --wtype requires --weight");
        if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else throw EngineError("bad_input", std::string(cmd) + ": --wtype must be fw, iw or pw");
        if (wkind == cecon::WeightKind::Probability && vce == "iid")
            throw EngineError("bad_input", std::string(cmd) + ": [pweight=] requires vce(robust)");
        if (wkind != cecon::WeightKind::Probability && vce != "iid")
            throw EngineError("unsupported", std::string(cmd) + ": fweights and iweights are fitted "
                                             "with the default VCE only");
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    // --offset V: Stata's offset(), a term in the linear predictor with its
    // coefficient fixed at 1; the start is the same cutpoints-only point, the
    // model test Wald and no e(ll_0) (vs-stata bank 13941585 `ologit rep78 mpg
    // weight, offset(off)`: iteration 0 -118.81283, Wald chi2(2) 23.28).
    const std::string offset_var = opt(a, "--offset");
    GlmInputs g = load_glm(ds, cmd, y, split_csv(xcsv), cluster_names, weight_var, wkind, offset_var);
    const double* wt = g.weights.empty() ? nullptr : g.weights.data();
    const double* off = g.offset.empty() ? nullptr : g.offset.data();
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    // Drop the trailing intercept (see the file header). Pure truncation: it is the
    // LAST column of the col-major design, so the surviving bytes do not move.
    if (g.k > 0 && !g.names.empty() && g.names.back() == "(intercept)") {
        g.k -= 1;
        g.X.resize(static_cast<size_t>(g.n) * g.k);
        g.names.pop_back();
    }
    if (g.k == 0)
        throw EngineError("bad_input", std::string(cmd) + ": no covariates left after dropping the "
                                       "intercept; ordered models need at least one regressor.");
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            std::string(cmd) + ": vce(cluster) requires at least 2 clusters, but the cluster "
            "variable has " + std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    std::vector<int> cat;
    std::vector<double> cat_values = ord_categories(g.y.data(), g.n, cat);
    const int J = static_cast<int>(cat_values.size());
    if (J < 2)
        throw EngineError("bad_input",
            std::string(cmd) + ": outcome '" + y + "' takes fewer than 2 distinct values.");
    const int np = g.k + J - 1;

    // --iterate N: ml's iterate(#) (Stata default 300; 0 reports the start, the
    // cutpoints-only fit). The fit stops after N Newton iterations, converged or not.
    int max_iter = 300;
    if (has(a, "--iterate")) {
        const std::string it_s = opt(a, "--iterate");
        char* end = nullptr;
        const long v = std::strtol(it_s.c_str(), &end, 10);
        if (it_s.empty() || end == nullptr || *end != '\0' || v < 0 || v > 16000) {
            std::fprintf(stderr, "%s: --iterate must be an integer in [0, 16000] (got %s).\n", cmd,
                         it_s.c_str());
            return 2;
        }
        max_iter = static_cast<int>(v);
    }
    // --cns: constraints() (ml_constraints.h). Parameters are the slopes in
    // design order, then /cut1 ... /cut(J-1); every term names the first equation.
    // A row naming a coefficient the model does not have is dropped (r(111)), as
    // makecns drops a constraint that causes an error; cns_dropped reports it.
    MlCns cns;
    std::vector<std::vector<double>> cns_rows;  // the kept rows, [R | r] in θ order
    json cns_dropped = json::array();
    if (has(a, "--cns")) {
        const std::vector<CnsRowSpec> spec = parse_cns_spec(opt(a, "--cns"));
        std::vector<double> R, rr;
        for (size_t ri = 0; ri < spec.size(); ++ri) {
            const CnsRowSpec& row = spec[ri];
            std::vector<double> line(static_cast<size_t>(np), 0.0);
            bool bad = false;
            for (const auto& t : row.terms) {
                int p = -1;
                if (t.name.rfind("/cut", 0) == 0) {
                    const int j = std::atoi(t.name.c_str() + 4);
                    if (j >= 1 && j <= J - 1) p = g.k + j - 1;
                } else {
                    for (int c = 0; c < g.k; ++c)
                        if (g.names[static_cast<size_t>(c)] == t.name) p = c;
                }
                if (!(t.eq.empty() || t.eq == "#1") || p < 0) {
                    bad = true;
                    break;
                }
                line[static_cast<size_t>(p)] += t.coef;
            }
            if (bad) {
                cns_dropped.push_back({{"row", ri}, {"rc", 111}});
                continue;
            }
            R.insert(R.end(), line.begin(), line.end());
            rr.push_back(row.rhs);
        }
        if (!rr.empty() && !ml_cns_build(R, rr, static_cast<int>(rr.size()), np, cns))
            throw EngineError("bad_input", std::string(cmd) + ": the constraints are inconsistent");
        for (size_t i = 0; i < rr.size(); ++i) {
            if (std::find(cns.dropped.begin(), cns.dropped.end(), static_cast<int>(i)) != cns.dropped.end())
                continue;
            std::vector<double> line(R.begin() + static_cast<std::ptrdiff_t>(i) * np,
                                     R.begin() + static_cast<std::ptrdiff_t>(i + 1) * np);
            line.push_back(rr[i]);
            cns_rows.push_back(std::move(line));
        }
    }
    OglmResult r = oglm_fit(g.X.data(), g.n, g.k, cat.data(), J, link, max_iter, wt, off,
                            cns.active() ? &cns : nullptr);
    if (!r.ok)
        throw EngineError("no_convergence",
            std::string(cmd) + ": MLE failed (singular information matrix or non-convergent fit).");

    // V = bread (iid) or a sandwich bread·meat·bread over the per-observation score
    // rows. bread is oglm_fit's converged (−H)^{-1}; the score rows come back in the
    // SAME parameter order (slopes, then cutpoints), so no re-blocking is needed.
    //
    // SMALL-SAMPLE FACTOR. Stata's ML multiplier, NOT the regression-style
    // (N-1)/(N-K)·G/(G-1) that vcov.cpp applies to feols/2SLS:
    //   vce(robust)  → N/(N-1)
    //   vce(cluster) → G/(G-1)
    // Same P5 precedent as logit/probit/tobit (cmds_mle.cpp:284-297), and VERIFIED
    // here rather than assumed — tests/unit/test_oglm.cpp asserts every SE of the
    // P7 oracle `ologit yord x1 x2 i.region, vce(cluster id)` (200 clusters) and
    // `oprobit yord x1 x2 i.region, vce(robust)` (N=4,671) against these factors and
    // matches to <1e-5 relative; the regression-style factor would shift every
    // cluster SE by (N-1)/(N-K) ≈ 1.0008, an order of magnitude outside that.
    std::vector<double> V = r.vcov;
    int n_clusters = 0;
    if (vce == "robust" || vce == "cluster") {
        std::vector<double> S = oglm_scores(g.X.data(), g.n, g.k, cat.data(), J, link, r, wt, off);
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

    json coefs = json::array();
    for (int c = 0; c < g.k; ++c) {
        double b = r.beta[static_cast<size_t>(c)], se = sd(c);
        double z = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"b", b}, {"se", se}, {"z", z},
                         {"p", (se > 0) ? z_twosided_pvalue(z) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    // Cutpoints carry b/se/CI but NO z/p — Stata leaves those cells blank for
    // /cut1 /cut2 (a cutpoint's "= 0" null is not a hypothesis anyone poses).
    json cuts = json::array();
    for (int c = 0; c < J - 1; ++c) {
        double b = r.cuts[static_cast<size_t>(c)], se = sd(g.k + c);
        cuts.push_back({{"name", "/cut" + std::to_string(c + 1)}, {"b", b}, {"se", se},
                        {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    // Wald χ² over the SLOPES ONLY — the cutpoints are not part of the "all
    // coefficients are zero" null Stata reports (chi2(6) for 6 slopes + 2 cuts).
    double chi2 = 0;
    {
        std::vector<double> Vs(static_cast<size_t>(g.k) * g.k);
        for (int i = 0; i < g.k; ++i)
            for (int j = 0; j < g.k; ++j)
                Vs[static_cast<size_t>(i) * g.k + j] = V[static_cast<size_t>(i) * np + j];
        if (la::spd_inverse(Vs.data(), g.k) == 0)
            for (int i = 0; i < g.k; ++i)
                for (int j = 0; j < g.k; ++j)
                    chi2 += r.beta[static_cast<size_t>(i)] * Vs[static_cast<size_t>(i) * g.k + j] *
                            r.beta[static_cast<size_t>(j)];
    }

    // The model test Stata reports: with the default (observed-information) VCE it is
    // the likelihood-ratio statistic 2(ll - ll_0) against the cutpoints-only model
    // (bank d8de89e3 `ologit rep78 mpg weight`: LR chi2(2) 14.35758750211474 =
    // 2(-86.513267 + 93.692061)); under vce(robust)/vce(cluster) it is the Wald
    // statistic above.
    // constraints(): ml's model test is the Wald test of the slopes, a slope the
    // constraints fix dropping out of it (bank eae58c67 `ologit rep78 mpg weight,
    // const(1)` with mpg = -.1: Wald chi2(1) 25.02 = (b_weight / se_weight)^2).
    int df_chi2 = g.k;
    if (cns.active()) {
        std::vector<double> th = r.beta;
        th.insert(th.end(), r.cuts.begin(), r.cuts.end());
        std::vector<int> idx;
        for (int c = 0; c < g.k; ++c) idx.push_back(c);
        chi2 = ml_cns_wald(V, np, th, idx, &df_chi2);
    }
    const bool lr = (vce == "iid") && off == nullptr && !cns.active();
    if (lr) chi2 = 2.0 * (r.ll - r.ll_0);
    // e(N): the weight sum under fweights, the row count otherwise.
    int64_t n_obs = g.n;
    if (wkind == cecon::WeightKind::Frequency) {
        double sw = 0.0;
        for (double w : g.weights) sw += w;
        n_obs = static_cast<int64_t>(std::llround(sw));
    }
    json out = {{"estimator", cmd}, {"n_obs", n_obs}, {"n_dropped_missing", g.n_dropped},
                {"n_cats", J}, {"cat_values", cat_values}, {"vce", vce},
                {"coefs", coefs}, {"cuts", cuts},
                {"ll", r.ll}, {"ll_0", r.ll_0},
                {"r2_pseudo", (r.ll_0 != 0) ? 1.0 - r.ll / r.ll_0 : 0.0},
                {"chi2", chi2}, {"df_chi2", df_chi2}, {"p_chi2", chi2_pvalue(chi2, df_chi2)},
                {"chi2type", lr ? "LR" : "Wald"},
                {"iters", r.iters}, {"converged", r.converged}};
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    if (wkind != cecon::WeightKind::None) out["n_rows"] = g.n;  // the e(sample) handshake's rows
    // Under offset() there is no constant-only comparison model: no e(ll_0) / e(r2_p).
    if (off != nullptr) {
        out.erase("ll_0");
        out.erase("r2_pseudo");
        out["offset"] = offset_var;
    }
    // Under constraints() there is no constant-only comparison model either; the
    // kept constraint rows [R | r] (e(Cns)) and the coefficients they fix.
    if (cns.active()) {
        out.erase("ll_0");
        out.erase("r2_pseudo");
        out["cns"] = cns_rows;
        out["rank"] = cns.nfree;
        for (auto& c : out["coefs"])
            if (c["se"].get<double>() == 0.0) c["constrained"] = true;
        for (auto& c : out["cuts"])
            if (c["se"].get<double>() == 0.0) c["constrained"] = true;
    }
    if (!cns_dropped.empty()) out["cns_dropped"] = cns_dropped;
    add_warnings(out, g.warnings);
    // --emit-v (BUG-1196): the fit's V, slopes then cutpoints -- Stata's e(V)
    // order and metric (StataBE 19.5 colfullnames e(b) `yo:x1 yo:x2 /cut1 /cut2`).
    if (has(a, "--emit-v")) {
        std::vector<std::string> names = g.names;
        for (int c = 0; c < J - 1; ++c) names.push_back("/cut" + std::to_string(c + 1));
        add_e_v(out, names, V);
        // e(V_modelbased): under vce(robust)/vce(cluster) Stata also posts the
        // observed-information variance the sandwich was built on.
        if (vce != "iid") {
            json tmp;
            add_e_v(tmp, names, r.vcov);
            out["e_Vmb"] = std::move(tmp["e_V"]);
        }
    }
    // ml's iteration log (iteration 0 is the cutpoints-only start, e(ll_0)) and the
    // gradient at the reported estimates, slopes then cutpoints (e(ilog), e(gradient)).
    out["ilog"] = r.ilog;
    out["gradient"] = r.gradient;
    // The largest fitted probability of an observation's own outcome at the
    // estimates: an observation whose outcome the model predicts with certainty is
    // "completely determined" (e(N_cd)); below 1 - 1e-6 no observation can be.
    {
        std::vector<double> theta = r.beta;
        theta.insert(theta.end(), r.cuts.begin(), r.cuts.end());
        double pmax = 0.0;
        for (int64_t i = 0; i < g.n; ++i) {
            double xb = 0.0;
            for (int c = 0; c < g.k; ++c)
                xb += g.X[static_cast<size_t>(c) * static_cast<size_t>(g.n) + static_cast<size_t>(i)] *
                      r.beta[static_cast<size_t>(c)];
            if (off) xb += off[static_cast<size_t>(i)];
            const int j = cat[static_cast<size_t>(i)];
            const auto F = [&](double t) {
                return link == OrdLink::Logit ? ogl::lcdf(t) : ogl::ncdf(t);
            };
            const double hi = (j == J - 1) ? 1.0 : F(r.cuts[static_cast<size_t>(j)] - xb);
            const double lo = (j == 0) ? 0.0 : F(r.cuts[static_cast<size_t>(j - 1)] - xb);
            pmax = std::max(pmax, hi - lo);
        }
        out["max_p_own"] = pmax;
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace

int cmd_ologit(const std::vector<std::string>& a) { return run_oglm(a, OrdLink::Logit, "ologit"); }

int cmd_oprobit(const std::vector<std::string>& a) { return run_oglm(a, OrdLink::Probit, "oprobit"); }

}  // namespace cli
}  // namespace cecon
