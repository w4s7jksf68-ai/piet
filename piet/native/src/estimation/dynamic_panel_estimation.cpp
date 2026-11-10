// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Dynamic panel estimation.
#include "piet/handlers/estimation.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

ojson runXtabond(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-657: Stata's `_xt, trequired` refusals (the texts of _xt.ado's
  // time-required branch, both r(459)); was unsupported_option = r(198).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar and timevar; use xtset",
                                  std::nullopt, std::nullopt, std::nullopt, "r(459)"));
  if (!session.timevar.has_value() || session.timevar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify timevar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  if (parsed.varlist.size() < 2)
    return engineErrorBlockM4(
        "syntax_error: xtabond requires a dependent variable and at least one regressor",
        "syntax_error");
  std::vector<std::string> argv = {
      "xtabond", "--y", parsed.varlist[0], "--x",
      joinCommaM4(std::vector<std::string>(parsed.varlist.begin() + 1, parsed.varlist.end())),
      "--xtset", *session.panelvar + "," + *session.timevar};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  std::string lags = "1";
  for (const auto& [k, v] : parsed.opts) {
    if (k == "lags" && !v.isBool) lags = trimM4(v.strValue);
    else if (k == "twostep" && v.isBool && v.boolValue)
      return engineErrorBlockM4(
          "Option `twostep` is a valid Stata option that this version of PIET does not implement "
          "for `xtabond` (only the one-step estimator is available).",
          "unsupported_option");
  }
  argv.push_back("--lags");
  argv.push_back(lags);
  // BUG-383 #5: the engine now implements BOTH one-step VCEs, and Stata's bare
  // `xtabond` is the CONVENTIONAL one — the old code here hardcoded --vce
  // robust for every call, which kept the bare form red against the capture
  // even after the engine grew the right default. Forward what the user asked:
  // nothing -> engine default (conventional); vce(robust) -> robust.
  std::string vceReq;  // empty = let the engine default (conventional) stand
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !v.isBool) {
      std::string lv = lowerM4(trimM4(v.strValue));
      if (lv == "robust") {
        vceReq = "robust";
        continue;
      }
      if (lv != "robust")
        return engineErrorBlockM4(
            "Option `vce(" + lv + ")` is a valid Stata option that this version of PIET does not "
            "implement for `xtabond` (conventional [the default] and vce(robust) are available).",
            "unsupported_option");
    }
  }
  if (!vceReq.empty()) {
    argv.push_back("--vce");
    argv.push_back(vceReq);
  }
  return runP7Mle("xtabond", argv, session);
}

// ===========================================================================
// P7 — nonlinear: `nl (y = EXPR)` and `gmm (RESID), instruments(...) onestep`
//
// The parenthesised expression is passed to the engine VERBATIM. PIET does not
// parse it: the engine owns the one expression evaluator (promoted out of
// nlcom), and a second parse here is exactly the shadow-evaluator shape that
// makes two layers disagree quietly. Macro expansion has already happened
// upstream, which is correct — Stata expands macros inside nl expressions too.


}  // namespace piet::handlers
