// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"
#include "estimators/nnmatch.h"
#include "core/distributions.h"
#include <set>

namespace cecon::cli {

// `cobra nnmatch` — the engine half of Stata's `teffects nnmatch (y mvarlist)
// (tvar)`. Design and every measured rule: src/estimators/nnmatch.{h,cpp} and
// internal capture.
//
// Factor terms arrive BY NAME through --fv and are expanded inside the kernel,
// never as columns minted into the caller's data: Stata leaves no `_I*` behind
// after `teffects nnmatch (y i.a) (t)`, and the ids this command returns are
// row numbers of the caller's frame, so the frame must not move underneath it.
int cmd_nnmatch(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), tvar = opt(a, "--t");
    auto xs = split_csv(opt(a, "--x"));
    auto fvs = split_csv(opt(a, "--fv"));
    if (data_missing(data) || y.empty() || tvar.empty() || (xs.empty() && fvs.empty())) {
        std::fprintf(stderr,
            "usage: cobra nnmatch --data f.hbin --y Y --t T [--x a,b] [--fv f1,f2]\n"
            "                     [--nn K] [--metric mahalanobis|euclidean|ivariance]\n"
            "                     [--stat ate|atet] [--vce-nn H] [--level L] [--keepif EXPR]\n"
            "                     [--out f.json]\n"
            "  Abadie-Imbens nearest-neighbour matching ([CAUSAL] teffects nnmatch).\n");
        return 2;
    }
    NnmatchOpts o;
    const std::string nnOpt = opt(a, "--nn");
    if (!nnOpt.empty()) o.nneighbor = std::stoi(nnOpt);
    const std::string vnOpt = opt(a, "--vce-nn");
    if (!vnOpt.empty()) o.vce_nn = std::stoi(vnOpt);
    const std::string mOpt = opt(a, "--metric");
    if (!mOpt.empty()) o.metric = mOpt;
    // Engine-only: `dtolerance(#)` is measured ONLY in the refuters' degenerate
    // near-tie fixture (refute_B3 R2/R3), where e(V) itself is a named
    // divergence, so the PIET surface refuses the option by name. The flag
    // exists here because check_engine.py's H2 arms need it.
    const std::string dtOpt = opt(a, "--dtolerance");
    if (!dtOpt.empty()) {
        o.dtolerance = std::stod(dtOpt);
        if (!(o.dtolerance > 0))
            throw EngineError("bad_input", "nnmatch: dtolerance(#) must be a real greater than "
                                           "zero", "r(198)");
    }
    // `level(#)` is a REPORTING option: it moves ci_lo/ci_hi and nothing else.
    // The `== 95.0 ? 0.05` spelling below is the house form (cmds_mle.cpp:83,
    // cmds_panel.cpp:225, ...): `1.0 - 95/100` is one ulp off the literal 0.05
    // that z_critical short-circuits on, and that ulp is visible in a freeze.
    const std::string lvOpt = opt(a, "--level");
    const double level = lvOpt.empty() ? 95.0 : std::stod(lvOpt);
    if (!(level > 0 && level < 100))
        throw EngineError("bad_input", "nnmatch: --level must be between 0 and 100", "r(198)");

    const std::string sOpt = opt(a, "--stat");
    if (!sOpt.empty()) {
        if (sOpt != "ate" && sOpt != "atet")
            throw EngineError("bad_input", "nnmatch: --stat must be ate or atet", "r(198)");
        o.atet = (sOpt == "atet");
    }

    // NOT maybe_keepif: the gen() ids Stata writes are _n of the FULL data even
    // under `if` (measured, INFO|nn_edge|ifodd_* — an odd-rows-only subsample
    // returns odd ids), so the row filter has to stay a MASK over the original
    // frame instead of a rewritten one.
    const Dataset& ds = acquire_data(data);
    const std::string kif = opt(a, "--keepif");
    std::vector<char> keep(static_cast<std::size_t>(ds.n_rows()), 1);
    if (!kif.empty()) keep = eval_keepif_mask(data, kif);

