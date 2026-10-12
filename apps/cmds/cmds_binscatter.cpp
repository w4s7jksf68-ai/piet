#include "common.h"
#include "commands.h"

#include "core/matrix.h"
#include "estimators/ols.h"

// P-QE wave — binscatter (engine half). Stata's `binscatter y x, nquantiles(N)`
// prints NOTHING to the console (rc 0, a graph only) — so this JSON is the ONLY
// place the numbers live; the PIET layer draws the picture from it. Everything
// this command needs is already reused, not reimplemented: Frame::xtile
// (src/data/frame.cpp) owns the quantile-binning tie rule, and build_design +
// ols_fit (the same pair `cobra ols` calls) own the overlay fit line, so the
// fit here is byte-identical to a plain `cobra ols --y Y --x X` over the same
// sample.

namespace cecon {
namespace cli {

int cmd_binscatter(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), yname = opt(a, "--y"), xname = opt(a, "--x");
    if (data_missing(data) || yname.empty() || xname.empty()) {
        std::fprintf(stderr,
            "usage: cobra binscatter --data f.hbin --y Y --x X [--nquantiles N] [--out f.json]\n"
            "  Splits X into N quantile bins (Stata `xtile`; default N=20), reports the\n"
            "  within-bin mean of Y against the within-bin mean of X, and overlays a\n"
            "  linear fit (plain OLS of Y on X) over the SAME listwise-deleted sample.\n"
            "  Stata binscatter's controls() and absorb() are NOT supported by this\n"
            "  build and are rejected by name if passed.\n");
        return 2;
    }
    // Out of scope this wave (decided in advance, not a gap): naming the refused
    // option is the parity contract here — silently ignoring controls()/absorb()
    // would answer a different question than the one asked.
    if (has(a, "--controls"))
        throw EngineError("unsupported",
            "binscatter: controls() is not supported in this build (residualizing "
            "on covariates before binning is not implemented) — rerun without "
            "--controls");
    if (has(a, "--absorb"))
        throw EngineError("unsupported",
            "binscatter: absorb() is not supported in this build (fixed-effect "
            "absorption before binning is not implemented) — rerun without "
            "--absorb");

    int nq = std::stoi(opt(a, "--nquantiles", "20"));
    if (nq < 2)
        throw EngineError("bad_input",
            "binscatter: --nquantiles must be >= 2 (got " + std::to_string(nq) + ")");

    const Dataset& ds = acquire_data(data);

    // Listwise-deleted (y, x) sample — the SAME sample `cobra ols --y Y --x X`
    // would use (build_design drops a row if either is missing). Binning and the
    // fit line both run over this sample, which is what makes the two commands'
    // slopes comparable at all.
    DesignMatrix dm = build_design(ds, yname, {xname}, /*intercept=*/true);
    if (dm.n == 0)
        throw EngineError("no_obs",
            "binscatter: no rows left after dropping missing " + yname + "/" + xname);
    OlsResult r = ols_fit(dm);
    // dm.xnames / r.names = ["(intercept)", xname] (build_design puts the
    // intercept first) — r.beta parallels that order.
    double fit_intercept = r.beta[0], fit_slope = r.beta[1];

    // Bin the listwise-deleted x sample with the house xtile machinery (its tie
    // rule — ties fall in the lower group — is NOT redefined here). A scratch
    // Frame built via the bulk-build API (set_n_rows_hint/append_f64_column/
    // finalize_after_bulk_load) is the documented way to hand xtile a column that
    // didn't come from an on-disk dataset.
    Frame binfr;
    binfr.set_n_rows_hint(dm.n);
    {
        const double* xcol = dm.col(1);
        binfr.append_f64_column(xname, std::vector<double>(xcol, xcol + dm.n));
    }
    binfr.finalize_after_bulk_load();
    binfr.xtile("__bin", xname, nq);
    const auto& bincol = binfr.col("__bin").f64;

    std::vector<double> sum_x(static_cast<size_t>(nq) + 1, 0.0);
    std::vector<double> sum_y(static_cast<size_t>(nq) + 1, 0.0);
    std::vector<int64_t> cnt(static_cast<size_t>(nq) + 1, 0);
    const double* xcol = dm.col(1);
    for (int64_t i = 0; i < dm.n; ++i) {
        // x is already listwise-non-missing here, so xtile cannot have emitted
        // NaN for this row; the group is in [1, nq] by construction.
        int g = static_cast<int>(bincol[static_cast<std::size_t>(i)]);
        sum_x[static_cast<size_t>(g)] += xcol[i];
        sum_y[static_cast<size_t>(g)] += dm.y[i];
        cnt[static_cast<size_t>(g)] += 1;
    }

    json bins = json::array();
    int n_bins = 0;
    for (int g = 1; g <= nq; ++g) {
        int64_t c = cnt[static_cast<size_t>(g)];
        if (c == 0) continue;  // Stata drops an empty bin rather than plotting a NaN mean
        ++n_bins;
        bins.push_back({{"bin", g},
                        {"x_mean", sum_x[static_cast<size_t>(g)] / static_cast<double>(c)},
                        {"y_mean", sum_y[static_cast<size_t>(g)] / static_cast<double>(c)},
                        {"n", c}});
    }

    json out = {
        {"estimator", "binscatter"},
        {"depvar", yname},
        {"xvar", xname},
        {"nquantiles", nq},
        {"n_bins", n_bins},
        {"bins", bins},
        {"fit", {{"intercept", fit_intercept}, {"slope", fit_slope}}},
        {"n_obs", dm.n},
        {"n_dropped", dm.n_dropped},
        {"n_raw", ds.n_rows()},
    };
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
