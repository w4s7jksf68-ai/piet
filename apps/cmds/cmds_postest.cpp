#include "common.h"
#include "commands.h"

#include "estimators/feols.h"
#include "estimators/ols.h"
#include "estimators/posthest.h"
#include "estimators/nlcom.h"
#include "estimators/logit.h"
#include "estimators/probit.h"
#include "estimators/fepois.h"
#include "estimators/xtreg.h"
#include "vcov/vcov.h"  // hc_meat/cluster_meat/Factor — P5 margins pweight+cluster sandwich (mirrors cmd_logit)

namespace cecon {
namespace cli {

namespace {

// Map a --vcov string to the Vcov enum (shared by feols-path commands in this file).
Vcov parse_vcov(const std::string& s) {
    if (s == "hc1" || s == "HC1") return Vcov::HC1;
    if (s == "cluster") return Vcov::Cluster;
    if (s == "twoway" || s == "multiway") return Vcov::Multiway;
    return Vcov::IID;
}

// True iff a design-column name is an interaction (`a#b`) or a factor level
// (BUG-101 `level.var`, e.g. "1.treated") rather than a bare continuous main
// effect. Used ONLY to route `margins` (P5/BUG-158-159): a design with none of
// these AND no weights/cluster/vce takes the exact old per-column code path
// (margins_logit/probit/poisson), byte-identical to before this fix existed;
// anything else takes the new interaction/factor/weight-aware margins_ame.
bool margins_name_is_factor_or_interaction(const std::string& nm) {
    if (nm.find('#') != std::string::npos) return true;
    size_t dot = nm.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= nm.size()) return false;
    size_t i = (nm[0] == '-') ? 1 : 0;
    if (i >= dot) return false;
    for (size_t j = i; j < dot; ++j)
        if (!std::isdigit(static_cast<unsigned char>(nm[j]))) return false;
    return true;
}

// P7 (`margins, dydx() at()`): true when design column `nm` is DERIVED from base
// variable `var` without being the plain column `var` itself — a factor level
// ("2.treated") or any interaction part ("treated#x1", "1.treated#c.x1"). Used to
// refuse an --at override the already-expanded design cannot honour, instead of
// fixing the main effect and silently leaving the derived terms as observed.
// Segment-aware so `treated` never matches `untreated` or `treated_lag`.
bool margins_name_mentions_var(const std::string& nm, const std::string& var) {
    if (nm == var) return false;
    size_t pos = 0;
    while (pos < nm.size()) {
        size_t hash = nm.find('#', pos);
        std::string seg = nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
        // strip a leading factor/continuous operator: "2.", "i.", "c.", "1b."
        size_t dot = seg.find('.');
        if (dot != std::string::npos) seg = seg.substr(dot + 1);
        if (seg == var) return true;
        if (hash == std::string::npos) break;
        pos = hash + 1;
    }
    return false;
}

// contract_A §31: project a just-completed fit into the storable PostestModel — the
// exact field set lincom/nlcom/test consume. Takes the FeolsResult by rvalue (every
// call site passes the prvalue straight out of feols_fit) and MOVES the three big
// vectors out of it, so the full-specification path pays no copy for being storable.
//
// The vcov travels VERBATIM: this is the single point at which the fit's own
// covariance (whatever --vcov produced) becomes the covariance every later
// handle-addressed post-estimation call will use. Nothing here re-derives, defaults,
// or substitutes it — an `iid` fallback is not reachable from this function.
PostestModel postest_project(FeolsResult&& r) {
    PostestModel m;
    m.names = std::move(r.names);
    m.beta = std::move(r.beta);
    m.vcov_matrix = std::move(r.vcov_matrix);
    m.vcov_type = std::move(r.vcov_type);
    m.n_clusters = r.n_clusters;
    m.dof_resid = r.dof.dof_resid;
    m.n = r.n;
    return m;
}

namespace {

// ---- BUG-203: post-estimation off the model that was ACTUALLY fitted --------
//
// Until this batch `lincom`/`nlcom`/`test` had no `--model` flag at all, so the
// three producers above fed `feols_fit()` unconditionally and a caller who had
// estimated `logit y x1 x2` got the LINEAR-PROBABILITY-MODEL combination back
// with no error. Measured against StataNow 19.5 (n=120, seed 203203):
// `logit ybin x1 x2` then `lincom x1` — Stata 1.087742905079 (se .266346342116),
// this engine 0.221521751160 (se .046231897247), which is exactly what its own
// `regress ybin x1 x2` then `lincom x1` prints. `poisson ybin x1 x2` then
// `lincom x1` returned that SAME 0.221521751160, i.e. the estimator was never
// read at all.
//
// The refit mirrors cmd_margins' GLM branch (further down this file) exactly —
// load_glm + logit_fit / probit_fit / fepois_fit, the same information-matrix
// covariance — because that branch is the in-tree precedent for answering
// post-estimation off a real ML fit, and sharing its machinery is what keeps
// the two surfaces from drifting apart (BUG-188/BUG-194 were about that
// surface).
//
// Fills `out` and returns true; on any refusal it has already written a message
// to stderr and returns false. It refuses rather than approximating: this whole
// function exists to remove a silent wrong answer, so it must not introduce one.
bool postest_fit_glm(const std::string& model, const Dataset& ds, const std::string& y,
                     const std::vector<std::string>& xs, PostestModel& out) {
    if (model != "logit" && model != "probit" && model != "poisson") {
        std::fprintf(stderr,
            "post-estimation --model: '%s' has no maximum-likelihood refit in this engine "
            "(supported: logit, probit, poisson).\n", model.c_str());
        return false;
    }
    GlmInputs g = load_glm(ds, "postest", y, xs);
    if (g.n == 0) {
        std::fprintf(stderr, "post-estimation --model %s: no rows after dropping missing.\n",
                     model.c_str());
        return false;
    }
    if (model == "logit") {
        if (!check_binary(g.y, "lincom/nlcom/test(logit)", y)) return false;
        LogitResult lr = logit_fit(g.X.data(), g.n, g.k, g.y.data());
        if (lr.separated_perfect) {
            std::fprintf(stderr,
                "post-estimation --model logit: outcome %s predicts data perfectly; r(2000)\n",
                y.c_str());
            return false;
        }
        out.beta = lr.beta;
        out.vcov_matrix = lr.bread;  // (X'WX)^{-1} — the OIM asymptotic covariance
    } else if (model == "probit") {
        if (!check_binary(g.y, "lincom/nlcom/test(probit)", y)) return false;
        ProbitResult pr = probit_fit(g.X.data(), g.n, g.k, g.y.data());
        if (pr.separated_perfect) {
            std::fprintf(stderr,
                "post-estimation --model probit: outcome %s predicts data perfectly; r(2000)\n",
                y.c_str());
            return false;
        }
        out.beta = pr.beta;
        // BUG-140's boundary, and the ONE place this refit cannot take the fitter's
        // own `V`. probit is a NON-canonical link, so the Fisher/expected
        // information (X'WX)^{-1} that `pr.V` carries is NOT the covariance Stata's
        // `probit` reports — Stata uses the OBSERVED Hessian, weight lambda(lambda+eta).
        // cmd_probit (cmds_mle.cpp) computes exactly this for its default iid SE;
        // taking `pr.V` here instead made `lincom x1` after `probit` report
        // se .152915189 where the probit table itself (and Stata) say .154831744 —
        // a 1.2% gap, measured, not rounding. Post-estimation MUST reproduce the
        // fit's own covariance, so this is the observed-information bread.
        //
        // This was the THIRD copy of the observed-information loop (cmds_mle.cpp's
        // cmd_probit and cmd_margins' vce(robust) branch below carried the other
        // two), and that triplication is exactly how BUG-210 happened: BUG-140
        // swept one copy and `margins`' iid branches, which had no copy at all and
        // simply read the Fisher r.V, were never revisited. BUG-210 hoisted it into
        // src/estimators/probit.h; this is now a delegation, and so are the others.
        // The SPD fallback to the Fisher bread lives inside that one definition.
        // tests/unit/test_lincom.cpp's test_lincom_ml_refit_bug203 pins this site
        // against the live Stata number, so a divergence here fails a test rather
        // than shipping quietly.
        out.vcov_matrix =
            probit_observed_info(pr, g.X.data(), g.n, g.k, g.y.data()).bread;
    } else {
        for (double vv : g.y) {
            if (vv < 0) {
                std::fprintf(stderr, "post-estimation --model poisson: %s has negatives.\n",
                             y.c_str());
                return false;
            }
        }
        ModelData md;
        md.has_intercept = true;
        md.dm.n = g.n; md.dm.k = g.k;
        md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
        FepoisResult pf = fepois_fit(md, Vcov::IID);
        out.beta = pf.beta;
        out.vcov_matrix = pf.vcov_matrix;
    }
    out.names = g.names;   // x names then "(intercept)" — lincom_parse maps `_cons` onto it
    out.vcov_type = "oim";
    out.n_clusters = 0;
    out.dof_resid = 0;     // an ML fit has none; `asymptotic` below is what the tails read
    out.n = g.n;
    out.asymptotic = true;
    return true;
}

// The reference degrees of freedom / critical value / p-value the shared tails
// below must use for a model. ONE definition, three call sites, so lincom,
// nlcom and test can never disagree about which distribution a fit is
// referenced to. For a least-squares fit these reproduce the pre-BUG-203
// expressions literally (`n_clusters > 0 ? G-1 : dof_resid`, t(dof), t p-value);
// for an ML fit they are the normal, which is what Stata prints after
// logit/probit/poisson.
double postest_dof(const PostestModel& M) {
    if (M.asymptotic) return 0.0;
    return (M.n_clusters > 0) ? (M.n_clusters - 1.0) : static_cast<double>(M.dof_resid);
}
// BUG-333: `level` was hard-wired to 95 here, which is why `lincom, level(90)`
// and `nlcom, level(90)` had nothing to forward to and PIET rejected the option
// outright. The parameter defaults to 95.0 so `test` (Stata's `test` has no
// level()) and every other caller keep their exact previous behaviour; only
// lincom/nlcom pass a user value. alpha is derived the same way cmds_mle.cpp's
// logit does it, so the two surfaces cannot drift apart.
double postest_crit(const PostestModel& M, double dof, double level) {
    const double alpha = (level == 95.0) ? 0.05 : 1.0 - level / 100.0;
    if (M.asymptotic) return z_critical(alpha);
    return (dof > 0) ? t_critical(alpha, dof) : 0.0;
}
double postest_pvalue(const PostestModel& M, double t, double dof) {
    if (M.asymptotic) return z_twosided_pvalue(t);
    return (dof > 0) ? t_twosided_pvalue(t, dof) : 1.0;
}

// Fit the `--data/--y/--x[/--fe/--vcov/--cluster]` specification the way the
// caller's ESTIMATOR would have. Reads every model-spec flag unconditionally so
// each one is marked consumed regardless of which branch runs (the E6
// unconsumed-flag net would otherwise fire on the branch that does not read it).
// Returns 0 on success, or the CLI exit code to return on failure.
//
// `--model` absent (or naming the least-squares family) keeps the historical
// path byte-for-byte: same build_model, same parse_vcov default, same
// feols_fit. This is what every existing eval case, freeze fixture and PIET
// call after `regress`/`reghdfe` takes.
int postest_fit_spec(const std::vector<std::string>& a, const Dataset& ds, const std::string& y,
                     const std::string& xcsv, PostestModel& fitted) {
    const std::string model = opt(a, "--model");
    const std::string fe = opt(a, "--fe");
    const std::string vcov_type = opt(a, "--vcov", "iid");
    const std::string cluster = opt(a, "--cluster");
    const bool ols_family = model.empty() || model == "ols" || model == "feols" ||
                            model == "regress" || model == "reg";
    if (!ols_family) {
        // The ML refit is the plain OIM fit with no absorbed effects and no
        // alternative VCE — exactly what cmd_margins' GLM branch supports.
        // Honouring the flag while ignoring --fe/--vcov/--cluster would answer
        // under a specification the model was not fitted under, which is the
        // very shape this fix removes, so it refuses instead.
        if (!fe.empty() || !cluster.empty() || (vcov_type != "iid" && vcov_type != "oim")) {
            std::fprintf(stderr,
                "post-estimation --model %s: fixed effects and non-default vcov are not "
                "supported for a maximum-likelihood refit in this engine (got"
                "%s%s%s%s%s%s).\n",
                model.c_str(),
                fe.empty() ? "" : " --fe ", fe.empty() ? "" : fe.c_str(),
                cluster.empty() ? "" : " --cluster ", cluster.empty() ? "" : cluster.c_str(),
                (vcov_type == "iid" || vcov_type == "oim") ? "" : " --vcov ",
                (vcov_type == "iid" || vcov_type == "oim") ? "" : vcov_type.c_str());
            return 2;
        }
        if (!postest_fit_glm(model, ds, y, split_csv(xcsv), fitted)) return 2;
        return 0;
    }
    // BUG-369: the refit re-parses the ORIGINAL varlist, so an `L.x` regressor
    // needs the tsset/xtset metadata the FIT was given — .hbin does not persist
    // it. Without this, `regress y L.x` fitted fine and `test L.x` on the very
    // same model died with ts_no_tsset. Empty timevar (no flags) keeps every
    // existing caller's design byte-identical, exactly as the estimators do.
    const cecon::TsMeta tsmeta = parse_tsmeta(a);
    ModelData md = build_model(ds, y, split_csv(xcsv), split_csv(fe), split_csv(cluster),
                               /*offset_name=*/"", /*drop_singleton=*/true,
                               tsmeta.timevar.empty() ? nullptr : &tsmeta);
    fitted = postest_project(feols_fit(md, parse_vcov(vcov_type)));
    return 0;
}

}  // namespace

// Parse a linear-form side ("2*x1 + x2 - 3") into coef-name multipliers + a
// constant. Multipliers accumulate into coef[name]; bare numbers into konst.
// Returns false on a parse error.
bool parse_linear_side(const std::string& side,
                       std::unordered_map<std::string, double>& coef,
                       double& konst, std::string& err) {
    std::string s;
    for (char c : side) if (c != ' ' && c != '\t') s += c;
    if (s.empty()) return true;
    size_t i = 0;
    int sign = 1;
    if (s[0] == '+') { i = 1; } else if (s[0] == '-') { sign = -1; i = 1; }
    while (i < s.size()) {
        // read one term up to the next +/- (not inside a number's exponent — none here)
        size_t j = i;
        while (j < s.size() && s[j] != '+' && s[j] != '-') ++j;
        std::string term = s.substr(i, j - i);
        if (term.empty()) { err = "empty term in '" + side + "'"; return false; }
        // split term into optional numeric coefficient (with optional '*') and a name
        size_t p = 0;
        while (p < term.size() && (std::isdigit((unsigned char)term[p]) || term[p] == '.')) ++p;
        double mult = 1.0;
        std::string name;
        if (p == term.size()) {            // pure number → constant
            konst += sign * std::stod(term);
        } else {
            if (p > 0) mult = std::stod(term.substr(0, p));
            if (term[p] == '*') ++p;       // optional '*'
            name = term.substr(p);
            if (name.empty()) { err = "bad term '" + term + "'"; return false; }
            coef[name] += sign * mult;
        }
        // next sign
        if (j < s.size()) { sign = (s[j] == '-') ? -1 : 1; i = j + 1; }
        else i = j;
    }
    return true;
}

// Build R (J*k row-major) and q (length J) from --joint names and --constraint
// equations, against the fitted coefficient names. Returns false on error.
bool build_restrictions(const std::vector<std::string>& names,
                        const std::vector<std::string>& joint,
                        const std::vector<std::string>& constraints,
                        std::vector<double>& R, std::vector<double>& q,
                        int& J, std::string& err) {
    int k = static_cast<int>(names.size());
    auto idx = [&](const std::string& nm) -> int {
        for (int c = 0; c < k; ++c) if (names[static_cast<size_t>(c)] == nm) return c;
        return -1;
    };
    std::vector<std::vector<double>> rows;
    std::vector<double> qs;
    for (const auto& nm : joint) {
        int c = idx(nm);
        if (c < 0) { err = "test: variable '" + nm + "' not among regressors"; return false; }
        std::vector<double> row(static_cast<size_t>(k), 0.0);
        row[static_cast<size_t>(c)] = 1.0;
        rows.push_back(std::move(row));
        qs.push_back(0.0);
    }
    for (const auto& con : constraints) {
        size_t eq = find_assign(con);
        if (eq == std::string::npos) { err = "test: constraint '" + con + "' has no '='"; return false; }
        std::unordered_map<std::string, double> lhs, rhs;
        double kl = 0, kr = 0;
        if (!parse_linear_side(con.substr(0, eq), lhs, kl, err)) return false;
        if (!parse_linear_side(con.substr(eq + 1), rhs, kr, err)) return false;
        std::vector<double> row(static_cast<size_t>(k), 0.0);
        for (auto& [nm, m] : lhs) {
            int c = idx(nm); if (c < 0) { err = "test: variable '" + nm + "' not among regressors"; return false; }
            row[static_cast<size_t>(c)] += m;
        }
        for (auto& [nm, m] : rhs) {
            int c = idx(nm); if (c < 0) { err = "test: variable '" + nm + "' not among regressors"; return false; }
            row[static_cast<size_t>(c)] -= m;
        }
        rows.push_back(std::move(row));
        qs.push_back(kr - kl);  // lhs - rhs = 0 → R β = (kr - kl)
    }
    J = static_cast<int>(rows.size());
    if (J == 0) { err = "test: need --joint or --constraint"; return false; }
    R.assign(static_cast<size_t>(J) * k, 0.0);
    q.assign(static_cast<size_t>(J), 0.0);
    for (int r = 0; r < J; ++r) {
        for (int c = 0; c < k; ++c) R[static_cast<size_t>(r) * k + c] = rows[static_cast<size_t>(r)][static_cast<size_t>(c)];
        q[static_cast<size_t>(r)] = qs[static_cast<size_t>(r)];
    }
    return true;
}

// standard normal CDF + inverse (for the mediation z-test and BC bootstrap cutoffs).
double med_ncdf(double x) { return 0.5 * std::erfc(-x * 0.7071067811865476); }
double med_ninv(double p) {
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
    if (p <= 0) return -1e300; if (p >= 1) return 1e300;
    double plow = 0.02425, phigh = 1 - plow, q, r, x;
    if (p < plow) { q = std::sqrt(-2 * std::log(p)); x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1); }
    else if (p <= phigh) { q = p - 0.5; r = q * q; x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q / (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1); }
    else { q = std::sqrt(-2 * std::log(1 - p)); x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1); }
    double e = med_ncdf(x) - p, u = e * 2.5066282746310002 * std::exp(0.5 * x * x);
    return x - u / (1 + 0.5 * x * u);
}

}  // namespace

