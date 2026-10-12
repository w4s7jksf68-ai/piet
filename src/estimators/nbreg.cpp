#include "estimators/nbreg.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "core/linalg.h"

namespace cecon {

// Forward declarations (defined below).
double digamma(double x);
double trigamma(double x);

// We parameterize α via lnα. The parameter vector is θ = [β; lnα], length k+1.
//
//   μ_i = exp(η_i),  η_i = X_i β + off_i
//   r   = 1/α,                 p_i = r / (r + μ_i)
//   log L_i = lgamma(y_i + r) - lgamma(r) - lgamma(y_i + 1)
//             + r log(p_i) + y_i log(1 - p_i)
//
// Score / Hessian wrt β use the standard NB2 form:
//   ∂L/∂β   = Σ (y_i - μ_i) / (1 + α μ_i) * x_i
//   -∂²/∂β² = Σ μ_i (1 + α y_i) / (1 + α μ_i)^2 * x_i x_i'   (observed info)
// We use the OBSERVED information for the Hessian — the weight carries the
// realized y_i (not E[y_i]=μ_i), so this is Stata's nbreg (ml d2) Hessian, NOT
// the expected/Fisher info. That is why the default (iid) SE matches Stata's;
// see the per-term comments at the Hessian assembly below.
//
// For lnα we use ψ (digamma) and ψ_1 (trigamma) derivatives. NOTE: the observed
// information is NOT block-diagonal in (β, lnα) — the cross weight
// α μ (y-μ)/(1+α μ)^2 is nonzero and IS assembled below; it vanishes only in
// expectation (E[y-μ]=0), i.e. only for the Fisher info we do not use.

namespace {

inline double safe_log(double x) { return std::log(std::max(x, 1e-300)); }

}  // namespace

NbregResult nbreg(const double* X, int64_t n_obs, int k,
                  const double* y, const double* offset,
                  const double* cluster_var,
                  int maxit, double tol) {
    NbregResult R;
    R.k = k;
    R.n_obs = n_obs;
    R.beta.assign(static_cast<size_t>(k), 0.0);
    R.lnalpha = 0.0;  // start at α=1 (overdispersion ~ 1, between Poisson and NB)

    // Initialize β from Poisson MLE? For simplicity, start at zeros with intercept
    // = log(ybar). The user is expected to include an intercept as a column of X.
    // We don't auto-add — caller's responsibility. Find any constant column and
    // set its β to log(ybar); other betas start at 0.
    double sy = 0;
    int64_t ny = 0;
    for (int64_t i = 0; i < n_obs; ++i) { sy += y[i]; ++ny; }
    double ybar = (ny > 0) ? (sy / static_cast<double>(ny)) : 1.0;
    double logyb = std::log(std::max(ybar, 1e-12));
    for (int c = 0; c < k; ++c) {
        bool is_const = true;
        double v0 = X[static_cast<int64_t>(c) * n_obs + 0];
        for (int64_t i = 1; i < n_obs; ++i)
            if (X[static_cast<int64_t>(c) * n_obs + i] != v0) { is_const = false; break; }
        if (is_const && v0 == 1.0) R.beta[static_cast<size_t>(c)] = logyb;
    }

    std::vector<double> eta(static_cast<size_t>(n_obs));
    std::vector<double> mu(static_cast<size_t>(n_obs));
    std::vector<double> score(static_cast<size_t>(k + 1), 0.0);
    std::vector<double> negH(static_cast<size_t>(k + 1) * (k + 1), 0.0);
    std::vector<double> negH_inv(static_cast<size_t>(k + 1) * (k + 1), 0.0);
    std::vector<double> step(static_cast<size_t>(k + 1), 0.0);
    // Step-halving candidate. Hoisted with the rest: the line search overwrites all k
    // entries before eval_ll reads any of them, so its value at construction is dead
    // and the buffer can be reused across NR iterations byte-for-byte.
    std::vector<double> b_try(static_cast<size_t>(k), 0.0);

    // Poisson (α=0) fit: warm-starts the NB Newton AND is the α→0 boundary fallback.
    // NR on β with the Poisson score Σ(y-μ)x and info Σμ x x' (concave ⇒ safe). For the
    // canonical log-link Poisson, observed and expected/Fisher info coincide (Σμ x x'
    // carries no y), so this cached info is an exact boundary SE either way. Caches it +
    // the Poisson ll at the fit.
    std::vector<double> pois_fish(static_cast<size_t>(k) * k, 0.0);
    double pois_ll = 0.0;
    auto poisson_fit = [&]() {
        std::vector<double> gb(k), Hb(static_cast<size_t>(k) * k),
            Hi(static_cast<size_t>(k) * k), db(k);
        for (int rep = 0; rep < 200; ++rep) {
            std::fill(gb.begin(), gb.end(), 0.0);
            std::fill(Hb.begin(), Hb.end(), 0.0);
            for (int64_t i = 0; i < n_obs; ++i) {
                double s = offset ? offset[i] : 0.0;
                for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
                double mi = std::exp(s), e = y[i] - mi;
                for (int c = 0; c < k; ++c) {
                    double xc = X[static_cast<int64_t>(c) * n_obs + i];
                    gb[static_cast<size_t>(c)] += e * xc;
                    double* col = Hb.data() + static_cast<size_t>(c) * k;
                    for (int rr = 0; rr < k; ++rr) col[static_cast<size_t>(rr)] += mi * xc * X[static_cast<int64_t>(rr) * n_obs + i];
                }
            }
            std::copy(Hb.begin(), Hb.end(), Hi.begin());
            if (la::spd_inverse(Hi.data(), k) != 0) break;
            la::matvec(Hi.data(), k, gb.data(), db.data());
            double mag = 0;
            for (int c = 0; c < k; ++c) { R.beta[static_cast<size_t>(c)] += db[static_cast<size_t>(c)]; mag += std::fabs(db[static_cast<size_t>(c)]); }
            if (mag < 1e-13) break;
        }
        std::fill(pois_fish.begin(), pois_fish.end(), 0.0);
        pois_ll = 0.0;
        for (int64_t i = 0; i < n_obs; ++i) {
            double s = offset ? offset[i] : 0.0;
            for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
            double mi = std::exp(s);
            pois_ll += -mi + y[i] * safe_log(mi) - std::lgamma(y[i] + 1.0);
            for (int c = 0; c < k; ++c) {
                double* col = pois_fish.data() + static_cast<size_t>(c) * k;
                for (int rr = 0; rr < k; ++rr)
                    col[static_cast<size_t>(rr)] += mi * X[static_cast<int64_t>(c) * n_obs + i] * X[static_cast<int64_t>(rr) * n_obs + i];
            }
        }
    };
    poisson_fit();  // warm-start β at the Poisson MLE

    // NB ll at trial (β, lnα) for the step-halving line search. α clamped away from 0 so
    // deep-below-floor trials stay finite (they are discarded by the boundary check).
    auto eval_ll = [&](const std::vector<double>& b, double lna) {
        double a = std::max(std::exp(lna), 1e-12), rr = 1.0 / a, ll = 0;
        for (int64_t i = 0; i < n_obs; ++i) {
            double s = offset ? offset[i] : 0.0;
            for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n_obs + i] * b[static_cast<size_t>(c)];
            double mi = std::exp(s), rpmu = rr + mi;
            ll += std::lgamma(y[i] + rr) - std::lgamma(rr) - std::lgamma(y[i] + 1.0)
                + rr * (std::log(rr) - safe_log(rpmu)) + y[i] * (safe_log(mi) - safe_log(rpmu));
        }
        return ll;
    };
    const double LNA_FLOOR = -18.0;  // α ≈ 1.5e-8: at/below this treat as the α→0 boundary
    // α ≈ 2.5e-3. Above this, a non-positive (lnα,lnα) information means something
    // other than the boundary and must stay a hard failure; below it, α is already
    // negligible and the Poisson limit is the answer either way.
    const double LNA_NEAR_ZERO = -6.0;
    bool boundary = false;

