#include "common.h"
#include "commands.h"

#include "estimators/aer_smooth.h"
#include "estimators/pca.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

// AER bundle H1 — the engine half of the four verbs PIET's bundle-H handlers
// were refusing by name: `tsfilter hp`, `lpoly`, `lowess` and `pca`.
//
// Every statistical decision here was verified against StataNow 19.5 BEFORE it
// was written (plans/ERRATA-AER-H-SPECS-20260904.md), because bundle H's tests
// would otherwise have been written from the same transcription as its code.
// The mathematics lives in src/estimators/aer_smooth.* and src/estimators/pca.*;
// this file is argument handling, the sample, and the JSON.

namespace cecon {
namespace cli {

namespace {

// The rows this command may use: `keep` marks them, indexed like the dataset.
std::vector<char> keep_mask(const Dataset& ds, const std::vector<const double*>& cols) {
    std::vector<char> keep(static_cast<std::size_t>(ds.n_rows()), 1);
    for (int64_t i = 0; i < ds.n_rows(); ++i) {
        for (const double* c : cols) {
            if (std::isnan(c[i])) { keep[static_cast<std::size_t>(i)] = 0; break; }
        }
    }
    return keep;
}

}  // namespace

// ---------------------------------------------------------------------------
// tsfilter hp

int cmd_hpfilter(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), yname = opt(a, "--y");
    if (data_missing(data) || yname.empty()) {
        std::fprintf(stderr,
            "usage: cobra hpfilter --data f.hbin --y Y [--lambda L | --tsfmt %%tq]\n"
            "                      [--panel ID --time T] [--out f.json]\n"
            "  Hodrick-Prescott: tau = (I + lambda*K'K)^-1 y, cycle = y - tau.\n"
            "  --tsfmt supplies the Ravn-Uhlig rescaled default when --lambda is absent\n"
            "  (%%ty 6.25, %%th 100, %%tq 1600, %%tm 129600, %%tw 33177600,\n"
            "   %%td 110930628906.25); anything else defaults to 1600.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* y = require_num(ds, "hpfilter", yname);

    double lambda;
    const std::string lamOpt = opt(a, "--lambda");
    if (!lamOpt.empty()) {
        lambda = std::stod(lamOpt);
        if (!(lambda > 0))
            throw EngineError("bad_input", "hpfilter: --lambda must be positive");
    } else {
        const double byFmt = hp_lambda_for_format(opt(a, "--tsfmt"));
        lambda = (byFmt > 0) ? byFmt : 1600.0;
    }

    const std::string pname = opt(a, "--panel");
    // BUG-427: `--time T` was in this command's own usage text and was read
    // NOWHERE, so it reached the post-hoc unconsumed-flag net and came back as
    // "unrecognized option" AFTER the run had already written `--out` (the net
    // is by design: dispatch_guard checks consumption after the command
    // returns). It does the thing the usage text advertises now: the filter
    // runs each series in TIME order rather than in file order. Without it an
    // unsorted panel was filtered in whatever order the rows happened to sit
    // in, which is a wrong answer at rc 0 — the reason the option was drafted.
    const std::string tname = opt(a, "--time");
    const double* tvar = tname.empty() ? nullptr : require_num(ds, "hpfilter", tname);
    const auto order_by_time = [&](std::vector<int64_t>& idx) {
        if (tvar == nullptr) return;
        std::stable_sort(idx.begin(), idx.end(), [&](int64_t a2, int64_t b2) {
            const double ta = tvar[a2], tb = tvar[b2];
            if (std::isnan(ta) || std::isnan(tb)) return false;  // missing keeps its place
            return ta < tb;
        });
    };
    const int64_t n = ds.n_rows();
    std::vector<double> cycle(static_cast<std::size_t>(n),
                              std::numeric_limits<double>::quiet_NaN());
    std::vector<double> trend = cycle;
    std::vector<std::string> notes;
    int n_panels = 0;

    // Panels are independent, so each is filtered on its own contiguous run of
    // non-missing rows. A SHORT PANEL IS NOT AN ERROR: live StataBE prints
    // `note: insufficient observations for panel <#>`, leaves that panel's
    // cycle/trend missing, and completes with rc 0 while every other panel is
    // computed. Only the single-series case refuses (see the else branch).
    if (!pname.empty()) {
        const double* pid = require_num(ds, "hpfilter", pname);
        std::vector<std::pair<double, std::vector<int64_t>>> groups;
        std::map<double, std::size_t> at;
        for (int64_t i = 0; i < n; ++i) {
            if (std::isnan(pid[i]) || std::isnan(y[i])) continue;
            auto it = at.find(pid[i]);
            if (it == at.end()) {
                at.emplace(pid[i], groups.size());
                groups.emplace_back(pid[i], std::vector<int64_t>{i});
            } else {
                groups[it->second].second.push_back(i);
            }
        }
        for (auto& [gid, idx] : groups) {
            order_by_time(idx);
            ++n_panels;
            if (idx.size() < 3) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "insufficient observations for panel %g", gid);
                notes.emplace_back(buf);
                continue;
            }
            std::vector<double> yy;
            yy.reserve(idx.size());
            for (int64_t i : idx) yy.push_back(y[i]);
            HpResult hr = hp_filter(yy, lambda);
            for (std::size_t t = 0; t < idx.size(); ++t) {
                cycle[static_cast<std::size_t>(idx[t])] = hr.cycle[t];
                trend[static_cast<std::size_t>(idx[t])] = hr.trend[t];
            }
        }
    } else {
        std::vector<int64_t> idx;
        for (int64_t i = 0; i < n; ++i)
            if (!std::isnan(y[i])) idx.push_back(i);
        order_by_time(idx);
        // Single series: Stata REFUSES rather than returning a zero cycle, and
        // the two short cases carry different codes.
        if (idx.size() == 1)
            throw EngineError("bad_input", "hpfilter: insufficient observations", "r(2001)");
        if (idx.size() == 2)
            throw EngineError("bad_input",
                              "hpfilter: insufficient observations for the specified sample",
                              "r(498)");
        if (idx.empty())
            throw EngineError("bad_input", "hpfilter: no observations", "r(2000)");
        n_panels = 1;
        std::vector<double> yy;
        yy.reserve(idx.size());
        for (int64_t i : idx) yy.push_back(y[i]);
        HpResult hr = hp_filter(yy, lambda);
        for (std::size_t t = 0; t < idx.size(); ++t) {
            cycle[static_cast<std::size_t>(idx[t])] = hr.cycle[t];
            trend[static_cast<std::size_t>(idx[t])] = hr.trend[t];
        }
    }