    const double* yy = require_num(ds, "nnmatch", y);
    const double* tv = require_num(ds, "nnmatch", tvar);
    // NO duplicate-name refusal here. `teffects nnmatch (y x1 x1) (t)` was an
    // r(198) "duplicate matching variable x1" in the first landing -- an
    // INVENTION, refuted by measurement (StataBE 19.5, 2026-09-09, 200-row
    // fixture): Stata answers `note: x1 omitted because of collinearity.` and
    // an ATE of 1.47459772296015e-01, SE 5.11611461378533e-02, k_nnmin 3,
    // k_nnmax 6. The kernel's own left-to-right _rmcoll sweep (nnmatch.cpp,
    // "Modified Gram-Schmidt on the CENTRED columns") already implements
    // exactly that answer and NAMES what it dropped, so the guard was
    // preempting the machinery that was right. A repeated column is just a
    // collinear design.
    std::vector<const double*> xc, fc;
    for (const auto& v : xs) {
        if (v == y || v == tvar)
            throw EngineError("bad_input",
                              "nnmatch: " + v + " may not appear both as an outcome/treatment and "
                              "as a matching variable", "r(198)");
        xc.push_back(require_num(ds, "nnmatch", v));
    }
    for (const auto& v : fvs) {
        if (v == y || v == tvar)
            throw EngineError("bad_input",
                              "nnmatch: " + v + " may not appear both as an outcome/treatment and "
                              "as a matching variable", "r(198)");
        fc.push_back(require_num(ds, "nnmatch", v));
    }

    NnmatchResult R = nnmatch_fit(yy, tv, xs, xc, fvs, fc, keep, o);

    const std::string eq = (o.atet ? "ATET:r" : "ATE:r");
    char lv[64];
    std::snprintf(lv, sizeof(lv), "%lldvs%lld", static_cast<long long>(R.treated_level),
                  static_cast<long long>(R.control_level));
    const std::string bname = eq + lv + "." + tvar;

    const double se = std::sqrt(std::max(0.0, R.V));
    json ct = json::array();
    {
        // z_critical's contract is P(|Z| > z*) == alpha (core/distributions.h:97),
        // i.e. it takes the WHOLE two-sided tail -- 0.05 for a 95% interval, not
        // 0.025.  Measured against StataBE on the golden fixture: the half-tail
        // spelling returned [-.10513, .15631] where Stata prints
        // [-.0887182, .1399005] (18.5% / 11.7% out), because it is a 97.5%
        // interval wearing a 95% label.
        const double z = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
        json tstat = se > 0 ? json(R.tau / se) : json(nullptr);
        json pv = se > 0 ? json(std::erfc(std::fabs(R.tau / se) / std::sqrt(2.))) : json(nullptr);
        ct.push_back({{"name", bname}, {"b", R.tau}, {"se", se}, {"t", tstat}, {"p", pv},
                      {"ci_lo", R.tau - z * se}, {"ci_hi", R.tau + z * se}});
    }

    // Full-frame arrays: a row the `if` (or listwise deletion) excluded gets an
    // EMPTY id list and a null te/po — never 0, never a silently borrowed value.
    const int64_t nAll = ds.n_rows();
    json ids = json::array(), dists = json::array(), te = json::array(), po0 = json::array(),
         po1 = json::array();
    for (int64_t i = 0; i < nAll; ++i) {
        ids.push_back(json::array());
        dists.push_back(json::array());
        te.push_back(nullptr); po0.push_back(nullptr); po1.push_back(nullptr);
    }
    for (std::size_t k = 0; k < R.rows.size(); ++k) {
        const int64_t r = R.rows[k];
        ids[r] = R.omega[k];
        dists[r] = R.dist[k];
        te[r] = R.te[k]; po0[r] = R.po0[k]; po1[r] = R.po1[k];
    }

