// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/logit.h"
#include "estimators/zicount.h"
#include "vcov/vcov.h"  // cluster_meat — fracreg's cluster sandwich reuses the shared helper

// P7 wave — zip, zinb, fracreg.
//
//   zip     zero-inflated Poisson           (Stata `zip y x…, inflate(_cons)`)
//   zinb    zero-inflated negative binomial (Stata `zinb y x…, inflate(_cons)`)
//   fracreg fractional-response quasi-MLE   (Stata `fracreg logit y x…`)
//
// The mixture math lives in src/estimators/zicount.h/.cpp; `fracreg` reuses
// logit.cpp's IRLS unchanged (the ONLY difference from `logit` is that the
// outcome is validated as 0 ≤ y ≤ 1 rather than y ∈ {0,1}, and that there is no
// iid VCE — Stata's fracreg is a quasi-MLE and always reports a sandwich).

namespace cecon {
namespace cli {

namespace {

// Two-sided normal p-value for a z statistic (the asymptotic inference Stata
// prints for all three of these commands).
double z_pvalue(double z) {
    return z_twosided_pvalue(z);
}

// Slope-block Wald χ² → the three header keys. `ks` slopes (the design contract
// puts the intercept LAST, so they are the leading ks parameters). When the block
// is numerically degenerate — zi::wald_chi2 says so — the statistic is reported
// MISSING (chi2/p null, df 0) instead of being read off a singular inverse; that
// is exactly what Stata prints for this wave's `fracreg logit` (`Wald chi2(0) = .`,
// `Prob > chi2 = .`), whose outcome is the model's own inverse-logit and whose
// robust meat is therefore at machine noise.
void add_wald(json& out, const std::vector<double>& V, int np,
              const std::vector<double>& beta, int ks) {
    double chi2 = 0;
    if (ks > 0 && zi::wald_chi2(V.data(), np, beta.data(), ks, chi2)) {
        out["chi2"] = chi2;
        out["df_chi2"] = ks;
        out["p_chi2"] = chi2_pvalue(chi2, static_cast<double>(ks));
    } else {
        out["chi2"] = nullptr;
        out["df_chi2"] = 0;
        out["p_chi2"] = nullptr;
    }
}

// The zero-inflated log likelihood of Stata's zip_llf / zinb_llf (zip_plf /
// zinb_plf with `probit`): with count index eta = x'b + offset, mu = exp(eta),
// inflation index zeta = z'g and pi = F(zeta) (logistic, or the normal):
//   y = 0:  ln D, D = pi + (1-pi) f0(mu)
//   y > 0:  ln(1-pi) + ln f(y|mu)
// f Poisson, or NB2 with alpha = exp(lnalpha) (r = 1/alpha, p = 1/(1+alpha mu),
// f0 = p^r). Parameters [b (kx), g (kz) (, lnalpha)]. The gradient and the
// Hessian are analytic (ml's e2, as the ado evaluators compute them): every
// second derivative is the observation's derivative in (eta, zeta, lnalpha)
// times the design's outer products.
inline double zi_trigamma(double x) {
    double s = 0.0;
    while (x < 6.0) {
        s += 1.0 / (x * x);
        x += 1.0;
    }
    const double i = 1.0 / x, i2 = i * i;
    return s + i + 0.5 * i2 + i * i2 * (1.0 / 6.0 - i2 * (1.0 / 30.0 - i2 * (1.0 / 42.0 - i2 / 30.0)));
}

class ZiMlObjective final : public MlObjective {
public:
    ZiMlObjective(const double* X, int kx, const double* Z, int kz, int64_t n, const double* y,
                  const double* off, const double* w, bool nb, bool probit)
        : X_(X), Z_(Z), kx_(kx), kz_(kz), n_(n), y_(y), off_(off), w_(w), nb_(nb), probit_(probit) {}
    int np() const { return kx_ + kz_ + (nb_ ? 1 : 0); }
    double loglik(const std::vector<double>& t) override { return eval(t, nullptr, nullptr, nullptr); }
    double evaluate(const std::vector<double>& t, std::vector<double>& g, std::vector<double>& A) override {
        const int p = np();
        g.assign(static_cast<size_t>(p), 0.0);
        A.assign(static_cast<size_t>(p) * static_cast<size_t>(p), 0.0);
        return eval(t, &g, &A, nullptr);
    }
    // per-observation scores, n x np column-major (the sandwich's meat)
    void scores(const std::vector<double>& t, std::vector<double>& S) {
        S.assign(static_cast<size_t>(n_) * static_cast<size_t>(np()), 0.0);
        eval(t, nullptr, nullptr, &S);
    }

private:
    double eval(const std::vector<double>& t, std::vector<double>* g, std::vector<double>* A,
                std::vector<double>* S) const {
        const int P = np();
        const size_t N = static_cast<size_t>(n_);
        const int ka = kx_ + kz_;  // lnalpha's index
        double lna = nb_ ? t[static_cast<size_t>(ka)] : 0.0;
        const double a = nb_ ? std::exp(lna) : 0.0;
        const double r = nb_ ? 1.0 / a : 0.0;
        const double lgr = nb_ ? std::lgamma(r) : 0.0;
        const double dgr = nb_ ? zi::digamma(r) : 0.0;
        const double tgr = nb_ ? zi_trigamma(r) : 0.0;
        double LL = 0.0;
        std::vector<double> xi(static_cast<size_t>(kx_)), zi_(static_cast<size_t>(kz_));
        for (int64_t i = 0; i < n_; ++i) {
            const size_t ii = static_cast<size_t>(i);
            double eta = off_ != nullptr ? off_[i] : 0.0;
            for (int c = 0; c < kx_; ++c) {
                xi[static_cast<size_t>(c)] = X_[static_cast<size_t>(c) * N + ii];
                eta += xi[static_cast<size_t>(c)] * t[static_cast<size_t>(c)];
            }
            double zeta = 0.0;
            for (int c = 0; c < kz_; ++c) {
                zi_[static_cast<size_t>(c)] = Z_[static_cast<size_t>(c) * N + ii];
                zeta += zi_[static_cast<size_t>(c)] * t[static_cast<size_t>(kx_ + c)];
            }
            if (eta > 700.0) eta = 700.0;
            const double mu = std::exp(eta);
            double pi, l1mpi, F1, F2;  // F(zeta), ln(1-F), F', F''
            if (probit_) {
                pi = 0.5 * std::erfc(-zeta / std::sqrt(2.0));
                l1mpi = std::log(0.5 * std::erfc(zeta / std::sqrt(2.0)));
                F1 = std::exp(-0.5 * zeta * zeta) / std::sqrt(2.0 * 3.14159265358979323846);
                F2 = -zeta * F1;
            } else {
                if (zeta >= 0) {
                    const double e = std::exp(-zeta);
                    pi = 1.0 / (1.0 + e);
                    l1mpi = -zeta - std::log1p(e);
                } else {
                    const double e = std::exp(zeta);
                    pi = e / (1.0 + e);
                    l1mpi = -std::log1p(e);
                }
                F1 = pi * (1.0 - pi);
                F2 = F1 * (1.0 - 2.0 * pi);
            }
            const double omp = 1.0 - pi;
            const double yi = y_[i];
            const double wi = w_ != nullptr ? w_[i] : 1.0;
            // first (Le, Lz, La) and second (Lee, Lez, Lzz, Lea, Lza, Laa) derivatives
            double li, Le, Lz, La = 0.0, Lee, Lez, Lzz, Lea = 0.0, Lza = 0.0, Laa = 0.0;
            const double pp = nb_ ? 1.0 / (1.0 + a * mu) : 0.0;
            if (yi == 0.0) {
                double f0, f0e, f0ee, f0a = 0.0, f0aa = 0.0, f0ea = 0.0;
                if (nb_) {
                    const double lp = std::log(pp);
                    f0 = std::exp(r * lp);
                    const double E = -mu * pp;                       // (ln f0)_eta
                    const double Ee = -mu * pp + a * mu * mu * pp * pp;
                    const double G = -r * lp - mu * pp;              // (ln f0)_lnalpha
                    const double Ga = r * lp + mu * pp + a * mu * mu * pp * pp;
                    const double Ea = a * mu * mu * pp * pp;
                    f0e = f0 * E;
                    f0ee = f0 * (E * E + Ee);
                    f0a = f0 * G;
                    f0aa = f0 * (G * G + Ga);
                    f0ea = f0 * (E * G + Ea);
                } else {
                    f0 = std::exp(-mu);
                    f0e = -mu * f0;
                    f0ee = mu * f0 * (mu - 1.0);
                }
                const double D = pi + omp * f0;
                li = std::log(D);
                const double De = omp * f0e, Dz = F1 * (1.0 - f0), Da = omp * f0a;
                const double Dee = omp * f0ee, Dez = -F1 * f0e, Dzz = F2 * (1.0 - f0);
                const double Dea = omp * f0ea, Dza = -F1 * f0a, Daa = omp * f0aa;
                Le = De / D;
                Lz = Dz / D;
                La = Da / D;
                Lee = Dee / D - Le * Le;
                Lez = Dez / D - Le * Lz;
                Lzz = Dzz / D - Lz * Lz;
                Lea = Dea / D - Le * La;
                Lza = Dza / D - Lz * La;
                Laa = Daa / D - La * La;
            } else {
                const double q = F1 / omp;  // -(d/dzeta) ln(1-F)
                Lz = -q;
                Lzz = -F2 / omp - q * q;
                Lez = 0.0;
                if (nb_) {
                    const double lp = std::log(pp);
                    li = l1mpi + std::lgamma(yi + r) - lgr - std::lgamma(yi + 1.0) + r * lp +
                         yi * std::log1p(-pp);
                    Le = pp * (yi - mu);
                    Lee = -a * mu * pp * pp * (yi - mu) - pp * mu;
                    const double Ad = zi::digamma(yi + r) - dgr;
                    La = -r * (Ad + lp) - (mu - yi) * pp;
                    Laa = r * (Ad + lp) + r * r * (zi_trigamma(yi + r) - tgr) + mu * pp + (mu - yi) * a * mu * pp * pp;
                    Lea = -(yi - mu) * a * mu * pp * pp;
                } else {
                    li = l1mpi - mu + yi * eta - std::lgamma(yi + 1.0);
                    Le = yi - mu;
                    Lee = -mu;
                }
            }
            LL += wi * li;
            if (g) {
                for (int c = 0; c < kx_; ++c) (*g)[static_cast<size_t>(c)] += wi * Le * xi[static_cast<size_t>(c)];
                for (int c = 0; c < kz_; ++c) (*g)[static_cast<size_t>(kx_ + c)] += wi * Lz * zi_[static_cast<size_t>(c)];
                if (nb_) (*g)[static_cast<size_t>(ka)] += wi * La;
            }
            if (A) {
                const auto add = [&](int rr, int cc, double v) {
                    (*A)[static_cast<size_t>(rr) * static_cast<size_t>(P) + static_cast<size_t>(cc)] -= wi * v;
                };
                for (int r1 = 0; r1 < kx_; ++r1) {
                    const double x1 = xi[static_cast<size_t>(r1)];
                    for (int c1 = 0; c1 <= r1; ++c1) add(r1, c1, Lee * x1 * xi[static_cast<size_t>(c1)]);
                }
                for (int r1 = 0; r1 < kz_; ++r1) {
                    const double z1 = zi_[static_cast<size_t>(r1)];
                    for (int c1 = 0; c1 < kx_; ++c1) add(kx_ + r1, c1, Lez * z1 * xi[static_cast<size_t>(c1)]);
                    for (int c1 = 0; c1 <= r1; ++c1) add(kx_ + r1, kx_ + c1, Lzz * z1 * zi_[static_cast<size_t>(c1)]);
                }
                if (nb_) {
                    for (int c1 = 0; c1 < kx_; ++c1) add(ka, c1, Lea * xi[static_cast<size_t>(c1)]);
                    for (int c1 = 0; c1 < kz_; ++c1) add(ka, kx_ + c1, Lza * zi_[static_cast<size_t>(c1)]);
                    add(ka, ka, Laa);
                }
            }
            if (S) {
                for (int c = 0; c < kx_; ++c) (*S)[static_cast<size_t>(c) * N + ii] = Le * xi[static_cast<size_t>(c)];
                for (int c = 0; c < kz_; ++c) (*S)[static_cast<size_t>(kx_ + c) * N + ii] = Lz * zi_[static_cast<size_t>(c)];
                if (nb_) (*S)[static_cast<size_t>(ka) * N + ii] = La;
            }
        }
        if (A)
            for (int r1 = 0; r1 < P; ++r1)
                for (int c1 = r1 + 1; c1 < P; ++c1)
                    (*A)[static_cast<size_t>(r1) * static_cast<size_t>(P) + static_cast<size_t>(c1)] =
                        (*A)[static_cast<size_t>(c1) * static_cast<size_t>(P) + static_cast<size_t>(r1)];
        return LL;
    }
    const double *X_, *Z_;
    int kx_, kz_;
    int64_t n_;
    const double *y_, *off_, *w_;
    bool nb_, probit_;
};

// `zip`/`zinb depvar indepvars [if] [weight], inflate(varlist | _cons) [probit
// offset() exposure() vce() iterate()]`, fitted the way zip.ado / zinb.ado fit
// it: the constant-only model (the count equation's constant beside the whole
// inflation equation; zip starts its constant at ln(sum w y / sum w exp(offset)),
// zinb at ln(mean y) - mean(offset) with lnalpha 0, the inflation coefficients
// at 0), then the full model from it with the count slopes at 0, each by ml's
// Newton-Raphson (ml_newton_path). e(ll_0) is the constant-only fit's; the
// default VCE's model test is the LR test of the count slopes against it.
int run_zi_ml(const std::vector<std::string>& a, bool nb) {
    const char* cmd = nb ? "zinb" : "zip";
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    const std::string inflate = opt(a, "--inflate", "_cons");
    const std::string vce = opt(a, "--vce", "iid");
    const std::string cluster_var = opt(a, "--cluster");
    // --cluster-markout: the cluster came from the cluster(varname) option, which
    // zip.ado / zinb.ado mark out of the whole sample before anything is fitted
    // (the zero count and the constant-only fit exclude a missing cluster too);
    // without it (vce(cluster c)) only ml's full fit marks it out.
    const bool cluster_markout = has(a, "--cluster-markout");
    const bool probit = has(a, "--probit");
    const std::string offset_var = opt(a, "--offset"), exposure_var = opt(a, "--exposure");
    const std::string wvar = opt(a, "--weight"), wtype = opt(a, "--wtype", "fweight");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra %s --data f.hbin --y Y --x X1,X2 --inflate Z1,Z2|_cons [--probit] "
                     "[--offset V|--exposure V] [--weight W --wtype fweight|pweight] [--vce iid|robust|cluster] "
                     "[--cluster C] [--iterate N] [--level P] [--keepif EXPR] [--out f.json]\n",
                     cmd);
        return 2;
    }
    if (vce != "iid" && vce != "robust" && vce != "cluster")
        throw EngineError("bad_input", std::string(cmd) + ": --vce must be one of iid|robust|cluster");
    if (vce == "cluster" && cluster_var.empty())
        throw EngineError("bad_input", std::string(cmd) + ": --vce cluster requires --cluster C");
    const bool level_set = has(a, "--level");
    const double level = std::stod(opt(a, "--level", "95"));
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
    const EsampleRequest esreq = esample_request(a, cmd);
    const std::string es_data = data, es_keepif = opt(a, "--keepif");
    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    const std::string offv = !exposure_var.empty() ? exposure_var : offset_var;
    const cecon::WeightKind wk = wvar.empty() ? cecon::WeightKind::None
                                 : (wtype == "pweight" || wtype == "pw") ? cecon::WeightKind::Probability
                                                      : cecon::WeightKind::Frequency;
    GlmInputs g = load_glm(ds, cmd, y, split_csv(xcsv), {}, wvar, wk, offv);
    std::vector<std::string> zvars;
    if (inflate != "_cons") zvars = split_csv(inflate);
    GlmInputs gz = load_glm(ds, cmd, y, zvars.empty() ? std::vector<std::string>{} : zvars);
    // vce(cluster c): ml marks out a missing cluster for the full model only; the
    // ado's own sample (its zero count, the constant-only fit) keeps those rows
    // (bank 178b2877 `zip ..., vce(cluster industry)`: e(N) 2215, e(N_zero) 339 and
    // e(ll_0) as without the cluster).
    GlmInputs gc;
    if (!cluster_names.empty()) gc = load_glm(ds, cmd, y, {}, cluster_names);
    // the joint sample: rows both designs kept (listwise over every variable)
    std::vector<int64_t> keep;
    std::vector<size_t> ix, iz;
    {
        size_t a1 = 0, b1 = 0;
        while (a1 < g.keep.size() && b1 < gz.keep.size()) {
            if (g.keep[a1] == gz.keep[b1]) {
                keep.push_back(g.keep[a1]);
                ix.push_back(a1);
                iz.push_back(b1);
                ++a1;
                ++b1;
            } else if (g.keep[a1] < gz.keep[b1]) {
                ++a1;
            } else {
                ++b1;
            }
        }
    }
    // exposure(v) enters as ln(v): a nonpositive exposure has a missing log, which
    // the ados mark out of the sample with the other missing values
    if (!exposure_var.empty()) {
        std::vector<int64_t> k2;
        std::vector<size_t> ix2, iz2;
        for (size_t j = 0; j < keep.size(); ++j) {
            if (!(g.offset[ix[j]] > 0.0)) continue;
            k2.push_back(keep[j]);
            ix2.push_back(ix[j]);
            iz2.push_back(iz[j]);
        }
        keep.swap(k2);
        ix.swap(ix2);
        iz.swap(iz2);
    }
    // the full model's rows (a position into keep) and each one's cluster code
    std::vector<size_t> fullRows;
    std::vector<int64_t> fullCodes;
    {
        size_t c1 = 0;
        for (size_t j = 0; j < keep.size(); ++j) {
            if (cluster_names.empty()) {
                fullRows.push_back(j);
                continue;
            }
            while (c1 < gc.keep.size() && gc.keep[c1] < keep[j]) ++c1;
            if (c1 < gc.keep.size() && gc.keep[c1] == keep[j]) {
                fullRows.push_back(j);
                fullCodes.push_back(gc.cluster[0].codes[c1]);
            }
        }
        if (cluster_markout && fullRows.size() != keep.size()) {
            std::vector<int64_t> k2;
            std::vector<size_t> ix2, iz2;
            for (size_t j : fullRows) {
                k2.push_back(keep[j]);
                ix2.push_back(ix[j]);
                iz2.push_back(iz[j]);
            }
            keep.swap(k2);
            ix.swap(ix2);
            iz.swap(iz2);
            for (size_t j = 0; j < fullRows.size(); ++j) fullRows[j] = j;
        }
    }
    int64_t n = static_cast<int64_t>(keep.size());
    require_sample(n);
    const int kx = g.k, kz = gz.k;
    std::vector<double> X(static_cast<size_t>(n) * static_cast<size_t>(kx)),
        Z(static_cast<size_t>(n) * static_cast<size_t>(kz)), yv(static_cast<size_t>(n)), off, w;
    for (int64_t i = 0; i < n; ++i) {
        const size_t ii = static_cast<size_t>(i);
        yv[ii] = g.y[ix[ii]];
        for (int c = 0; c < kx; ++c)
            X[static_cast<size_t>(c) * static_cast<size_t>(n) + ii] = g.X[static_cast<size_t>(c) * static_cast<size_t>(g.n) + ix[ii]];
        for (int c = 0; c < kz; ++c)
            Z[static_cast<size_t>(c) * static_cast<size_t>(n) + ii] = gz.X[static_cast<size_t>(c) * static_cast<size_t>(gz.n) + iz[ii]];
    }
    if (!offv.empty()) {
        off.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            const double o = g.offset[ix[static_cast<size_t>(i)]];
            off[static_cast<size_t>(i)] = exposure_var.empty() ? o : std::log(o);
        }
    }
    double sumw = static_cast<double>(n);
    if (!wvar.empty()) {
        w.resize(static_cast<size_t>(n));
        sumw = 0.0;
        for (int64_t i = 0; i < n; ++i) sumw += (w[static_cast<size_t>(i)] = g.weights[ix[static_cast<size_t>(i)]]);
    }
    int64_t n_zero = 0;
    double wzero = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        const double v = yv[static_cast<size_t>(i)];
        if (v < 0.0)
            throw EngineError("bad_input", y + "<0 in some obs");
        if (v == 0.0) {
            ++n_zero;
            wzero += w.empty() ? 1.0 : w[static_cast<size_t>(i)];
        }
    }
    if (n_zero == n) throw EngineError("bad_input", y + " never varies; always equal to zero");
    const double* offp = off.empty() ? nullptr : off.data();
    const double* wp = w.empty() ? nullptr : w.data();
    double sumw0 = sumw;

    MlMaxOptions mo;
    if (has(a, "--iterate")) mo.maxiter = std::stoi(opt(a, "--iterate"));
    // A mixture likelihood is not globally concave: where -H is not positive
    // definite, ml reports "(not concave)" and takes the Marquardt step instead of
    // ending the fit; a positive definite -H is the plain Newton step as before.
    mo.spd_concavity = true;
    // ---- constant-only model: the count constant beside the full inflation equation
    const bool hasCons = !g.names.empty() && g.names.back() == "(intercept)";
    std::vector<double> ones(static_cast<size_t>(n), 1.0);
    ZiMlObjective obj0(ones.data(), 1, Z.data(), kz, n, yv.data(), offp, wp, nb, probit);
    double c0;
    if (!nb) {
        double num = 0.0, den = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            const double wi = wp ? wp[i] : 1.0;
            num += yv[static_cast<size_t>(i)] * wi;
            den += wi * (offp ? std::exp(offp[i]) : 1.0);
        }
        c0 = std::log(num / den);
    } else {
        // summ depvar [aweight] (unweighted without a weight); minus mean(offset)
        double sy = 0.0, sw = 0.0, so = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            const double wi = wp ? wp[i] : 1.0;
            sy += wi * yv[static_cast<size_t>(i)];
            sw += wi;
            if (offp) so += offp[i];
        }
        c0 = std::log(sy / sw) - (offp ? so / static_cast<double>(n) : 0.0);
    }
    std::vector<double> start0(static_cast<size_t>(1 + kz + (nb ? 1 : 0)), 0.0);
    start0[0] = c0;
    MlMaxOptions mo0 = mo;  // the ado passes its maximize options to the prefit too
    double ll0 = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> from0 = start0;
    if (hasCons) {
        const MlPath p0 = ml_newton_path(obj0, start0, mo0);
        if (!p0.failed && std::isfinite(p0.ll)) {
            ll0 = p0.ll;
            from0 = p0.b;
        }
    }
    // ---- full model: the count slopes at 0, the rest from the constant-only fit
    const int64_t n0 = n;
    if (fullRows.size() != static_cast<size_t>(n)) {
        const int64_t nf = static_cast<int64_t>(fullRows.size());
        require_sample(nf);
        std::vector<double> X2(static_cast<size_t>(nf) * static_cast<size_t>(kx)), Z2(static_cast<size_t>(nf) * static_cast<size_t>(kz)),
            y2(static_cast<size_t>(nf)), o2, w2;
        for (int64_t i = 0; i < nf; ++i) {
            const size_t src = fullRows[static_cast<size_t>(i)];
            y2[static_cast<size_t>(i)] = yv[src];
            for (int c = 0; c < kx; ++c)
                X2[static_cast<size_t>(c) * static_cast<size_t>(nf) + static_cast<size_t>(i)] = X[static_cast<size_t>(c) * static_cast<size_t>(n0) + src];
            for (int c = 0; c < kz; ++c)
                Z2[static_cast<size_t>(c) * static_cast<size_t>(nf) + static_cast<size_t>(i)] = Z[static_cast<size_t>(c) * static_cast<size_t>(n0) + src];
            if (!off.empty()) o2.push_back(off[src]);
            if (!w.empty()) w2.push_back(w[src]);
        }
        std::vector<int64_t> k2;
        for (size_t j : fullRows) k2.push_back(keep[j]);
        X.swap(X2);
        Z.swap(Z2);
        yv.swap(y2);
        off.swap(o2);
        w.swap(w2);
        keep.swap(k2);
        n = nf;
        offp = off.empty() ? nullptr : off.data();
        wp = w.empty() ? nullptr : w.data();
    }
    ZiMlObjective obj(X.data(), kx, Z.data(), kz, n, yv.data(), offp, wp, nb, probit);
    const int np = obj.np();
    std::vector<double> start(static_cast<size_t>(np), 0.0);
    if (hasCons) start[static_cast<size_t>(kx - 1)] = from0[0];
    for (int c = 0; c < kz + (nb ? 1 : 0); ++c) start[static_cast<size_t>(kx + c)] = from0[static_cast<size_t>(1 + c)];
    if (nb) {
        // The negative-binomial mixture can be weakly identified. Require a
        // small Newton decrement rather than stopping on a small change in ll.
        mo.tol = 1e-10;
        mo.ltol = 0.0;
        mo.nrtol = 1e-12;
    }
    const MlPath path = ml_newton_path(obj, start, mo);
    bool finite = !path.failed && std::isfinite(path.ll) &&
                  path.inv_curvature.size() == static_cast<size_t>(np) * static_cast<size_t>(np);
    for (double v : path.b) finite = finite && std::isfinite(v);
    if (!finite) { std::fprintf(stderr, "%s: MLE failed.\n", cmd); return 1; }
    std::vector<double> V = path.inv_curvature;
    int n_clusters = 0;
    const bool sandwich = vce != "iid" || wk == cecon::WeightKind::Probability;
    if (sandwich) {
        std::vector<double> S;
        obj.scores(path.b, S);
        std::vector<double> meat(static_cast<size_t>(np) * static_cast<size_t>(np), 0.0);
        double q;
        if (vce == "cluster") {
            std::map<int64_t, int> idOf;
            std::vector<int> codes(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) {
                const int64_t code = fullCodes[static_cast<size_t>(i)];
                auto it = idOf.find(code);
                if (it == idOf.end()) it = idOf.emplace(code, static_cast<int>(idOf.size())).first;
                codes[static_cast<size_t>(i)] = it->second;
            }
            n_clusters = static_cast<int>(idOf.size());
            if (n_clusters < 2)
                throw EngineError("bad_input", std::string(cmd) + ": vce(cluster) requires at least 2 clusters");
            std::vector<double> T(static_cast<size_t>(n_clusters) * static_cast<size_t>(np), 0.0);
            for (int64_t i = 0; i < n; ++i)
                for (int c = 0; c < np; ++c)
                    T[static_cast<size_t>(codes[static_cast<size_t>(i)]) * static_cast<size_t>(np) + static_cast<size_t>(c)] +=
                        (wp ? wp[i] : 1.0) * S[static_cast<size_t>(c) * static_cast<size_t>(n) + static_cast<size_t>(i)];
            for (int gq = 0; gq < n_clusters; ++gq)
                for (int r = 0; r < np; ++r)
                    for (int c = 0; c < np; ++c)
                        meat[static_cast<size_t>(r) * static_cast<size_t>(np) + static_cast<size_t>(c)] +=
                            T[static_cast<size_t>(gq) * static_cast<size_t>(np) + static_cast<size_t>(r)] *
                            T[static_cast<size_t>(gq) * static_cast<size_t>(np) + static_cast<size_t>(c)];
            q = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
        } else {
            // A frequency weight is w_i replicate observations: each row's outer
            // product counts w_i times and N is the sum of the weights (ml's robust
            // variance with fweights = the expanded data's). A probability weight
            // scales the score: meat (w_i s_i)(w_i s_i)', N the rows.
            const bool freq = wk == cecon::WeightKind::Frequency && wp != nullptr;
            double nq = 0.0;
            for (int64_t i = 0; i < n; ++i) {
                const double wi = wp ? wp[i] : 1.0;
                const double f = freq ? wi : wi * wi;
                nq += freq ? wi : 1.0;
                for (int r = 0; r < np; ++r)
                    for (int c = 0; c < np; ++c)
                        meat[static_cast<size_t>(r) * static_cast<size_t>(np) + static_cast<size_t>(c)] +=
                            f * S[static_cast<size_t>(r) * static_cast<size_t>(n) + static_cast<size_t>(i)] *
                            S[static_cast<size_t>(c) * static_cast<size_t>(n) + static_cast<size_t>(i)];
            }
            q = nq / (nq - 1.0);
        }
        std::vector<double> tmp(static_cast<size_t>(np) * static_cast<size_t>(np), 0.0), Vs(tmp.size(), 0.0);
        const auto at = [np](std::vector<double>& m, int r, int c) -> double& {
            return m[static_cast<size_t>(r) * static_cast<size_t>(np) + static_cast<size_t>(c)];
        };
        for (int r = 0; r < np; ++r)
            for (int c = 0; c < np; ++c) {
                double s1 = 0.0;
                for (int j = 0; j < np; ++j) s1 += at(V, r, j) * at(meat, j, c);
                at(tmp, r, c) = s1;
            }
        for (int r = 0; r < np; ++r)
            for (int c = 0; c < np; ++c) {
                double s1 = 0.0;
                for (int j = 0; j < np; ++j) s1 += at(tmp, r, j) * at(V, j, c);
                at(Vs, r, c) = s1 * q;
            }
        for (int r = 0; r < np; ++r)
            for (int c = 0; c < r; ++c) {
                const double m = 0.5 * (at(Vs, r, c) + at(Vs, c, r));
                at(Vs, r, c) = m;
                at(Vs, c, r) = m;
            }
        V = Vs;
    }
    auto se_at = [&](int j) { return std::sqrt(std::max(0.0, V[static_cast<size_t>(j) * static_cast<size_t>(np) + static_cast<size_t>(j)])); };
    auto row = [&](const std::string& nm, int j) {
        const double b = path.b[static_cast<size_t>(j)], se = se_at(j), z = tstat(b, se);
        return json{{"name", nm}, {"b", b}, {"se", se}, {"z", z}, {"p", (se > 0) ? z_pvalue(z) : 1.0},
                    {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}};
    };
    json coefs = json::array(), infl = json::array();
    std::vector<std::string> vnames;
    for (int c = 0; c < kx; ++c) {
        coefs.push_back(row(g.names[static_cast<size_t>(c)], c));
        vnames.push_back("count:" + g.names[static_cast<size_t>(c)]);
    }
    for (int c = 0; c < kz; ++c) {
        infl.push_back(row(gz.names[static_cast<size_t>(c)], kx + c));
        vnames.push_back("inflate:" + gz.names[static_cast<size_t>(c)]);
    }
    if (nb) vnames.push_back("lnalpha");
    double sumwFull = static_cast<double>(n);
    if (wp != nullptr) {
        sumwFull = 0.0;
        for (int64_t i = 0; i < n; ++i) sumwFull += wp[i];
    }
    (void)sumw0;
    const double nObs = wk == cecon::WeightKind::Frequency ? sumwFull : static_cast<double>(n);
    json out = {{"estimator", cmd}, {"inflate_spec", inflate}, {"vce", vce},
                {"n_obs", nObs}, {"n_zero", wk == cecon::WeightKind::Frequency ? wzero : static_cast<double>(n_zero)},
                {"n_nonzero", nObs - (wk == cecon::WeightKind::Frequency ? wzero : static_cast<double>(n_zero))},
                {"converged", path.converged}, {"iters", path.ic}, {"ic", path.ic},
                {"coefs", coefs}, {"inflate", infl}, {"ll", path.ll}, {"ilog", path.ilog}};
    out["inflate_link"] = probit ? "probit" : "logit";
    out["n_rows"] = n;  // the rows behind a frequency-weighted N (the e(sample) handshake)
    if (std::isfinite(ll0)) out["ll_0"] = ll0;
    if (nb) {
        const int j = kx + kz;
        const double lna = path.b[static_cast<size_t>(j)], se = se_at(j);
        out["lnalpha"] = {{"b", lna}, {"se", se}, {"z", tstat(lna, se)}, {"p", se > 0 ? z_pvalue(tstat(lna, se)) : 1.0},
                          {"ci_lo", lna - zc * se}, {"ci_hi", lna + zc * se}};
        out["alpha"] = {{"b", std::exp(lna)}, {"se", std::exp(lna) * se},
                        {"ci_lo", std::exp(lna - zc * se)}, {"ci_hi", std::exp(lna + zc * se)}};
    }
    {
        json gr = json::array();
        for (double v : path.gradient) gr.push_back(v);
        out["gradient"] = gr;
    }
    // model test: the count slopes (every count column but the constant)
    const int ks = hasCons ? kx - 1 : kx;
    if (ks > 0) {
        if (!sandwich && std::isfinite(ll0)) {
            const double lr = 2.0 * (path.ll - ll0);
            out["chi2"] = lr;
            out["chi2type"] = "LR";
            out["p_chi2"] = chi2_pvalue(lr, static_cast<double>(ks));
        } else {
            double chi2 = 0.0;
            if (zi::wald_chi2(V.data(), np, path.b.data(), ks, chi2)) {
                out["chi2"] = chi2;
                out["p_chi2"] = chi2_pvalue(chi2, static_cast<double>(ks));
            } else {
                out["chi2"] = nullptr;
                out["p_chi2"] = nullptr;
            }
            out["chi2type"] = "Wald";
        }
        out["df_chi2"] = ks;
    } else {
        out["df_chi2"] = 0;
    }
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    add_e_v(out, vnames, V);
    if (sandwich) {
        // e(V_modelbased): the inverse of -H the sandwich was built on
        json M = json::array();
        for (int r1 = 0; r1 < np; ++r1) {
            json rowj = json::array();
            for (int c1 = 0; c1 < np; ++c1)
                rowj.push_back(path.inv_curvature[static_cast<size_t>(r1) * static_cast<size_t>(np) + static_cast<size_t>(c1)]);
            M.push_back(rowj);
        }
        out["e_Vmb"] = {{"names", vnames}, {"matrix", M}};
    }
    {
        EsampleRows esrows;
        for (int64_t i = 0; i < n; ++i) esrows.keep.push_back(keep[static_cast<size_t>(i)]);
        esrows.loaded = &ds;
        emit_esample(esreq, cmd, es_data, es_keepif, false, esrows, out);
    }
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace

