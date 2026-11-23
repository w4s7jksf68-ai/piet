// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include "core/matrix.h"
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace cecon {
struct OaxacaResult {
    std::vector<std::string> names;
    std::vector<double> beta, V; // full covariance, column-major
    std::array<int64_t,2> n{};
    bool omitted = false;
};
// Linear pooled twofold decomposition. dm includes an intercept; group is 0/1
// on the same complete sample. Jann (2008), robust suest and stochastic means.
// A thin wrapper over oaxaca_decompose (the by variable is the 0/1 group).
OaxacaResult oaxaca_fit(const DesignMatrix& dm, const std::vector<int>& group, bool relax);

// ---- the general Blinder-Oaxaca decomposition (Jann 2008, oaxaca 4.1.1) ------
// Rows are the candidate sample (if/in, weight and by already applied); every
// model drops its own rows with a missing y or regressor.
struct OaxacaData {
    int64_t n = 0;
    std::vector<double> y;
    std::vector<std::string> vars;         // coefficient variables, Stata's order, no _cons
    std::vector<std::vector<double>> X;    // one column per var
    std::vector<char> regressor;           // 1 = in the models; 0 = a normalize() base only
    std::vector<double> byv;               // the by variable's own values (pooled model regressor)
    std::vector<int> group;                // 0 = group 1, 1 = group 2
    std::vector<double> w;                 // empty = unweighted
    std::string wtype;                     // "fw" "aw" "pw" "iw"
    std::vector<double> cl;                // cluster ids; empty = none
    // reference(name): the stored regression, refit on its own e(sample)
    std::vector<double> refY;
    std::vector<std::string> refNames;     // its regressors (the constant is added)
    std::vector<std::vector<double>> refX;
    std::vector<char> refMask;
};
struct OaxacaSpec {
    int model = 0;     // 0 linear, 1 logit, 2 probit
    int tf = 1;        // 1 threefold, 2 threefold(reverse), 0 reference coefficients b*
    int ref = 0;       // with tf==0: 1 pooled, 2 omega, 3 weight(), 4 reference()
    std::vector<double> weights;
    bool split = false, detail = true, se = true, suest = false, robust = false;
    std::array<bool, 2> robustG{{false, false}};  // model1()/model2(): vce(robust) for one group
    bool relax = false, clusterMeans = false, check = true;
    std::vector<std::pair<std::string, std::vector<std::string>>> vgroups;
    // normalize(): first = the constant ("_cons" or a variable), second = every dummy
    std::vector<std::pair<std::string, std::vector<std::string>>> normalize;
    std::vector<std::string> adjust, fixedVars;
    bool fixedAll = false;
    std::vector<std::pair<std::string, double>> x1, x2;
};
struct OaxacaOut {
    std::vector<std::string> names, names0;   // eq:name
    std::vector<double> b, V, b0, V0;         // V/V0 row-major (symmetric)
    double N = 0, N1 = 0, N2 = 0;
    int64_t nClust = 0;
    int kEq = 0;
    std::vector<char> esample;                // per input row
    std::vector<std::string> notes;           // the ado's "(...)" lines, in order
    std::string legend;                       // detail() groups as e(legend) spells them
    bool robustModels = false;                // every group model reported a robust V
    bool vWarning = false;                    // the posted V failed the post screen (zeroed)
};
OaxacaOut oaxaca_decompose(const OaxacaData& d, const OaxacaSpec& s);
}
