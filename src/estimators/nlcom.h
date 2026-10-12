// cecon — nlcom: nonlinear combinations of fitted coefficients with delta-method
// SE. Var(g) = G' V G, G = numeric central-difference Jacobian at beta-hat (matches
// Stata nlcom/testnl). Coefficients are referenced as _b[name] (and _b[_cons]).
//
// P7: the recursive-descent parser that used to live here was PROMOTED to
// src/core/expr_eval.h so `nl` and `gmm` share it instead of growing a second
// copy of the grammar. nlcom's semantics are unchanged — it compiles with
// allow_params/allow_vars OFF, which is exactly how the old grammar behaved:
//   * `{` was not a token, so it fell through to the number scan and raised
//     "nlcom: parse error"; with allow_params off it still does.
//   * a bare identifier raised "nlcom: bare identifier 'x' (reference
//     coefficients as _b[name])"; that is expr_eval's default hint text.
//   * every other message keeps its wording via Options::tag = "nlcom".
// The tree expr_eval builds has the same shape/associativity as the old direct
// evaluation, so the arithmetic is performed in the same order and the numbers
// are bit-identical (tests/unit/test_nlcom.cpp is the standing check).
//
// THE ONE MEASURED DIFFERENCE, recorded rather than glossed. The old evaluator
// resolved `_b[...]` DURING the parse, so an expression malformed in two ways at
// once — an unknown coefficient AND a syntax error — reported the coefficient
// first. Compiling to a tree finishes parsing before any lookup happens, so the
// syntax error now wins:
//     "_b[nope]*(1"      was "unknown coefficient 'nope'", now "missing ')'"
//     "_b[nope] junk"    was "unknown coefficient 'nope'", now "trailing input near 'junk'"
//     "_b[nope]+sin(1)"  was "unknown coefficient 'nope'", now "unknown function 'sin'"
// Every WELL-FORMED expression and every singly-malformed one is unchanged, text
// included (verified by replaying the CLI error surface against a pre-promotion
// binary). Reporting the syntax error first is also the more useful order.
#pragma once
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/expr_eval.h"

namespace cecon {
namespace nlc {

using cecon::expr::trim;

// nlcom's view of the shared evaluator: coefficients only.
inline const expr::Options& options() {
    static const expr::Options o = [] {
        expr::Options t;
        t.tag = "nlcom";
        t.allow_params = false;
        t.allow_vars = false;
        t.allow_coefs = true;
        return t;
    }();
    return o;
}

}  // namespace nlc

struct NlcomTerm {
    std::string name;
    double est = 0, se = 0;
};

// Evaluate each "label=expr" (or bare "expr") combination of the coefficients and
// its delta-method SE. `V` is the k*k coefficient covariance (symmetric).
inline std::vector<NlcomTerm> nlcom_apply(const std::vector<std::string>& names,
                                          const std::vector<double>& beta,
                                          const std::vector<double>& V,
                                          const std::vector<std::string>& exprs) {
    const int k = static_cast<int>(names.size());
    std::unordered_map<std::string, int> idx;
    for (int j = 0; j < k; ++j) idx[names[j]] = j;
    int icpt = -1;
    for (int j = 0; j < k; ++j) if (names[j] == "(intercept)") icpt = j;

    std::vector<double> b = beta;  // mutable copy perturbed for the Jacobian
    // Returns false (rather than throwing here) for an unknown name; the shared
    // evaluator then raises "nlcom: unknown coefficient '<nm>'" — the same text
    // the in-file lookup used to throw.
    const expr::Resolver lookup = [&](const std::string& nm, double& out) -> bool {
        auto it = idx.find(nm);
        if (it != idx.end()) { out = b[it->second]; return true; }
        if ((nm == "_cons" || nm == "cons") && icpt >= 0) { out = b[icpt]; return true; }
        return false;
    };

    std::vector<NlcomTerm> out;
    for (size_t e = 0; e < exprs.size(); ++e) {
        const std::string& spec = exprs[e];
        size_t eq = spec.find('=');
        std::string label = (eq != std::string::npos) ? nlc::trim(spec.substr(0, eq)) : ("_nl_" + std::to_string(e + 1));
        std::string rhs = (eq != std::string::npos) ? spec.substr(eq + 1) : spec;

        // Compiled once per combination instead of re-parsed per evaluation; the
        // 2k+1 evaluations below all walk the same tree.
        const expr::Program prog = expr::Program::compile(rhs, nlc::options());
        b = beta;
        double est = prog.eval(nullptr, lookup);
        std::vector<double> G(k, 0.0);
        for (int j = 0; j < k; ++j) {
            double h = 1e-6 * std::max(std::fabs(beta[j]), 1.0);
            b[j] = beta[j] + h; double fp = prog.eval(nullptr, lookup);
            b[j] = beta[j] - h; double fm = prog.eval(nullptr, lookup);
            b[j] = beta[j];
            G[j] = (fp - fm) / (2 * h);
        }
        double var = 0;
        for (int aa = 0; aa < k; ++aa) {
            double gv = 0;
            for (int bb = 0; bb < k; ++bb) gv += G[bb] * V[aa + bb * k];
            var += G[aa] * gv;
        }
        out.push_back({label, est, (var > 0) ? std::sqrt(var) : 0.0});
    }
    return out;
}

}  // namespace cecon
