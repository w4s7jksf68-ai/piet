#include "common.h"
#include "commands.h"

namespace cecon {
namespace cli {

// ── corr / pwcorr ────────────────────────────────────────────────────────────
int cmd_corr(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    bool pairwise = has(a, "--pairwise") || (!a.empty() && a[0] == "pwcorr");
    bool sig = has(a, "--sig");
    if (data_missing(data) || vars.size() < 2) {
        std::fprintf(stderr, "usage: cobra corr --data f.hbin --vars v1,v2,.. [--pairwise] [--sig]\n");
        return 2;
    }
    if (sig && !pairwise) throw EngineError("bad_input", "corr --sig requires --pairwise");
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int k = static_cast<int>(vars.size());
    const int64_t N = ds.n_rows();
    std::vector<const double*> col(k);
    for (int j = 0; j < k; ++j) col[j] = require_num(ds, "corr", vars[j]);

    // Pearson r over the rows where the predicate keeps obs i.
    auto pearson = [&](int i, int j, const std::vector<char>& keep, int64_t& nout) {
        double sx = 0, sy = 0; int64_t n = 0;
        for (int64_t r = 0; r < N; ++r) if (keep[r]) { sx += col[i][r]; sy += col[j][r]; ++n; }
        nout = n;
        if (n < 2) return std::numeric_limits<double>::quiet_NaN();
        double mx = sx / n, my = sy / n, sxy = 0, sxx = 0, syy = 0;
        for (int64_t r = 0; r < N; ++r) if (keep[r]) {
            double dx = col[i][r] - mx, dy = col[j][r] - my;
            sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
        }
        double den = std::sqrt(sxx * syy);
        return den > 0 ? sxy / den : std::numeric_limits<double>::quiet_NaN();
    };

    std::vector<double> R(static_cast<size_t>(k) * k, 1.0), P(static_cast<size_t>(k) * k,
                                                             std::numeric_limits<double>::quiet_NaN());
    std::vector<long long> NB(static_cast<size_t>(k) * k, 0);
    int64_t listN = 0;
    std::vector<char> listwise;
    if (!pairwise) {  // correlate: one common sample across all vars
        listwise.assign(static_cast<size_t>(N), 1);
        for (int64_t r = 0; r < N; ++r)
            for (int j = 0; j < k; ++j) if (std::isnan(col[j][r])) { listwise[r] = 0; break; }
        for (int64_t r = 0; r < N; ++r) if (listwise[r]) ++listN;
    }
    json coefs = json::array();
    for (int i = 0; i < k; ++i) {
        for (int j = 0; j < k; ++j) {
            if (i == j) {
                std::vector<char> ki(static_cast<size_t>(N), 0); int64_t ni = 0;
                for (int64_t r = 0; r < N; ++r) if (!std::isnan(col[i][r])) { ki[r] = 1; ++ni; }
                NB[static_cast<size_t>(i) * k + j] = ni;
                continue;
            }
            std::vector<char> keep;
            if (pairwise) {
                keep.assign(static_cast<size_t>(N), 0);
                for (int64_t r = 0; r < N; ++r)
                    keep[r] = (!std::isnan(col[i][r]) && !std::isnan(col[j][r])) ? 1 : 0;
            } else keep = listwise;
            int64_t n = 0;
            double r = pearson(i, j, keep, n);
            R[static_cast<size_t>(i) * k + j] = r;
            NB[static_cast<size_t>(i) * k + j] = n;
            double p = std::numeric_limits<double>::quiet_NaN();
            if (sig && n >= 3 && std::fabs(r) < 1.0) {
                double t = std::fabs(r) * std::sqrt(static_cast<double>(n - 2)) / std::sqrt(1 - r * r);
                p = t_twosided_pvalue(t, static_cast<double>(n - 2));
            } else if (sig && std::fabs(r) >= 1.0 && n >= 3) p = 0.0;
            P[static_cast<size_t>(i) * k + j] = p;
            if (i < j) {  // one coef row per unique pair (tidy)
                json row = {{"name", vars[i] + "~" + vars[j]}, {"b", r}, {"n", n}};
                if (sig) row["p"] = std::isnan(p) ? json(nullptr) : json(p);
                coefs.push_back(row);
            }
        }
    }
    auto mat = [&](const std::vector<double>& M) {
        json m = json::array();
        for (int i = 0; i < k; ++i) { json rr = json::array();
            for (int j = 0; j < k; ++j) { double v = M[static_cast<size_t>(i) * k + j];
                rr.push_back(std::isnan(v) ? json(nullptr) : json(v)); }
            m.push_back(rr); }
        return m;
    };
    json out = {{"estimator", "corr"}, {"method", pairwise ? "pwcorr" : "correlate"},
                {"pairwise", pairwise}, {"sig", sig}, {"variables", vars},
                {"r", mat(R)}, {"coefs", coefs}};
    if (pairwise) {
        json nb = json::array();
        for (int i = 0; i < k; ++i) { json rr = json::array();
            for (int j = 0; j < k; ++j) rr.push_back(NB[static_cast<size_t>(i) * k + j]); nb.push_back(rr); }
        out["nobs"] = nb;
        if (sig) out["p"] = mat(P);
        out["n_obs"] = N;
    } else { out["n_obs"] = listN; }
    emit(out, opt(a, "--out"));
    return 0;
}

namespace {

// gather (value, group) pairs into 2 ascending groups; throws unless exactly 2.
// Only used by cmd_sdtest/cmd_ttest below (same TU) — no external linkage needed.
void two_groups(const double* x, const double* g, int64_t N, const char* cmd,
                std::vector<double>& v1, std::vector<double>& v2, double& gv1, double& gv2) {
    std::vector<std::pair<double, double>> gx;  // (group, value)
    for (int64_t i = 0; i < N; ++i)
        if (!std::isnan(x[i]) && !std::isnan(g[i])) gx.push_back({g[i], x[i]});
    std::vector<double> gvals;
    for (auto& p : gx) gvals.push_back(p.first);
    std::sort(gvals.begin(), gvals.end());
    gvals.erase(std::unique(gvals.begin(), gvals.end()), gvals.end());
    if (gvals.size() != 2)
        throw EngineError("bad_input", std::string(cmd) + ": --by must define exactly 2 groups (found " +
                          std::to_string(gvals.size()) + ")");
    gv1 = gvals[0]; gv2 = gvals[1];
    for (auto& p : gx) (p.first == gv1 ? v1 : v2).push_back(p.second);
}

// Independent-samples t-test core (pooled-variance vs unequal/Satterthwaite vs
// Welch), given each sample's precomputed mean/sd/n. This is the SAME formula
// cmd_ttest's --by() two-sample path has always used; BS-089 (`ttest x y,
// unpaired`) reuses it verbatim for the two-variable independent-samples form
// instead of duplicating the arithmetic (byte-identical operation order to the
// prior inline version — no ULP drift for the existing --by() callers).
struct TwoSampleDiff { double diff, se, df, t, tc; };
TwoSampleDiff two_sample_diff(double m1, double s1, int64_t n1, double m2, double s2, int64_t n2,
                              bool unequal, bool welch, double alpha) {
    double diff = m1 - m2, se, df;
    if (unequal) {
        double a1 = s1 * s1 / n1, a2 = s2 * s2 / n2;
        se = std::sqrt(a1 + a2);
        df = welch  // Welch's df vs Satterthwaite's (Stata: welch vs unequal)
                 ? (a1 + a2) * (a1 + a2) / (a1 * a1 / (n1 + 1) + a2 * a2 / (n2 + 1)) - 2.0
                 : (a1 + a2) * (a1 + a2) / (a1 * a1 / (n1 - 1) + a2 * a2 / (n2 - 1));
    } else {
        double sp2 = ((n1 - 1) * s1 * s1 + (n2 - 1) * s2 * s2) / (n1 + n2 - 2.0);
        se = std::sqrt(sp2 * (1.0 / n1 + 1.0 / n2));
        df = n1 + n2 - 2.0;
    }
    double t = diff / se, tc = t_critical(alpha, df);
    return {diff, se, df, t, tc};
}

}  // namespace

// ── sdtest (variance comparison) ─────────────────────────────────────────────
int cmd_sdtest(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars", opt(a, "--var")));
    std::string by = opt(a, "--by"), against = opt(a, "--against");
    double level = std::stod(opt(a, "--level", "95"));
    if (data_missing(data) || vars.empty() || (by.empty() && against.empty())) {
        std::fprintf(stderr, "usage: cobra sdtest --data f.hbin --var V (--by G | --against SD0) [--level 95]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    double alpha = 1.0 - level / 100.0;
    if (!against.empty()) {  // one-sample chi2
        double sd0 = std::stod(against);
        std::vector<double> v = nonmissing(require_num(ds, "sdtest", vars[0]), N);
        double mean, sd; mean_sd(v, mean, sd);
        int64_t n = static_cast<int64_t>(v.size());
        double df = static_cast<double>(n - 1), se = sd / std::sqrt(static_cast<double>(n));
        double chi2 = df * sd * sd / (sd0 * sd0);
        double p_l = chi2_cdf(chi2, df), p_u = chi2_pvalue(chi2, df);
        double tc = t_critical(alpha, df);
        json out = {{"estimator", "sdtest"}, {"mode", "one_sample"}, {"var", vars[0]}, {"sd0", sd0},
                    {"level", level}, {"n_obs", n}, {"mean", mean}, {"sd", sd}, {"se", se},
                    {"ci_lo", mean - tc * se}, {"ci_hi", mean + tc * se}, {"df", df}, {"chi2", chi2},
                    {"p", std::min(1.0, 2 * std::min(p_l, p_u))}, {"p_lower", p_l}, {"p_upper", p_u}};
        emit(out, opt(a, "--out"));
        return 0;
    }
    std::vector<double> v1, v2; double gv1, gv2;
    two_groups(require_num(ds, "sdtest", vars[0]), require_num(ds, "sdtest", by), N, "sdtest", v1, v2, gv1, gv2);
    double m1, s1, m2, s2; mean_sd(v1, m1, s1); mean_sd(v2, m2, s2);
    int64_t n1 = v1.size(), n2 = v2.size();
    double df1 = n1 - 1.0, df2 = n2 - 1.0, F = (s1 * s1) / (s2 * s2);
    double p_l = f_cdf(F, df1, df2), p_u = f_pvalue(F, df1, df2);
    auto grp = [&](double m, double s, int64_t n, double gv) {
        double se = s / std::sqrt(static_cast<double>(n)), tc = t_critical(alpha, n - 1.0);
        return json{{"value", gv}, {"n", n}, {"mean", m}, {"sd", s}, {"se", se},
                    {"ci_lo", m - tc * se}, {"ci_hi", m + tc * se}};
    };
    std::vector<double> all = v1; all.insert(all.end(), v2.begin(), v2.end());
    double cm, cs; mean_sd(all, cm, cs);
    json out = {{"estimator", "sdtest"}, {"mode", "two_sample"}, {"var", vars[0]}, {"by", by},
                {"level", level}, {"n_obs", n1 + n2}, {"F", F}, {"df1", df1}, {"df2", df2},
                {"p", std::min(1.0, 2 * std::min(p_l, p_u))}, {"p_lower", p_l}, {"p_upper", p_u},
                {"sd_1", s1}, {"sd_2", s2}, {"groups", json::array({grp(m1, s1, n1, gv1), grp(m2, s2, n2, gv2)})},
                {"combined", {{"n", n1 + n2}, {"mean", cm}, {"sd", cs}}}};
    emit(out, opt(a, "--out"));
    return 0;
}

// ── ttest (one-sample / two-sample / paired / unpaired) ──────────────────────
int cmd_ttest(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars", opt(a, "--var")));
    std::string by = opt(a, "--by"), against = opt(a, "--against");
    bool welch = has(a, "--welch");        // Stata `welch`: implies unequal variances
    bool unequal = welch || has(a, "--unequal");
    // BS-059: swap which group/variable is "first" — mirror-image output (group/
    // var order, mean_1/mean_2, and CI bounds swap; diff and t flip sign; |t|, p,
    // and CI width are unchanged). Applies to the --by() two-sample path and the
    // two-variable form (paired or --unpaired); meaningless for one-sample.
    bool reverse = has(a, "--reverse");
    // BS-089: on the two-VARIABLE form only, treat vars[0]/vars[1] as two
    // independent samples (same math as --by()) instead of a row-wise paired
    // diff. Real Stata rejects `unpaired` combined with `by()` or a one-sample
    // `against` — those are different data shapes, not "unpaired ignored".
    bool unpaired = has(a, "--unpaired");
    double level = std::stod(opt(a, "--level", "95"));
    double alpha = 1.0 - level / 100.0;
    if (data_missing(data) || vars.empty() || (by.empty() && against.empty() && vars.size() < 2)) {
        std::fprintf(stderr, "usage: cobra ttest --data f.hbin --var V (--against MU | --by G [--unequal] [--welch] [--reverse]) "
                             "| --vars V1,V2 [--unpaired [--unequal] [--welch]] [--reverse]  [--level 95]\n");
        return 2;
    }
    if (unpaired && (!by.empty() || !against.empty()))
        throw EngineError("bad_input", "ttest: --unpaired may not be combined with --by or --against");
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    auto finish = [&](json out, double b, double se, double t, double df, double ci_lo, double ci_hi,
                      const std::string& name) {
        double p = t_twosided_pvalue(t, df), p_l = t_cdf(t, df);
        out["t"] = t; out["df"] = df; out["p"] = p; out["p_lower"] = p_l; out["p_upper"] = 1.0 - p_l;
        out["level"] = level; out["se"] = se; out["ci_lo"] = ci_lo; out["ci_hi"] = ci_hi;
        out["coefs"] = json::array({{{"name", name}, {"b", b}, {"se", se}, {"t", t}, {"p", p},
                                     {"ci_lo", ci_lo}, {"ci_hi", ci_hi}}});
        emit(out, opt(a, "--out"));
        return 0;
    };
    if (!against.empty()) {  // one-sample
        double mu = std::stod(against);
        std::vector<double> v = nonmissing(require_num(ds, "ttest", vars[0]), N);
        double m, s; mean_sd(v, m, s); int64_t n = v.size();
        double df = n - 1.0, se = s / std::sqrt(static_cast<double>(n)), t = (m - mu) / se;
        double tc = t_critical(alpha, df);
        json out = {{"estimator", "ttest"}, {"mode", "one_sample"}, {"var", vars[0]}, {"mu", mu},
                    {"n_obs", n}, {"mean", m}, {"sd", s}};
        return finish(out, m, se, t, df, m - tc * se, m + tc * se, vars[0]);
    }
    if (!by.empty()) {  // two-sample
        std::vector<double> v1, v2; double gv1, gv2;
        two_groups(require_num(ds, "ttest", vars[0]), require_num(ds, "ttest", by), N, "ttest", v1, v2, gv1, gv2);
        // reverse: swap the two group-splits (and their group values) BEFORE any
        // downstream stat is computed, so group_1/mean_1/... genuinely become
        // group_2/mean_2/...'s former values (not just a post-hoc sign flip).
        if (reverse) { std::swap(v1, v2); std::swap(gv1, gv2); }
        double m1, s1, m2, s2; mean_sd(v1, m1, s1); mean_sd(v2, m2, s2);
        int64_t n1 = static_cast<int64_t>(v1.size()), n2 = static_cast<int64_t>(v2.size());
        TwoSampleDiff r = two_sample_diff(m1, s1, n1, m2, s2, n2, unequal, welch, alpha);
        json out = {{"estimator", "ttest"},
                    {"mode", welch ? "two_sample_welch" : unequal ? "two_sample_unequal" : "two_sample"},
                    {"var", vars[0]}, {"by", by}, {"n_1", n1}, {"n_2", n2}, {"mean_1", m1}, {"mean_2", m2},
                    {"sd_1", s1}, {"sd_2", s2}, {"mean_diff", r.diff}};
        return finish(out, r.diff, r.se, r.t, r.df, r.diff - r.tc * r.se, r.diff + r.tc * r.se, "diff");
    }
    // two-variable form: name1/name2 = vars[0]/vars[1], swapped under --reverse.
    std::string name1 = vars[0], name2 = vars[1];
    if (reverse) std::swap(name1, name2);
    const double* x = require_num(ds, "ttest", name1);
    const double* y = require_num(ds, "ttest", name2);
    if (unpaired) {  // BS-089: independent samples — same math as --by() above
        std::vector<double> v1 = nonmissing(x, N), v2 = nonmissing(y, N);
        double m1, s1, m2, s2; mean_sd(v1, m1, s1); mean_sd(v2, m2, s2);
        int64_t n1 = static_cast<int64_t>(v1.size()), n2 = static_cast<int64_t>(v2.size());
        TwoSampleDiff r = two_sample_diff(m1, s1, n1, m2, s2, n2, unequal, welch, alpha);
        json out = {{"estimator", "ttest"},
                    {"mode", welch ? "unpaired_welch" : unequal ? "unpaired_unequal" : "unpaired"},
                    {"var1", name1}, {"var2", name2}, {"n_1", n1}, {"n_2", n2}, {"mean_1", m1}, {"mean_2", m2},
                    {"sd_1", s1}, {"sd_2", s2}, {"mean_diff", r.diff}};
        return finish(out, r.diff, r.se, r.t, r.df, r.diff - r.tc * r.se, r.diff + r.tc * r.se, "diff");
    }
    // paired (default): name1 - name2, row-wise on jointly-nonmissing obs
    std::vector<double> d;
    for (int64_t i = 0; i < N; ++i) if (!std::isnan(x[i]) && !std::isnan(y[i])) d.push_back(x[i] - y[i]);
    double md, sdd; mean_sd(d, md, sdd); int64_t n = d.size();
    double df = n - 1.0, se = sdd / std::sqrt(static_cast<double>(n)), t = md / se, tc = t_critical(alpha, df);
    json out = {{"estimator", "ttest"}, {"mode", "paired"}, {"var1", name1}, {"var2", name2},
                {"n_obs", n}, {"mean_diff", md}, {"sd_diff", sdd}};
    return finish(out, md, se, t, df, md - tc * se, md + tc * se, "diff");
}

namespace {

// Stata percentile of an ascending-sorted vector (matches frame.cpp's stata_pctile).
// Only used by cmd_tabstat/cmd_codebook below (same TU) — no external linkage needed.
double cli_pctile(const std::vector<double>& s, double p) {
    int64_t n = static_cast<int64_t>(s.size());
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    if (n == 1) return s[0];
    double Pi = n * p / 100.0, fl = std::floor(Pi);
    if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < n)
        return (s[static_cast<int64_t>(Pi) - 1] + s[static_cast<int64_t>(Pi)]) / 2.0;
    int64_t idx = static_cast<int64_t>(fl) + 1;
    if (idx > n) idx = n; if (idx < 1) idx = 1;
    return s[idx - 1];
}
// One tabstat statistic over a value vector (mutates a sorted copy for pctiles).
double stat_of(const std::vector<double>& v, const std::string& st) {
    const double NA = std::numeric_limits<double>::quiet_NaN();
    int64_t n = static_cast<int64_t>(v.size());
    // count/sum are well-defined even for a degenerate (empty/singleton) group — Stata posts
    // r(N)=0 and r(sum)=0 for an all-missing by-group, NOT missing. Answer these before the
    // n==0 guard so the count no longer short-circuits to missing.
    if (st == "n" || st == "count" || st == "N") return static_cast<double>(n);
    double s = 0; for (double x : v) s += x;
    if (st == "sum" || st == "total") return s;
    if (n == 0) return NA;  // mean/sd/min/... are undefined for an empty group → missing
    double mean = s / n;
    if (st == "mean") return mean;
    // Sample variance/sd/semean are undefined for n<2 (n-1=0): Stata shows "." (missing), NOT 0.
    double ss = 0; for (double x : v) ss += (x - mean) * (x - mean);
    double var = n > 1 ? ss / (n - 1) : NA, sd = std::sqrt(var);
    if (st == "sd") return sd;
    if (st == "var" || st == "variance" || st == "Var") return var;
    if (st == "cv" || st == "CV") return mean != 0 ? sd / mean : NA;
    if (st == "semean" || st == "se") return n > 1 ? sd / std::sqrt(static_cast<double>(n)) : NA;
    double mn = v[0], mx = v[0]; for (double x : v) { mn = std::min(mn, x); mx = std::max(mx, x); }
    if (st == "min" || st == "Min") return mn;
    if (st == "max" || st == "Max") return mx;
    if (st == "range") return mx - mn;
    std::vector<double> srt = v; std::sort(srt.begin(), srt.end());
    if (st == "median" || st == "p50") return cli_pctile(srt, 50);
    if (st.size() > 1 && (st[0] == 'p' || st[0] == 'P')) return cli_pctile(srt, std::stod(st.substr(1)));
    return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace

// ── tabstat ──────────────────────────────────────────────────────────────────
int cmd_tabstat(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), by = opt(a, "--by");
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    std::vector<std::string> stats = split_csv(opt(a, "--stats", "mean,sd,min,max,n"));
    if (data_missing(data) || vars.empty()) {
        std::fprintf(stderr, "usage: cobra tabstat --data f.hbin --vars v1,v2 [--by G] [--stats mean,sd,min,max,n,p50]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    std::vector<const double*> col(vars.size());
    for (size_t j = 0; j < vars.size(); ++j) col[j] = require_num(ds, "tabstat", vars[j]);
    auto block = [&](std::function<bool(int64_t)> keep) {
        json b = json::object();
        for (size_t j = 0; j < vars.size(); ++j) {
            std::vector<double> v;
            for (int64_t i = 0; i < N; ++i) if (keep(i) && !std::isnan(col[j][i])) v.push_back(col[j][i]);
            json sv = json::object();
            for (const auto& st : stats) sv[st] = stat_of(v, st);
            b[vars[j]] = sv;
        }
        return b;
    };
    json out = {{"estimator", "tabstat"}, {"n_obs", N}, {"vars", vars}, {"stats", stats},
                {"by", by.empty() ? json(nullptr) : json(by)},
                {"total", block([](int64_t) { return true; })}};
    if (!by.empty()) {
        const double* g = require_num(ds, "tabstat", by);
        std::vector<double> gv;
        for (int64_t i = 0; i < N; ++i) if (!std::isnan(g[i])) gv.push_back(g[i]);
        std::sort(gv.begin(), gv.end()); gv.erase(std::unique(gv.begin(), gv.end()), gv.end());
        json groups = json::array();
        for (double gval : gv)
            groups.push_back({{"value", gval}, {"stats", block([&](int64_t i) { return g[i] == gval; })}});
        out["groups"] = groups;
    }
    emit(out, opt(a, "--out"));
    return 0;
}

namespace {

// category list of a column: per-row code (-1 = missing), level labels, count.
// Only used by cmd_tabulate below (same TU) — no external linkage needed.
struct Categ { std::vector<int> code; std::vector<std::string> labels; std::vector<double> values; };
Categ categorize(const Dataset& ds, const std::string& name) {
    const Column& c = ds.col(name);
    int64_t N = ds.n_rows();
    Categ r; r.code.resize(static_cast<size_t>(N));
    if (c.dtype == DType::Factor) {
        // Stata lists factor levels in SORTED (alphabetical) label order, not the
        // stored first-seen dictionary order (C1-F8 / golden G7). Emit sorted
        // labels while `value` keeps each level's ORIGINAL stored code, and remap
        // per-row codes onto the sorted positions so freq[] lines up.
        const int32_t* cc = c.codes();
        const int K = static_cast<int>(c.levels.size());
        std::vector<int> perm(K);
        std::iota(perm.begin(), perm.end(), 0);
        std::stable_sort(perm.begin(), perm.end(),
                         [&](int a, int b) { return c.levels[a] < c.levels[b]; });
        std::vector<int> inv(K);
        for (int l = 0; l < K; ++l) {
            inv[perm[l]] = l;
            r.labels.push_back(c.levels[perm[l]]);
            r.values.push_back(static_cast<double>(perm[l]));  // original stored code
        }
        for (int64_t i = 0; i < N; ++i) r.code[i] = (cc[i] < 0) ? -1 : inv[cc[i]];
    } else {
        const double* x = c.f64();
        std::vector<double> vals;
        for (int64_t i = 0; i < N; ++i) if (!std::isnan(x[i])) vals.push_back(x[i]);
        std::sort(vals.begin(), vals.end()); vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
        std::unordered_map<double, int> idx;
        for (size_t l = 0; l < vals.size(); ++l) {
            idx[vals[l]] = static_cast<int>(l); r.values.push_back(vals[l]);
            char bb[32]; if (vals[l] == std::floor(vals[l]) && std::fabs(vals[l]) < 1e15)
                std::snprintf(bb, sizeof(bb), "%lld", static_cast<long long>(vals[l]));
            else std::snprintf(bb, sizeof(bb), "%g", vals[l]);
            r.labels.push_back(bb);
        }
        for (int64_t i = 0; i < N; ++i) r.code[i] = std::isnan(x[i]) ? -1 : idx[x[i]];
    }
    return r;
}

}  // namespace

// ── tabulate (one-way / two-way) ─────────────────────────────────────────────
int cmd_tabulate(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    std::string fweight = opt(a, "--fweight");
    if (data_missing(data) || vars.empty()) {
        std::fprintf(stderr, "usage: cobra tabulate --data f.hbin --vars V [,V2] [--fweight W] [--chi2]\n"
                             "  --fweight W: Stata [fw=W] — each cell freq = Sum(w); integer weights only\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    // fweight (optional): validate the WHOLE weight column up front (same rule/rationale as
    // Frame::summarize/collapse) — a non-integer, non-missing weight is an honest bad_input.
    const double* w = fweight.empty() ? nullptr : require_num(ds, "tabulate", fweight);
    if (w) {
        for (int64_t i = 0; i < N; ++i) {
            if (std::isnan(w[i])) continue;
            if (w[i] < 0.0)  // BUG-107: negative frequency weight is a hard error (would give negative cell counts)
                throw EngineError("bad_input",
                    "tabulate: fweight '" + fweight + "' has a negative value (" +
                    std::to_string(w[i]) + "); frequency weights must be non-negative", fweight);
            if (w[i] != std::floor(w[i]))
                throw EngineError("bad_input",
                    "tabulate: fweight '" + fweight + "' has a non-integer value (" +
                    std::to_string(w[i]) + "); Stata requires integer frequency weights", fweight);
        }
    }
    if (vars.size() == 1) {
        Categ ca = categorize(ds, vars[0]);
        int K = static_cast<int>(ca.labels.size());
        std::vector<long long> freq(K, 0); long long miss = 0;
        if (!w) {
            // ---- unweighted path: BYTE-UNCHANGED from pre-fweight cobra ----
            for (int64_t i = 0; i < N; ++i) { if (ca.code[i] < 0) ++miss; else ++freq[ca.code[i]]; }
        } else {
            // each cell's frequency = Sum(w) of the rows landing in it; a missing weight drops
            // the row entirely (not even counted in `miss`) — same rule as summarize/collapse.
            for (int64_t i = 0; i < N; ++i) {
                if (std::isnan(w[i])) continue;
                long long wl = static_cast<long long>(std::llround(w[i]));
                if (ca.code[i] < 0) miss += wl; else freq[ca.code[i]] += wl;
            }
        }
        long long tot = 0; for (auto f : freq) tot += f;
        json levels = json::array(); double cum = 0;
        for (int l = 0; l < K; ++l) {
            if (freq[l] == 0 && ds.col(vars[0]).dtype != DType::Factor) continue;  // numeric: only observed
            double pct = tot ? 100.0 * freq[l] / tot : 0; cum += pct;
            levels.push_back({{"value", ca.values[l]}, {"label", ca.labels[l]}, {"freq", freq[l]},
                              {"percent", pct}, {"cum", cum}});
        }
        json out = {{"estimator", "tabulate"}, {"oneway", true}, {"var", vars[0]}, {"n_obs", tot},
                    {"n_missing", miss}, {"n_levels", static_cast<int>(levels.size())}, {"levels", levels}};
        emit(out, opt(a, "--out"));
        return 0;
    }
    // two-way: vars[0]=rows, vars[1]=cols
    Categ ra = categorize(ds, vars[0]), ka = categorize(ds, vars[1]);
    int R = static_cast<int>(ra.labels.size()), C = static_cast<int>(ka.labels.size());
    std::vector<std::vector<long long>> O(R, std::vector<long long>(C, 0));
    std::vector<long long> rt(R, 0), ct(C, 0); long long n = 0;
    if (!w) {
        // ---- unweighted path: BYTE-UNCHANGED from pre-fweight cobra ----
        for (int64_t i = 0; i < N; ++i) {
            if (ra.code[i] < 0 || ka.code[i] < 0) continue;
            ++O[ra.code[i]][ka.code[i]]; ++rt[ra.code[i]]; ++ct[ka.code[i]]; ++n;
        }
    } else {
        // each cell's frequency = Sum(w); a missing weight drops the row entirely.
        for (int64_t i = 0; i < N; ++i) {
            if (ra.code[i] < 0 || ka.code[i] < 0 || std::isnan(w[i])) continue;
            long long wl = static_cast<long long>(std::llround(w[i]));
            O[ra.code[i]][ka.code[i]] += wl; rt[ra.code[i]] += wl; ct[ka.code[i]] += wl; n += wl;
        }
    }
    // drop empty rows/cols so R/C and df match Stata's observed table
    auto compact = [&]() {
        std::vector<int> rk, ck;
        for (int i = 0; i < R; ++i) if (rt[i] > 0) rk.push_back(i);
        for (int j = 0; j < C; ++j) if (ct[j] > 0) ck.push_back(j);
        std::vector<std::vector<long long>> O2(rk.size(), std::vector<long long>(ck.size(), 0));
        std::vector<long long> rt2(rk.size(), 0), ct2(ck.size(), 0);
        std::vector<std::string> rl, cl; std::vector<double> rv, cv;
        for (size_t i = 0; i < rk.size(); ++i) { rt2[i] = rt[rk[i]]; rl.push_back(ra.labels[rk[i]]); rv.push_back(ra.values[rk[i]]); }
        for (size_t j = 0; j < ck.size(); ++j) { ct2[j] = ct[ck[j]]; cl.push_back(ka.labels[ck[j]]); cv.push_back(ka.values[ck[j]]); }
        for (size_t i = 0; i < rk.size(); ++i) for (size_t j = 0; j < ck.size(); ++j) O2[i][j] = O[rk[i]][ck[j]];
        O = O2; rt = rt2; ct = ct2; R = static_cast<int>(rk.size()); C = static_cast<int>(ck.size());
        ra.labels = rl; ra.values = rv; ka.labels = cl; ka.values = cv;
    };
    compact();
    double chi2 = 0, lr = 0;
    for (int i = 0; i < R; ++i) for (int j = 0; j < C; ++j) {
        double E = static_cast<double>(rt[i]) * ct[j] / n, Oij = O[i][j];
        if (E > 0) chi2 += (Oij - E) * (Oij - E) / E;
        if (Oij > 0 && E > 0) lr += 2.0 * Oij * std::log(Oij / E);
    }
    int df = (R - 1) * (C - 1);
    double V = std::sqrt(chi2 / (static_cast<double>(n) * (std::min(R, C) - 1)));
    // concordant/discordant for gamma + tau-b (row/col indices treated as ordinal)
    double P = 0, Q = 0;
    for (int i = 0; i < R; ++i) for (int j = 0; j < C; ++j) {
        long long gt = 0, lt = 0;
        for (int i2 = i + 1; i2 < R; ++i2) for (int j2 = 0; j2 < C; ++j2) {
            if (j2 > j) gt += O[i2][j2]; else if (j2 < j) lt += O[i2][j2];
        }
        P += static_cast<double>(O[i][j]) * gt; Q += static_cast<double>(O[i][j]) * lt;
    }
    double gamma = (P + Q) > 0 ? (P - Q) / (P + Q) : std::numeric_limits<double>::quiet_NaN();
    double tot_pairs = static_cast<double>(n) * (n - 1) / 2.0, tx = 0, ty = 0;
    for (int i = 0; i < R; ++i) tx += static_cast<double>(rt[i]) * (rt[i] - 1) / 2.0;
    for (int j = 0; j < C; ++j) ty += static_cast<double>(ct[j]) * (ct[j] - 1) / 2.0;
    double taub = (P - Q) / std::sqrt((tot_pairs - tx) * (tot_pairs - ty));
    json cells = json::array();
    for (int i = 0; i < R; ++i) { json row = json::array();
        for (int j = 0; j < C; ++j) row.push_back(O[i][j]); cells.push_back(row); }
    json out = {{"estimator", "tabulate"}, {"oneway", false}, {"row_var", vars[0]}, {"col_var", vars[1]},
                {"n_obs", n}, {"r", R}, {"c", C}, {"row_levels", ra.values}, {"col_levels", ka.values},
                {"cells", cells}, {"row_totals", rt}, {"col_totals", ct},
                {"chi2", chi2}, {"df", df}, {"p", chi2_pvalue(chi2, df)},
                {"lr_chi2", lr}, {"p_lr", chi2_pvalue(lr, df)}, {"cramers_v", V},
                {"gamma", gamma}, {"taub", taub}};
    emit(out, opt(a, "--out"));
    return 0;
}

// ── codebook ─────────────────────────────────────────────────────────────────
int cmd_codebook(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data");
    if (data_missing(data)) { std::fprintf(stderr, "usage: cobra codebook --data f.hbin [--vars v1,..]\n"); return 2; }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const int64_t N = ds.n_rows();
    std::vector<std::string> vars = split_csv(opt(a, "--vars"));
    if (vars.empty()) for (const Column& c : ds.columns()) vars.push_back(c.name);
    json entries = json::array();
    for (const std::string& name : vars) {
        const Column& c = ds.col(name);
        json e = {{"name", name}};
        if (c.dtype == DType::Factor) {
            const int32_t* cd = c.codes();
            int64_t nm = 0; std::vector<long long> freq(c.levels.size(), 0);
            for (int64_t i = 0; i < N; ++i) { if (cd[i] < 0) ++nm; else ++freq[cd[i]]; }
            int uniq = 0; for (auto f : freq) if (f > 0) ++uniq;
            e["type"] = "factor"; e["N"] = N; e["n"] = N - nm; e["n_missing"] = nm;
            e["n_unique"] = uniq;
        } else {
            const double* x = c.f64();
            std::vector<double> v; for (int64_t i = 0; i < N; ++i) if (!std::isnan(x[i])) v.push_back(x[i]);
            int64_t nm = N - static_cast<int64_t>(v.size());
            std::vector<double> srt = v; std::sort(srt.begin(), srt.end());
            std::vector<double> uq = srt; uq.erase(std::unique(uq.begin(), uq.end()), uq.end());
            e["type"] = "numeric"; e["N"] = N; e["n"] = static_cast<int64_t>(v.size()); e["n_missing"] = nm;
            e["n_unique"] = static_cast<int>(uq.size());
            if (!v.empty()) {
                e["mean"] = stat_of(v, "mean"); e["sd"] = stat_of(v, "sd"); e["var"] = stat_of(v, "var");
                e["min"] = srt.front(); e["max"] = srt.back();
                for (int p : {10, 25, 50, 75, 90}) e["p" + std::to_string(p)] = cli_pctile(srt, p);
                if (static_cast<int>(uq.size()) <= 10) {  // low-cardinality: frequency table
                    json freq = json::array();
                    for (double val : uq) { long long f = 0; for (double z : v) if (z == val) ++f;
                        freq.push_back({{"value", val}, {"freq", f}}); }
                    e["freq"] = freq;
                }
            }
        }
        entries.push_back(e);
    }
    json out = {{"estimator", "codebook"}, {"n_obs", N}, {"n_vars", static_cast<int>(vars.size())},
                {"entries", entries}};
    emit(out, opt(a, "--out"));
    return 0;
}

// collapse — aggregate to one row per by-group (Stata collapse). --stat uses the
// Stata "(fn) var..." syntax; default fn = mean.
int cmd_collapse(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), spec = opt(a, "--stat"), out = opt(a, "--out");
    std::string fweight = opt(a, "--fweight");
    if (data_missing(data) || spec.empty() || out_missing(out)) {
        std::fprintf(stderr, "usage: cobra collapse --data f.hbin --stat \"(mean) y1 y2 (sum) z\" "
                             "--by g1,g2 [--fweight W] [--keepif EXPR] --out new.hbin\n"
                             "  stats: mean sum sd count max min median p<NN> first last\n"
                             "  --fweight W: Stata [fw=W] (integer weights only; not combinable with median/p<NN>)\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    std::vector<Frame::CollapseSpec> specs;
    {
        std::stringstream ss(spec);
        std::string tok, cur = "mean";
        while (ss >> tok) {
            if (tok.front() == '(' && tok.back() == ')') { cur = tok.substr(1, tok.size() - 2); continue; }
            auto eq = tok.find('=');  // alias=var (Stata newname=oldname)
            if (eq != std::string::npos) specs.push_back({cur, tok.substr(eq + 1), tok.substr(0, eq)});
            else specs.push_back({cur, tok, ""});
        }
    }
    if (specs.empty()) { std::fprintf(stderr, "collapse: no variables in --stat\n"); return 2; }
    const Dataset& ds = acquire_data(data);
    Frame fr = frame_from_acquired(ds);
    Frame res = fr.collapse(specs, split_csv(opt(a, "--by")), fweight);
    int64_t n_groups = res.n_rows();  // capture before the move into commit_frame
    if (!commit_frame(std::move(res), out))  // §26: stage into resident session, or write+notice one-shot
        std::fprintf(stderr, "wrote %s (%lld groups)\n", out.c_str(), static_cast<long long>(n_groups));
    return 0;
}

}  // namespace cli
}  // namespace cecon