    json out = {
        {"estimator", "hpfilter"},
        {"depvar", yname},
        {"lambda", lambda},
        {"n", n},
        {"n_panels", n_panels},
        {"cycle", cycle},
        {"trend", trend},
        {"notes", notes},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

// ---------------------------------------------------------------------------
// lowess

int cmd_lowess(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), yname = opt(a, "--y"), xname = opt(a, "--x");
    if (data_missing(data) || yname.empty() || xname.empty()) {
        std::fprintf(stderr,
            "usage: cobra lowess --data f.hbin --y Y --x X [--bw B] [--mean] [--noweight]\n"
            "                    [--keepif EXPR] [--out f.json]\n"
            "  Tricube LOWESS with NO robustness iterations ([R] lowess p.1604).\n"
            "  A --bw outside the OPEN interval (0,1) is silently replaced by 0.8,\n"
            "  which is what lowess.ado does; the emitted `bwidth` is the EFFECTIVE one.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* y = require_num(ds, "lowess", yname);
    const double* x = require_num(ds, "lowess", xname);

    LowessOpts o;
    const std::string bwOpt = opt(a, "--bw");
    if (!bwOpt.empty()) o.bwidth = std::stod(bwOpt);
    o.mean = has(a, "--mean");
    o.noweight = has(a, "--noweight");

    // Ascending by x, ties broken by ROW INDEX so the order is deterministic
    // (Stata's own sort is not stable and the goldens carry a tolnote for it).
    std::vector<int64_t> idx;
    for (int64_t i = 0; i < ds.n_rows(); ++i)
        if (!std::isnan(x[i]) && !std::isnan(y[i])) idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(),
                     [&](int64_t p, int64_t q) { return x[p] < x[q]; });

    std::vector<double> xs, ys;
    xs.reserve(idx.size());
    ys.reserve(idx.size());
    for (int64_t i : idx) { xs.push_back(x[i]); ys.push_back(y[i]); }

    const std::vector<double> sm = lowess_fit(xs, ys, o);
    std::vector<double> out_col(static_cast<std::size_t>(ds.n_rows()),
                                 std::numeric_limits<double>::quiet_NaN());
    for (std::size_t t = 0; t < idx.size(); ++t)
        out_col[static_cast<std::size_t>(idx[t])] = sm[t];

    json out = {
        {"estimator", "lowess"},
        {"depvar", yname},
        {"xvar", xname},
        {"bwidth", lowess_effective_bwidth(o.bwidth)},  // the SUBSTITUTED value
        {"mean", o.mean},
        {"noweight", o.noweight},
        {"n_obs", static_cast<int64_t>(idx.size())},
        {"smooth", out_col},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

// ---------------------------------------------------------------------------
// lpoly

int cmd_lpoly(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), yname = opt(a, "--y"), xname = opt(a, "--x");
    if (data_missing(data) || yname.empty() || xname.empty()) {
        std::fprintf(stderr,
            "usage: cobra lpoly --data f.hbin --y Y --x X [--degree D] [--kernel K]\n"
            "                   [--bw H] [--n G] [--pwidth P] [--keepif EXPR] [--out f.json]\n"
            "  Local polynomial smoothing with the Fan-Gijbels rule-of-thumb bandwidth.\n"
            "  Kernels: epanechnikov (default) epan2 biweight cosine gaussian parzen\n"
            "  rectangle triangle.\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* y = require_num(ds, "lpoly", yname);
    const double* x = require_num(ds, "lpoly", xname);

    LpolyOpts o;
    const std::string kname = opt(a, "--kernel");
    if (!lpoly_kernel_from_name(kname, &o.kernel))
        throw EngineError("bad_input", "lpoly: unknown kernel '" + kname + "'");
    const std::string dOpt = opt(a, "--degree");
    if (!dOpt.empty()) o.degree = std::stoi(dOpt);
    if (o.degree < 0) throw EngineError("bad_input", "lpoly: --degree must be >= 0");
    const std::string bwOpt = opt(a, "--bw");
    if (!bwOpt.empty()) o.bwidth = std::stod(bwOpt);
    const std::string pwOpt = opt(a, "--pwidth");
    if (!pwOpt.empty()) o.pwidth = std::stod(pwOpt);
    const std::string nOpt = opt(a, "--n");
    if (!nOpt.empty()) o.n = std::stoi(nOpt);

    std::vector<double> xs, ys;
    for (int64_t i = 0; i < ds.n_rows(); ++i) {
        if (std::isnan(x[i]) || std::isnan(y[i])) continue;
        xs.push_back(x[i]);
        ys.push_back(y[i]);
    }
    if (xs.size() < 2) throw EngineError("bad_input", "lpoly: too few observations");
    // Sorting is not required by the local fit, but it makes the ROT's global
    // polynomial and the grid deterministic in the same order every run.
    {
        std::vector<std::size_t> ord(xs.size());
        std::iota(ord.begin(), ord.end(), 0);
        std::stable_sort(ord.begin(), ord.end(), [&](std::size_t p, std::size_t q) { return xs[p] < xs[q]; });
        std::vector<double> xt(xs.size()), yt(ys.size());
        for (std::size_t t = 0; t < ord.size(); ++t) { xt[t] = xs[ord[t]]; yt[t] = ys[ord[t]]; }
        xs.swap(xt);
        ys.swap(yt);
    }

    LpolyResult r = lpoly_fit(xs, ys, o);
    if (r.grid.empty()) {
        // The only way here is an unpinned rule-of-thumb constant. Refuse BY
        // NAME rather than inventing one: the constants are Stata-effective
        // values backed out of live runs, and only p=1 and p=3 (plus rectangle
        // at p=5) have been measured.
        throw EngineError("unsupported",
            std::string("lpoly: no rule-of-thumb bandwidth constant is pinned for kernel '") +
            lpoly_kernel_name(o.kernel) + "' at degree " + std::to_string(o.degree) +
            " — pass --bw explicitly");
    }

    json out = {
        {"estimator", "lpoly"},
        {"depvar", yname},
        {"xvar", xname},
        {"kernel", lpoly_kernel_name(o.kernel)},
        {"degree", o.degree},
        {"bwidth", r.bwidth},
        {"pwidth", r.pwidth},
        {"grid", r.grid},
        {"smooth", r.smooth},
        {"se", nullptr},
        {"n_obs", static_cast<int64_t>(xs.size())},
        {"ngrid", r.ngrid},  // SUCCESSFUL fits, which can be < grid.size()
    };
    emit(out, opt(a, "--out"));
    return 0;
}

// ---------------------------------------------------------------------------
// pca

int cmd_pca(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), xs = opt(a, "--x");
    if (data_missing(data) || xs.empty()) {
        std::fprintf(stderr,
            "usage: cobra pca --data f.hbin --x V1,V2,... [--covariance] [--comp N]\n"
            "                 [--mineigen E] [--keepif EXPR] [--out f.json]\n"
            "  Principal components on the correlation (default) or covariance matrix.\n");
        return 2;
    }
    if (has(a, "--covariance") && has(a, "--correlation"))
        throw EngineError("bad_input", "pca: options correlation and covariance are exclusive",
                          "r(198)");
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);

    std::vector<std::string> names = split_csv(xs);
    std::vector<const double*> cols;
    cols.reserve(names.size());
    for (const auto& nm : names) cols.push_back(require_num(ds, "pca", nm));

    PcaOpts o;
    o.covariance = has(a, "--covariance");
    const std::string cOpt = opt(a, "--comp");
    if (!cOpt.empty()) {
        o.components = std::stoi(cOpt);
        // pcamat.ado errors 125 here; the manual's contrary sentence ("larger
        // than the number of variables is equivalent to the number of
        // variables") is wrong and the code is what ships.
        if (o.components < 1 || o.components > static_cast<int>(names.size()))
            throw EngineError("bad_input",
                              "pca: comp() should be between 1 and " +
                                  std::to_string(names.size()),
                              "r(125)");
    }
    const std::string mOpt = opt(a, "--mineigen");
    if (!mOpt.empty()) o.mineigen = std::stod(mOpt);

    const std::vector<char> keep = keep_mask(ds, cols);
    PcaResult R = pca_fit(names, cols, keep, o);

    // f == 0 posts a deliberately SHORT record: Stata prints "(no components
    // retained)" and posts neither e(L) nor e(Psi) nor trace/lndet/cond.
    json out = {
        {"estimator", "pca"},
        {"n_obs", R.n_obs},
        {"p", R.p},
        {"f", R.f},
        {"ctype", R.ctype},
        {"eigenvalues", R.ev},
        {"c", R.c},
        {"means", R.means},
        {"vars", R.vars},
        {"dropped", R.dropped},
    };
    if (!R.sds.empty()) out["sds"] = R.sds;  // correlation PCA only
    if (R.f > 0) {
        out["loadings"] = R.loadings;
        out["psi"] = R.psi;
        out["components"] = R.components_tbl;
        out["rho"] = R.rho;
        out["trace"] = R.trace;
        out["lndet"] = R.lndet_missing ? json(nullptr) : json(R.lndet);
        out["cond"] = R.cond_missing ? json(nullptr) : json(R.cond);
    } else {
        out["rho"] = 0.0;
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