// mediation: product-of-coefficients indirect effect a*b on a COMMON (complete-case)
// sample. M-model: M = a*X + controls; Y-model: Y = c'*X + b*M + controls. Reports
// Sobel/Aroian/Goodman delta SEs and a Philox bootstrap (percentile + BC) CI of a*b.
int cmd_mediate(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), med = opt(a, "--mediator"), x = opt(a, "--treat");
    if (data_missing(data) || y.empty() || med.empty() || x.empty()) {
        std::fprintf(stderr, "usage: cobra mediate --data f.hbin --y Y --mediator M --treat X "
                             "[--controls c1,c2] [--reps 1000] [--seed 12345] [--level 95] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<std::string> ctrls = split_csv(opt(a, "--controls"));
    int reps = std::stoi(opt(a, "--reps", "1000"));
    if (reps < 1) { std::fprintf(stderr, "mediate: --reps must be >= 1.\n"); return 2; }
    uint64_t seed = static_cast<uint64_t>(std::stoll(opt(a, "--seed", "12345")));
    double level = std::stod(opt(a, "--level", "95"));
    double alpha = 1.0 - level / 100.0;

    const Dataset& ds = acquire_data(data);
    const double* yr = require_num(ds, "mediate", y);
    const double* mr = require_num(ds, "mediate", med);
    const double* xr = require_num(ds, "mediate", x);
    std::vector<const double*> cr;
    for (const auto& c : ctrls) cr.push_back(require_num(ds, "mediate", c));
    int64_t nraw = ds.n_rows();
    std::vector<const double*> all = {yr, mr, xr};
    for (auto* p : cr) all.push_back(p);
    auto keep = listwise_keep(all, nraw);
    const int64_t n = static_cast<int64_t>(keep.size());
    if (n == 0) { std::fprintf(stderr, "mediate: no rows after dropping missing.\n"); return 1; }

    auto Y = gather_rows(yr, keep), M = gather_rows(mr, keep), Xv = gather_rows(xr, keep);
    std::vector<std::vector<double>> C;
    for (auto* p : cr) C.push_back(gather_rows(p, keep));
    const int nc = static_cast<int>(ctrls.size());

    // M-design [X, controls.., 1] (X at col 0); Y-design [X, M, controls.., 1] (X@0, M@1)
    const int kM = 1 + nc + 1, kY = 2 + nc + 1;
    std::vector<double> DM(static_cast<size_t>(n) * kM), DY(static_cast<size_t>(n) * kY);
    for (int64_t i = 0; i < n; ++i) { DM[i] = Xv[i]; DM[static_cast<int64_t>(kM - 1) * n + i] = 1.0; }
    for (int c = 0; c < nc; ++c) for (int64_t i = 0; i < n; ++i) DM[static_cast<int64_t>(1 + c) * n + i] = C[c][i];
    for (int64_t i = 0; i < n; ++i) { DY[i] = Xv[i]; DY[n + i] = M[i]; DY[static_cast<int64_t>(kY - 1) * n + i] = 1.0; }
    for (int c = 0; c < nc; ++c) for (int64_t i = 0; i < n; ++i) DY[static_cast<int64_t>(2 + c) * n + i] = C[c][i];

    auto ols = [&](const std::vector<double>& D, int k, const std::vector<double>& yv,
                   const int64_t* idx, int64_t m, std::vector<double>& beta, std::vector<double>* sd) -> bool {
        std::vector<double> XtX(static_cast<size_t>(k) * k, 0.0), Xty(k, 0.0);
        for (int64_t ii = 0; ii < m; ++ii) {
            int64_t r = idx ? idx[ii] : ii;
            for (int p = 0; p < k; ++p) {
                double xp = D[static_cast<int64_t>(p) * n + r];
                Xty[p] += xp * yv[r];
                for (int q = p; q < k; ++q) XtX[p + q * k] += xp * D[static_cast<int64_t>(q) * n + r];
            }
        }
        for (int p = 0; p < k; ++p) for (int q = 0; q < p; ++q) XtX[p + q * k] = XtX[q + p * k];
        std::vector<double> Inv = XtX;
        if (la::spd_inverse(Inv.data(), k) != 0) return false;
        beta.resize(k);
        la::matvec(Inv.data(), k, Xty.data(), beta.data());
        if (sd) {
            double ssr = 0;
            for (int64_t ii = 0; ii < m; ++ii) {
                int64_t r = idx ? idx[ii] : ii;
                double e = yv[r]; for (int p = 0; p < k; ++p) e -= D[static_cast<int64_t>(p) * n + r] * beta[p];
                ssr += e * e;
            }
            double s2 = ssr / static_cast<double>(m - k);
            sd->resize(k);
            for (int p = 0; p < k; ++p) (*sd)[p] = std::sqrt(s2 * Inv[p + p * k]);
        }
        return true;
    };

    std::vector<double> bM, sM, bY, sY;
    if (!ols(DM, kM, M, nullptr, n, bM, &sM) || !ols(DY, kY, Y, nullptr, n, bY, &sY) ||
        static_cast<int>(bM.size()) < kM || static_cast<int>(sM.size()) < kM ||
        static_cast<int>(bY.size()) < kY || static_cast<int>(sY.size()) < kY) {
        std::fprintf(stderr, "mediate: regression failed (singular design — collinear controls, constant treatment, or too few observations).\n");
        return 1;
    }
    double av = bM[0], se_a = sM[0], bv = bY[1], se_b = sY[1], cp = bY[0];
    double indirect = av * bv, total = cp + indirect;
    double va = se_a * se_a, vb = se_b * se_b;
    double sobel = std::sqrt(av * av * vb + bv * bv * va);
    double aroian = std::sqrt(av * av * vb + bv * bv * va + va * vb);
    double gd = av * av * vb + bv * bv * va - va * vb;
    double goodman = (gd > 0) ? std::sqrt(gd) : std::nan("");
    double zc = med_ninv(1 - alpha / 2);
    double zstat = (sobel > 0) ? indirect / sobel : 0.0;

    // Philox pairs bootstrap of a*b
    std::vector<double> abr(reps);
    std::vector<int64_t> idx(n);
    std::vector<double> rb1, rb2;
    for (int rep = 0; rep < reps; ++rep) {
        for (int64_t g = 0; g < n; ++g) idx[g] = Philox::draw32(seed, static_cast<uint64_t>(rep), static_cast<uint64_t>(g)) % static_cast<uint64_t>(n);
        if (ols(DM, kM, M, idx.data(), n, rb1, nullptr) && ols(DY, kY, Y, idx.data(), n, rb2, nullptr))
            abr[rep] = rb1[0] * rb2[1];
        else abr[rep] = std::nan("");
    }
    std::sort(abr.begin(), abr.end());
    auto pctl = [&](double pp) -> double {
        double rr = pp * (reps - 1);
        int lo = static_cast<int>(std::floor(rr));
        if (lo < 0) return abr.front();
        if (lo >= reps - 1) return abr.back();
        return abr[lo] + (rr - lo) * (abr[lo + 1] - abr[lo]);
    };
    int cntlt = 0; for (double v : abr) if (v < indirect) ++cntlt;
    double z0 = med_ninv(static_cast<double>(cntlt) / reps);
    double zq = med_ninv(1 - alpha / 2);

    json out = {
        {"estimator", "mediate"}, {"n_obs", n},
        {"a", av}, {"se_a", se_a}, {"b", bv}, {"se_b", se_b},
        {"indirect", indirect}, {"direct", cp}, {"total", total},
        {"proportion_mediated", (total != 0) ? indirect / total : std::nan("")},
        {"se_sobel", sobel}, {"se_aroian", aroian}, {"se_goodman", goodman},
        {"z", zstat}, {"p", z_twosided_pvalue(zstat)},
        {"ci_lo", indirect - zc * sobel}, {"ci_hi", indirect + zc * sobel},
        {"bootstrap", {{"reps", reps}, {"seed", seed},
                       {"ci_lo_pct", pctl(alpha / 2)}, {"ci_hi_pct", pctl(1 - alpha / 2)},
                       {"ci_lo_bc", pctl(med_ncdf(2 * z0 - zq))}, {"ci_hi_bc", pctl(med_ncdf(2 * z0 + zq))}}},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

// nlcom: nonlinear combinations of fitted coefficients with delta-method SE.
// Var(g) = G' V G, G = numeric central-difference Jacobian at beta-hat (matches
// Stata nlcom/testnl). t(resid-dof) [or t(G-1) under cluster] like regress.
// Both entry forms of §31 apply here too — see cmd_lincom's note. The delta-method
// Jacobian is taken at the STORED beta and contracted with the STORED V, so a
// handle-addressed nlcom reproduces the fitting one exactly.
// BUG-333: shared --level parse for lincom/nlcom. Range matches every other
// cobra surface ([10, 99.99], cmds_mle.cpp:296) so a user sees one rule.
// Returns false and prints on a bad value; the caller returns 2.
bool postest_level(const std::vector<std::string>& a, const char* who, double* level) {
    *level = 95.0;
    if (!has(a, "--level")) return true;
    *level = std::stod(opt(a, "--level", "95"));
    if (*level < 10.0 || *level > 99.99) {
        std::fprintf(stderr, "%s: --level must be in [10, 99.99] (got %s).\n", who,
                     opt(a, "--level").c_str());
        return false;
    }
    return true;
}

int cmd_nlcom(const std::vector<std::string>& a) {
    const PostestModel* stored = serve_active_model();
    PostestModel fitted;
    if (!stored) {
        std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
        if (data_missing(data) || y.empty() || xcsv.empty()) {
            std::fprintf(stderr,
                "usage: cobra nlcom --data f.hbin --y Y --x X1,X2,... [--fe F] [--vcov ...] [--cluster C] "
                "[--model logit|probit|poisson] "
                "[--keepif EXPR] [--level P] --expr \"name=f(_b[X1],_b[X2],..)\" [--expr ...]\n"
                "  e.g. quadratic turning point:  --expr \"turn=-_b[time]/(2*_b[time2])\"\n");
            return 2;
        }
        data = maybe_keepif(data, opt(a, "--keepif"));
        const Dataset& ds = acquire_data(data);
        if (int rc = postest_fit_spec(a, ds, y, xcsv, fitted)) return rc;  // BUG-203
        serve_publish_model(fitted);
    }
    const PostestModel& M = stored ? *stored : fitted;

    std::vector<std::string> exprs = opt_all(a, "--expr");
    if (exprs.empty()) { std::fprintf(stderr, "nlcom: need at least one --expr\n"); return 2; }

    const double dof = postest_dof(M);   // BUG-203: 0 (normal reference) on an ML fit
    double nlcom_level = 95.0;
    if (!postest_level(a, "nlcom", &nlcom_level)) return 2;
    const double tc = postest_crit(M, dof, nlcom_level);

    std::vector<NlcomTerm> terms;
    try { terms = nlcom_apply(M.names, M.beta, M.vcov_matrix, exprs); }
    catch (const std::exception& ex) { std::fprintf(stderr, "%s\n", ex.what()); return 2; }

    json arr = json::array();
    for (const auto& tm : terms) {
        double t = (tm.se > 0) ? tm.est / tm.se : 0.0;
        arr.push_back({{"name", tm.name}, {"b", tm.est}, {"se", tm.se}, {"t", t}, {"df", dof},
                       {"p", (tm.se > 0) ? postest_pvalue(M, t, dof) : 1.0},
                       {"ci_lo", tm.est - tc * tm.se}, {"ci_hi", tm.est + tc * tm.se}});
    }
    json out = {{"estimator", "nlcom"}, {"coefs", arr},
                {"vcov_type", M.vcov_type}, {"n_obs", M.n}, {"df", dof}};
    emit(out, opt(a, "--out"));
    return 0;
}

namespace {

// Parse a LINEAR expression over coefficient names into a weight vector + offset.
// Supports terms "[c*]name", bare "name" (weight 1), and pure numeric constants;
// "_cons" maps to the intercept. Top-level +/- separate terms (linear ⇒ no parens).
// Only used by cmd_lincom() below (same TU) — no external linkage needed.
bool lincom_parse(const std::string& expr, const std::vector<std::string>& names,
                  std::vector<double>& w, double& offset, std::string& err) {
    w.assign(names.size(), 0.0);
    offset = 0.0;
    auto coef_index = [&](const std::string& nm) -> int {
        std::string n = (nm == "_cons" || nm == "cons") ? "(intercept)" : nm;
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == n) return static_cast<int>(i);
        return -1;
    };
    std::string s;
    for (char c : expr) if (!std::isspace(static_cast<unsigned char>(c))) s += c;  // strip spaces
    if (s.empty()) { err = "empty expression"; return false; }
    size_t i = 0;
    while (i < s.size()) {
        double sign = 1.0;
        if (s[i] == '+') { ++i; }
        else if (s[i] == '-') { sign = -1.0; ++i; }
        // read a term up to the next top-level + or -
        size_t j = i;
        while (j < s.size() && s[j] != '+' && s[j] != '-') ++j;
        std::string term = s.substr(i, j - i);
        i = j;
        if (term.empty()) { err = "malformed expression near operator"; return false; }
        auto star = term.find('*');
        std::string numpart, varpart;
        if (star != std::string::npos) { numpart = term.substr(0, star); varpart = term.substr(star + 1); }
        else {
            // bare term: a pure number (constant) or a bare coefficient name
            char* end = nullptr;
            double v = std::strtod(term.c_str(), &end);
            if (end && *end == '\0') { offset += sign * v; continue; }  // numeric constant
            varpart = term;  // bare coefficient name (weight 1)
        }
        double coef = 1.0;
        if (!numpart.empty()) {
            char* end = nullptr;
            coef = std::strtod(numpart.c_str(), &end);
            if (!end || *end != '\0') { err = "bad coefficient '" + numpart + "'"; return false; }
        }
        int idx = coef_index(varpart);
        if (idx < 0) { err = "unknown coefficient '" + varpart + "'"; return false; }
        w[static_cast<size_t>(idx)] += sign * coef;
    }
    return true;
}

}  // namespace

// lincom — linear combination of fitted coefficients (exact analytic SE).
//
// TWO ways in, ONE arithmetic tail (contract_A §31):
//   (a) the full model specification (--data/--y/--x/--fe/--vcov/--cluster) — reads the
//       dataset and FITS, exactly as it always has; then publishes the fit so a serve
//       session can mint a handle for it;
//   (b) a session model handle (a `postest` serve request) — the stored fit's own beta
//       and vcov answer the combination directly. NO dataset read, NO re-fit.
// Form (b) exists because form (a) was re-reading a 164 MB dataset and re-running HDFE
// to answer one r'β that had already been computed: 54 calls x ~0.25 s per dependent
// variable in the real analysis pass.
//
// The tail below is literally shared by both forms, so a handle-addressed answer is
// byte-identical to the re-fitting one by construction rather than by re-verification.
int cmd_lincom(const std::vector<std::string>& a) {
    std::vector<std::string> exprs = opt_all(a, "--exp");
    const PostestModel* stored = serve_active_model();  // non-null only under `postest`
    PostestModel fitted;
    if (!stored) {
        std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
        if (data_missing(data) || y.empty() || xcsv.empty() || exprs.empty()) {
            std::fprintf(stderr,
                "usage: cobra lincom --data f.hbin --y Y --x X1,X2,... [--fe F] [--vcov ...] [--cluster C] "
                "[--model logit|probit|poisson] "
                "[--keepif EXPR] [--level P] --exp \"X1 + X2\" [--exp \"2*X1 - X3\"]\n"
                "  linear combination r'β with exact SE sqrt(r'Vr); bare names = coefficients, _cons = intercept.\n");
            return 2;
        }
        data = maybe_keepif(data, opt(a, "--keepif"));
        const Dataset& ds = acquire_data(data);
        if (int rc = postest_fit_spec(a, ds, y, xcsv, fitted)) return rc;  // BUG-203
        serve_publish_model(fitted);  // frame-mode only; no-op one-shot / stateless §25
    } else if (exprs.empty()) {
        std::fprintf(stderr, "lincom: --exp is required.\n");
        return 2;
    }
    // The model spec flags are deliberately NOT read on the handle path, so passing
    // both a handle and (say) --vcov is caught by the E6 unconsumed-flag net as an
    // honest error instead of one of the two silently winning.
    const PostestModel& M = stored ? *stored : fitted;
    const int k = static_cast<int>(M.names.size());
    const double dof = postest_dof(M);   // BUG-203: 0 (normal reference) on an ML fit
    double lincom_level = 95.0;
    if (!postest_level(a, "lincom", &lincom_level)) return 2;
    const double tc = postest_crit(M, dof, lincom_level);

    json arr = json::array();
    for (const std::string& e : exprs) {
        std::vector<double> w;
        double offset;
        std::string err;
        if (!lincom_parse(e, M.names, w, offset, err)) { std::fprintf(stderr, "lincom: %s\n", err.c_str()); return 2; }
        double est = offset, var = 0;
        for (int p = 0; p < k; ++p) est += w[p] * M.beta[p];
        // r'Vr against the model's OWN covariance — the fitted hc1/cluster/multiway V,
        // never a re-derived or defaulted one (BS-098 / BS-013).
        for (int p = 0; p < k; ++p) {
            double row = 0;
            for (int q = 0; q < k; ++q) row += w[q] * M.vcov_matrix[static_cast<size_t>(q) * k + p];
            var += w[p] * row;
        }
        double se = std::sqrt(std::max(0.0, var));
        double t = (se > 0) ? est / se : 0.0;
        arr.push_back({{"exp", e}, {"b", est}, {"se", se}, {"t", t}, {"df", dof},
                       {"p", (se > 0) ? postest_pvalue(M, t, dof) : 1.0},
                       {"ci_lo", est - tc * se}, {"ci_hi", est + tc * se}});
    }
    json out = {{"estimator", "lincom"}, {"coefs", arr}, {"vcov_type", M.vcov_type},
                {"n_obs", M.n}, {"df", dof}};
    emit(out, opt(a, "--out"));
    return 0;
}

// Both entry forms of §31 apply here too — see cmd_lincom's note.
int cmd_test(const std::vector<std::string>& a) {
    const PostestModel* stored = serve_active_model();
    PostestModel fitted;
    if (!stored) {
        std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
        if (data_missing(data) || y.empty() || xcsv.empty()) {
            std::fprintf(stderr,
                "usage: cobra test --data f.hbin --y Y --x X1,X2,... "
                "[--fe F] [--vcov iid|hc1|cluster|twoway] [--cluster C] "
                "[--model logit|probit|poisson] [--keepif EXPR] "
                "(--joint a,b,c | --constraint \"a=b\" [--constraint \"a+b=1\" ...])\n");
            return 2;
        }
        data = maybe_keepif(data, opt(a, "--keepif"));
        const Dataset& ds = acquire_data(data);
        if (int rc = postest_fit_spec(a, ds, y, xcsv, fitted)) return rc;  // BUG-203
        serve_publish_model(fitted);
    }
    const PostestModel& M = stored ? *stored : fitted;

    std::vector<std::string> joint = split_csv(opt(a, "--joint"));
    std::vector<std::string> constraints = opt_all(a, "--constraint");
    std::vector<double> R, q; int J = 0; std::string err;
    if (!build_restrictions(M.names, joint, constraints, R, q, J, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    WaldResult w = wald_test(M.beta.data(), M.vcov_matrix.data(),
                             static_cast<int>(M.names.size()), R.data(), q.data(), J);
    if (!w.ok) { std::fprintf(stderr, "test: %s\n", w.error.c_str()); return 1; }
    // Same cluster (G-1) branch as lincom/nlcom's `dof` above: when the fitted
    // model's VCE is clustered, Stata's F-reference dof is F(J, G-1), not the
    // residual dof. (No eval golden pinned the old dof_resid-unconditional
    // behavior under a clustered fit — grepped tests/ before changing; every
    // pinned `test` case is vcov_type "iid" with n_clusters 0/absent, so those
    // stay byte-identical here.)
    double df2 = postest_dof(M);
    json restr = json::array();
    for (const auto& nm : joint) restr.push_back(nm + " = 0");
    for (const auto& c : constraints) restr.push_back(c);
    // BUG-203: an ML fit has no residual dof, so F(J, df2) has no reference
    // distribution to be read against — Stata prints chi2(J) there and nothing
    // else. Emitting an F with df2 = 0 would be a number no consumer could use
    // honestly, so the F/df2 triple is simply absent on that path (both
    // consumers are `contains`-guarded: PIET's kPassthrough copy, and
    // eval/macros.cpp's r() map, whose "F or chi2" guard already anticipates a
    // chi2-only block). The least-squares branch keeps its historical key ORDER
    // as well as its values — `json` here is nlohmann::ordered_json, so the
    // emitted bytes are unchanged for every existing caller.
    json out = M.asymptotic
        ? json{
            {"estimator", "test"},
            {"restrictions", restr},
            {"J", J}, {"df1", J},
            {"chi2", w.chi2}, {"p_chi2", chi2_pvalue(w.chi2, J)},
            {"vcov_type", M.vcov_type}, {"n_obs", M.n},
          }
        : json{
            {"estimator", "test"},
            {"restrictions", restr},
            {"J", J}, {"df1", J}, {"df2", df2},
            {"chi2", w.chi2}, {"p_chi2", chi2_pvalue(w.chi2, J)},
            {"F", w.F}, {"p_F", f_pvalue(w.F, J, df2)},
            {"vcov_type", M.vcov_type}, {"n_obs", M.n},
          };
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_chow(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string by = opt(a, "--by");
    if (data_missing(data) || y.empty() || xcsv.empty() || by.empty()) {
        std::fprintf(stderr,
            "usage: cobra chow --data f.hbin --y Y --x X1,X2 --by GROUP "
            "[--vcov hc1|cluster --cluster C] [--keepif EXPR] [--out f.json]\n"
            "  classic homoskedastic SSR F + heteroskedasticity-robust Wald on interactions\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "chow", y);
    const double* graw = require_num(ds, "chow", by);
    auto xs = split_csv(xcsv);
    int kx = static_cast<int>(xs.size());
    std::vector<const double*> xraw(kx);
    for (int c = 0; c < kx; ++c) xraw[c] = require_num(ds, "chow", xs[c]);
    // shared listwise row set over (y, group, x...)
    std::vector<const double*> all = {yraw, graw};
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    if (keep.empty()) { std::fprintf(stderr, "chow: no rows after dropping missing.\n"); return 1; }
    int64_t n = static_cast<int64_t>(keep.size());
    auto yv = gather_rows(yraw, keep);
    auto gv = gather_rows(graw, keep);
    std::vector<std::vector<double>> xv(kx);
    for (int c = 0; c < kx; ++c) xv[c] = gather_rows(xraw[c], keep);
    // distinct groups in first-appearance order
    std::vector<double> levels;
    std::vector<int> gcode(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        double gval = gv[static_cast<size_t>(i)];
        int code = -1;
        for (size_t l = 0; l < levels.size(); ++l) if (levels[l] == gval) { code = static_cast<int>(l); break; }
        if (code < 0) { code = static_cast<int>(levels.size()); levels.push_back(gval); }
        gcode[static_cast<size_t>(i)] = code;
    }
    int G = static_cast<int>(levels.size());
    if (G < 2) { std::fprintf(stderr, "chow: --by '%s' has < 2 groups.\n", by.c_str()); return 2; }
    int kc = kx + 1;  // regressors incl. intercept

    // Helper: build a DesignMatrix [x..., 1] for a row subset and OLS-fit it, return SSR.
    auto fit_ssr = [&](const std::vector<int64_t>& rows, bool& ok) -> double {
        int64_t m = static_cast<int64_t>(rows.size());
        if (m <= kc) { ok = false; return 0.0; }
        DesignMatrix dm; dm.n = m; dm.k = kc; dm.X.assign(static_cast<size_t>(m) * kc, 0.0);
        dm.y.assign(static_cast<size_t>(m), 0.0);
        for (int c = 0; c < kx; ++c) { dm.xnames.push_back(xs[static_cast<size_t>(c)]);
            for (int64_t i = 0; i < m; ++i) dm.X[static_cast<size_t>(c) * m + i] = xv[static_cast<size_t>(c)][static_cast<size_t>(rows[static_cast<size_t>(i)])]; }
        dm.xnames.push_back("(intercept)");
        for (int64_t i = 0; i < m; ++i) { dm.X[static_cast<size_t>(kx) * m + i] = 1.0; dm.y[static_cast<size_t>(i)] = yv[static_cast<size_t>(rows[static_cast<size_t>(i)])]; }
        OlsResult r = ols_fit(dm);
        ok = true; return r.ssr;
    };
    std::vector<int64_t> allrows(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) allrows[static_cast<size_t>(i)] = i;
    bool ok_p = true;
    double ssr_p = fit_ssr(allrows, ok_p);
    // One bucketing pass replaces one full-column rescan per group. Each bucket ends up
    // in ascending row order — exactly the order the per-group scan produced — so
    // fit_ssr sees the same rows in the same sequence and returns the same SSR bit for
    // bit. Rows whose group code is out of range are excluded here just as the old
    // `gcode[i] == g` test excluded them.
    std::vector<std::vector<int64_t>> grp_rows(static_cast<size_t>(G));
    for (int64_t i = 0; i < n; ++i) {
        int gi = gcode[static_cast<size_t>(i)];
        if (gi >= 0 && gi < G) grp_rows[static_cast<size_t>(gi)].push_back(i);
    }
    double ssr_g = 0; bool ok_g = true; std::vector<int> gsize(static_cast<size_t>(G), 0);
    for (int g = 0; g < G; ++g) {
        const std::vector<int64_t>& rows = grp_rows[static_cast<size_t>(g)];
        gsize[static_cast<size_t>(g)] = static_cast<int>(rows.size());
        bool ok; double s = fit_ssr(rows, ok);
        if (!ok) { ok_g = false; } else ssr_g += s;
    }
    json classic;
    if (ok_p && ok_g) {
        double df1 = static_cast<double>((G - 1) * kc);
        double df2 = static_cast<double>(n - static_cast<int64_t>(G) * kc);
        double F = ((ssr_p - ssr_g) / df1) / (ssr_g / df2);
        classic = {{"F", F}, {"df1", df1}, {"df2", df2}, {"p", f_pvalue(F, df1, df2)},
                   {"ssr_pooled", ssr_p}, {"ssr_groups", ssr_g}, {"ok", true}};
    } else {
        classic = {{"ok", false}, {"error", "a group has too few observations (n_g <= k)"}};
    }

    // Robust: augmented design [x..., (G-1) group dummies, (G-1)*kx interactions, 1],
    // feols (no FE) with --vcov, Wald-test all dummy+interaction terms jointly = 0.
    json robust;
    {
        std::string vcov_type = opt(a, "--vcov", "hc1");
        int n_extra = (G - 1) * (kx + 1);          // dummies + interactions
        int kk = kx + n_extra + 1;                  // + intercept
        DesignMatrix dm; dm.n = n; dm.k = kk;
        dm.X.assign(static_cast<size_t>(n) * kk, 0.0); dm.y.assign(static_cast<size_t>(n), 0.0);
        int col = 0;
        for (int c = 0; c < kx; ++c) { dm.xnames.push_back(xs[static_cast<size_t>(c)]);
            for (int64_t i = 0; i < n; ++i) dm.X[static_cast<size_t>(col) * n + i] = xv[static_cast<size_t>(c)][static_cast<size_t>(i)];
            ++col; }
        std::vector<int> test_cols;
        // Each dummy/interaction column is nonzero only on its own group's rows, and
        // dm.X was already assign()ed to 0.0, so writing just the members reproduces the
        // matrix the old row-wise ternary built — the same cells, the same values —
        // while touching O(n) cells per group instead of O(n) per group PER column.
        for (int g = 1; g < G; ++g) {  // baseline = group 0
            const std::vector<int64_t>& rows = grp_rows[static_cast<size_t>(g)];
            dm.xnames.push_back("g" + std::to_string(g));
            for (int64_t i : rows) dm.X[static_cast<size_t>(col) * n + i] = 1.0;
            test_cols.push_back(col); ++col;
            for (int c = 0; c < kx; ++c) {
                dm.xnames.push_back("g" + std::to_string(g) + "_x" + std::to_string(c));
                for (int64_t i : rows) dm.X[static_cast<size_t>(col) * n + i] =
                    xv[static_cast<size_t>(c)][static_cast<size_t>(i)];
                test_cols.push_back(col); ++col;
            }
        }
        dm.xnames.push_back("(intercept)");
        for (int64_t i = 0; i < n; ++i) { dm.X[static_cast<size_t>(col) * n + i] = 1.0; dm.y[static_cast<size_t>(i)] = yv[static_cast<size_t>(i)]; }
        ModelData md; md.dm = std::move(dm); md.has_intercept = true;
        if (vcov_type == "cluster" || vcov_type == "twoway") {
            // cluster needs the factor; fall back to hc1 unless user wired clusters via build_model — keep simple: hc1.
            vcov_type = "hc1";
        }
        FeolsResult r = feols_fit(md, parse_vcov(vcov_type));
        int J = static_cast<int>(test_cols.size());
        std::vector<double> Rm(static_cast<size_t>(J) * r.names.size(), 0.0);
        std::vector<double> qz(static_cast<size_t>(J), 0.0);
        for (int j = 0; j < J; ++j) Rm[static_cast<size_t>(j) * r.names.size() + test_cols[static_cast<size_t>(j)]] = 1.0;
        WaldResult w = wald_test(r.beta.data(), r.vcov_matrix.data(),
                                 static_cast<int>(r.names.size()), Rm.data(), qz.data(), J);
        double df2 = static_cast<double>(r.dof.dof_resid);
        if (w.ok)
            robust = {{"chi2", w.chi2}, {"p_chi2", chi2_pvalue(w.chi2, J)},
                      {"F", w.F}, {"df1", J}, {"df2", df2}, {"p_F", f_pvalue(w.F, J, df2)},
                      {"vcov_type", r.vcov_type}, {"ok", true}};
        else robust = {{"ok", false}, {"error", w.error}};
    }

    json out = {
        {"estimator", "chow"}, {"by", by}, {"G", G},
        {"group_sizes", gsize}, {"n_obs", n},
        {"classic", classic}, {"robust", robust},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_margins(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string model = opt(a, "--model", "logit");
    // `--dydx VARS` (Stata `margins, dydx()`) SELECTS which fitted variables are
    // reported (P5/BUG-158-159): read unconditionally here (like every other
    // flag below) so it is always marked "consumed" regardless of which branch
    // below returns — the --at branches ignore it (predictive margins at a
    // point has no dydx() concept), the plain AME path at the bottom honors it.
    // Empty means "report every non-interaction main-effect/level column",
    // matching the exact old row set for a caller that never asks for one.
    std::string dydx_str = opt(a, "--dydx");
    // Weighted + cluster-robust AME (P5/BUG-158-159): --weight/--wtype mirror
    // `cobra logit`'s own pweight flags exactly (only pw is supported — Stata
    // has no other meaningful weight type for an ML AME); --vce/--cluster
    // mirror `cobra logit --vce cluster --cluster C`. Read unconditionally so
    // they are consumed even on the --at branches (which do not use them).
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    std::string vce = opt(a, "--vce", "iid");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
            "usage: cobra margins --data f.hbin --y Y --x X1,X2 [--out f.json]\n"
            "  AME (default): --model logit|probit|poisson  -> dydx + delta SE\n"
            "  predictive margins (linear): --at \"v1=val1,v2=val2\" [--at ...] [--fe F] [--vcov ..] [--cluster C]\n"
            "  predictive margins (GLM): --at ... --model logit|probit|poisson -> average adjusted prediction\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);

    // ---- Predictive margins at fixed covariate values (LINEAR model), `margins, at()` ----
    // A linear predictive margin at a point = x̄(at)'β where x̄(at) is the average design
    // row with the at()-listed covariates overridden (AsObserved means for the rest);
    // delta-method SE = sqrt(x̄(at)' V x̄(at)). Each --at "v1=val1,v2=val2,..." is one point.
    std::vector<std::string> atspecs = opt_all(a, "--at");
    // "flag present" vs "default" — opt(a,"--model","logit") always returns "logit" when
    // --model is omitted, so that default must not leak into the --at path (BUG: --at used
    // to silently compute the LINEAR margin even when --model logit|probit|poisson was passed).
    bool model_given = has(a, "--model");
    if (!atspecs.empty() && !model_given) {
        ModelData md = build_model(ds, y, split_csv(xcsv), split_csv(opt(a, "--fe")),
                                   split_csv(opt(a, "--cluster")));
        FeolsResult r = feols_fit(md, parse_vcov(opt(a, "--vcov", "iid")));
        const int k = static_cast<int>(r.names.size());
        // md.dm.X holds only the md.dm.k design columns actually solved. With NO FE that
        // is k columns incl. a "(intercept)" ones column (mean = 1). Under FE, feols_fit
        // appends a reconstructed "_cons" row to r.names (GAP B / 3217393) so r.names.size()
        // == md.dm.k + 1, and md.dm.X has NO _cons column (and its slope cols are demeaned).
        // The intercept/_cons design value is the constant 1, so pin its mean to 1.0 rather
        // than reading md.dm.X out of bounds. Byte-identical for no-FE (ones-column mean==1).
        std::vector<double> mean(k, 0.0);  // observed column means of the design (intercept = 1)
        for (int j = 0; j < k; ++j) {
            if (r.names[static_cast<size_t>(j)] == "(intercept)" ||
                r.names[static_cast<size_t>(j)] == "_cons") { mean[j] = 1.0; continue; }
            if (j >= md.dm.k) { mean[j] = 0.0; continue; }  // defensive: no backing column
            const double* col = md.dm.X.data() + static_cast<int64_t>(j) * md.dm.n;
            double s = 0; for (int64_t i = 0; i < md.dm.n; ++i) s += col[i];
            mean[j] = s / static_cast<double>(md.dm.n);
        }
        std::unordered_map<std::string, int> idx;
        for (int j = 0; j < k; ++j) idx[r.names[j]] = j;
        const double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0) : static_cast<double>(r.dof.dof_resid);
        const double tc = (dof > 0) ? t_critical(0.05, dof) : 0.0;
        json arr = json::array();
        for (const auto& spec : atspecs) {
            std::vector<double> xb = mean;  // AsObserved means, then apply overrides
            for (const auto& tok : split_csv(spec)) {
                size_t eq = tok.find('=');
                if (eq == std::string::npos) { std::fprintf(stderr, "margins --at: bad token '%s'\n", tok.c_str()); return 2; }
                std::string nm = trimx(tok.substr(0, eq));
                auto it = idx.find(nm);
                if (it == idx.end()) { std::fprintf(stderr, "margins --at: unknown covariate '%s'\n", nm.c_str()); return 2; }
                try {
                    xb[it->second] = std::stod(trimx(tok.substr(eq + 1)));
                } catch (const std::exception&) {
                    std::fprintf(stderr, "margins --at: '%s' has a non-numeric value.\n", tok.c_str());
                    return 2;
                }
            }
            double mg = 0;
            for (int j = 0; j < k; ++j) mg += xb[j] * r.beta[j];
            double var = 0;
            for (int aa = 0; aa < k; ++aa) {
                double gv = 0;
                for (int bb = 0; bb < k; ++bb) gv += xb[bb] * r.vcov_matrix[aa + bb * k];
                var += xb[aa] * gv;
            }
            double se = (var > 0) ? std::sqrt(var) : 0.0;
            double t = (se > 0) ? mg / se : 0.0;
            arr.push_back({{"at", spec}, {"margin", mg}, {"se", se}, {"t", t}, {"df", dof},
                           {"p", (se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0},
                           {"ci_lo", mg - tc * se}, {"ci_hi", mg + tc * se}});
        }
        json out = {{"estimator", "margins"}, {"type", "predictive"}, {"vcov_type", r.vcov_type},
                    {"n_obs", r.n}, {"df", dof}, {"margins", arr}};
        emit(out, opt(a, "--out"));
        return 0;
    }
    // ---- P7: marginal EFFECTS at fixed covariate values, `margins, dydx() at()` ----
    // Stata allows dydx() and at() together; until P7 this engine treated them as
    // mutually exclusive (the --at branches read --dydx only to mark it consumed,
    // then ignored it), so `margins, dydx(x1 x2) at(treated=(0 1))` silently
    // answered a different question — an average adjusted PREDICTION instead of an
    // average marginal EFFECT.
    //
    // Semantics: the model is fitted ONCE on the real data; for each at-point the
    // design is copied with the at-listed columns overridden counterfactually, and
    // the AME is taken over that copy with the original beta and V. That is exactly
    // margins_ame's existing job, called once per at-point.
    if (!atspecs.empty() && model_given && !dydx_str.empty()) {
        if (model != "logit" && model != "probit" && model != "poisson") {
            std::fprintf(stderr, "margins: --model must be logit|probit|poisson\n");
            return 2;
        }
        GlmInputs g = load_glm(ds, "margins", y, split_csv(xcsv));
        if (g.n == 0) { std::fprintf(stderr, "margins: no rows after dropping missing.\n"); return 1; }

        std::vector<double> beta, V;
        GlmLink link = GlmLink::Logit;
        if (model == "logit") {
            if (!check_binary(g.y, "margins(logit)", y)) return 2;
            LogitResult lr = logit_fit(g.X.data(), g.n, g.k, g.y.data());
            if (lr.separated_perfect) {
                std::fprintf(stderr, "margins(logit): outcome %s predicts data perfectly; r(2000)\n", y.c_str());
                return 2;
            }
            beta = lr.beta; V = lr.bread; link = GlmLink::Logit;
        } else if (model == "probit") {
            if (!check_binary(g.y, "margins(probit)", y)) return 2;
            ProbitResult pr = probit_fit(g.X.data(), g.n, g.k, g.y.data());
            // BUG-210's FOURTH site, found by the sweep the BUG-140 audit did not do:
            // this at()+dydx branch took the Fisher bread pr.V like the two plain
            // margins branches did. Non-canonical link => observed != expected info.
            beta = pr.beta;
            V = probit_observed_info(pr, g.X.data(), g.n, g.k, g.y.data()).bread;
            link = GlmLink::Probit;
        } else {
            for (double vv : g.y) if (vv < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
            ModelData md; md.has_intercept = true; md.dm.n = g.n; md.dm.k = g.k;
            md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
            FepoisResult pf = fepois_fit(md, Vcov::IID);
            beta = pf.beta; V = pf.vcov_matrix; link = GlmLink::Poisson;
        }

        const std::vector<std::string> request_vars = split_vars(dydx_str);
        // One BLOCK per at-point, not one flat row list. A flat table repeats the
        // same variable names across at-points, and every name-keyed consumer
        // downstream (the tidy coeftable builder, PIET's AME remap) then collapses
        // them to whichever came last — measured: both at-points reported the
        // treated=1 effect. The equations[] shape already exists for exactly this
        // (reg3, mlogit) and renders as Stata's own grouped output, so it is
        // reused rather than inventing an `x1@_at1` spelling.
        json at_groups = json::array();
        int at_index = 0;
        for (const auto& spec : atspecs) {
            ++at_index;
            std::vector<double> Xat = g.X;  // counterfactual copy, per at-point
            json at_desc = json::array();
            for (const auto& kv : split_csv(spec)) {
                auto eq = kv.find('=');
                if (eq == std::string::npos)
                    throw EngineError("bad_input",
                        "margins: --at entry '" + kv + "' must be VAR=VALUE");
                const std::string var = trimx(kv.substr(0, eq));
                const double val = std::stod(kv.substr(eq + 1));
                // Override the design column that IS this variable. A column that
                // merely MENTIONS it (a factor level or an interaction term) cannot
                // be recomputed from the already-expanded design, so it is an honest
                // reject — computing the effect while leaving such a column at its
                // observed value would answer a different question silently.
                bool hit = false;
                for (int c = 0; c < g.k; ++c) {
                    const std::string& nm = g.names[static_cast<size_t>(c)];
                    if (nm == var) {
                        for (int64_t i = 0; i < g.n; ++i)
                            Xat[static_cast<int64_t>(c) * g.n + i] = val;
                        hit = true;
                    } else if (nm != "(intercept)" && margins_name_mentions_var(nm, var)) {
                        throw EngineError("unsupported",
                            "margins: --at cannot fix '" + var + "' because the design also "
                            "carries the derived term '" + nm + "'; that column would keep its "
                            "observed value and the reported effect would not be the one asked for");
                    }
                }
                if (!hit)
                    throw EngineError("bad_input",
                        "margins: --at names '" + var + "', which is not a design column of this model");
                at_desc.push_back({{"var", var}, {"value", val}});
            }
            AmeResult m = margins_ame(link, Xat.data(), g.n, g.k, g.names, beta.data(), V.data(),
                                      nullptr, request_vars);
            if (!m.ok) { std::fprintf(stderr, "%s\n", m.error.c_str()); return 2; }
            json rows = json::array();
            for (const auto& row : m.rows) {
                const double t = tstat(row.dydx, row.se);
                const double zc = 1.959963984540054;  // 95% normal critical value
                rows.push_back({{"name", row.name},
                                {"b", row.dydx},
                                {"dydx", row.dydx},
                                {"se", row.se},
                                {"t", t},
                                {"p", row.se > 0 ? z_twosided_pvalue(t) : 1.0},
                                {"ci_lo", row.dydx - zc * row.se},
                                {"ci_hi", row.dydx + zc * row.se}});
            }
            at_groups.push_back({{"outcome", "_at" + std::to_string(at_index)},
                                 {"at_index", at_index},
                                 {"at", at_desc},
                                 {"coefs", rows},
                                 {"coeftable", rows}});
        }
        json out = {{"estimator", "margins"}, {"model", model}, {"n_obs", g.n},
                    {"vce", "oim"}, {"n_at", at_index}, {"equations", at_groups}};
        emit(out, opt(a, "--out"));
        return 0;
    }

    if (!atspecs.empty() && model_given) {
        // ---- Predictive margins at fixed covariate values (GLM), `margins --at .. --model ..` ----
        // Average adjusted prediction: margin = (1/n) Σ_i g(x_i(at)'β) over the estimation
        // sample, where x_i(at) is row i's design with the --at covariates overridden (the
        // rest as-observed) — NOT the "at means" convention the linear path above uses.
        // Delta-method SE = sqrt(J' V J), J = (1/n) Σ_i g'(x_i(at)'β) · x_i(at). Reuses the
        // same IID-only fit machinery as the dydx/AME path below (no --vcov/--cluster here,
        // matching that path).
        if (model != "logit" && model != "probit" && model != "poisson") {
            std::fprintf(stderr, "margins: --model must be logit|probit|poisson\n");
            return 2;
        }
        GlmInputs g = load_glm(ds, "margins", y, split_csv(xcsv));
        if (g.n == 0) { std::fprintf(stderr, "margins: no rows after dropping missing.\n"); return 1; }
        std::vector<double> beta, V;
        double dof = 0;
        if (model == "logit") {
            if (!check_binary(g.y, "margins(logit)", y)) return 2;
            LogitResult lr = logit_fit(g.X.data(), g.n, g.k, g.y.data());
            beta = lr.beta; V = lr.bread;
            dof = static_cast<double>(g.n - g.k);  // no residual-dof field on LogitResult; Wald N-k approx (cf. tobit's n-k-1)
        } else if (model == "probit") {
            if (!check_binary(g.y, "margins(probit)", y)) return 2;
            ProbitResult pr = probit_fit(g.X.data(), g.n, g.k, g.y.data());
            // BUG-210's FIFTH site: the at() PREDICTIVE branch, same Fisher-bread
            // defect. Measured RED at rel 1.3e-2 on the at(x2=1) point before this.
            beta = pr.beta;
            V = probit_observed_info(pr, g.X.data(), g.n, g.k, g.y.data()).bread;
            dof = static_cast<double>(g.n - g.k);
        } else {
            for (double vv : g.y) if (vv < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
            ModelData md; md.has_intercept = true; md.dm.n = g.n; md.dm.k = g.k;
            md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
            FepoisResult pf = fepois_fit(md, Vcov::IID);
            beta = pf.beta; V = pf.vcov_matrix;
            dof = (pf.n_clusters > 0) ? (pf.n_clusters - 1.0) : static_cast<double>(pf.dof.dof_resid);
        }
        const int k = g.k;
        std::unordered_map<std::string, int> idx;
        for (int j = 0; j < k; ++j) idx[g.names[static_cast<size_t>(j)]] = j;
        // BUG-194: this is a predictive margin off a maximum-likelihood (OIM) fit —
        // Stata prints `Model VCE: OIM` and a normal `z` column here, not t(dof); there
        // is no residual dof for an ML model. `dof` above is a Wald N-k approximation
        // kept only for the reported "df" field, and must not multiply the CI. Use the
        // same fixed 95% normal quantile the asymptotic branch of add_inference
        // (common.cpp) uses.
        const double tc = z_critical(0.05);
        const double INV_SQRT_2PI = 0.3989422804014327;
        json arr = json::array();
        for (const auto& spec : atspecs) {
            std::vector<double> Xat = g.X;  // per-row design, overridden below (col-major n*k)
            for (const auto& tok : split_csv(spec)) {
                size_t eq = tok.find('=');
                if (eq == std::string::npos) { std::fprintf(stderr, "margins --at: bad token '%s'\n", tok.c_str()); return 2; }
                std::string nm = trimx(tok.substr(0, eq));
                auto it = idx.find(nm);
                if (it == idx.end()) { std::fprintf(stderr, "margins --at: unknown covariate '%s'\n", nm.c_str()); return 2; }
                double val;
                try { val = std::stod(trimx(tok.substr(eq + 1))); }
                catch (const std::exception&) {
                    std::fprintf(stderr, "margins --at: '%s' has a non-numeric value.\n", tok.c_str());
                    return 2;
                }
                double* col = Xat.data() + static_cast<int64_t>(it->second) * g.n;
                for (int64_t i = 0; i < g.n; ++i) col[i] = val;
            }
            double mg = 0;
            std::vector<double> J(static_cast<size_t>(k), 0.0);
            for (int64_t i = 0; i < g.n; ++i) {
                double eta = 0;
                for (int j = 0; j < k; ++j) eta += Xat[static_cast<int64_t>(j) * g.n + i] * beta[static_cast<size_t>(j)];
                double gi, dgi;
                if (model == "logit") { gi = 1.0 / (1.0 + std::exp(-eta)); dgi = gi * (1.0 - gi); }
                else if (model == "probit") { gi = med_ncdf(eta); dgi = INV_SQRT_2PI * std::exp(-0.5 * eta * eta); }
                else { gi = std::exp(eta); dgi = gi; }
                mg += gi;
                for (int j = 0; j < k; ++j) J[static_cast<size_t>(j)] += dgi * Xat[static_cast<int64_t>(j) * g.n + i];
            }
            mg /= static_cast<double>(g.n);
            for (int j = 0; j < k; ++j) J[static_cast<size_t>(j)] /= static_cast<double>(g.n);
            double var = 0;
            for (int aa = 0; aa < k; ++aa) {
                double gv = 0;
                for (int bb = 0; bb < k; ++bb) gv += J[static_cast<size_t>(bb)] * V[static_cast<size_t>(aa) + static_cast<size_t>(bb) * k];
                var += J[static_cast<size_t>(aa)] * gv;
            }
            double se = (var > 0) ? std::sqrt(var) : 0.0;
            double t = (se > 0) ? mg / se : 0.0;
            // p-value shares tc's reference distribution (BUG-194): normal, not t(dof).
            arr.push_back({{"at", spec}, {"margin", mg}, {"se", se}, {"t", t}, {"df", dof},
                           {"p", se > 0 ? z_twosided_pvalue(t) : 1.0},
                           {"ci_lo", mg - tc * se}, {"ci_hi", mg + tc * se}, {"model", model}});
        }
        json out = {{"estimator", "margins"}, {"type", "predictive"}, {"model", model},
                    {"n_obs", g.n}, {"df", dof}, {"margins", arr}};
        emit(out, opt(a, "--out"));
        return 0;
    }

    // ---- Average marginal effects (AME / `margins, dydx()`) -----------------------
    if (model != "logit" && model != "probit" && model != "poisson") {
        std::fprintf(stderr, "margins: --model must be logit|probit|poisson\n");
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster") {
        std::fprintf(stderr, "margins: --vce must be one of iid|robust|cluster (got %s).\n", vce.c_str());
        return 2;
    }
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "margins: --vce cluster requires --cluster C.\n");
        return 2;
    }
    // Stata pweight (P5/BUG-158-159): mirrors `cobra logit`'s own validation
    // exactly — only pw is meaningful for an AME, and pweight always forces a
    // sandwich VCE (no iid pweight estimator exists in Stata).
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "margins: --wtype requires --weight");
        if (wtype != "pw" && wtype != "pweight")
            throw EngineError("bad_input", "margins: --wtype must be pw (only pweight is supported)");
        wkind = cecon::WeightKind::Probability;
        if (vce == "iid")
            throw EngineError("bad_input",
                "margins: [pweight=] requires vce(robust) or vce(cluster ...); Stata has no "
                "iid VCE under pweight");
    }
    // probit/poisson's underlying estimators don't (yet) carry every VCE shape a
    // pweighted/clustered logit does — reject honestly rather than silently
    // dropping the request (BUG-110 failure mode).
    if (model == "probit" && wkind != cecon::WeightKind::None) {
        std::fprintf(stderr, "margins(probit): [pweight=] is not supported (probit_fit has no "
                             "weighted estimator); use --model logit.\n");
        return 2;
    }
    if (model == "probit" && vce == "cluster") {
        std::fprintf(stderr, "margins(probit): --vce cluster is not supported; use --model logit.\n");
        return 2;
    }

    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, "margins", y, split_csv(xcsv), cluster_names, weight_var, wkind);
    if (g.n == 0) { std::fprintf(stderr, "margins: no rows after dropping missing.\n"); return 1; }
    if (vce == "cluster" && (g.cluster.empty() || g.cluster[0].n_levels < 2))
        throw EngineError("bad_input",
            "margins: vce(cluster) requires at least 2 clusters, but the cluster variable has " +
            std::to_string(g.cluster.empty() ? 0 : g.cluster[0].n_levels));

    // Route: a plain-continuous, unweighted, iid design (no `#`/`level.var`
    // columns, no --weight/--vce/--cluster/--dydx) takes the EXACT old
    // per-column path below — byte-identical output to before this fix existed.
    // Anything else (an interaction, a factor level, a weight, a non-iid VCE, or
    // an explicit --dydx selection) takes the new margins_ame (P5/BUG-158-159).
    bool interaction_or_factor = false;
    for (const auto& nm : g.names)
        if (nm != "(intercept)" && margins_name_is_factor_or_interaction(nm)) { interaction_or_factor = true; break; }
    bool use_general = interaction_or_factor || wkind != cecon::WeightKind::None ||
                       vce != "iid" || !cluster_var.empty() || !dydx_str.empty();

    json coefs = json::array();
    int n_clusters_out = 0;
    if (!use_general) {
        MarginsResult m;
        if (model == "logit") {
            if (!check_binary(g.y, "margins(logit)", y)) return 2;
            LogitResult r = logit_fit(g.X.data(), g.n, g.k, g.y.data());
            m = margins_logit(g.X.data(), g.n, g.k, r.beta.data(), r.bread.data());
        } else if (model == "probit") {
            if (!check_binary(g.y, "margins(probit)", y)) return 2;
            ProbitResult r = probit_fit(g.X.data(), g.n, g.k, g.y.data());
            // BUG-210, second iid site: this "exact old per-column path" passed the
            // FISHER bread r.V too, so plain `margins --model probit` (no --dydx) was
            // wrong by the same 1.5% as the --dydx branch — measured identical SEs
            // pre-fix. probit is non-canonical; the delta method must be taken against
            // the OBSERVED-information covariance Stata and `cobra probit` both report.
            m = margins_probit(g.X.data(), g.n, g.k, r.beta.data(),
                               probit_observed_info(r, g.X.data(), g.n, g.k, g.y.data()).bread.data());
        } else {
            for (double v : g.y) if (v < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
            // pooled poisson = fepois with no FE; build a ModelData with intercept.
            ModelData md; md.has_intercept = true; md.dm.n = g.n; md.dm.k = g.k;
            md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
            FepoisResult r = fepois_fit(md, Vcov::IID);
            m = margins_poisson(g.X.data(), g.n, g.k, r.beta.data(), r.vcov_matrix.data(), nullptr);
        }
        for (int c = 0; c < g.k; ++c) {
            if (g.names[static_cast<size_t>(c)] == "(intercept)") continue;  // no AME for the constant
            double se = m.se[static_cast<size_t>(c)];
            coefs.push_back({{"name", g.names[static_cast<size_t>(c)]}, {"dydx", m.dydx[static_cast<size_t>(c)]},
                             {"se", se}, {"t", tstat(m.dydx[static_cast<size_t>(c)], se)}});
        }
    } else {
        std::vector<double> beta_out, V_out;
        GlmLink link = GlmLink::Logit;
        const double* wptr = g.weights.empty() ? nullptr : g.weights.data();
        if (model == "logit") {
            link = GlmLink::Logit;
            if (!check_binary(g.y, "margins(logit)", y)) return 2;
            LogitResult r = logit_fit(g.X.data(), g.n, g.k, g.y.data(), 100, 1e-14, wptr);
            if (r.separated_perfect) {
                std::fprintf(stderr, "margins(logit): outcome %s predicts data perfectly; r(2000)\n", y.c_str());
                return 2;
            }
            beta_out = r.beta;
            V_out = r.bread;
            // Sandwich VCE (mirrors cmd_logit exactly, see cmds_mle.cpp cmd_logit):
            // meat's per-row score e_i = (y_i - p_i), pre-scaled by the raw pweight
            // when present (the pweight-correct meat, since bread already folded
            // the same weight into IRLS — logit is a canonical link, no BUG-140
            // observed/expected-info split needed).
            if (vce == "robust" || vce == "cluster") {
                std::vector<double> e(static_cast<size_t>(g.n));
                for (int64_t i = 0; i < g.n; ++i) {
                    double ei = g.y[static_cast<size_t>(i)] - r.p[static_cast<size_t>(i)];
                    if (wptr) ei *= wptr[static_cast<size_t>(i)];
                    e[static_cast<size_t>(i)] = ei;
                }
                std::vector<double> meat(static_cast<size_t>(g.k) * g.k, 0.0);
                double factor;
                if (vce == "robust") {
                    for (int64_t i = 0; i < g.n; ++i) {
                        double ei = e[static_cast<size_t>(i)];
                        double e2 = ei * ei;
                        for (int c = 0; c < g.k; ++c) {
                            double xc = g.X[static_cast<int64_t>(c) * g.n + i];
                            for (int d = 0; d < g.k; ++d)
                                meat[static_cast<size_t>(c) * g.k + d] += e2 * xc * g.X[static_cast<int64_t>(d) * g.n + i];
                        }
                    }
                    factor = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
                } else {
                    cecon::cluster_meat(g.X.data(), g.n, g.k, e.data(), g.cluster[0], meat.data(), &n_clusters_out);
                    if (n_clusters_out < 2)
                        throw EngineError("bad_input",
                            "margins: vce(cluster) requires at least 2 clusters, but the cluster "
                            "variable has " + std::to_string(n_clusters_out));
                    factor = static_cast<double>(n_clusters_out) / static_cast<double>(n_clusters_out - 1);
                }
                std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
                V_out.assign(static_cast<size_t>(g.k) * g.k, 0.0);
                la::sandwich(r.bread.data(), meat.data(), g.k, factor, V_out.data(), tmp.data());
            }
        } else if (model == "probit") {
            link = GlmLink::Probit;
            if (!check_binary(g.y, "margins(probit)", y)) return 2;
            ProbitResult r = probit_fit(g.X.data(), g.n, g.k, g.y.data());
            if (r.separated_perfect) {
                std::fprintf(stderr, "margins(probit): outcome %s predicts data perfectly; r(2000)\n", y.c_str());
                return 2;
            }
            beta_out = r.beta;
            // BUG-210: the iid branch used to take the Fisher bread r.V here ("matches
            // the OLD margins_probit's V convention", i.e. the PRE-BUG-140 one), while
            // only the vce(robust) branch below built the observed information. probit's
            // link is non-canonical, so that made `margins, dydx()` after probit report
            // se .037501556827 where Stata reports .038064606025 — 1.5% narrow on every
            // CI on this path, with the point estimate correct, which is the signature of
            // a covariance-source mismatch rather than a fitting difference. Both branches
            // now read the ONE observed-information definition in src/estimators/probit.h.
            ProbitObservedInfo oi = probit_observed_info(r, g.X.data(), g.n, g.k, g.y.data());
            V_out = oi.bread;
            if (vce == "robust") {
                // Robust sandwich on that same observed-info bread (mirrors cmd_probit's
                // own vce(robust), see cmds_mle.cpp) with Stata's ML multiplier N/(N-1).
                double rfac = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
                std::vector<double> tmp(static_cast<size_t>(g.k) * g.k, 0.0);
                V_out.assign(static_cast<size_t>(g.k) * g.k, 0.0);
                la::sandwich(oi.bread.data(), oi.meat.data(), g.k, rfac, V_out.data(), tmp.data());
            }
        } else {
            link = GlmLink::Poisson;
            for (double v : g.y) if (v < 0) { std::fprintf(stderr, "margins(poisson): y has negatives.\n"); return 2; }
            ModelData md; md.has_intercept = true; md.dm.n = g.n; md.dm.k = g.k;
            md.dm.X = g.X; md.dm.y = g.y; md.dm.xnames = g.names; md.keep = g.keep;
            md.weights = g.weights; md.weight_kind = g.weight_kind;
            Vcov vt = Vcov::IID;
            if (vce == "cluster") { md.cluster = g.cluster; vt = Vcov::Cluster; }
            else if (vce == "robust") { vt = Vcov::HC1; }
            FepoisResult r = fepois_fit(md, vt);
            beta_out = r.beta;
            V_out = r.vcov_matrix;
            n_clusters_out = r.n_clusters;
        }
        std::vector<std::string> request_vars = dydx_str.empty() ? std::vector<std::string>{} : split_vars(dydx_str);
        AmeResult m = margins_ame(link, g.X.data(), g.n, g.k, g.names, beta_out.data(), V_out.data(),
                                  wptr, request_vars);
        if (!m.ok) { std::fprintf(stderr, "%s\n", m.error.c_str()); return 2; }
        for (const auto& row : m.rows)
            coefs.push_back({{"name", row.name}, {"dydx", row.dydx}, {"se", row.se}, {"t", tstat(row.dydx, row.se)}});
    }
    json out = {{"estimator", "margins"}, {"model", model}, {"n_obs", g.n}, {"coefs", coefs}};
    if (use_general) {
        out["vce"] = vce;
        if (wkind != cecon::WeightKind::None) { out["weight_var"] = weight_var; out["wtype"] = "pw"; }
        if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters_out; }
    }
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_predict(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string outpath = opt(a, "--out");
    // Create ONLY the columns the caller asked for. Creating both unconditionally
    // appended a second, unrequested variable and — when the dataset already had one
    // named `resid` — silently overwrote the user's own data (Stata's `predict yhat, xb`
    // touches nothing but `yhat`). With neither flag given the standalone CLI keeps its
    // documented both-columns behaviour, which is the only shape any caller relied on.
    bool wantXb = has(a, "--xb"), wantResid = has(a, "--resid");
    if (!wantXb && !wantResid) wantXb = wantResid = true;
    std::string xbname = opt(a, "--xb", "xb"), rname = opt(a, "--resid", "resid");
    // Restricted-sample fit (Stata `regress ... if/in ...` then `predict` over the
    // FULL dataset). --fit-data names an already-materialized restricted file — the
    // shape PIET needs for `in` ranges, which are `_n`-positional and have no cobra
    // keepif predicate (PIET's applyIfAndInFilter, piet/native/src/inrange_filter.cpp,
    // materializes those into a scratch .hbin and passes ITS path here). --fit-keepif
    // applies a keepif EXPR to --data directly for callers that already have one.
    // Neither given -> fit sample == predict data, byte-identical to the prior
    // behavior (the only shape this function had before this option existed).
    std::string fit_data_opt = opt(a, "--fit-data");
    std::string fit_keepif = opt(a, "--fit-keepif");
    if (data_missing(data) || y.empty() || xcsv.empty() || out_missing(outpath)) {
        std::fprintf(stderr, "usage: cobra predict --data f.hbin --y Y --x X1,X2 [--fe F] "
                             "--out new.hbin [--xb NAME] [--resid NAME] "
                             "[--model logit|probit|poisson [--pred NAME]] "
                             "[--fit-data FIT.hbin | --fit-keepif EXPR]\n"
                             "  appends fitted values and/or residuals (within-residuals under --fe);\n"
                             "  with neither flag both are appended under their default names.\n"
                             "  --model re-fits with the named ML estimator and appends its Stata-default\n"
                             "  statistic (--pred: pr after logit/probit, n after poisson) and/or --xb;\n"
                             "  --resid/--fe are not offered on that path.\n"
                             "  --fit-data/--fit-keepif fit on a restricted sample while xb/resid are\n"
                             "  still evaluated over every row of --data with non-missing covariates.\n");
        return 2;
    }
    const Dataset& ds = acquire_data(data);
    auto fes = split_csv(opt(a, "--fe"));
    auto xs = split_csv(xcsv);
    bool restricted_fit = !fit_data_opt.empty() || !fit_keepif.empty();
    if (restricted_fit && !fes.empty()) {
        // Out-of-sample prediction under absorbed FE would need the FE's fitted
        // per-level intercepts reconstructed and applied to rows the FE model never
        // saw (undefined for FE levels absent from the fit sample) — not attempted.
        std::fprintf(stderr,
            "predict: --fit-data/--fit-keepif restricted-sample fitting is not "
            "supported together with --fe (fixed-effects out-of-sample prediction "
            "is undefined).\n");
        return 2;
    }
    // Dataset::open (not acquire_data, which is a single-slot cache — see its own
    // doc comment) so the fit dataset and the full predict dataset (`ds`, already
    // acquired above) can be alive at the same time.
    Dataset fit_owned;
    const Dataset* fit_ds = &ds;
    if (!fit_data_opt.empty()) {
        fit_owned = Dataset::open(fit_data_opt);
        fit_ds = &fit_owned;
    } else if (!fit_keepif.empty()) {
        std::string fit_path = maybe_keepif(data, fit_keepif);
        if (fit_path != data) {
            fit_owned = Dataset::open(fit_path);
            fit_ds = &fit_owned;
        }
    }
    // ---- BUG-204's open half (BUG-297): `--model` = the maximum-likelihood
    // prediction path. Everything ABOVE this branch (usage, restricted-fit
    // plumbing) is shared; everything BELOW it is the historical least-squares
    // path, byte-for-byte — `--model` absent (or naming the least-squares
    // family) never enters here, which is what keeps every existing predict
    // caller, eval case and freeze fixture unchanged.
    //
    // The refit is postest_fit_spec — the ONE dispatcher BUG-203 built for
    // lincom/nlcom/test — so `predict --model logit` answers off the identical
    // logit_fit/probit_fit/fepois_fit coefficients those verbs use (and
    // inherits its refusals: --fe / --cluster / non-default --vcov, and any
    // model outside logit|probit|poisson). The statistic emitted is Stata's
    // DEFAULT for the model — `pr` after logit/probit, `n` after poisson —
    // under `--pred NAME`; `--xb NAME` is the linear index. `--resid` is an
    // honest refusal: Stata's `predict, residuals` after an ML fit is a
    // DIFFERENT statistic (Pearson residuals), not y - xb, and serving y - xb
    // under that name would be exactly the silent-wrong shape BUG-204 removed.
    //
    // Inverse links are verbatim cmd_margins' GLM branch (gi above): logistic,
    // med_ncdf, exp — one convention, two surfaces, no drift. Stata evaluates
    // the prediction for EVERY row with non-missing covariates (y is not
    // needed), so the loop mirrors the restricted-fit OLS loop below:
    // name-keyed lookup over the full dataset's expanded design, NaN rows
    // skipped, never renumbered.
    const std::string ml_model = opt(a, "--model");
    const bool ols_family = ml_model.empty() || ml_model == "ols" || ml_model == "feols" ||
                            ml_model == "regress" || ml_model == "reg";
    if (!ols_family) {
        if (has(a, "--resid")) {
            std::fprintf(stderr,
                "predict --model %s: residuals are not supported after a maximum-likelihood "
                "fit in this engine (Stata's `predict, residuals` there is a Pearson residual, "
                "not y - xb). Supported: the model's default statistic (--pred NAME: pr after "
                "logit/probit, n after poisson) and --xb NAME.\n", ml_model.c_str());
            return 2;
        }
        PostestModel M;
        if (int rc = postest_fit_spec(a, *fit_ds, y, xcsv, M)) return rc;  // BUG-203's dispatcher
        const bool wantXbMl = has(a, "--xb");
        const bool wantPred = has(a, "--pred") || !wantXbMl;
        const std::string predname = opt(a, "--pred", ml_model == "poisson" ? "n" : "pr");
        int64_t Nml = ds.n_rows();
        TsMeta tsm_ml;
        auto xcols_ml = expand_varlist(ds, xs, tsm_ml, /*bare_factor=*/false);
        std::unordered_map<std::string, const std::vector<double>*> colmap_ml;
        colmap_ml.reserve(xcols_ml.size() * 2);
        for (const auto& nc : xcols_ml) colmap_ml.emplace(nc.first, &nc.second);
        const int kk = static_cast<int>(M.names.size());
        std::vector<double> pred_full(static_cast<size_t>(Nml), std::nan(""));
        std::vector<double> xbml_full(static_cast<size_t>(Nml), std::nan(""));
        for (int64_t i = 0; i < Nml; ++i) {
            double eta = 0.0;
            bool ok = true;
            for (int c = 0; c < kk; ++c) {
                const std::string& nm = M.names[static_cast<size_t>(c)];
                double v;
                if (nm == "(intercept)" || nm == "_cons") {
                    v = 1.0;
                } else {
                    auto it = colmap_ml.find(nm);
                    if (it == colmap_ml.end()) { ok = false; break; }
                    v = (*it->second)[static_cast<size_t>(i)];
                    if (std::isnan(v)) { ok = false; break; }
                }
                eta += v * M.beta[static_cast<size_t>(c)];
            }
            if (!ok) continue;
            xbml_full[static_cast<size_t>(i)] = eta;
            if (ml_model == "logit")       pred_full[static_cast<size_t>(i)] = 1.0 / (1.0 + std::exp(-eta));
            else if (ml_model == "probit") pred_full[static_cast<size_t>(i)] = med_ncdf(eta);
            else                           pred_full[static_cast<size_t>(i)] = std::exp(eta);
        }
        Frame frml = frame_from_acquired(ds);
        if (wantPred) frml.set_f64_column(predname, std::move(pred_full));
        if (wantXbMl) frml.set_f64_column(xbname, std::move(xbml_full));
        commit_frame(std::move(frml), outpath);
        json outml = {{"estimator", "predict"}, {"model", ml_model}, {"n_obs", M.n},
                      {"n_rows", Nml}, {"pred", wantPred ? predname : std::string()},
                      {"xb", wantXbMl ? xbname : std::string()},
                      {"resid", std::string()}, {"out", outpath}, {"within", false}};
        emit(outml, "");
        return 0;
    }

    ModelData md = build_model(*fit_ds, y, xs, fes, {});
    FeolsResult r = feols_fit(md, Vcov::IID);  // demeans md.dm in place (no-op if no FE)
    int64_t n = md.dm.n; int k = md.dm.k;
    int64_t N = ds.n_rows();
    std::vector<double> xb_full(static_cast<size_t>(N), std::nan(""));
    std::vector<double> rs_full(static_cast<size_t>(N), std::nan(""));
    if (fit_ds == &ds) {
        // Fit sample == predict data: exactly the prior (pre-restricted-fit) code path.
        for (int64_t i = 0; i < n; ++i) {
            double fit = 0;
            for (int c = 0; c < k; ++c) fit += md.dm.X[static_cast<size_t>(c) * n + i] * r.beta[c];
            double resid = md.dm.y[static_cast<size_t>(i)] - fit;
            int64_t orig = md.keep[static_cast<size_t>(i)];
            xb_full[static_cast<size_t>(orig)] = fit;
            rs_full[static_cast<size_t>(orig)] = resid;
        }
    } else {
        // Restricted fit: evaluate xb/resid over EVERY row of the full predict
        // dataset whose covariates are non-missing — xb is a pure linear-predictor
        // evaluation and Stata computes it wherever the covariates are present,
        // regardless of y (resid additionally needs y, since resid = y - xb).
        // Build the same factor-expanded design over the FULL dataset (the engine's
        // `level.varname` naming convention, BUG-101, makes terms name-comparable
        // across the two datasets) and look each of the fit's coefficients up by
        // name: a level the full data has but the fit sample didn't never entered
        // r.names, so it is simply never referenced here; a level the fit sample
        // needed that the full data is missing on a given row makes that row's
        // lookup miss (or read NaN), and the row is left NaN — the fit-sample's own
        // coefficient/level set is never renumbered or silently dropped.
        TsMeta tsm_full;
        auto xcols_full = expand_varlist(ds, xs, tsm_full, /*bare_factor=*/false);
        std::unordered_map<std::string, const std::vector<double>*> colmap;
        colmap.reserve(xcols_full.size() * 2);
        for (const auto& nc : xcols_full) colmap.emplace(nc.first, &nc.second);
        const double* yfull = require_num(ds, "predict", y);
        const int kk = static_cast<int>(r.names.size());
        for (int64_t i = 0; i < N; ++i) {
            double fit = 0.0;
            bool ok = true;
            for (int c = 0; c < kk; ++c) {
                const std::string& nm = r.names[static_cast<size_t>(c)];
                double v;
                if (nm == "(intercept)" || nm == "_cons") {
                    v = 1.0;
                } else {
                    auto it = colmap.find(nm);
                    if (it == colmap.end()) { ok = false; break; }
                    v = (*it->second)[static_cast<size_t>(i)];
                    if (std::isnan(v)) { ok = false; break; }
                }
                fit += v * r.beta[static_cast<size_t>(c)];
            }
            if (!ok) continue;
            xb_full[static_cast<size_t>(i)] = fit;
            double yi = yfull[i];
            if (!std::isnan(yi)) rs_full[static_cast<size_t>(i)] = yi - fit;
        }
    }
    Frame fr = frame_from_acquired(ds);
    if (wantXb) fr.set_f64_column(xbname, std::move(xb_full));
    if (wantResid) fr.set_f64_column(rname, std::move(rs_full));
    commit_frame(std::move(fr), outpath);  // §26: stage into resident session, or write one-shot (status JSON below)
    json out = {{"estimator", "predict"}, {"n_obs", n}, {"n_rows", N},
                {"xb", wantXb ? xbname : std::string()},
                {"resid", wantResid ? rname : std::string()},
                {"fe", fes}, {"out", outpath},
                {"within", !fes.empty()}};
    emit(out, "");  // status to stdout
    return 0;
}

int cmd_hausman(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string ivar = opt(a, "--ivar");
    if (data_missing(data) || y.empty() || xcsv.empty() || ivar.empty()) {
        std::fprintf(stderr, "usage: cobra hausman --data f.hbin --y Y --x X1,X2 --ivar ID [--out f.json]\n"
                             "  FE (feols, fe=ivar) vs RE (xtreg) on the common time-varying slopes.\n");
        return 2;
    }
    const Dataset& ds = acquire_data(data);
    auto xs = split_csv(xcsv);
    // FE via feols absorbing ivar.
    ModelData md = build_model(ds, y, xs, {ivar}, {});
    FeolsResult fe = feols_fit(md, Vcov::IID);  // names = xs (no intercept under FE)
    // RE via xtreg (CLI-style listwise + collinear drop already inside xtreg path).
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "hausman", y);
    const double* iraw = require_num(ds, "hausman", ivar);
    int kx = static_cast<int>(xs.size());
    std::vector<const double*> xraw(kx);
    for (int c = 0; c < kx; ++c) xraw[c] = require_num(ds, "hausman", xs[c]);
    std::vector<const double*> all = {yraw, iraw};
    for (auto* p : xraw) all.push_back(p);
    auto keep = listwise_keep(all, n_raw);
    int64_t n = static_cast<int64_t>(keep.size());
    auto yv = gather_rows(yraw, keep); auto iv = gather_rows(iraw, keep);
    std::vector<double> Xre(static_cast<size_t>(n) * kx);
    for (int c = 0; c < kx; ++c) { auto col = gather_rows(xraw[c], keep);
        std::copy(col.begin(), col.end(), Xre.data() + static_cast<int64_t>(c) * n); }
    XtregReResult re = xtreg_re(Xre.data(), n, kx, yv.data(), iv.data());
    // RE names: xs[0..kx-1] then _cons; RE V is (kx+1)*(kx+1) with _cons last.
    // Common set = the time-varying slopes that FE identifies (fe.names ⊆ xs).
    std::vector<int> fe_idx, re_idx;
    std::vector<std::string> common;
    for (size_t f = 0; f < fe.names.size(); ++f) {
        // find this name in xs (RE order)
        for (int c = 0; c < kx; ++c)
            if (xs[static_cast<size_t>(c)] == fe.names[f]) {
                fe_idx.push_back(static_cast<int>(f)); re_idx.push_back(c);
                common.push_back(fe.names[f]); break;
            }
    }
    int kc = static_cast<int>(common.size());
    if (kc == 0) { std::fprintf(stderr, "hausman: no common coefficients.\n"); return 1; }
    int kfe = static_cast<int>(fe.names.size());
    int kre = kx + 1;
    if (static_cast<int>(fe.beta.size()) < kfe || static_cast<int>(re.beta.size()) < kre ||
        static_cast<int>(fe.vcov_matrix.size()) < kfe * kfe ||
        static_cast<int>(re.vcov_matrix.size()) < kre * kre) {
        std::fprintf(stderr, "hausman: degenerate FE or RE fit (single panel unit, collinear regressors, or too few observations); cannot form the comparison.\n");
        return 1;
    }
    std::vector<double> bFE(kc), bRE(kc), VFE(static_cast<size_t>(kc) * kc, 0.0), VRE(static_cast<size_t>(kc) * kc, 0.0);
    for (int i = 0; i < kc; ++i) {
        bFE[static_cast<size_t>(i)] = fe.beta[static_cast<size_t>(fe_idx[static_cast<size_t>(i)])];
        bRE[static_cast<size_t>(i)] = re.beta[static_cast<size_t>(re_idx[static_cast<size_t>(i)])];
        for (int j = 0; j < kc; ++j) {
            VFE[static_cast<size_t>(j) * kc + i] = fe.vcov_matrix[static_cast<size_t>(fe_idx[static_cast<size_t>(j)]) * kfe + fe_idx[static_cast<size_t>(i)]];
            VRE[static_cast<size_t>(j) * kc + i] = re.vcov_matrix[static_cast<size_t>(re_idx[static_cast<size_t>(j)]) * kre + re_idx[static_cast<size_t>(i)]];
        }
    }
    HausmanResult h = hausman_test(bFE.data(), VFE.data(), bRE.data(), VRE.data(), kc);
    if (!h.ok) { std::fprintf(stderr, "hausman: %s\n", h.error.c_str()); return 1; }
    json diffs = json::array();
    for (int i = 0; i < kc; ++i)
        diffs.push_back({{"name", common[static_cast<size_t>(i)]}, {"b_fe", bFE[static_cast<size_t>(i)]},
                         {"b_re", bRE[static_cast<size_t>(i)]}, {"diff", bFE[static_cast<size_t>(i)] - bRE[static_cast<size_t>(i)]}});
    json out = {{"estimator", "hausman"}, {"chi2", h.chi2}, {"dof", h.dof},
                {"p", chi2_pvalue(h.chi2, h.dof)}, {"coefs", diffs}};
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
