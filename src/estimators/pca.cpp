#include "estimators/pca.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/error.h"
#include "core/linalg.h"

namespace cecon {

namespace {

// Stata's chop(x, eps): snap to the nearest integer when within eps of it.
double chop(double x, double eps) {
    const double r = std::round(x);
    return (std::fabs(x - r) < eps) ? r : x;
}

}  // namespace

PcaResult pca_fit(const std::vector<std::string>& names, const std::vector<const double*>& cols,
                  const std::vector<char>& keep, const PcaOpts& o) {
    PcaResult R;
    const std::size_t nrows = keep.size();

    // ---- listwise-complete row set -----------------------------------------
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < nrows; ++i) {
        if (!keep[i]) continue;
        bool ok = true;
        for (const double* c : cols) {
            if (std::isnan(c[i])) { ok = false; break; }
        }
        if (ok) rows.push_back(i);
    }
    const std::size_t N = rows.size();
    // pca.ado screens the SAMPLE before it screens the variables.
    if (N == 0) throw EngineError("bad_input", "pca: no observations", "r(2000)");
    if (N == 1) throw EngineError("bad_input", "pca: insufficient observations", "r(2001)");

    // ---- zero-variance screening -------------------------------------------
    // Silently dropped with a note, exactly as pca.ado does; the note text is
    // the caller's to print.
    std::vector<std::size_t> keepCols;
    for (std::size_t j = 0; j < cols.size(); ++j) {
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        for (std::size_t r : rows) {
            const double v = cols[j][r];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (hi <= lo) R.dropped.push_back(names[j]);
        else keepCols.push_back(j);
    }
    if (keepCols.empty())
        throw EngineError("bad_input", "pca: all variables dropped because of zero variance",
                          "r(498)");
    if (keepCols.size() < 2)
        throw EngineError("bad_input", "pca: too few variables", "r(102)");

    const int p = static_cast<int>(keepCols.size());
    R.p = p;
    R.n_obs = static_cast<int>(N);
    R.ctype = o.covariance ? "covariance" : "correlation";

    // ---- means, then the (N-1)-divisor covariance --------------------------
    // pca.ado: `matrix accum C = varlist, dev means(Means) nocons` followed by
    // `matrix C = (1/(nobs-1)) * C`. The manual never states the divisor, and n
    // instead of n-1 would scale every covariance eigenvalue by n/(n-1) --
    // 1.37% at n=74.
    R.means.assign(static_cast<std::size_t>(p), 0.0);
    for (int j = 0; j < p; ++j) {
        double s = 0;
        for (std::size_t r : rows) s += cols[keepCols[static_cast<std::size_t>(j)]][r];
        R.means[static_cast<std::size_t>(j)] = s / static_cast<double>(N);
    }
    std::vector<double> cov(static_cast<std::size_t>(p) * static_cast<std::size_t>(p), 0.0);
    for (int a = 0; a < p; ++a) {
        for (int b = a; b < p; ++b) {
            double s = 0;
            for (std::size_t r : rows) {
                s += (cols[keepCols[static_cast<std::size_t>(a)]][r] - R.means[static_cast<std::size_t>(a)]) *
                     (cols[keepCols[static_cast<std::size_t>(b)]][r] - R.means[static_cast<std::size_t>(b)]);
            }
            s /= static_cast<double>(N - 1);
            cov[static_cast<std::size_t>(a * p + b)] = s;
            cov[static_cast<std::size_t>(b * p + a)] = s;
        }
    }
    R.vars.assign(static_cast<std::size_t>(p), 0.0);
    R.sds.assign(static_cast<std::size_t>(p), 0.0);
    for (int j = 0; j < p; ++j) {
        R.vars[static_cast<std::size_t>(j)] = cov[static_cast<std::size_t>(j * p + j)];
        R.sds[static_cast<std::size_t>(j)] = std::sqrt(R.vars[static_cast<std::size_t>(j)]);
    }

    R.c = cov;
    if (!o.covariance) {
        for (int a = 0; a < p; ++a)
            for (int b = 0; b < p; ++b)
                R.c[static_cast<std::size_t>(a * p + b)] =
                    cov[static_cast<std::size_t>(a * p + b)] /
                    (R.sds[static_cast<std::size_t>(a)] * R.sds[static_cast<std::size_t>(b)]);
    } else {
        // e(sds) is posted ONLY for correlation PCA -- `confirm matrix e(sds)`
        // returns rc 111 after a covariance run.
        R.sds.clear();
    }

    // ---- eigendecomposition -------------------------------------------------
    std::vector<double> A = R.c;
    std::vector<double> w(static_cast<std::size_t>(p), 0.0);
    if (la::sym_eig(A.data(), p, w.data()) != 0)
        throw EngineError("numerical", "pca: eigendecomposition failed");

    // sym_eig returns ASCENDING with eigenvectors in COLUMNS; Stata wants
    // descending.
    std::vector<double> ev(static_cast<std::size_t>(p));
    std::vector<double> vecs(static_cast<std::size_t>(p) * static_cast<std::size_t>(p));
    for (int j = 0; j < p; ++j) {
        const int src = p - 1 - j;
        ev[static_cast<std::size_t>(j)] = w[static_cast<std::size_t>(src)];
        for (int i = 0; i < p; ++i)
            vecs[static_cast<std::size_t>(i * p + j)] = A[static_cast<std::size_t>(src * p + i)];
    }

    // CheckEigenvalues: a genuinely negative eigenvalue is an error, not a clip.
    const double lmax = ev[0];
    for (int j = 0; j < p; ++j) {
        if (ev[static_cast<std::size_t>(j)] < -1e-5 * std::fabs(lmax))
            throw EngineError("numerical", "pca: negative eigenvalue found where none expected",
                              "r(198)");
    }
    // RELATIVE 1e-8 zeroing against lambda_max, applied BEFORE trace/lndet/cond
    // and before the mineigen retention loop. This is the mechanism that makes a
    // collinear run report missing lndet AND missing cond AND a reduced f.
    for (int j = 0; j < p; ++j) {
        if (lmax > 0 && ev[static_cast<std::size_t>(j)] / lmax < 1e-8)
            ev[static_cast<std::size_t>(j)] = 0.0;
    }

    // Sign convention: flip iff 1'v < 0. Exactly 0 is left alone -- Stata
    // delegates the sign to `matrix symeigen` and has no tie rule of its own, so
    // this case is OUR convention and not parity.
    for (int j = 0; j < p; ++j) {
        double s = 0;
        for (int i = 0; i < p; ++i) s += vecs[static_cast<std::size_t>(i * p + j)];
        if (s < 0)
            for (int i = 0; i < p; ++i) vecs[static_cast<std::size_t>(i * p + j)] *= -1.0;
    }
    R.ev = ev;

    // ---- retention ----------------------------------------------------------
    // f = min(components() or p, #{LEADING i : lambda_i >= mineigen}); retention
    // is >= , measured at the boundary. The default mineigen is 1e-5, so the
    // default f is NOT p.
    int nf = (o.components > 0) ? std::min(o.components, p) : p;
    while (nf > 0 && ev[static_cast<std::size_t>(nf - 1)] < o.mineigen) --nf;
    R.f = nf;

    // ---- trace, lndet, cond -------------------------------------------------
    double sum = 0;
    for (int j = 0; j < p; ++j) sum += ev[static_cast<std::size_t>(j)];
    // RELATIVE 1e-6 integer snap, from the EIGENVALUE SUM (not the C diagonal --
    // they differ in the last bits). auto covariance PCA: raw 9304104.43132173
    // becomes exactly 9304104, a 4.6e-8 relative move, which is 46x outside a
    // 1e-9 golden and is why a naive sum(C_ii) fails there.
    R.trace = chop(sum, std::fabs(sum) * 1e-6);

    double ld = 0;
    bool anyZero = false;
    for (int j = 0; j < p; ++j) {
        if (ev[static_cast<std::size_t>(j)] <= 0) { anyZero = true; break; }
        ld += std::log(ev[static_cast<std::size_t>(j)]);
    }
    R.lndet = anyZero ? 0.0 : ld;
    R.lndet_missing = anyZero;

    const double lmin = ev[static_cast<std::size_t>(p - 1)];
    if (lmin > 0) {
        R.cond = std::sqrt(lmax / lmin);  // sqrt of the RATIO, over the FULL spectrum
        R.cond_missing = false;
    } else {
        R.cond = 0;
        R.cond_missing = true;
    }

    double topsum = 0;
    for (int j = 0; j < nf; ++j) topsum += ev[static_cast<std::size_t>(j)];
    R.rho = (R.trace != 0) ? topsum / R.trace : 0.0;

    // ---- loadings, uniquenesses --------------------------------------------
    R.loadings.assign(static_cast<std::size_t>(p) * static_cast<std::size_t>(std::max(nf, 0)), 0.0);
    for (int i = 0; i < p; ++i)
        for (int j = 0; j < nf; ++j)
            R.loadings[static_cast<std::size_t>(i * nf + j)] = vecs[static_cast<std::size_t>(i * p + j)];

    R.psi.assign(static_cast<std::size_t>(p), 0.0);
    for (int i = 0; i < p; ++i) {
        double comm = 0;
        for (int j = 0; j < nf; ++j) {
            const double l = vecs[static_cast<std::size_t>(i * p + j)];
            comm += ev[static_cast<std::size_t>(j)] * l * l;  // the lambda factor is load-bearing
        }
        // ABSOLUTE 1e-6 chop, unlike the trace's relative one. This is why an
        // f = p run prints exact zeros instead of ~1e-16 residue.
        R.psi[static_cast<std::size_t>(i)] = chop(R.c[static_cast<std::size_t>(i * p + i)] - comm, 1e-6);
    }

    // ---- e(components): p x 4, for ALL p rows ------------------------------
    R.components_tbl.assign(static_cast<std::size_t>(p) * 4, 0.0);
    double cum = 0;
    for (int j = 0; j < p; ++j) {
        const double lam = ev[static_cast<std::size_t>(j)];
        const double diff = (j + 1 < p) ? lam - ev[static_cast<std::size_t>(j + 1)]
                                        : std::numeric_limits<double>::quiet_NaN();
        const double prop = (R.trace != 0) ? lam / R.trace : 0.0;
        cum += prop;
        R.components_tbl[static_cast<std::size_t>(j * 4 + 0)] = lam;
        R.components_tbl[static_cast<std::size_t>(j * 4 + 1)] = diff;  // last row is missing
        R.components_tbl[static_cast<std::size_t>(j * 4 + 2)] = prop;
        R.components_tbl[static_cast<std::size_t>(j * 4 + 3)] = cum;
    }
    return R;
}

}  // namespace cecon
