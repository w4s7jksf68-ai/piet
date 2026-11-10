// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private declarations shared by the egen family: datamgmt/egen.cpp (the egen
// command), datamgmt/egen_computed_functions.cpp (the egen functions PIET computes
// itself) and datamgmt/generate_by_group.cpp ([bysort:] generate/replace via
// --bygen). Split from egen.cpp without changing behavior.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

#include <map>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/strutil.h"
#include "piet/scratch_seq.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace egen_detail {

// A5-04: forwards to the shared strutil copy (piet/strutil.h) — kept as a
// thin wrapper (not removed) so this file's many `trim(...)`/`lower(...)`
// call sites didn't need touching. Pure dedup, no behavior change.
inline std::string trim(const std::string& s) { return strutil::trim(s); }

inline ojson engineErrorBlock(const std::string& message, const std::string& code = "engine_error") {
  return eval::engineError(message, code);
}

// BUG-038(a): forwards to the shared mapping point (piet::cobraErrorMessage,
// cobra_runner.h) — kept as a thin wrapper (not removed) so this file's
// call sites didn't need touching. Pure dedup, no behavior change here.
inline std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// Defined in egen.cpp.
CobraError errorFromRaw(const CobraRunResult& r);

inline std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);  // BUG-389: one body, registered at mint
}

// BUG-1001: each egen function's OWN option vocabulary: the option names (help
// egen) and the shortest abbreviation StataBE 19.5 accepts for each, measured
// (StataBE oracle arms E_*): `mode(v), min`
// runs and `mi` is r(198), so minmode is {"minmode", 3}; `total(v), m` runs, so
// total's missing is {"missing", 1}, while `mode(v), mis` is r(198) and `miss` runs,
// so mode's is {"missing", 4}. Where the help file's underlining differs from what
// Stata accepts (num:mode, lab:el; `n()` and `l` run), the measurement wins.
// `valued` options take a (...) argument. `by()` is not listed: every function
// takes it (the row functions refuse it, r(190)). `unimplemented` holds options
// Stata accepts and this build does not compute; they are refused by name, never
// accepted and dropped. A function absent from this table keeps the pre-BUG-1001
// net in runEgen.
struct EgenOptSpec {
  const char* full;
  size_t minLen;
  bool valued;
};
struct EgenFnVocab {
  std::vector<EgenOptSpec> known;
  std::vector<EgenOptSpec> unimplemented;
};

// Defined in egen.cpp.
const std::map<std::string, EgenFnVocab>& egenFnVocab();
const EgenOptSpec* matchEgenOpt(const std::string& key, const std::vector<EgenOptSpec>& specs);
ojson egenRcError(const std::string& message, int rc, const std::string& code = "engine_error");
std::optional<ojson> egenOptionNet(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                   const EgenFnVocab& vocab,
                                   std::vector<std::pair<std::string, eval::OptValue>>* canonical,
                                   std::optional<ojson>* unimplemented,
                                   bool missingInFull = false);

// Defined in generate_by_group.cpp.
std::string makeBygenExpr(const std::string& expr, const std::vector<std::string>& byVars,
                          const std::optional<std::vector<std::string>>& sortVars);

// Defined in egen_computed_functions.cpp.
std::optional<ojson> runEgenAdoFunction(const std::string& fn, const std::string& newvar,
                                        const std::string& argText,
                                        const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                        const std::optional<std::string>& ifExpr,
                                        const std::optional<std::string>& inRange,
                                        const std::string& storeType, SessionState& session);

}  // namespace egen_detail
}  // namespace piet::handlers