int cmd_zip(const std::vector<std::string>& a) { return run_zi_ml(a, false); }

int cmd_zinb(const std::vector<std::string>& a) { return run_zi_ml(a, true); }

// The Bernoulli quasi-log-likelihood of `fracreg logit` for y in [0,1]:
// sum_i y_i eta_i - ln(1 + exp(eta_i)), score y_i - p_i, curvature p_i (1 - p_i).
class FracLogitObjective final : public MlObjective {
public:
    FracLogitObjective(const double* X, int64_t n, int k, const double* y)
        : X_(X), n_(n), k_(k), y_(y), eta_(static_cast<size_t>(n)), s_(static_cast<size_t>(n)),
          scaled_(static_cast<size_t>(n) * static_cast<size_t>(k)) {}
    double loglik(const std::vector<double>& b) override {
        index(b);
        double ll = 0.0;
        for (int64_t i = 0; i < n_; ++i) ll += term(eta_[static_cast<size_t>(i)], y_[i]);
        return ll;
    }
    double evaluate(const std::vector<double>& b, std::vector<double>& g, std::vector<double>& A) override {
        index(b);
        double ll = 0.0;
        for (int64_t i = 0; i < n_; ++i) {
            const size_t ii = static_cast<size_t>(i);
            const double e = eta_[ii];
            ll += term(e, y_[i]);
            const double p = 1.0 / (1.0 + std::exp(-e));
            s_[ii] = y_[i] - p;
            const double root = std::sqrt(std::max(0.0, p * (1.0 - p)));
            for (int c = 0; c < k_; ++c)
                scaled_[static_cast<size_t>(c) * static_cast<size_t>(n_) + ii] =
                    X_[static_cast<size_t>(c) * static_cast<size_t>(n_) + ii] * root;
        }
        g.assign(static_cast<size_t>(k_), 0.0);
        A.assign(static_cast<size_t>(k_) * static_cast<size_t>(k_), 0.0);
        la::xtv(X_, n_, k_, s_.data(), g.data());
        la::xtx(scaled_.data(), n_, k_, A.data());
        return ll;
    }

private:
    void index(const std::vector<double>& b) { la::mat_vec(X_, n_, k_, b.data(), eta_.data()); }
    // y eta - ln(1 + e^eta), without overflow for a large |eta|
    static double term(double e, double y) {
        const double soft = e > 0 ? e + std::log1p(std::exp(-e)) : std::log1p(std::exp(e));
        return y * e - soft;
    }
    const double* X_;
    int64_t n_;
    int k_;
    const double* y_;
    std::vector<double> eta_, s_, scaled_;
};

