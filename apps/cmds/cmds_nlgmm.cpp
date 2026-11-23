// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "core/expr_eval.h"
#include "core/ident.h"
#include "estimators/nlgmm.h"

#include <algorithm>
#include <cmath>
#include <map>

// P7 wave — nl, gmm.
//
// Both verbs are driven by a user EXPRESSION rather than a --y/--x varlist, so
// neither uses load_glm: the columns the sample depends on are exactly the ones
// the expression names, and this file does its own listwise deletion over those.
//
//   nl   --expr "(y = {b0=2}+{b1=.5}*x1+{b2=-.2}*x2)"
//        The top-level '=' splits the dependent variable off the model; the
//        sample is the rows where the dependent variable AND every referenced
//        variable are non-missing.
//
//   gmm  --expr "(y-{b0=2}-{b1=.5}*x1-{b2=-.2}*x2)" --instruments x1,x2,z --onestep
//        The whole expression IS the residual. The sample is built the way Stata
//        builds it, in TWO stages, and the two stages are visible in the output:
//        first a markout on the instruments (4,671 rows on the P7 fixture),
//        then the rows whose residual is missing when evaluated at the initial
//        values are dropped (22 more) and reported — Stata's
//        "note: 22 missing values returned for equation 1 at initial values."
//        The count is MEASURED, never assumed; the P7 fixture happens to make
//        the two stages distinguishable because y's 22 missings and x2's 129 are
//        disjoint sets.
//
// Oracle: internal capture p7_stata.log lines 833-886.

namespace cecon {
namespace cli {

namespace {

// Drop a wrapping paren pair, so both Stata's own `(y = ...)` form and a bare
// `y = ...` are accepted. Only strips a paren that actually wraps the WHOLE
// string — "(a)+(b)" is left alone.
std::string strip_outer_parens(std::string s) {
    s = trimx(s);
    while (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        int d = 0;
        bool wraps = true;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '(') ++d;
            else if (s[i] == ')') {
                if (--d == 0 && i + 1 < s.size()) { wraps = false; break; }
            }
        }
        if (!wraps) break;
        s = trimx(s.substr(1, s.size() - 2));
    }
    return s;
}

// Index of the assignment '=' at nesting depth 0. find_assign() in common.cpp is
// paren-BLIND, which would split "{b0=2}" at the parameter's initial value; this
// one tracks () {} [] depth so only the model's own '=' is found.
size_t top_level_assign(const std::string& s) {
    int paren = 0, brace = 0, brack = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(') ++paren;
        else if (c == ')') --paren;
        else if (c == '{') ++brace;
        else if (c == '}') --brace;
        else if (c == '[') ++brack;
        else if (c == ']') --brack;
        else if (c == '=' && paren == 0 && brace == 0 && brack == 0) {
            if (i + 1 < s.size() && s[i + 1] == '=') { ++i; continue; }   // ==
            if (i > 0 && (s[i - 1] == '=' || s[i - 1] == '<' || s[i - 1] == '>' || s[i - 1] == '!'))
                continue;                                                 // <= >= != and the tail of ==
            return i;
        }
    }
    return std::string::npos;
}

bool is_var_token(const std::string& s) {
    if (s.empty()) return false;
    if (!cecon::ident::isIdentStartByte(s[0])) return false;
    for (char c : s)
        if (!cecon::ident::isIdentByte(c)) return false;
    return true;
}

// Compile the expression body with the verb's own tag, so every rejection reads
// "nl: ..." / "gmm: ..." and surfaces as an EngineError rather than a bare
// std::runtime_error from deep inside the evaluator.
expr::Program compile_body(const std::string& body, const char* cmd) {
    expr::Options o;
    o.tag = cmd;
    o.allow_params = true;
    o.allow_vars = true;
    o.allow_coefs = false;  // _b[...] refers to a FITTED model; there is none here yet
    o.allow_eq_params = std::string(cmd) == "gmm";  // {eq:var} reaches gmm already expanded
    try {
        return expr::Program::compile(body, o);
    } catch (const std::runtime_error& ex) {
        throw EngineError("bad_input", ex.what());
    }
}

// Pull the expression's variables (and, for nl, the dependent variable) out of
// the dataset as a row-major n_kept x nv block, so one pointer bump per row
// feeds expr::VarSource.
struct ExprData {
    std::vector<int64_t> keep;
    std::vector<double> vals;  // ROW-major: row i's variables at vals[i*nv]
    int nv = 0;
    int64_t n = 0;
};

// `listwise_vars` decides whether the EXPRESSION's own variables take part in
// the listwise deletion. nl says yes; gmm says no, because Stata's gmm marks out
// on the instruments only and then reports the expression's remaining missings
// separately (see the file header). Either way every named column is looked up,
// so a typo is an honest missing_column error rather than a quietly smaller N.
ExprData gather_expr_vars(const Dataset& ds, const char* cmd,
                          const std::vector<std::string>& vars,
                          const std::vector<std::string>& extra_listwise, bool listwise_vars) {
    ExprData d;
    d.nv = static_cast<int>(vars.size());
    std::vector<const double*> vptr;
    vptr.reserve(vars.size());
    for (const std::string& v : vars) vptr.push_back(require_num(ds, cmd, v));
    std::vector<const double*> lw;
    if (listwise_vars) lw = vptr;
    for (const std::string& v : extra_listwise) lw.push_back(require_num(ds, cmd, v));
    d.keep = listwise_keep(lw, ds.n_rows());
    d.n = static_cast<int64_t>(d.keep.size());
    d.vals.assign(static_cast<size_t>(d.n) * static_cast<size_t>(d.nv > 0 ? d.nv : 1), 0.0);
    for (int j = 0; j < d.nv; ++j) {
        const double* src = vptr[static_cast<size_t>(j)];
        for (int64_t i = 0; i < d.n; ++i)
            d.vals[static_cast<size_t>(i) * static_cast<size_t>(d.nv) + static_cast<size_t>(j)] =
                src[d.keep[static_cast<size_t>(i)]];
    }
    return d;
}

std::vector<double> initial_values(const expr::Program& prog) {
    std::vector<double> b0;
    b0.reserve(prog.params().size());
    for (const expr::Param& p : prog.params()) b0.push_back(p.init);
    return b0;
}

json iteration_log(const std::vector<NlIter>& its, const char* key) {
    json a = json::array();
    for (const NlIter& it : its) a.push_back({{key, it.obj}, {"backed_up", it.backed_up}});
    return a;
}

}  // namespace