    double prev_ll = -1e300;
    bool conv = false;
    int iter;
    for (iter = 0; iter < maxit; ++iter) {
        double a = std::exp(R.lnalpha);
        double r = 1.0 / a;
        // η = X β + offset
        for (int64_t i = 0; i < n_obs; ++i) {
            double s = offset ? offset[i] : 0.0;
            for (int c = 0; c < k; ++c)
                s += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
            eta[static_cast<size_t>(i)] = s;
            mu[static_cast<size_t>(i)] = std::exp(s);
        }
        // Log-lik, score (β, lnα), Hessian (observed info).
        double ll = 0;
        std::fill(score.begin(), score.end(), 0.0);
        std::fill(negH.begin(), negH.end(), 0.0);
        double info_aa = 0;
        double score_a = 0;
        for (int64_t i = 0; i < n_obs; ++i) {
            double yi = y[i];
            double mi = mu[static_cast<size_t>(i)];
            double w = 1.0 + a * mi;                // 1 + α μ
            double rpmu = r + mi;                   // r + μ = 1/α + μ
            // log-likelihood
            ll += std::lgamma(yi + r) - std::lgamma(r) - std::lgamma(yi + 1.0)
                + r * (std::log(r) - safe_log(rpmu))
                + yi * (safe_log(mi) - safe_log(rpmu));
            // score wrt β: (y - μ)/(1 + α μ) * x
            double resb = (yi - mi) / w;
            for (int c = 0; c < k; ++c)
                score[static_cast<size_t>(c)] += resb * X[static_cast<int64_t>(c) * n_obs + i];
            // observed -∂²L/∂β² weight = μ(1 + α y) / (1 + α μ)²
            // (Stata's d2 uses the observed Hessian, not Fisher; we follow.)
            double wi = mi * (1.0 + a * yi) / (w * w);
            for (int c = 0; c < k; ++c) {
                double v = wi * X[static_cast<int64_t>(c) * n_obs + i];
                double* col = negH.data() + static_cast<size_t>(c) * (k + 1);
                for (int rr = 0; rr < k; ++rr)
                    col[static_cast<size_t>(rr)] += v * X[static_cast<int64_t>(rr) * n_obs + i];
            }
            // score wrt lnα:
            //   ∂L/∂lnα = α * ∂L/∂α
            //   ∂log L_i / ∂α = - r² [ψ(y+r) - ψ(r) + log(r/(r+μ)) + (y-μ)/(r+μ) · ???]
            // Use the standard form derived from log L:
            //   ∂L/∂(lnα) = - r [ ψ(y+r) - ψ(r) + log(r/(r+μ)) ] + (y - μ) μ α / (1 + α μ)
            // Actually: derive ∂/∂r * ∂r/∂(lnα) where ∂r/∂(lnα) = -r.
            //   ∂L_i/∂r = ψ(y_i+r) - ψ(r) + log(r/(r+μ_i)) + (μ_i - y_i)/(r + μ_i)
            //   ∂L_i/∂(lnα) = -r * ∂L_i/∂r
            double drdL = (digamma(yi + r) - digamma(r))
                        + std::log(r) - safe_log(rpmu)
                        + (mi - yi) / rpmu;
            score_a += -r * drdL;
            // Information for lnα (expected): sum -r² * E[∂²L/∂r²]  + diagonal term.
            // Use the closed-form expression:
            //   I_aa = Σ_i { r² [ψ_1(r) - E[ψ_1(y+r)]] - r/(r+μ)*y_expected term }
            // For ML in Stata, the OPG approximation is sometimes used. We use
            // an empirical Hessian here (observed info) which is what Stata's
            // ml d2 yields. The trigamma terms:
            //   ∂²L_i/∂r² = ψ_1(y_i+r) - ψ_1(r) + 1/r - 2/(r+μ_i) + (y_i+r)/(r+μ_i)²
            // Note: 1/r from ∂/∂r[r·log(r) ...] etc. Derivation matches Cameron-Trivedi.
            double d2drr = trigamma(yi + r) - trigamma(r)
                         + 1.0/r - 2.0/rpmu + (yi + r) / (rpmu * rpmu);
            // ∂²L_i / ∂(lnα)² = ∂(∂L/∂(lnα))/∂lnα
            //                 = ∂(-r·∂L/∂r)/∂lnα
            //                 = -∂r/∂lnα * ∂L/∂r - r * ∂²L/∂r² * ∂r/∂lnα
            //                 =  r * ∂L/∂r + r² * ∂²L/∂r²
            double d2_lna = r * drdL + r * r * d2drr;
            info_aa += -d2_lna;   // negH for lnα,lnα
            // Cross-derivative ∂²L/∂β∂(lnα), observed:
            //   ∂L/∂β_c = Σ x_c (y-μ)/(1+αμ).
            //   ∂/∂α [(y-μ)/(1+αμ)] = -(y-μ)·μ / (1+αμ)²
            //   ∂/∂(lnα) = α · (above), so cross contribution to -∂²L is
            //   +α·μ(y-μ)/(1+αμ)² per x_c.
            double cross_w = a * mi * (yi - mi) / (w * w);
            for (int c = 0; c < k; ++c) {
                double xv = X[static_cast<int64_t>(c) * n_obs + i];
                negH[static_cast<size_t>(k) * (k + 1) + c] += cross_w * xv;
                negH[static_cast<size_t>(c) * (k + 1) + k] += cross_w * xv;
            }
        }
        score[static_cast<size_t>(k)] = score_a;
        negH[static_cast<size_t>(k) * (k + 1) + k] = info_aa;

        // Solve and update.
        // α→0 boundary, SECOND detector (the LNA_FLOOR check below is the first).
        // As α→0 the (lnα,lnα) observed information collapses toward 0, and with
        // r = 1/α ~ 1e7 the digamma/trigamma difference that forms it loses every
        // significant digit to cancellation — it lands slightly NEGATIVE while lnα
        // is still above LNA_FLOOR. negH is then not positive definite, Cholesky
        // correctly refuses it, and the old code returned a "no coefficients"
        // failure for a fit that is simply Poisson.
        //
        // Measured on the underdispersed `round(spirits*3)` counts from the
        // fatalities panel: iter 17, lnα = -17.45 (floor is -18), info_aa =
        // -4.6e-06 against β-block entries of ~1e3. One more halved step would
        // have crossed the floor and taken the boundary path.
        //
        // Whether the cancellation lands just above or just below zero is decided
        // by the last bits of digamma/trigamma, so this is a latent numerical bug
        // on EVERY platform, not a Windows one — it simply surfaced here first.
        // Guarded on lnα being deep in boundary territory so a genuinely singular
        // design (collinear X) still reports as the failure it is.
        if (info_aa <= 0.0 && R.lnalpha < LNA_NEAR_ZERO) {
            boundary = true;
            R.iters = iter + 1;
            R.loglik = ll;
            break;
        }
        std::copy(negH.begin(), negH.end(), negH_inv.begin());
        if (la::spd_inverse(negH_inv.data(), k + 1) != 0) {
            R.converged = false; R.iters = iter; R.loglik = ll; return R;
        }
        la::matvec(negH_inv.data(), k + 1, score.data(), step.data());
        // Step-halving: accept the full Newton step only if ll does not decrease (the
        // logit/probit line-search idiom); tames the NB α→0 boundary divergence.
        double lam = 1.0;
        double lna_try = R.lnalpha;
        for (int hlv = 0; hlv < 40; ++hlv) {
            for (int c = 0; c < k; ++c) b_try[static_cast<size_t>(c)] = R.beta[static_cast<size_t>(c)] + lam * step[static_cast<size_t>(c)];
            lna_try = R.lnalpha + lam * step[static_cast<size_t>(k)];
            if (eval_ll(b_try, lna_try) >= ll) break;
            lam *= 0.5;
        }
        double smag = 0;
        for (int c = 0; c < k; ++c) { R.beta[static_cast<size_t>(c)] = b_try[static_cast<size_t>(c)]; smag += std::fabs(lam * step[static_cast<size_t>(c)]); }
        R.lnalpha = lna_try;
        smag += std::fabs(lam * step[static_cast<size_t>(k)]);
        // α→0 boundary (equidispersed data): stop and hand off to the Poisson limit below.
        if (R.lnalpha < LNA_FLOOR) { boundary = true; R.iters = iter + 1; R.loglik = ll; break; }

        if (std::fabs(ll - prev_ll) < tol * (std::fabs(ll) + 1.0) && smag < tol) {
            conv = true;
            R.loglik = ll;
            R.iters = iter + 1;
            break;
        }
        prev_ll = ll;
        R.iters = iter + 1;
        R.loglik = ll;
    }
    R.converged = conv;
    if (boundary) {
        // α→0 boundary: NB2 collapses to Poisson (equidispersed data). Report the exact
        // Poisson MLE — coefs, ll, Fisher-based SEs — matching `poisson`. α/lnα are
        // boundary noise (Stata prints α≈0 with a MISSING lnα SE), so report α=0 and a
        // zero (missing) lnα SE. Re-solve Poisson to pin β exactly, then load the Poisson
        // Fisher into the β-block of negH_inv so the SE code below is Poisson-exact and
        // the robust/cluster sandwich (which reads α=0 ⇒ e_i=y_i-μ_i) is the Poisson one.
        poisson_fit();
        R.alpha = 0.0; R.lnalpha = LNA_FLOOR; R.loglik = pois_ll; R.converged = true;
        for (int64_t i = 0; i < n_obs; ++i) {
            double s = offset ? offset[i] : 0.0;
            for (int c = 0; c < k; ++c) s += X[static_cast<int64_t>(c) * n_obs + i] * R.beta[static_cast<size_t>(c)];
            mu[static_cast<size_t>(i)] = std::exp(s);
        }
        std::vector<double> Fi(pois_fish);
        la::spd_inverse(Fi.data(), k);
        std::fill(negH_inv.begin(), negH_inv.end(), 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr)
                negH_inv[static_cast<size_t>(c) * (k + 1) + rr] = Fi[static_cast<size_t>(c) * k + rr];
    } else {
        R.alpha = std::exp(R.lnalpha);
    }

