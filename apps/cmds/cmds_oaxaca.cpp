// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"
#include "estimators/oaxaca.h"
#include "core/distributions.h"
#include <set>

namespace cecon::cli {
namespace {
std::vector<std::pair<std::string, std::vector<std::string>>> groups_of(const json& j) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    if (!j.is_array()) return out;
    for (const auto& g : j) out.push_back({g.at(0).get<std::string>(), g.at(1).get<std::vector<std::string>>()});
    return out;
}
std::vector<std::pair<std::string, double>> vals_of(const json& j) {
    std::vector<std::pair<std::string, double>> out;
    if (!j.is_array()) return out;
    for (const auto& g : j) out.push_back({g.at(0).get<std::string>(), g.at(1).get<double>()});
    return out;
}
}  // namespace

// `cobra oaxaca --data F --spec JSON [--keepif EXPR] [--esample-out PATH] [--out f.json]`
// The spec: y, vars (coefficient variables, Stata order), regressor (0/1 per var),
// by, swap, model (linear|logit|probit), tf (1 threefold, 2 reverse, 0 b*),
// ref (pooled|omega|weights), weights [..], split, detail, vgroups [[name,[vars]]],
// normalize [[cons,[dummies]]], adjust [..], fixed (true | [..]), x1/x2 [[var,val]],
// se, suest, robust, cluster, cluster_means, relax, check, wvar, wtype, level.
int cmd_oaxaca(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::string specs = opt(a, "--spec");
    const std::string keepif = opt(a, "--keepif");
    const EsampleRequest esreq = esample_request(a, "oaxaca");
    // The flag form of the first engine pass (`--y Y --x X1,X2 --by G --pooled [--relax]
    // [--level #]`, the pooled twofold decomposition the eval manifest and the options
    // contract still drive) is the spec PIET sends for `oaxaca y x1 x2, by(g) pooled
    // [relax]`: every x a regressor, reference = the pooled model, suest standard errors.
    if (specs.empty() && !opt(a, "--y").empty()) {
        const std::string lev = opt(a, "--level");
        const double level = lev.empty() ? 95 : std::stod(lev);
        if (opt(a, "--by").empty() || !has(a, "--pooled") || !(level > 0 && level < 100))
            throw EngineError("bad_input",
                              "oaxaca: requires --data --y --by --pooled; --level must be between 0 and 100");
        const auto xs = split_csv(opt(a, "--x"));
        json sp = {{"y", opt(a, "--y")}, {"by", opt(a, "--by")}, {"vars", xs},
                   {"regressor", std::vector<int>(xs.size(), 1)}, {"tf", 0}, {"ref", "pooled"},
                   {"suest", true}, {"relax", has(a, "--relax")}, {"level", level}};
        specs = sp.dump();
    }
    if (data_missing(data) || specs.empty()) throw EngineError("bad_input", "oaxaca: requires --data and --spec");
    const json sp = json::parse(specs);
    const std::string data_all = data;
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);
    const std::string y = sp.at("y").get<std::string>(), by = sp.at("by").get<std::string>();
    const auto vars = sp.at("vars").get<std::vector<std::string>>();
    const auto reg = sp.at("regressor").get<std::vector<int>>();
    const double* yy = require_num(ds, "oaxaca", y);
    const double* gg = require_num(ds, "oaxaca", by);
    std::vector<const double*> xx;
    for (const auto& v : vars) xx.push_back(require_num(ds, "oaxaca", v));
    const std::string wvar = sp.value("wvar", std::string()), wtype = sp.value("wtype", std::string());
    const double* ww = wvar.empty() ? nullptr : require_num(ds, "oaxaca", wvar);
    const std::string clv = sp.value("cluster", std::string());
    const double* cc = clv.empty() ? nullptr : require_num(ds, "oaxaca", clv);

    // marksample (zeroweight, novarlist) + markout by
    std::vector<int64_t> rows;
    std::set<double> levels;
    for (int64_t i = 0; i < ds.n_rows(); ++i) {
        if (ww) {
            if (!std::isfinite(ww[i])) continue;
            if (ww[i] < 0) throw EngineError("bad_input", "negative weights encountered", "r(402)");
            if (wtype == "fw" && ww[i] != std::floor(ww[i]))
                throw EngineError("bad_input", "may not use noninteger frequency weights", "r(401)");
        }
        if (!std::isfinite(gg[i])) continue;
        levels.insert(gg[i]);
        rows.push_back(i);
    }
    if (levels.size() > 2) throw EngineError("bad_input", "more than 2 groups found, only 2 groups allowed", "r(420)");
    if (levels.size() < 2 || rows.empty()) throw EngineError("no_obs", "no observations", "r(2000)");
    const bool swap = sp.value("swap", false);
    const double g1 = swap ? *levels.rbegin() : *levels.begin(), g2 = swap ? *levels.begin() : *levels.rbegin();

    OaxacaData d;
    d.n = static_cast<int64_t>(rows.size());
    d.vars = vars;
    d.regressor.assign(reg.begin(), reg.end());
    d.X.assign(vars.size(), std::vector<double>(d.n));
    d.y.resize(d.n);
    d.byv.resize(d.n);
    d.group.resize(d.n);
    if (ww) d.w.resize(d.n);
    d.wtype = wtype;
    if (cc) d.cl.resize(d.n);
    for (int64_t r = 0; r < d.n; ++r) {
        const int64_t i = rows[r];
        d.y[r] = yy[i];
        d.byv[r] = gg[i];
        d.group[r] = gg[i] == g1 ? 0 : 1;
        for (size_t j = 0; j < vars.size(); ++j) d.X[j][r] = xx[j][i];
        if (ww) d.w[r] = ww[i];
        if (cc) d.cl[r] = cc[i];
    }
    OaxacaSpec s;
    const std::string model = sp.value("model", std::string("linear"));
    s.model = model == "logit" ? 1 : (model == "probit" ? 2 : 0);
    s.tf = sp.value("tf", 1);
    const std::string ref = sp.value("ref", std::string());
    s.ref = ref == "pooled" ? 1 : (ref == "omega" ? 2 : (ref == "weights" ? 3 : (ref == "reference" ? 4 : 0)));
    if (s.ref == 4) {
        const double* ry = require_num(ds, "oaxaca", sp.at("ref_y").get<std::string>());
        const double* rm = require_num(ds, "oaxaca", sp.at("ref_sample").get<std::string>());
        d.refNames = sp.at("ref_x").get<std::vector<std::string>>();
        std::vector<const double*> rx;
        for (const auto& v : d.refNames) rx.push_back(require_num(ds, "oaxaca", v));
        d.refY.resize(d.n);
        d.refMask.resize(d.n);
        d.refX.assign(rx.size(), std::vector<double>(d.n));
        int64_t all = 0, in = 0;
        for (int64_t i = 0; i < ds.n_rows(); ++i) all += rm[i] == 1;
        for (int64_t i : rows) in += rm[i] == 1;
        if (all != in)
            throw EngineError("unsupported", "oaxaca: a reference() model estimated on rows outside the decomposition's sample is not implemented in this version");
        for (int64_t r = 0; r < d.n; ++r) {
            const int64_t i = rows[r];
            d.refY[r] = ry[i];
            d.refMask[r] = rm[i] == 1;
            for (size_t j = 0; j < rx.size(); ++j) d.refX[j][r] = rx[j][i];
        }
    }
    if (sp.contains("weights")) s.weights = sp["weights"].get<std::vector<double>>();
    s.split = sp.value("split", false);
    s.detail = sp.value("detail", true);
    s.se = sp.value("se", true);
    s.suest = sp.value("suest", false);
    s.robust = sp.value("robust", false);
    s.relax = sp.value("relax", false);
    s.check = sp.value("check", true);
    s.clusterMeans = sp.value("cluster_means", false);
    if (sp.contains("robust_models")) {
        s.robustG[0] = sp["robust_models"].at(0).get<bool>();
        s.robustG[1] = sp["robust_models"].at(1).get<bool>();
    }
    if (sp.contains("vgroups")) s.vgroups = groups_of(sp["vgroups"]);
    if (sp.contains("normalize")) s.normalize = groups_of(sp["normalize"]);
    if (sp.contains("adjust")) s.adjust = sp["adjust"].get<std::vector<std::string>>();
    if (sp.contains("fixed")) {
        if (sp["fixed"].is_boolean()) s.fixedAll = sp["fixed"].get<bool>();
        else s.fixedVars = sp["fixed"].get<std::vector<std::string>>();
    }
    if (sp.contains("x1")) s.x1 = vals_of(sp["x1"]);
    if (sp.contains("x2")) s.x2 = vals_of(sp["x2"]);
    if (s.tf == 0 && s.ref == 3 && s.weights.empty()) throw EngineError("bad_input", "oaxaca: weight() needs a number");
    if (s.model != 0 && !d.w.empty())
        throw EngineError("unsupported", "oaxaca: logit/probit with weights is not implemented in this version");

    OaxacaOut r = oaxaca_decompose(d, s);
    const double level = sp.value("level", 95.0);
    json o = {{"estimator", "oaxaca"}, {"depvar", y}, {"n_obs", r.N}, {"N_1", r.N1}, {"N_2", r.N2},
              {"group_1", g1}, {"group_2", g2}, {"by", by}, {"level", level}, {"k_eq", r.kEq},
              {"robust_models", r.robustModels}};
    {
        int64_t nr = 0;
        for (char e : r.esample) nr += e ? 1 : 0;
        o["n_rows"] = nr;
    }
    if (r.nClust > 0) o["N_clust"] = r.nClust;
    if (!r.legend.empty()) o["legend"] = r.legend;
    o["notes"] = r.notes;
    const int m = static_cast<int>(r.b.size());
    json ct = json::array();
    const double z = z_critical(1 - level / 100);
    for (int i = 0; i < m; ++i) {
        json row = {{"name", r.names[i]}, {"b", r.b[i]}};
        if (s.se) {
            const double se = std::sqrt(std::max(0., r.V[static_cast<size_t>(i) * m + i])), b = r.b[i];
            row["se"] = se;
            row["t"] = se > 0 ? json(b / se) : json(nullptr);
            row["p"] = se > 0 ? json(std::erfc(std::fabs(b / se) / std::sqrt(2.))) : json(nullptr);
            row["ci_lo"] = se > 0 ? json(b - z * se) : json(nullptr);
            row["ci_hi"] = se > 0 ? json(b + z * se) : json(nullptr);
        }
        ct.push_back(row);
    }
    o["coeftable"] = ct;
    auto mat = [](const std::vector<double>& v, int n) {
        json M = json::array();
        for (int i = 0; i < n; ++i) {
            json row = json::array();
            for (int j = 0; j < n; ++j) row.push_back(v[static_cast<size_t>(i) * n + j]);
            M.push_back(row);
        }
        return M;
    };
    o["e_b"] = {{"names", r.names}, {"values", r.b}};
    o["e_b0"] = {{"names", r.names0}, {"values", r.b0}};
    if (s.se) {
        o["e_V"] = {{"names", r.names}, {"matrix", mat(r.V, m)}};
        o["e_V0"] = {{"names", r.names0}, {"matrix", mat(r.V0, static_cast<int>(r.b0.size()))}};
    }
    if (esreq.active()) {
        // e(sample) over the loaded rows
        EsampleRows er;
        for (int64_t k = 0; k < d.n; ++k)
            if (r.esample[k]) er.keep.push_back(rows[k]);
        er.loaded = &ds;
        emit_esample(esreq, "oaxaca", data_all, keepif, false, er, o);
    }
    emit(o, opt(a, "--out"));
    return 0;
}
}