// Stata's starting point for fracreg: the linear regression of y on the design.
std::vector<double> frac_ols_start(const double* X, int64_t n, int k, const double* y) {
    std::vector<double> A(static_cast<size_t>(k) * static_cast<size_t>(k), 0.0), b(static_cast<size_t>(k), 0.0),
        xy(static_cast<size_t>(k), 0.0);
    la::xtx(X, n, k, A.data());
    la::xtv(X, n, k, y, xy.data());
    if (la::spd_inverse(A.data(), k) != 0) return {};
    for (int r = 0; r < k; ++r)
        for (int c = 0; c < k; ++c)
            b[static_cast<size_t>(r)] += A[static_cast<size_t>(r) * static_cast<size_t>(k) + static_cast<size_t>(c)] *
                                         xy[static_cast<size_t>(c)];
    return b;
}

// fracreg — fractional response quasi-MLE (Stata `fracreg logit y x…`): ml's
// modified Newton-Raphson on the Bernoulli quasi-likelihood from the linear
// regression's coefficients, stopping where ml's documented rule stops (StataBE
// 19.5 bank 2bef1615 `fracreg logit fr weight length`: e(ic) 3, e(ll)
// -44.450904071827146; e(ll_0) is the constant-only fit's, started at mean(y) the
// same way). The bread of the sandwich is the inverse of -H at the final iterate.
int cmd_fracreg(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    std::string link = opt(a, "--link", "logit");
    std::string vce = opt(a, "--vce", "robust");
    std::string cluster_var = opt(a, "--cluster");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra fracreg --data f.hbin --y Y --x X1,X2 [--link logit] "
                             "[--vce robust|cluster] [--cluster C] [--level P] [--keepif EXPR] [--out f.json]\n"
                             "  fractional-response quasi-MLE for 0 <= y <= 1; the VCE is always a\n"
                             "  sandwich (Stata's fracreg has no iid VCE).\n");
        return 2;
    }
    if (link != "logit")
        throw EngineError("unsupported",
                          "fracreg: only --link logit is implemented (got '" + link + "')");
    if (vce != "robust" && vce != "cluster")
        throw EngineError("bad_input",
                          "fracreg: --vce must be robust or cluster; the quasi-MLE has no iid VCE "
                          "(got '" + vce + "')");
    if (vce == "cluster" && cluster_var.empty()) {
        std::fprintf(stderr, "fracreg: --vce cluster requires --cluster C.\n");
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "fracreg: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    const double zc = z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    std::vector<std::string> cluster_names;
    if (!cluster_var.empty()) cluster_names.push_back(cluster_var);
    GlmInputs g = load_glm(ds, "fracreg", y, split_csv(xcsv), cluster_names);
    require_sample(g.n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    for (double v : g.y)
        if (!(v >= 0.0 && v <= 1.0))
            throw EngineError("bad_input",
                              "fracreg: outcome '" + y + "' must satisfy 0 <= y <= 1 (found " +
                              std::to_string(v) + ")");

    MlMaxOptions mo;
    if (has(a, "--iterate")) mo.maxiter = std::stoi(opt(a, "--iterate"));
    std::vector<double> start = frac_ols_start(g.X.data(), g.n, g.k, g.y.data());
    if (start.empty()) { std::fprintf(stderr, "fracreg: quasi-MLE failed (singular information).\n"); return 1; }
    FracLogitObjective obj(g.X.data(), g.n, g.k, g.y.data());
    const MlPath path = ml_newton_path(obj, start, mo);
    bool finite = !path.failed && std::isfinite(path.ll) && path.inv_curvature.size() ==
                  static_cast<size_t>(g.k) * static_cast<size_t>(g.k);
    for (double v : path.b) finite = finite && std::isfinite(v);
    if (!finite) { std::fprintf(stderr, "fracreg: quasi-MLE failed (singular information).\n"); return 1; }
    struct { std::vector<double> beta, bread; } r{path.b, path.inv_curvature};

    // Sandwich: score e_i = y_i - p_i (the quasi-likelihood's), bread = inv(-H).
    std::vector<double> eta(static_cast<size_t>(g.n));
    la::mat_vec(g.X.data(), g.n, g.k, r.beta.data(), eta.data());
    std::vector<double> e(static_cast<size_t>(g.n)), pr(static_cast<size_t>(g.n));
    for (int64_t i = 0; i < g.n; ++i) {
        pr[static_cast<size_t>(i)] = 1.0 / (1.0 + std::exp(-eta[static_cast<size_t>(i)]));
        e[static_cast<size_t>(i)] = g.y[static_cast<size_t>(i)] - pr[static_cast<size_t>(i)];
    }
    std::vector<double> meat(static_cast<size_t>(g.k) * g.k, 0.0);
    double factor;
    int n_clusters = 0;
    if (vce == "robust") {
        for (int64_t i = 0; i < g.n; ++i) {
            double ei = e[static_cast<size_t>(i)], e2 = ei * ei;
            for (int c = 0; c < g.k; ++c) {
                double xc = g.X[static_cast<int64_t>(c) * g.n + i];
                for (int d = 0; d < g.k; ++d)
                    meat[static_cast<size_t>(c) * g.k + d] += e2 * xc * g.X[static_cast<int64_t>(d) * g.n + i];
            }
        }
        // Same ML small-sample multiplier as logit/probit/tobit vce(robust).
        factor = g.n > 1 ? static_cast<double>(g.n) / static_cast<double>(g.n - 1) : 1.0;
    } else {
        cecon::cluster_meat(g.X.data(), g.n, g.k, e.data(), g.cluster[0], meat.data(), &n_clusters);
        if (n_clusters < 2)
            throw EngineError("bad_input",
                              "fracreg: vce(cluster) requires at least 2 clusters, but the cluster "
                              "variable has " + std::to_string(n_clusters));
        factor = static_cast<double>(n_clusters) / (static_cast<double>(n_clusters) - 1.0);
    }
    std::vector<double> V(static_cast<size_t>(g.k) * g.k, 0.0), tmp(static_cast<size_t>(g.k) * g.k, 0.0);
    la::sandwich(r.bread.data(), meat.data(), g.k, factor, V.data(), tmp.data());

    json coefs = json::array();
    for (int j = 0; j < g.k; ++j) {
        double se = std::sqrt(std::max(0.0, V[static_cast<size_t>(j) * g.k + j]));
        double b = r.beta[static_cast<size_t>(j)], z = tstat(b, se);
        coefs.push_back({{"name", g.names[static_cast<size_t>(j)]}, {"b", b}, {"se", se},
                         {"z", z}, {"p", (se > 0) ? z_pvalue(z) : 1.0},
                         {"ci_lo", b - zc * se}, {"ci_hi", b + zc * se}});
    }
    const double ll = path.ll;
    // e(ll_0): the constant-only quasi-likelihood fit, under the same maximize options
    double ll0 = 0.0;
    {
        std::vector<double> ones(static_cast<size_t>(g.n), 1.0);
        double ybar = 0.0;
        for (double v : g.y) ybar += v;
        ybar /= static_cast<double>(g.n);
        FracLogitObjective o0(ones.data(), g.n, 1, g.y.data());
        const MlPath p0 = ml_newton_path(o0, std::vector<double>{ybar}, mo);
        ll0 = (!p0.failed && std::isfinite(p0.ll)) ? p0.ll : frac::null_loglik(g.y.data(), g.n);
        if (ybar <= 0.0 || ybar >= 1.0) ll0 = 0.0;
    }
    json out = {{"estimator", "fracreg"}, {"depvar", y}, {"link", "logit"}, {"vce", vce},
                {"n_obs", g.n}, {"n_dropped_missing", g.n_dropped},
                {"converged", path.converged}, {"iters", path.ic},
                {"coefs", coefs}, {"ll", ll}};
    // ml's record: e(ic), e(ilog), e(gradient) and e(V_modelbased) (inverse of -H)
    out["ic"] = path.ic;
    out["ilog"] = path.ilog;
    {
        json gv = json::array();
        for (double v : path.gradient) gv.push_back(std::isfinite(v) ? json(v) : json(nullptr));
        out["e_gradient"] = {{"names", g.names}, {"values", gv}};
        json tmp;
        add_e_v(tmp, g.names, path.inv_curvature);
        out["e_Vmb"] = std::move(tmp["e_V"]);
    }
    if (has(a, "--emit-v")) add_e_v(out, g.names, V);
    if (ll0 != 0.0) {
        out["ll_0"] = ll0;
        out["r2_pseudo"] = 1.0 - ll / ll0;
    } else {  // degenerate outcome (ȳ at 0 or 1) — the null model is undefined
        out["ll_0"] = nullptr;
        out["r2_pseudo"] = nullptr;
    }
    add_wald(out, V, g.k, r.beta, g.k - 1);
    if (vce == "cluster") { out["cluster_var"] = cluster_var; out["n_clusters"] = n_clusters; }
    if (level_set) out["level"] = level;
    add_warnings(out, g.warnings);
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