    // Final IID SE = sqrt(diag(I^-1))
    R.se_iid.assign(static_cast<size_t>(k), 0.0);
    for (int c = 0; c < k; ++c)
        R.se_iid[static_cast<size_t>(c)] =
            std::sqrt(std::max(0.0, negH_inv[static_cast<size_t>(c) * (k + 1) + c]));
    R.se_lnalpha_iid =
        std::sqrt(std::max(0.0, negH_inv[static_cast<size_t>(k) * (k + 1) + k]));

    // Cluster-robust SE: sandwich with per-cluster score sums (β block only).
    if (cluster_var) {
        std::unordered_map<double, int> ci_of;
        std::vector<std::vector<double>> per_cl;
        per_cl.reserve(8192);
        // per-obs score wrt β at final β, lnα.
        double a = R.alpha;
        std::vector<double> s_i(static_cast<size_t>(n_obs) * k, 0.0);
        for (int64_t i = 0; i < n_obs; ++i) {
            double mi = mu[static_cast<size_t>(i)];
            double resb = (y[i] - mi) / (1.0 + a * mi);
            for (int c = 0; c < k; ++c)
                s_i[static_cast<size_t>(c) * n_obs + i] =
                    resb * X[static_cast<int64_t>(c) * n_obs + i];
        }
        std::vector<int> cl_of_row(static_cast<size_t>(n_obs));
        for (int64_t i = 0; i < n_obs; ++i) {
            double v = cluster_var[i];
            auto it = ci_of.find(v);
            int ci;
            if (it == ci_of.end()) {
                ci = static_cast<int>(per_cl.size());
                ci_of[v] = ci;
                per_cl.emplace_back(static_cast<size_t>(k), 0.0);
            } else ci = it->second;
            cl_of_row[static_cast<size_t>(i)] = ci;
        }
        for (int64_t i = 0; i < n_obs; ++i) {
            int ci = cl_of_row[static_cast<size_t>(i)];
            for (int c = 0; c < k; ++c)
                per_cl[static_cast<size_t>(ci)][static_cast<size_t>(c)] +=
                    s_i[static_cast<size_t>(c) * n_obs + i];
        }
        std::vector<double> G(static_cast<size_t>(k) * k, 0.0);
        for (const auto& sc : per_cl)
            for (int c = 0; c < k; ++c)
                for (int rr = 0; rr < k; ++rr)
                    G[static_cast<size_t>(c) * k + rr] += sc[static_cast<size_t>(c)] * sc[static_cast<size_t>(rr)];
        // V_rob = (negH^-1_ββ) * G * (negH^-1_ββ).
        // Extract the β-β block of negH_inv into a k*k matrix.
        std::vector<double> H_bb(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr)
                H_bb[static_cast<size_t>(c) * k + rr] =
                    negH_inv[static_cast<size_t>(c) * (k + 1) + rr];
        std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += H_bb[static_cast<size_t>(j) * k + rr] * G[static_cast<size_t>(c) * k + j];
                tmp[static_cast<size_t>(c) * k + rr] = s;
            }
        // Stata's nbreg cluster: q = M/(M-1) * (N-1)/(N-k_full) where k_full=k+1 (incl. lnα).
        int M = static_cast<int>(per_cl.size());
        double q = (static_cast<double>(M) / (M - 1.0))
                 * ((static_cast<double>(n_obs) - 1.0) / (static_cast<double>(n_obs) - static_cast<double>(k + 1)));
        std::vector<double> Vr(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += tmp[static_cast<size_t>(j) * k + rr] * H_bb[static_cast<size_t>(c) * k + j];
                Vr[static_cast<size_t>(c) * k + rr] = q * s;
            }
        R.se_robust.assign(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            R.se_robust[static_cast<size_t>(c)] =
                std::sqrt(std::max(0.0, Vr[static_cast<size_t>(c) * k + c]));
    } else {
        // HC1 sandwich (no cluster): G = Σ_i s_i s_i'.
        double a = R.alpha;
        std::vector<double> G(static_cast<size_t>(k) * k, 0.0);
        for (int64_t i = 0; i < n_obs; ++i) {
            double mi = mu[static_cast<size_t>(i)];
            double resb = (y[i] - mi) / (1.0 + a * mi);
            for (int c = 0; c < k; ++c) {
                double sc = resb * X[static_cast<int64_t>(c) * n_obs + i];
                for (int rr = 0; rr < k; ++rr) {
                    double sr = resb * X[static_cast<int64_t>(rr) * n_obs + i];
                    G[static_cast<size_t>(c) * k + rr] += sc * sr;
                }
            }
        }
        std::vector<double> H_bb(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr)
                H_bb[static_cast<size_t>(c) * k + rr] =
                    negH_inv[static_cast<size_t>(c) * (k + 1) + rr];
        std::vector<double> tmp(static_cast<size_t>(k) * k, 0.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += H_bb[static_cast<size_t>(j) * k + rr] * G[static_cast<size_t>(c) * k + j];
                tmp[static_cast<size_t>(c) * k + rr] = s;
            }
        std::vector<double> Vr(static_cast<size_t>(k) * k, 0.0);
        // BUG-164 (W:P5): NO finite-sample correction here. Stata applies the
        // regress-style N/(N-k) inflation to OLS robust VCE, but its
        // maximum-likelihood robust VCE — nbreg's included — uses the bare
        // sandwich. This code applied N/(N-k-1) anyway, so every reported
        // robust SE was too large by sqrt(N/(N-k-1)).
        //
        // Measured against the W:P5 answerbook (Stata 19.5,
        // `nbreg ycount x1 x2 i.region if t>=2, vce(robust)`, N=4,476, k+1=7):
        //   shipped  q = N/(N-k-1) -> se(x1) .0044682, ratio 1.000784 to Stata
        //            (= sqrt(4476/4469), i.e. exactly the wrong factor)
        //   q = 1    -> se(x1) .0044642, ratio 0.9998964 — now slightly SMALL
        //   q = N/(N-1) -> matches Stata to seven digits on all seven
        //            coefficients (0.9998964 x sqrt(4476/4475) = 1.0000081)
        // So Stata's ML robust VCE carries N/(N-1), not the regress-style
        // N/(N-k) this code borrowed.
        //
        // The CLUSTER branch above keeps its own q: Stata's ML cluster VCE
        // does carry M/(M-1), and that factor was verified against its own
        // oracle. Only the no-cluster path was wrong.
        const double q = static_cast<double>(n_obs) / (static_cast<double>(n_obs) - 1.0);
        for (int c = 0; c < k; ++c)
            for (int rr = 0; rr < k; ++rr) {
                double s = 0;
                for (int j = 0; j < k; ++j)
                    s += tmp[static_cast<size_t>(j) * k + rr] * H_bb[static_cast<size_t>(c) * k + j];
                Vr[static_cast<size_t>(c) * k + rr] = q * s;
            }
        R.se_robust.assign(static_cast<size_t>(k), 0.0);
        for (int c = 0; c < k; ++c)
            R.se_robust[static_cast<size_t>(c)] =
                std::sqrt(std::max(0.0, Vr[static_cast<size_t>(c) * k + c]));
    }
    return R;
}