    json o_ = {{"estimator", "nnmatch"},
               {"stat", o.atet ? "atet" : "ate"},
               {"metric", o.metric},
               {"depvar", y},
               {"tvar", tvar},
               {"n_obs", R.n}, {"n0", R.n0}, {"n1", R.n1},
               {"control_level", R.control_level}, {"treated_level", R.treated_level},
               {"k_nneighbor", o.nneighbor}, {"k_nnmin", R.k_nnmin}, {"k_nnmax", R.k_nnmax},
               {"k_robust", o.vce_nn}, {"dtolerance", o.dtolerance},
               // NOT "level": PIET resolves `e(<name>)` straight out of this
               // object by key (eval/macros.cpp resolveEScalar), and StataBE
               // posts NO e(level) for `teffects nnmatch` -- `display e(level)`
               // answers `.` even after `level(90)`
               // (aer_estim2_oracle.log INFO|nn_level|l90_e_level), and the
               // full ereturn capture of all 14 oracle fixtures
               // (teffects-nnmatch-20260908/oracle.txt SCALARS) has no `level`.
               // The interval level still belongs in the engine's own report.
               {"ci_level", level},
               {"vcov_type", "robust"},
               {"colnames", R.colnames}, {"dropped", R.dropped}};
    o_["coeftable"] = ct;
    o_["e_b"] = {{"names", json::array({bname})}, {"values", json::array({R.tau})}};
    o_["e_V"] = {{"names", json::array({bname})}, {"matrix", json::array({json::array({R.V})})}};
    o_["matches"] = {{"k_max", R.k_nnmax}, {"ids", ids}};
    o_["dist"] = dists;
    o_["te"] = te; o_["po0"] = po0; o_["po1"] = po1;
    json mstats = {{"names", R.colnames}, {"mean", R.mean}};
    if (!R.sd.empty()) mstats["sd"] = R.sd;
    o_["mstats"] = mstats;
    if (!R.mscale.empty()) {
        const int p = static_cast<int>(R.colnames.size());
        json ms = json::array();
        for (int i = 0; i < p; ++i) {
            json row = json::array();
            for (int j = 0; j < p; ++j) row.push_back(R.mscale[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * p]);
            ms.push_back(row);
        }
        o_["mscale"] = ms;
    }
    if (!R.dropped.empty()) {
        std::string w = "nnmatch: dropped as collinear:";
        for (const auto& dnm : R.dropped) w += " " + dnm;
        o_["warnings"] = json::array({w});
    }
    emit(o_, opt(a, "--out"));
    return 0;
}
}  // namespace cecon::cli

