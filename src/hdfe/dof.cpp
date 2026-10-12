#include "hdfe/dof.h"

#include <numeric>
#include <unordered_set>

namespace cecon {

namespace {

struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(int a, int b) { parent[find(a)] = find(b); }
};

// Connected components of the bipartite graph linking FE1 levels to FE2 levels.
int connected_components(const Factor& f1, const Factor& f2) {
    const int K1 = f1.n_levels, K2 = f2.n_levels;
    UnionFind uf(K1 + K2);
    const int64_t n = static_cast<int64_t>(f1.codes.size());
    for (int64_t i = 0; i < n; ++i) uf.unite(f1.codes[i], K1 + f2.codes[i]);
    std::unordered_set<int> roots;
    roots.reserve(K1 + K2);
    for (int v = 0; v < K1 + K2; ++v) roots.insert(uf.find(v));
    return static_cast<int>(roots.size());
}

}  // namespace

DofInfo compute_dof(int64_t n, int k_vars, const std::vector<Factor>& fe) {
    DofInfo d;
    d.n = n;
    d.k_vars = k_vars;
    for (const Factor& f : fe) d.fe_levels.push_back(f.n_levels);

    int sum_levels = 0;
    for (int L : d.fe_levels) sum_levels += L;

    if (fe.empty()) {
        d.k_fe = 0;
    } else if (fe.size() == 1) {
        d.k_fe = fe[0].n_levels;  // absorbs intercept too
    } else if (fe.size() == 2) {
        int c = connected_components(fe[0], fe[1]);
        d.k_fe = fe[0].n_levels + fe[1].n_levels - c;  // exact rank of 2-way dummies
        d.fe_exact = true;
    } else {
        // 3+ FE: conservative reghdfe-style estimate, flagged inexact.
        d.k_fe = fe[0].n_levels;
        for (size_t g = 1; g < fe.size(); ++g) d.k_fe += fe[g].n_levels - 1;
        d.fe_exact = false;
    }

    d.fe_redundant = sum_levels - d.k_fe;
    d.dof_resid = n - k_vars - d.k_fe;
    return d;
}

}  // namespace cecon
