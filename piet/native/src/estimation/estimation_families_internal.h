// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Private interfaces used across related command families.
#pragma once

#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "estimation_internal.h"
#include <set>

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace est_detail {
struct P7MleArgv {
  bool errored = false;
  ojson errorBlock;
  std::vector<std::string> argv;
};

std::optional<std::string> vceOptionShapeErrorM4(const eval::ParsedCommand& parsed);
P7MleArgv buildP7MleArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                         const std::vector<std::string>& vceAllowed);
// `in a/b` folded into the argv's --keepif as an observation-number condition
// (likelihood_support.cpp); an error block for a bound Stata refuses.
std::optional<ojson> pushInRangeKeepifM4(SessionState& session, const eval::ParsedCommand& parsed,
                                         std::vector<std::string>& argv, const std::string& stataCmd);
// The `if` an engine fit filters on, with `in a/b` folded in the same way
// (`(_n >= a & _n <= b) & (if)`); ifOut is parsed.ifExpr when there is no `in`.
std::optional<ojson> inRangeConditionM4(SessionState& session, const eval::ParsedCommand& parsed,
                                        std::optional<std::string>& ifOut, const std::string& stataCmd);
ojson runP7MleAsM4(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
                          SessionState& session, ojson* rawOut, const std::string& stataCmd);
// constraints() of the ml estimators (likelihood_support.cpp): a defined
// constraint parsed into terms `coef * [eq]name` and a right-hand side.
struct CnsTermM4 {
  double coef = 0.0;
  std::string eq;    // as typed inside [ ] or before `:` in _b[ ]; "" when none
  std::string name;  // the coefficient's name as typed
};
struct CnsDefM4 {
  int num = 0;
  std::vector<CnsTermM4> terms;
  double rhs = 0.0;
};
bool parseConstraintTextM4(const std::string& text, CnsDefM4& out, std::string& why);
// The constraints(numlist) option's defined constraints, in numlist order; false
// (with Stata's refusal in err) when the option is malformed or a constraint is
// not a linear equation. Empty when the option is absent.
bool readConstraintsM4(const eval::ParsedCommand& parsed, const SessionState& session,
                       std::vector<CnsDefM4>& out, ojson& err);
std::string cnsNumberTextM4(double v);
// `[eq]a - 2*[eq]b = .5`: Stata's display line of a constraint.
std::string cnsDisplayLineM4(const std::vector<std::pair<double, std::string>>& terms, double rhs);
}  // namespace est_detail
namespace fit_detail {
void putEScalarNL(SessionState& s, const std::string& n, double v);
void putEStringNL(SessionState& s, const std::string& n, const std::string& v);
void cacheLastModelM4(SessionState& session, const std::string& cobraCmd,
                       const std::vector<std::string>& argv, const ojson& block, const ojson& raw,
                       const std::string& esampleValues = std::string(),
                       const std::string& stataCmd = std::string());
// BUG-1482 (panel_estimation.cpp): the rank of a one-way clustered e_V, Stata's
// r(rankc); nullopt when e_V is absent or not square.
std::optional<double> clusteredVRankM4(const ojson& raw);
// One-way cluster with G - 1 < k: Stata's e(rank) (panel_estimation.cpp).
std::optional<double> fewClustersRankM4(const ojson& raw, double G, double k);
}  // namespace fit_detail
void applyBuiltinMultiwayReplyM4(ojson& result);
void postBuiltinMultiwayEM4(SessionState& session, const ojson& raw);
void postENameListsXtregFeM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed);
}  // namespace piet::handlers