namespace cecon::cli {
// `cobra teffects-ra --data F --spec JSON [--keepif E] [--esample-out P] [--out f.json]`
// spec: y, t, terms [{"var":v} | {"factor":v}], stat (0 ate 1 atet 2 pomeans),
// control / treated (levels), wvar, wtype (fw iw pw).
int cmd_teffects_ra(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    const std::string specs = opt(a, "--spec"), keepif = opt(a, "--keepif");
    const EsampleRequest esreq = esample_request(a, "teffects-ra");
    if (data_missing(data) || specs.empty()) throw EngineError("bad_input", "teffects-ra: requires --data and --spec");
    const json sp = json::parse(specs);
    const std::string data_all = data;
    const std::string yv = sp.at("y").get<std::string>(), tv = sp.at("t").get<std::string>();
    // Stata counts negative treatment values over the whole dataset, not only the
    // estimation sample (its "must contain nonnegative integers" check).
    bool tneg = false;
    if (!keepif.empty()) {
        const Dataset full = Dataset::open(data_all);
        const double* tf = require_num(full, "teffects", tv);
        for (int64_t i = 0; i < full.n_rows(); ++i)
            if (tf[i] < 0) tneg = true;
    }
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);
    const int64_t n = ds.n_rows();
    const double* y = require_num(ds, "teffects", yv);
    const double* t = require_num(ds, "teffects", tv);
    if (keepif.empty())
        for (int64_t i = 0; i < n; ++i)
            if (t[i] < 0) tneg = true;
    std::vector<double> w;
    const std::string wvar = sp.value("wvar", std::string());
    const std::string wtype = sp.value("wtype", std::string());
    if (!wvar.empty()) {
        const double* wc = require_num(ds, "teffects", wvar);
        w.assign(wc, wc + n);
        // the weight checks of Stata's sample marking, over the if/in sample: a
        // negative frequency or sampling weight and a noninteger frequency weight
        // are errors; a zero or missing weight leaves the observation out
        for (int64_t i = 0; i < n; ++i) {
            if (!std::isfinite(w[i])) continue;
            if (w[i] < 0 && (wtype == "fw" || wtype == "pw"))
                throw EngineError("bad_input", "negative weights encountered", "r(402)");
            if (wtype == "fw" && w[i] != std::floor(w[i]))
                throw EngineError("bad_input", "may not use noninteger frequency weights", "r(401)");
        }
    }
    // raw columns first (the sample is every term nonmissing)
    std::vector<std::pair<std::string, const double*>> raw;
    for (const auto& term : sp.at("terms")) {
        const bool fac = term.contains("factor");
        const std::string v = fac ? term["factor"].get<std::string>() : term["var"].get<std::string>();
        raw.push_back({(fac ? "i." : "") + v, require_num(ds, "teffects", v)});
    }
    std::vector<char> keep(n, 1);
    for (int64_t i = 0; i < n; ++i) {
        if (!std::isfinite(y[i]) || !std::isfinite(t[i])) keep[i] = 0;
        for (const auto& [nm, c] : raw)
            if (!std::isfinite(c[i])) keep[i] = 0;
        if (!w.empty() && !(std::isfinite(w[i]) && w[i] > 0)) keep[i] = 0;
    }
    std::vector<std::string> xnames, fvd;
    std::vector<std::vector<double>> xcols;
    for (const auto& [nm, c] : raw) {
        if (nm.rfind("i.", 0) == 0) {
            const std::string v = nm.substr(2);
            std::set<double> lv;
            for (int64_t i = 0; i < n; ++i)
                if (keep[i]) {
                    if (c[i] != std::floor(c[i]) || c[i] < 0)
                        throw EngineError("bad_input", "factor-variable levels must be nonnegative integers", "r(452)");
                    lv.insert(c[i]);
                }
            bool first = true;
            for (double l : lv) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.0f", l);
                const std::string name = std::string(buf) + (first ? "b." : ".") + v;
                xnames.push_back(name);
                fvd.push_back(name);
                std::vector<double> col(n);
                for (int64_t i = 0; i < n; ++i) col[i] = first ? 0.0 : (c[i] == l ? 1.0 : 0.0);
                if (!first)
                    for (int64_t i = 0; i < n; ++i)
                        if (!std::isfinite(c[i])) col[i] = NAN;
                xcols.push_back(std::move(col));
                first = false;
            }
        } else {
            xnames.push_back(nm);
            fvd.push_back(nm);
            xcols.emplace_back(c, c + n);
        }
    }
    TeRaOpts o;
    o.stat = sp.value("stat", 0);
    if (sp.contains("control")) { o.hasControl = true; o.control = sp["control"].get<double>(); }
    if (sp.contains("treated")) { o.hasTreated = true; o.treated = sp["treated"].get<double>(); }
    o.wtype = wtype;
    o.tvarHasNegative = tneg;
    TeRaResult r = teffects_ra_fit(y, t, xnames, xcols, w, keep, n, tv, o);
    json out = {{"estimator", "teffects"}, {"subcmd", "ra"}, {"n_obs", r.N}, {"control", r.control},
                {"treated", r.treated}, {"levels", r.levels}, {"counts", r.counts}, {"fvdvarlist", fvd}};
    const int P = static_cast<int>(r.b.size());
    json V = json::array();
    for (int i = 0; i < P; ++i) {
        json row = json::array();
        for (int j = 0; j < P; ++j) row.push_back(r.V[static_cast<size_t>(i) * P + j]);
        V.push_back(row);
    }
    out["e_b"] = {{"names", r.names}, {"values", r.b}};
    out["e_V"] = {{"names", r.names}, {"matrix", V}};
    int64_t nr = 0;
    for (char c : r.sample) nr += c ? 1 : 0;
    out["n_rows"] = nr;
    if (esreq.active()) {
        EsampleRows er;
        for (int64_t i = 0; i < n; ++i)
            if (r.sample[i]) er.keep.push_back(i);
        er.loaded = &ds;
        emit_esample(esreq, "teffects-ra", data_all, keepif, false, er, out);
    }
    emit(out, opt(a, "--out"));
    return 0;
}
}  // namespace cecon::cli
