#include "inference/wildboot.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/linalg.h"
#include "core/rng.h"
#include "core/threads.h"
#include "estimators/iv2sls.h"
#include "hdfe/demean.h"
#include "hdfe/dof.h"
#include "vcov/vcov.h"

namespace cecon {

int wildboot_fanout_threads(int reps, int G) {
    const int64_t g = static_cast<int64_t>(G);
    return threads_for_work(static_cast<int64_t>(reps) * g * g, g * 8 + 4096);
}

// Shared fast WCR bootstrap-t kernel. The per-replicate t* is, for both OLS and
// 2SLS, a function only of the per-cluster pieces:
//   numerator   N(v)  = sum_g v_g c_g,         c_g = a' s~_g
//   se*(v)^2    = ssf * sum_g d_g^2,           d_g = v_g c_g - sum_h v_h W[g,h]
// where s~_g are the score rows of the RESTRICTED residuals (using the score
// design Sdesign = X̃ for OLS, X̂ for 2SLS), a = row j of bread = (Sdesign'Sdesign)^-1,
// W[g,h] = a' B_g bread s~_h, and B_g = sum_{i in g} Sdesign_i Xactual_i'.
// (For OLS Sdesign==Xactual so B_g is symmetric; for 2SLS B_g mixes X̂ and X.)
static WildBootResult wcb_core(const Factor& cl, int64_t n, int k, const double* Sdesign,
                               const double* Xactual, const double* bread,
                               const double* eUnrestricted, const double* uR, int j,
                               double r, double ssf, double beta_j, int reps,
                               uint64_t seed, BootWeights weights, bool webb_auto,
                               const std::string& paramname) {
    const int G = cl.n_levels;
    if (G < 2) throw std::runtime_error("need >= 2 clusters");

    std::vector<double> a(k);
    for (int c = 0; c < k; ++c) a[c] = bread[j + static_cast<size_t>(c) * k];

    // t0 from unrestricted residuals
    std::vector<double> Se(static_cast<size_t>(G) * k);
    cluster_scores(Sdesign, n, k, eUnrestricted, cl, Se.data());
    double se2_0 = 0;
    for (int g = 0; g < G; ++g) {
        double eg = 0;
        for (int c = 0; c < k; ++c) eg += Se[g + static_cast<size_t>(c) * G] * a[c];
        se2_0 += eg * eg;
    }
    se2_0 *= ssf;
    const double se0 = std::sqrt(se2_0);
    const double t0 = (beta_j - r) / se0;

    // restricted per-cluster scores s~_g (score design), c_g = a . s~_g
    std::vector<double> St(static_cast<size_t>(G) * k);
    cluster_scores(Sdesign, n, k, uR, cl, St.data());
    std::vector<double> cg(G);
    for (int g = 0; g < G; ++g) {
        double s = 0;
        for (int c = 0; c < k; ++c) s += St[g + static_cast<size_t>(c) * G] * a[c];
        cg[g] = s;
    }

    // B_g = sum_{i in g} Sdesign_i Xactual_i'  (k x k per cluster, may be asymmetric)
    std::vector<double> B(static_cast<size_t>(G) * k * k, 0.0);
    const int32_t* gc = cl.codes.data();
    for (int64_t i = 0; i < n; ++i) {
        double* Bg = B.data() + static_cast<size_t>(gc[i]) * k * k;
        for (int c1 = 0; c1 < k; ++c1) {
            double si = Sdesign[static_cast<size_t>(c1) * n + i];
            for (int c2 = 0; c2 < k; ++c2)
                Bg[c1 + c2 * k] += si * Xactual[static_cast<size_t>(c2) * n + i];
        }
    }
    // w_g = bread * (a' B_g)';  W[g,h] = w_g . s~_h
    std::vector<double> wmat(static_cast<size_t>(G) * k);
    {
        std::vector<double> u(k), wg(k);
        for (int g = 0; g < G; ++g) {
            const double* Bg = B.data() + static_cast<size_t>(g) * k * k;
            for (int c2 = 0; c2 < k; ++c2) {
                double s = 0;
                for (int c1 = 0; c1 < k; ++c1) s += a[c1] * Bg[c1 + c2 * k];  // (a'B_g)[c2]
                u[c2] = s;
            }
            la::matvec(bread, k, u.data(), wg.data());
            for (int c = 0; c < k; ++c) wmat[g + static_cast<size_t>(c) * G] = wg[c];
        }
    }
    // High-end memory guard: W below is a dense G x G matrix of doubles, so its
    // memory is O(G^2). Cap G to keep W within a ~512MB budget: G^2 * 8B <= 512MB
    // (decimal MB = 1e6 B) => G <= sqrt(512e6 / 8) = 8000. A streaming variant that
    // never materializes the full G x G W (folding the g,h contraction into the
    // per-replicate loop instead) would lift this cap; parked as a future item,
    // not implemented here.
    constexpr int kMaxWildbootClusters = 8000;
    if (G > kMaxWildbootClusters) {
        long long mb_needed = std::llround(static_cast<double>(G) * static_cast<double>(G) *
                                           8.0 / 1e6);
        throw std::runtime_error(
            "wildboot: G=" + std::to_string(G) + " clusters would need " +
            std::to_string(mb_needed) + " MB for the GxG weight matrix (ceiling ~" +
            std::to_string(kMaxWildbootClusters) + "); reduce clusters or use a non-wildboot vce");
    }
    std::vector<double> W(static_cast<size_t>(G) * G);
    la::gemm_nt(wmat.data(), St.data(), W.data(), G, k, G);

    const bool webb = (weights == BootWeights::Webb) || (webb_auto && G < 12);

    std::vector<double> tstars(static_cast<size_t>(reps));
    // Fan-out sized by the settings-derived envelope (core + memory budget), see
    // wildboot_fanout_threads; each replicate writes only tstars[b].
    const int wildboot_threads = wildboot_fanout_threads(reps, G);
    parallel_for(reps, [&](int64_t b) {
        std::vector<double> v(G);
        for (int g = 0; g < G; ++g)
            v[g] = webb ? webb6(seed, b, g) : rademacher(seed, b, g);
        double N = 0;
        for (int g = 0; g < G; ++g) N += v[g] * cg[g];
        double se2 = 0;
        for (int g = 0; g < G; ++g) {
            double wv = 0;
            for (int h = 0; h < G; ++h) wv += W[g + static_cast<size_t>(h) * G] * v[h];
            double d = v[g] * cg[g] - wv;
            se2 += d * d;
        }
        se2 *= ssf;
        tstars[b] = (se2 > 0) ? N / std::sqrt(se2) : 0.0;
    }, wildboot_threads);

    long sym = 0, ge = 0, le = 0;
    const double at0 = std::fabs(t0);
    for (int b = 0; b < reps; ++b) {
        double t = tstars[b];
        if (std::fabs(t) >= at0) ++sym;
        if (t >= t0) ++ge;
        if (t <= t0) ++le;
    }
    WildBootResult R;
    R.reps = reps;
    R.weights = webb ? "webb" : "rademacher";
    R.param = paramname;
    R.null_beta = r;
    R.beta = beta_j;
    R.se = se0;
    R.t0 = t0;
    R.p_symmetric = (1.0 + sym) / (reps + 1.0);
    double pl = (1.0 + ge) / (reps + 1.0), pr = (1.0 + le) / (reps + 1.0);
    R.p_equaltail = std::min(1.0, 2.0 * std::min(pl, pr));
    R.n_clusters = G;
    R.seed = seed;
    R.webb_fallback = (webb && weights == BootWeights::Rademacher);
    R.tstars = std::move(tstars);
    return R;
}

// Small-sample factor matching compute_vcov one-way cluster, given a fit's dof.
static double cluster_ssf(int64_t n, int k, const std::vector<Factor>& fe,
                          const Factor& cl, int k_fe) {
    int k_adj = k + k_fe;
    for (const Factor& f : fe)
        if (fe_nested_in_cluster(f, cl)) k_adj -= (f.n_levels - 1);
    const int G = cl.n_levels;
    return (static_cast<double>(G) / (G - 1)) *
           (static_cast<double>(n - 1) / static_cast<double>(n - k_adj));
}

WildBootResult wild_cluster_bootstrap(ModelData& md, int j, double r, int reps,
                                      uint64_t seed, int cluster_index,
                                      BootWeights weights, bool webb_auto) {
    DesignMatrix& dm = md.dm;
    const int64_t n = dm.n;
    const int k = dm.k;
    if (j < 0 || j >= k) throw std::runtime_error("bad param index");
    if (md.cluster.empty())
        throw std::runtime_error("wild cluster bootstrap requires a cluster variable");
    const Factor& cl = md.cluster.at(cluster_index);

    demean(dm.y.data(), n, 1, md.fe);
    demean(dm.X.data(), n, k, md.fe);

    std::vector<double> bread(static_cast<size_t>(k) * k);
    la::xtx(dm.X.data(), n, k, bread.data());
    if (la::spd_inverse(bread.data(), k) != 0) throw std::runtime_error("X'X singular");
    std::vector<double> Xty(k), beta(k);
    la::xtv(dm.X.data(), n, k, dm.y.data(), Xty.data());
    la::matvec(bread.data(), k, Xty.data(), beta.data());
    std::vector<double> e(n);
    la::residual(dm.X.data(), n, k, beta.data(), dm.y.data(), e.data());

    DofInfo dof = compute_dof(n, k, md.fe);
    double ssf = cluster_ssf(n, k, md.fe, cl, dof.k_fe);

    // restricted fit: regress (y - r*X_j) on X_{-j}
    const int km = k - 1;
    std::vector<double> betaR(k);
    betaR[j] = r;
    if (km > 0) {
        std::vector<double> Xm(static_cast<size_t>(n) * km);
        int cc = 0;
        for (int c = 0; c < k; ++c) {
            if (c == j) continue;
            std::copy(dm.X.data() + static_cast<size_t>(c) * n,
                      dm.X.data() + static_cast<size_t>(c) * n + n,
                      Xm.data() + static_cast<size_t>(cc) * n);
            ++cc;
        }
        std::vector<double> yr(n);
        const double* Xj = dm.X.data() + static_cast<size_t>(j) * n;
        for (int64_t i = 0; i < n; ++i) yr[i] = dm.y[i] - r * Xj[i];
        std::vector<double> breadR(static_cast<size_t>(km) * km), Xmty(km), betaMj(km);
        la::xtx(Xm.data(), n, km, breadR.data());
        if (la::spd_inverse(breadR.data(), km) != 0)
            throw std::runtime_error("restricted X'X singular");
        la::xtv(Xm.data(), n, km, yr.data(), Xmty.data());
        la::matvec(breadR.data(), km, Xmty.data(), betaMj.data());
        cc = 0;
        for (int c = 0; c < k; ++c)
            if (c != j) betaR[c] = betaMj[cc++];
    }
    std::vector<double> uR(n);
    la::residual(dm.X.data(), n, k, betaR.data(), dm.y.data(), uR.data());

    return wcb_core(cl, n, k, dm.X.data(), dm.X.data(), bread.data(), e.data(), uR.data(),
                    j, r, ssf, beta[j], reps, seed, weights, webb_auto, dm.xnames[j]);
}

WildBootResult wild_cluster_bootstrap_iv(IvModelData& md, double r, int reps,
                                         uint64_t seed, int cluster_index,
                                         BootWeights weights, bool webb_auto) {
    if (md.kendog != 1)
        throw std::runtime_error("IV wild bootstrap currently supports exactly one endogenous regressor");
    if (md.cluster.empty())
        throw std::runtime_error("IV wild bootstrap requires a cluster variable");
    const Factor& cl = md.cluster.at(cluster_index);
    const int64_t n = md.n;
    const int kexog = md.kexog, kx = kexog + 1, kw = kexog + md.kinst;
    const int jendog = kexog;  // the endogenous coef is last; it is the tested param

    // demean everything
    demean(md.y.data(), n, 1, md.fe);
    demean(md.Xexog.data(), n, kexog, md.fe);
    demean(md.Xendog.data(), n, md.kendog, md.fe);
    demean(md.Zinst.data(), n, md.kinst, md.fe);

    // W=[exog,inst]; first stage Xhat=[exog, P_W endog]; X=[exog,endog]
    std::vector<double> W(static_cast<size_t>(n) * kw);
    for (int j = 0; j < kexog; ++j)
        std::copy(md.exog(j), md.exog(j) + n, W.data() + static_cast<int64_t>(j) * n);
    for (int j = 0; j < md.kinst; ++j)
        std::copy(md.inst(j), md.inst(j) + n, W.data() + static_cast<int64_t>(kexog + j) * n);
    std::vector<double> breadW(static_cast<size_t>(kw) * kw);
    la::xtx(W.data(), n, kw, breadW.data());
    if (la::spd_inverse(breadW.data(), kw) != 0) throw std::runtime_error("W'W singular");
    std::vector<double> Xhat(static_cast<size_t>(n) * kx), X(static_cast<size_t>(n) * kx);
    for (int j = 0; j < kexog; ++j) {
        std::copy(md.exog(j), md.exog(j) + n, Xhat.data() + static_cast<int64_t>(j) * n);
        std::copy(md.exog(j), md.exog(j) + n, X.data() + static_cast<int64_t>(j) * n);
    }
    std::copy(md.endog(0), md.endog(0) + n, X.data() + static_cast<int64_t>(kexog) * n);
    {
        std::vector<double> Wtv(kw), gamma(kw);
        la::xtv(W.data(), n, kw, md.endog(0), Wtv.data());
        la::matvec(breadW.data(), kw, Wtv.data(), gamma.data());
        la::mat_vec(W.data(), n, kw, gamma.data(), Xhat.data() + static_cast<int64_t>(kexog) * n);
    }

    std::vector<double> bread(static_cast<size_t>(kx) * kx);
    la::xtx(Xhat.data(), n, kx, bread.data());
    if (la::spd_inverse(bread.data(), kx) != 0) throw std::runtime_error("Xhat'Xhat singular");
    std::vector<double> Xhaty(kx), beta(kx);
    la::xtv(Xhat.data(), n, kx, md.y.data(), Xhaty.data());
    la::matvec(bread.data(), kx, Xhaty.data(), beta.data());
    std::vector<double> e(n);
    la::residual(X.data(), n, kx, beta.data(), md.y.data(), e.data());

    DofInfo dof = compute_dof(n, kx, md.fe);
    double ssf = cluster_ssf(n, kx, md.fe, cl, dof.k_fe);

    // restricted fit (single endog = r): regress (y - r*endog) on exog
    std::vector<double> betaR(kx);
    betaR[jendog] = r;
    if (kexog > 0) {
        std::vector<double> yr(n);
        const double* en = md.endog(0);
        for (int64_t i = 0; i < n; ++i) yr[i] = md.y[i] - r * en[i];
        std::vector<double> breadE(static_cast<size_t>(kexog) * kexog), Ety(kexog), be(kexog);
        la::xtx(md.Xexog.data(), n, kexog, breadE.data());
        if (la::spd_inverse(breadE.data(), kexog) != 0)
            throw std::runtime_error("restricted exog X'X singular");
        la::xtv(md.Xexog.data(), n, kexog, yr.data(), Ety.data());
        la::matvec(breadE.data(), kexog, Ety.data(), be.data());
        for (int c = 0; c < kexog; ++c) betaR[c] = be[c];
    }
    std::vector<double> uR(n);
    la::residual(X.data(), n, kx, betaR.data(), md.y.data(), uR.data());

    std::string pname = md.endog_names.empty() ? "endog" : md.endog_names[0];
    // score design = Xhat (instrumented); actual = X
    return wcb_core(cl, n, kx, Xhat.data(), X.data(), bread.data(), e.data(), uR.data(),
                    jendog, r, ssf, beta[jendog], reps, seed, weights, webb_auto, pname);
}

}  // namespace cecon