int cmd_nl(const std::vector<std::string>& a) {
    const std::string spec = opt(a, "--expr");
    std::string data = opt(a, "--data");
    const std::string vce = opt(a, "--vce", "iid");
    if (data_missing(data) || spec.empty()) {
        std::fprintf(stderr,
                     "usage: cobra nl --data f.hbin --expr \"(Y = <expr in {params} and vars>)\" "
                     "[--vce iid] [--weight W --wtype aw|pw] [--level P] [--keepif EXPR] "
                     "[--out f.json]\n"
                     "  nonlinear least squares (Stata `nl`). Parameters are written {name=init}\n"
                     "  and are solved for; every other identifier is a variable in the data.\n"
                     "  e.g. --expr \"(y = {b0=2}+{b1=.5}*x1+{b2=-.2}*x2)\"\n"
                     "  --weight W --wtype aw|pw  Stata [aweight=]/[pweight=]: normalized to sum to\n"
                     "               the row count; pw takes the robust sandwich (vce robust).\n");
        return 2;
    }
    // --vce: only the model-based (iid) covariance is pinned against Stata. A
    // robust NLS sandwich differs from Stata's only in the small-sample factor,
    // which is exactly the part no oracle here fixes — so it is rejected rather
    // than emitted as a number nothing verified.
    // BUG-1283: Stata's [aweight=] / [pweight=]. A pweight implies the robust sandwich,
    // whose n/(n-k) factor the pweight oracle pins (StataBE `nl (y = {b0} + {b1}*x1 +
    // {b2}*x2) [pw=w]`: _se[/b1] .071775268345285762); fw/iw would make N the sum of
    // the weights and are not served here.
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    bool pweight = false;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", "nl: --wtype requires --weight");
        if (wtype.empty() || wtype == "aw" || wtype == "aweight") pweight = false;
        else if (wtype == "pw" || wtype == "pweight") pweight = true;
        else
            throw EngineError("unsupported",
                "nl: --wtype must be aw or pw (got '" + wtype + "'); fweight and iweight are "
                "not implemented for nl");
    }
    if (vce != "iid" && !(pweight && vce == "robust"))
        throw EngineError("unsupported",
            "nl: --vce " + vce + " is not implemented; only the model-based (iid) covariance "
            "sigma2*(J'J)^-1, and a pweight's robust sandwich, are pinned against Stata in this "
            "build");
    const bool level_set = has(a, "--level");
    const double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "nl: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }

    const std::string body = strip_outer_parens(spec);
    const size_t eq = top_level_assign(body);
    if (eq == std::string::npos)
        throw EngineError("bad_input",
            "nl: --expr must have the form \"(depvar = <expression>)\" — no top-level '=' found "
            "in \"" + spec + "\"");
    const std::string dep = trimx(body.substr(0, eq));
    const std::string rhs = body.substr(eq + 1);
    if (!is_var_token(dep))
        throw EngineError("bad_input",
            "nl: the left side of '=' must be a single variable name (got \"" + dep + "\")");

    const expr::Program prog = compile_body(rhs, "nl");
    if (prog.params().empty())
        throw EngineError("bad_input",
            "nl: the model has no {parameters} to estimate — write them as {name} or {name=init}");

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* depcol = require_num(ds, "nl", dep);
    std::vector<std::string> listwise_extra{dep};
    if (!weight_var.empty()) listwise_extra.push_back(weight_var);
    ExprData d = gather_expr_vars(ds, "nl", prog.variables(), listwise_extra, /*listwise_vars=*/true);
    // A zero weight marks the row out (Stata's mark), a negative one is an error; the
    // rest are normalized to sum to the row count (nl.ado).
    std::vector<double> wn;
    double sum_w = 0;
    if (!weight_var.empty()) {
        const double* wcol = require_num(ds, "nl", weight_var);
        std::vector<int64_t> keep2;
        std::vector<double> vals2;
        for (int64_t i = 0; i < d.n; ++i) {
            const double wi = wcol[d.keep[static_cast<size_t>(i)]];
            if (wi < 0)
                throw EngineError("bad_input", "nl: weight '" + weight_var + "' has a negative value");
            if (wi == 0) continue;
            keep2.push_back(d.keep[static_cast<size_t>(i)]);
            for (int j = 0; j < d.nv; ++j)
                vals2.push_back(d.vals[static_cast<size_t>(i) * static_cast<size_t>(d.nv) +
                                       static_cast<size_t>(j)]);
            wn.push_back(wi);
            sum_w += wi;
        }
        d.keep = std::move(keep2);
        if (d.nv > 0) d.vals = std::move(vals2);
        d.n = static_cast<int64_t>(d.keep.size());
        for (double& v : wn) v *= static_cast<double>(d.n) / sum_w;
    }
    // BUG-1269: nl words its empty and too-small samples itself (StataBE 19.5,
    // `nl (y = {b0} + {b1}*x) if x > 20` / `if x == 1`), r(2000) / r(2001).
    const int64_t n_params = static_cast<int64_t>(prog.params().size());
    if (d.n == 0)
        throw EngineError("no_obs", "estimation sample indicator variable eliminates all observations",
                          "r(2000)");
    if (d.n < n_params)
        throw EngineError("no_obs", "cannot have fewer observations (" + std::to_string(d.n) +
                                        ") than parameters (" + std::to_string(n_params) + ")",
                          "r(2001)");
    std::vector<double> Y = gather_rows(depcol, d.keep);

    const int nv = d.nv;
    const double* vals = d.vals.data();
    const NlResidFn resid = [&](const std::vector<double>& b, int64_t i) {
        expr::VarSource vs;
        vs.values = vals + static_cast<size_t>(i) * static_cast<size_t>(nv);
        return Y[static_cast<size_t>(i)] - prog.eval(b.data(), vs, nullptr);
    };

    std::vector<std::string> pnames;
    for (const expr::Param& p : prog.params()) pnames.push_back(p.name);
    const NlResult r = nl_fit(resid, Y.data(), d.n, initial_values(prog), pnames,
                              wn.empty() ? nullptr : wn.data(), pweight);
    if (!r.ok) throw EngineError("no_convergence", r.err.empty() ? "nl: the fit failed" : r.err);

    const double tc = t_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0, r.df_r);
    json coefs = json::array();
    for (int j = 0; j < r.k; ++j) {
        const double b = r.beta[static_cast<size_t>(j)], se = r.se[static_cast<size_t>(j)];
        const double t = (se > 0) ? b / se : 0.0;
        coefs.push_back({{"name", "/" + r.names[static_cast<size_t>(j)]},
                         {"b", b}, {"se", se}, {"t", t},
                         {"p", (se > 0 && r.df_r > 0) ? t_twosided_pvalue(t, r.df_r) : 1.0},
                         {"ci_lo", b - tc * se}, {"ci_hi", b + tc * se}});
    }

    json out = {{"estimator", "nl"}, {"depvar", dep}, {"expr", spec},
                {"n_obs", d.n}, {"n_dropped_missing", ds.n_rows() - d.n},
                {"n_params", r.k}, {"vce", pweight ? std::string("robust") : vce},
                {"coefs", coefs},
                {"rss", r.rss}, {"mss", r.mss}, {"tss", r.tss},
                {"df_r", r.df_r}, {"df_m", r.df_m}, {"df_t", r.df_t},
                {"ms_r", r.rss / r.df_r}, {"ms_m", r.df_m > 0 ? r.mss / r.df_m : 0.0},
                {"ms_t", r.df_t > 0 ? r.tss / r.df_t : 0.0},
                {"r2", r.r2}, {"r2_adj", r.r2_adj}, {"rmse", r.rmse}, {"res_dev", r.res_dev},
                {"has_constant", r.cons_param >= 0},
                {"iterations", iteration_log(r.iters, "rss")},
                {"converged", r.converged}};
    // A weighted model with no constant term: Stata's TSS is the uncentred sum of
    // squares under the RAW weights (summarize y^2 [aw=w], r(sum)), while the RSS is
    // on the normalized ones, so e(tss), e(mss), e(r2) and e(r2_a) follow that TSS
    // (bank 400a322e `nl (mpg = {b0=40}*exp({b1=-0.0003}*weight)) [aweight=trunk]`:
    // e(tss) 449716 = sum trunk*mpg^2, e(rss) 761.80, e(mss) 448954.20, e(r2) .998306).
    // With a constant the centred TSS is the variance under normalized weights,
    // which the fit's own TSS already is. A pweight keeps the normalized TSS (bank
    // f81d642d `[pweight=trunk]`: e(mss) 31928.75).
    if (!wn.empty() && !pweight && r.cons_param < 0 && sum_w > 0 && d.n > 0) {
        const double tss = r.tss * sum_w / static_cast<double>(d.n);
        const double mss = tss - r.rss;
        out["tss"] = tss;
        out["mss"] = mss;
        out["ms_m"] = r.df_m > 0 ? mss / r.df_m : 0.0;
        out["ms_t"] = r.df_t > 0 ? tss / r.df_t : 0.0;
        out["r2"] = tss > 0 ? 1.0 - r.rss / tss : 0.0;
        out["r2_adj"] = (tss > 0 && r.df_t > 0 && r.df_r > 0)
                            ? 1.0 - (r.rss / r.df_r) / (tss / r.df_t)
                            : 0.0;
    }
    if (level_set) out["level"] = level;
    // Weighted only, so an unweighted reply is unchanged: e(wtype), e(sum_w).
    if (!weight_var.empty())
        out["weight"] = {{"var", weight_var}, {"type", pweight ? "pw" : "aw"}, {"sum_w", sum_w}};
    std::vector<std::string> notes;
    if (r.cons_param >= 0)
        notes.push_back("Parameter " + r.names[static_cast<size_t>(r.cons_param)] +
                        " is used as a constant term during estimation.");
    else
        // No parameter enters additively, so there is no centring to justify: the
        // ANOVA block above is built on the UNCENTRED total sum of squares. Stata
        // prints its own wording here; this build says what it actually did rather
        // than guessing at a string no oracle in this wave pinned.
        notes.push_back("No parameter is used as a constant term during estimation; R-squared is "
                        "computed against the uncentred total sum of squares.");
    add_warnings(out, notes);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_gmm(const std::vector<std::string>& a) {
    const std::vector<std::string> specs = opt_all(a, "--expr");
    std::string data = opt(a, "--data");
    const std::vector<std::string> instrs = opt_all(a, "--instruments");
    const std::string vce = opt(a, "--vce", "robust");
    const std::string wmatrix = opt(a, "--wmatrix", vce == "unadjusted" ? "unadjusted"
                                                    : vce == "cluster" ? "cluster" : "robust");
    const std::string winit = opt(a, "--winitial", "unadjusted");
    const std::string clusterVar = opt(a, "--cluster");
    const std::string wclusterVar = opt(a, "--wcluster", clusterVar);
    const std::string weightVar = opt(a, "--weight"), wtype = opt(a, "--wtype");
    const std::string eqnamesArg = opt(a, "--eqnames");
    const bool onestep = has(a, "--onestep"), igmm = has(a, "--igmm");
    (void)has(a, "--twostep");
    const EsampleRequest esreq = esample_request(a, "gmm");
    if (data_missing(data) || specs.empty() || instrs.empty()) {
        std::fprintf(stderr,
                     "usage: cobra gmm --data f.hbin --expr \"(<residual>)\" [--expr ...] "
                     "--instruments Z1,Z2 [--instruments ... one per --expr] [--eqnames a,b]\n"
                     "  [--onestep|--twostep|--igmm] [--winitial unadjusted|identity|"
                     "unadjusted_independent|identity_independent]\n"
                     "  [--wmatrix robust|unadjusted|cluster] [--wcluster V] [--vce robust|unadjusted|cluster] "
                     "[--cluster V]\n"
                     "  [--weight W --wtype aw|pw|fw|iw] [--level P] [--keepif EXPR] [--out f.json]\n"
                     "  Stata `gmm` (interactive). Each expression IS a residual; a constant is appended\n"
                     "  to every instrument list. Parameters {name} / {eq:name}; the default estimator is\n"
                     "  two-step with an unadjusted initial and a robust weight matrix.\n");
        return 2;
    }
    if (onestep && igmm) throw EngineError("bad_input", "gmm: --onestep and --igmm are exclusive");
    auto typeOf = [](const std::string& t, const char* what) {
        if (t == "robust") return 0;
        if (t == "unadjusted") return 1;
        if (t == "cluster") return 2;
        throw EngineError("bad_input", std::string("gmm: --") + what + " must be robust, unadjusted or cluster (got '" + t + "')");
    };
    const int vceT = typeOf(vce, "vce"), wmT = typeOf(wmatrix, "wmatrix");
    if ((vceT == 2 && clusterVar.empty()) || (wmT == 2 && wclusterVar.empty()))
        throw EngineError("bad_input", "gmm: cluster type needs --cluster VAR");
    int winitT = 0;
    bool winitIndep = false;
    if (winit == "unadjusted") winitT = 0;
    else if (winit == "identity") winitT = 1;
    else if (winit == "unadjusted_independent") { winitT = 0; winitIndep = true; }
    else if (winit == "identity_independent") winitT = 1;
    else throw EngineError("bad_input", "gmm: --winitial must be unadjusted, identity, unadjusted_independent or identity_independent");
    const bool level_set = has(a, "--level");
    const double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "gmm: --level must be in [10, 99.99] (got %s).\n", opt(a, "--level").c_str());
        return 2;
    }
    const int M = static_cast<int>(specs.size());
    if (instrs.size() != 1 && static_cast<int>(instrs.size()) != M)
        throw EngineError("bad_input", "gmm: give one --instruments, or one per --expr");

    // equations: compile, and map each program's parameters onto the global
    // first-appearance order across the equations (Stata's e(params)).
    std::vector<expr::Program> progs;
    std::vector<std::string> pnames;
    std::vector<double> b0;
    std::vector<std::vector<int>> pmap;
    std::vector<std::string> allVars;
    for (int m = 0; m < M; ++m) {
        const std::string body = strip_outer_parens(specs[static_cast<size_t>(m)]);
        if (top_level_assign(body) != std::string::npos)
            throw EngineError("bad_input",
                "gmm: --expr is the RESIDUAL, not an equation — it must not contain a top-level '=' "
                "(got \"" + specs[static_cast<size_t>(m)] + "\")");
        progs.push_back(compile_body(body, "gmm"));
        std::vector<int> mp;
        for (const expr::Param& p : progs.back().params()) {
            int at = -1;
            for (size_t q = 0; q < pnames.size(); ++q)
                if (pnames[q] == p.name) { at = static_cast<int>(q); break; }
            if (at < 0) {
                at = static_cast<int>(pnames.size());
                pnames.push_back(p.name);
                b0.push_back(p.init);
            } else if (p.init_set) {
                b0[static_cast<size_t>(at)] = p.init;
            }
            mp.push_back(at);
        }
        pmap.push_back(mp);
        for (const std::string& v : progs.back().variables())
            if (std::find(allVars.begin(), allVars.end(), v) == allVars.end()) allVars.push_back(v);
    }
    if (pnames.empty())
        throw EngineError("bad_input",
            "gmm: the model has no {parameters} to estimate — write them as {name} or {name=init}");
    std::vector<std::vector<std::string>> zs;
    std::vector<std::string> zAll;
    for (int m = 0; m < M; ++m) {
        zs.push_back(split_vars(instrs[instrs.size() == 1 ? 0 : static_cast<size_t>(m)]));
        for (const std::string& v : zs.back())
            if (std::find(zAll.begin(), zAll.end(), v) == zAll.end()) zAll.push_back(v);
    }
    std::vector<std::string> eqnames = split_csv(eqnamesArg);
    if (eqnames.size() != static_cast<size_t>(M)) {
        eqnames.clear();
        for (int m = 0; m < M; ++m) eqnames.push_back(std::to_string(m + 1));
    }

    const std::string original_data = data;
    const std::string keepif = opt(a, "--keepif");
    data = maybe_keepif(data, keepif);
    const Dataset& ds = acquire_data(data);

    // STAGE 1 — markout on the instruments, the weight and the cluster variables.
    std::vector<std::string> lw = zAll;
    if (!weightVar.empty()) lw.push_back(weightVar);
    if (vceT == 2) lw.push_back(clusterVar);
    if (wmT == 2 && wclusterVar != clusterVar) lw.push_back(wclusterVar);
    const ExprData d = gather_expr_vars(ds, "gmm", allVars, lw, /*listwise_vars=*/false);
    std::vector<int64_t> keep1;
    const double* wsrc = weightVar.empty() ? nullptr : require_num(ds, "gmm", weightVar);
    for (int64_t i = 0; i < d.n; ++i) {
        const int64_t row = d.keep[static_cast<size_t>(i)];
        if (wsrc && !(wsrc[row] > 0)) {
            if (wsrc[row] < 0) throw EngineError("bad_input", "negative weights encountered");
            continue;  // zero weight: out of the sample
        }
        keep1.push_back(i);
    }
    require_sample(static_cast<int64_t>(keep1.size()));
    const int64_t n_markout = static_cast<int64_t>(keep1.size());
    const int nv = d.nv;
    auto varIndex = [&](const std::string& v) {
        return static_cast<int>(std::find(allVars.begin(), allVars.end(), v) - allVars.begin());
    };
    // per-equation variable maps (program variable j -> column of d.vals)
    std::vector<std::vector<int>> vmap;
    for (int m = 0; m < M; ++m) {
        std::vector<int> mp;
        for (const std::string& v : progs[static_cast<size_t>(m)].variables()) mp.push_back(varIndex(v));
        vmap.push_back(mp);
    }
    auto evalEq = [&](int m, const std::vector<double>& b, const double* rowVals) {
        thread_local std::vector<double> lb, lv;
        const auto& pm = pmap[static_cast<size_t>(m)];
        lb.resize(pm.size());
        for (size_t q = 0; q < pm.size(); ++q) lb[q] = b[static_cast<size_t>(pm[q])];
        const auto& vm = vmap[static_cast<size_t>(m)];
        lv.resize(vm.size() ? vm.size() : 1);
        for (size_t q = 0; q < vm.size(); ++q) lv[q] = rowVals[vm[q]];
        expr::VarSource vs;
        vs.values = lv.data();
        return progs[static_cast<size_t>(m)].eval(lb.data(), vs, nullptr);
    };

    // STAGE 2 — drop the rows whose residual is missing at the initial values.
    std::vector<int64_t> sel;
    sel.reserve(keep1.size());
    int64_t n_init_missing = 0;
    for (int64_t i : keep1) {
        const double* rv = d.vals.data() + static_cast<size_t>(i) * static_cast<size_t>(nv);
        bool ok = true;
        for (int m = 0; m < M && ok; ++m) ok = std::isfinite(evalEq(m, b0, rv));
        if (ok) sel.push_back(i);
        else ++n_init_missing;
    }
    const int64_t n = static_cast<int64_t>(sel.size());
    if (n == 0) {
        std::fprintf(stderr, "gmm: the residual is missing on every row at the initial values.\n");
        return 1;
    }
    std::vector<double> vals(static_cast<size_t>(n) * static_cast<size_t>(nv > 0 ? nv : 1), 0.0);
    std::vector<int64_t> rows(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        rows[static_cast<size_t>(i)] = d.keep[static_cast<size_t>(sel[static_cast<size_t>(i)])];
        for (int j = 0; j < nv; ++j)
            vals[static_cast<size_t>(i) * static_cast<size_t>(nv) + static_cast<size_t>(j)] =
                d.vals[static_cast<size_t>(sel[static_cast<size_t>(i)]) * static_cast<size_t>(nv) + static_cast<size_t>(j)];
    }
    // instruments, constant last
    std::vector<std::vector<double>> Zs(static_cast<size_t>(M));
    std::vector<int> Ls;
    std::vector<const double*> Zp;
    for (int m = 0; m < M; ++m) {
        const auto& zm = zs[static_cast<size_t>(m)];
        const int L = static_cast<int>(zm.size()) + 1;
        Ls.push_back(L);
        auto& Z = Zs[static_cast<size_t>(m)];
        Z.assign(static_cast<size_t>(n) * static_cast<size_t>(L), 1.0);
        for (size_t c = 0; c < zm.size(); ++c) {
            const double* src = require_num(ds, "gmm", zm[c]);
            for (int64_t i = 0; i < n; ++i) Z[c * static_cast<size_t>(n) + static_cast<size_t>(i)] = src[rows[static_cast<size_t>(i)]];
        }
    }
    for (auto& Z : Zs) Zp.push_back(Z.data());
    // weights
    std::vector<double> wv, omv;
    double Nw = static_cast<double>(n);
    const bool freqW = wtype == "fw" || wtype == "fweight" || wtype == "iw" || wtype == "iweight";
    if (wsrc) {
        wv.resize(static_cast<size_t>(n));
        double sw = 0;
        for (int64_t i = 0; i < n; ++i) sw += (wv[static_cast<size_t>(i)] = wsrc[rows[static_cast<size_t>(i)]]);
        omv = wv;
        if (freqW) {
            Nw = sw;
        } else {
            for (double& v : wv) v *= static_cast<double>(n) / sw;
            for (size_t i = 0; i < wv.size(); ++i) omv[i] = wv[i] * wv[i];
        }
    }
    // clusters
    auto codes = [&](const std::string& var, std::vector<int64_t>* out) {
        const double* src = require_num(ds, "gmm", var);
        std::map<double, int64_t> id;
        out->resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            const double v = src[rows[static_cast<size_t>(i)]];
            auto it = id.find(v);
            if (it == id.end()) it = id.emplace(v, static_cast<int64_t>(id.size())).first;
            (*out)[static_cast<size_t>(i)] = it->second;
        }
    };
    std::vector<int64_t> vcl, wcl;
    if (vceT == 2) codes(clusterVar, &vcl);
    if (wmT == 2) codes(wclusterVar, &wcl);

    GmmExSpec sp;
    sp.M = M;
    sp.L = Ls;
    sp.Z = Zp;
    sp.n = n;
    sp.b0 = b0;
    const double* valsp = vals.data();
    sp.resid = [&](int m, const std::vector<double>& b, int64_t i) {
        return evalEq(m, b, valsp + static_cast<size_t>(i) * static_cast<size_t>(nv));
    };
    sp.w = wv.empty() ? nullptr : wv.data();
    sp.omega = omv.empty() ? nullptr : omv.data();
    sp.Nw = Nw;
    sp.winit = winitT;
    sp.winit_independent = winitIndep;
    sp.estimator = onestep ? 0 : (igmm ? 2 : 1);
    sp.wmatrix = wmT;
    sp.wclus = wcl.empty() ? nullptr : wcl.data();
    sp.vce = vceT;
    sp.vclus = vcl.empty() ? nullptr : vcl.data();
    const GmmExResult r = gmm_fit_ex(sp);
    if (!r.ok) throw EngineError(r.err_code.empty() ? "no_convergence" : r.err_code,
                                 r.err.empty() ? "gmm: the fit failed" : r.err);
    const int k = r.k, L = r.Ltot;

    // Asymptotic z inference — Stata's `gmm` header reports z, not t.
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    json coefs = json::array();
    for (int j = 0; j < k; ++j) {
        const double b = r.beta[static_cast<size_t>(j)];
        const double se = std::sqrt(std::max(0.0, r.V[static_cast<size_t>(j) * static_cast<size_t>(k) + static_cast<size_t>(j)]));
        const double z = (se > 0) ? b / se : 0.0;
        const std::string& nm = pnames[static_cast<size_t>(j)];
        coefs.push_back({{"name", nm.find(':') == std::string::npos ? "/" + nm : nm},
                         {"b", b}, {"se", se}, {"z", z},
                         // erfc(|z|/sqrt2) IS the two-sided normal tail (no 1 - Phi cancellation).
                         {"p", (se > 0) ? std::erfc(std::fabs(z) * 0.7071067811865476) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    std::vector<std::string> znames = zs[0];
    znames.push_back("_cons");
    auto sq = [](const std::vector<double>& m, int dim) {
        json rowsJ = json::array();
        for (int i = 0; i < dim; ++i) {
            json row = json::array();
            for (int j = 0; j < dim; ++j) row.push_back(m[static_cast<size_t>(i) * static_cast<size_t>(dim) + static_cast<size_t>(j)]);
            rowsJ.push_back(row);
        }
        return rowsJ;
    };
    json Gj = json::array();
    for (int l = 0; l < L; ++l) {
        json row = json::array();
        for (int j = 0; j < k; ++j) row.push_back(r.G[static_cast<size_t>(j) * static_cast<size_t>(L) + static_cast<size_t>(l)]);
        Gj.push_back(row);
    }
    json eqs = json::array();
    for (int m = 0; m < M; ++m) {
        json pl = json::array();
        for (int q : pmap[static_cast<size_t>(m)]) pl.push_back(pnames[static_cast<size_t>(q)]);
        json il = zs[static_cast<size_t>(m)];
        il.push_back("_cons");
        eqs.push_back({{"name", eqnames[static_cast<size_t>(m)]}, {"params", pl}, {"instruments", il}});
    }
    const char* estName = onestep ? "onestep" : (igmm ? "igmm" : "twostep");
    json gm = {{"params", pnames}, {"b", r.beta}, {"init", b0}, {"V", sq(r.V, k)},
               {"V_modelbased", sq(r.Vmb, k)}, {"S", sq(r.S, L)}, {"W", sq(r.W, L)}, {"G", Gj},
               {"Q", r.Q}, {"Q_criterion", r.Qcrit}, {"J", r.J}, {"J_df", L - k},
               // Stata's e(N) is floor(sum of weights) for iweights (as regress); J keeps the unfloored sum
               {"N", std::floor(Nw)}, {"n_moments", L}, {"rank", k}, {"ic", r.ic}, {"equations", eqs},
               {"estimator", estName}, {"converged", r.converged}};
    if (vceT == 2 || wmT == 2) gm["N_clust"] = r.n_clust;
    json out = {{"estimator", "gmm"}, {"expr", specs[0]},
                {"n_obs", n}, {"n_dropped_missing", ds.n_rows() - n},
                {"n_instrument_markout", n_markout},  // rows surviving stage 1 (see the header)
                {"n_params", k}, {"n_moments", L},
                {"instruments", znames}, {"onestep", onestep},
                {"winitial", winit}, {"vce", vce},
                {"coefs", coefs}, {"criterion", r.Qcrit},
                {"iterations", iteration_log(r.iters, "criterion")},
                {"converged", r.converged}, {"gmm", gm}};
    if (level_set) out["level"] = level;
    std::vector<std::string> notes;
    if (n_init_missing > 0)
        notes.push_back(std::to_string(static_cast<long long>(n_init_missing)) +
                        " missing values returned for equation 1 at initial values.");
    add_warnings(out, notes);
    if (esreq.active()) emit_esample(esreq, "gmm", original_data, keepif, false, {rows, &ds}, out);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
