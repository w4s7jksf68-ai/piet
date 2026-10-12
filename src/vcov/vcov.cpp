#include "vcov/vcov.h"
#include "core/error.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/linalg.h"

namespace cecon {

void hc_meat(const double* design, int64_t n, int k, const double* e, double* meat) {
    std::vector<double> W(static_cast<size_t>(n) * k);
    for (int j = 0; j < k; ++j) {
        const double* xj = design + static_cast<int64_t>(j) * n;
        double* wj = W.data() + static_cast<int64_t>(j) * n;
        for (int64_t i = 0; i < n; ++i) wj[i] = xj[i] * e[i];
    }
    la::xtx(W.data(), n, k, meat);
}

void cluster_scores(const double* design, int64_t n, int k, const double* e,
                    const Factor& cl, double* S) {
    const int G = cl.n_levels;
    std::fill(S, S + static_cast<size_t>(G) * k, 0.0);
    const int32_t* g = cl.codes.data();
    for (int j = 0; j < k; ++j) {
        const double* xj = design + static_cast<int64_t>(j) * n;
        double* Sj = S + static_cast<int64_t>(j) * G;
        for (int64_t i = 0; i < n; ++i) Sj[g[i]] += xj[i] * e[i];
    }
}

void cluster_meat(const double* design, int64_t n, int k, const double* e,
                  const Factor& cl, double* meat, int* n_clusters) {
    const int G = cl.n_levels;
    if (n_clusters) *n_clusters = G;
    std::vector<double> S(static_cast<size_t>(G) * k, 0.0);
    cluster_scores(design, n, k, e, cl, S.data());
    la::xtx(S.data(), G, k, meat);  // meat = S'S
}

void multiway_meat(const double* design, int64_t n, int k, const double* e,
                   const std::vector<const Factor*>& cls, double* meat) {
    const int d = static_cast<int>(cls.size());
    if (d < 1 || d > kMaxMultiwayClusters)
        throw EngineError("bad_input", "multiway meat needs 1.." +
                                           std::to_string(kMaxMultiwayClusters) +
                                           " cluster variables");
    const size_t kk = static_cast<size_t>(k) * k;
    std::fill(meat, meat + kk, 0.0);
    std::vector<double> m(kk);
    int Gdummy = 0;
    // Inclusion-exclusion (Cameron-Gelbach-Miller 2011, eq. 2.13): every non-empty
    // subset S of the dimensions contributes its intersection-cluster meat with sign
    // (-1)^(|S|+1). For d = 2 this is m1 + m2 - m12, the two-way form this replaced,
    // in the same order of additions.
    for (unsigned mask = 1; mask < (1u << d); ++mask) {
        int size = 0;
        Factor inter;
        const Factor* f = nullptr;
        for (int j = 0; j < d; ++j) {
            if (!(mask & (1u << j))) continue;
            if (!f) {
                f = cls[j];
            } else {
                inter = intersect_factors(*f, *cls[j]);
                f = &inter;
            }
            ++size;
        }
        cluster_meat(design, n, k, e, *f, m.data(), &Gdummy);
        const double sign = (size % 2 == 1) ? 1.0 : -1.0;
        for (size_t i = 0; i < kk; ++i) meat[i] += sign * m[i];
    }
}

bool fe_nested_in_cluster(const Factor& fe, const Factor& cl) {
    std::vector<int32_t> map(fe.n_levels, -1);
    const int64_t n = static_cast<int64_t>(fe.codes.size());
    for (int64_t i = 0; i < n; ++i) {
        int32_t f = fe.codes[i], c = cl.codes[i];
        if (map[f] == -1) map[f] = c;
        else if (map[f] != c) return false;
    }
    return true;
}

VcovResult compute_vcov(Vcov vt, const double* design, int64_t n, int k, const double* e,
                        const double* bread, int k_full, double sigma2,
                        const std::vector<Factor>& fe,
                        const std::vector<Factor>& cluster, int cluster_index, double n_eff,
                        bool ml_cluster_factor, bool keep_nested_fe_dof) {
    VcovResult out;
    out.V.assign(static_cast<size_t>(k) * k, 0.0);
    // See the header: n_eff <= 0 means "the observation count IS n" — every
    // unweighted call, and every aw/pw call, lands there.
    const double nd = (n_eff > 0.0) ? n_eff : static_cast<double>(n);

    if (vt == Vcov::IID) {
        for (size_t i = 0; i < out.V.size(); ++i) out.V[i] = sigma2 * bread[i];
        out.type = "iid";
        return out;
    }
    if (vt == Vcov::HC1) {
        std::vector<double> meat(static_cast<size_t>(k) * k);
        hc_meat(design, n, k, e, meat.data());
        double factor = nd / (nd - static_cast<double>(k_full));
        la::sandwich(bread, meat.data(), k, factor, out.V.data());
        out.type = "HC1";
        return out;
    }

    // Cluster (one-way) or Multiway (CGM, k-way over every cluster variable given)
    if (cluster.empty()) throw EngineError("bad_input", "no cluster variable provided");
    std::vector<const Factor*> cls;
    if (vt == Vcov::Cluster) {
        cls.push_back(&cluster.at(cluster_index));
    } else {
        if (cluster.size() < 2)
            throw EngineError("bad_input", "multiway vcov needs >= 2 cluster variables");
        // Every dimension takes part. Before k-way support only cluster[0..1] were read,
        // so a third name passed to --cluster was dropped without a word.
        if (cluster.size() > static_cast<size_t>(kMaxMultiwayClusters))
            throw EngineError("bad_input",
                              "multiway vcov supports at most " +
                                  std::to_string(kMaxMultiwayClusters) +
                                  " cluster variables, got " + std::to_string(cluster.size()));
        for (const Factor& c : cluster) cls.push_back(&c);
    }

    std::vector<double> meat(static_cast<size_t>(k) * k, 0.0);
    int Gdummy = 0;
    if (cls.size() == 1) {
        cluster_meat(design, n, k, e, *cls[0], meat.data(), &Gdummy);
    } else {
        multiway_meat(design, n, k, e, cls, meat.data());
    }

    int Gmin = cls[0]->n_levels;
    for (const Factor* c : cls) Gmin = std::min(Gmin, c->n_levels);
    // E5 (COUNCIL-20260710, User ruling 2026-07-10): cluster-robust SEs are undefined
    // with a single cluster — the G/(G-1) small-sample factor below divides by G-1=0,
    // yielding a non-finite (JSON-null) SE. Refuse explicitly instead of emitting a
    // degenerate result. NOTE: Stata itself runs G=1 clustering and reports a missing
    // SE (rc=0); this is a deliberate doctrine-owner override — an honest error beats
    // bug-for-bug fidelity here, and it matches wildboot.cpp's existing G<2 refusal.
    if (Gmin < 2)
        throw EngineError("bad_input",
                          "insufficient clusters: vce(cluster) requires at least 2 clusters, "
                          "but the cluster variable has " + std::to_string(Gmin));
    int k_adj = k_full;
    // BUG-568: the nested-FE exemption is reghdfe's / xtreg,fe's convention; areg
    // (keep_nested_fe_dof) charges every absorbed parameter. See the header.
    if (!keep_nested_fe_dof) {
        for (const Factor& f : fe) {
            bool nested = false;
            for (const Factor* c : cls)
                if (fe_nested_in_cluster(f, *c)) { nested = true; break; }
            if (nested) { k_adj -= (f.n_levels - 1); out.nested_fe.push_back(f.name); }
        }
    }
    out.n_clusters = Gmin;
    out.cluster_dof_k = k_adj;
    // BUG-175: likelihood commands drop the regression (N-1)/(N-K) term — Stata's
    // ML cluster multiplier is the bare G/(G-1). Measured against the P2 answerbook
    // (`poisson ycount x1 x2 i.region, vce(cluster id)`, N=4671 G=200 K=7): the pure
    // factor lands within 4.6e-06 relative on all seven SEs, while carrying the
    // regression term misses by 6.5e-04 — exactly sqrt((N-1)/(N-K)). See BUG-175 for
    // the full candidate sweep. k_adj still drives the REPORTED cluster_dof_k and the
    // nested-FE accounting above either way; only the multiplier changes.
    //
    // Multiway: ONE factor, built from the smallest single-dimension G, multiplies the
    // whole inclusion-exclusion meat — not a G_S/(G_S-1) per subset. Measured on
    // w2b_estim.log (N=60, absorb(id) nested in cluster id, K=3): Gmin/(Gmin-1)·(N-1)/(N-K)
    // reproduces reghdfe_cluster3 se[x1] .0575950937310231 (G = 10,6,4; Gmin 4) and
    // reghdfe_cluster2_ctl .060463723825293 (Gmin 6) to ~1e-15 relative on the oracle's
    // float-stored data, while the per-subset variant misses by 5.2% (3-way) and 1.7%
    // (2-way). e(df_r) = Gmin-1 (3 and 5 there) follows from n_clusters = Gmin.
    double factor = static_cast<double>(Gmin) / (Gmin - 1);
    if (!ml_cluster_factor) factor *= (nd - 1.0) / (nd - static_cast<double>(k_adj));
    la::sandwich(bread, meat.data(), k, factor, out.V.data());
    if (cls.size() > 1) out.npd_fix = (la::psd_clip(out.V.data(), k) > 0);
    if (cls.size() == 1) {
        out.type = "cluster(" + cls[0]->name + ")";
    } else {
        // Two dimensions keep the historical `twoway(a,b)` spelling (byte-identical);
        // three or more read `multiway(a,b,c)`.
        out.type = cls.size() == 2 ? "twoway(" : "multiway(";
        for (size_t d = 0; d < cls.size(); ++d) out.type += (d ? "," : "") + cls[d]->name;
        out.type += ")";
    }
    return out;
}

}  // namespace cecon