// Local helpers: digamma and trigamma. C++17 std::tgamma is available; lgamma_r
// not portable. For ψ and ψ_1 we use simple but accurate series + recurrence.
// (Cephes-style approximations.)
double digamma(double x) {
    if (x < 6.0) {
        // shift up to x >= 6 via ψ(x) = ψ(x+1) - 1/x.
        double sum = 0;
        while (x < 6.0) { sum -= 1.0 / x; x += 1.0; }
        // asymptotic for ψ(x) = log(x) - 1/(2x) - Σ B_2k / (2k x^{2k})
        double inv = 1.0 / x;
        double inv2 = inv * inv;
        return sum + std::log(x) - 0.5 * inv
             - inv2 * (1.0/12.0 - inv2*(1.0/120.0 - inv2*(1.0/252.0)));
    }
    double inv = 1.0 / x;
    double inv2 = inv * inv;
    return std::log(x) - 0.5 * inv
         - inv2 * (1.0/12.0 - inv2*(1.0/120.0 - inv2*(1.0/252.0)));
}

double trigamma(double x) {
    if (x < 6.0) {
        double sum = 0;
        while (x < 6.0) { sum += 1.0 / (x * x); x += 1.0; }
        double inv = 1.0 / x;
        double inv2 = inv * inv;
        return sum + inv + 0.5 * inv2
             + inv2 * inv * (1.0/6.0 - inv2*(1.0/30.0 - inv2*(1.0/42.0)));
    }
    double inv = 1.0 / x;
    double inv2 = inv * inv;
    return inv + 0.5 * inv2
         + inv2 * inv * (1.0/6.0 - inv2*(1.0/30.0 - inv2*(1.0/42.0)));
}

}  // namespace cecon
