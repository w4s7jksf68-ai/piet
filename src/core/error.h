// cecon — engine-level error carrying a stable, machine-readable code so a
// caller (LLM, pipeline script, binding) can branch on WHY a run failed without
// scraping a human message. Kept JSON-free: the engine never depends on the JSON
// layer; the CLI wraps an EngineError into {"error":{code,message,detail}}.
//
// Stable code vocabulary (do not rename — these are part of the public contract):
//   "missing_column"   a referenced column is absent from the dataset
//   "factor_misuse"    a factor column was passed where a numeric (f64) is required
//   "collinear"        a normal-equation matrix was not positive-definite
//   "under_identified" fewer instruments than endogenous regressors
//   "no_obs"           too few observations / non-positive residual dof
//   "no_convergence"   an iterative estimator failed to converge
//   "common_sample"    multi-outcome batch outcomes are not jointly observed
//   "bad_input"        an argument/option value is invalid for this estimator
//   "data_io"          the dataset file could not be read / is malformed
#pragma once
#include <stdexcept>
#include <string>
#include <utility>

namespace cecon {

struct EngineError : std::runtime_error {
    std::string code;
    std::string detail;
    EngineError(std::string code_, const std::string& msg, std::string detail_ = "")
        : std::runtime_error(msg), code(std::move(code_)), detail(std::move(detail_)) {}
};

// Exit-code taxonomy (also part of the contract):
//   0 ok, 1 internal/unexpected, 2 usage/argument, 3 data error, 4 numerical error
inline int exit_code_for(const std::string& code) {
    if (code == "missing_column" || code == "factor_misuse" || code == "common_sample" ||
        code == "data_io" || code == "bad_input")
        return 3;
    if (code == "collinear" || code == "under_identified" || code == "no_obs" ||
        code == "no_convergence")
        return 4;
    return 1;
}

}  // namespace cecon
