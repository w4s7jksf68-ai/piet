// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Mixed estimation.
#include "piet/handlers.h"
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
#include "lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation_internal.h"

#include "estimation_families_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

ojson runMixed(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // splitWs leaves `||` and `id:` as ordinary tokens.
  size_t bar = parsed.varlist.size();
  for (size_t i = 0; i < parsed.varlist.size(); ++i)
    if (parsed.varlist[i] == "||") { bar = i; break; }
  if (bar == parsed.varlist.size())
    return engineErrorBlockM4(
        "syntax_error: mixed requires a random-effects part, e.g. `mixed y x1 x2 || id:`",
        "syntax_error");
  if (bar < 2)
    return engineErrorBlockM4(
        "syntax_error: mixed requires a dependent variable and at least one regressor before `||`",
        "syntax_error");
  std::vector<std::string> after(parsed.varlist.begin() + static_cast<long>(bar) + 1,
                                 parsed.varlist.end());
  if (after.empty())
    return engineErrorBlockM4("syntax_error: mixed requires a grouping variable after `||`",
                              "syntax_error");
  for (const auto& t : after)
    if (t == "||")
      return engineErrorBlockM4(
          "`mixed` with more than one random-effects level is a valid Stata model that this "
          "version of PIET does not implement (only a single random intercept is available).",
          "unsupported_command");
  std::string grp = after[0];
  bool sawColon = false;
  if (!grp.empty() && grp.back() == ':') { grp.pop_back(); sawColon = true; }
  if (!sawColon)
    return engineErrorBlockM4("syntax_error: mixed's grouping variable must be followed by `:`",
                              "syntax_error");
  if (after.size() > 1)
    return engineErrorBlockM4(
        "`mixed` with random slopes after `|| " + grp + ":` is a valid Stata model that this "
        "version of PIET does not implement (only a random intercept is available).",
        "unsupported_command");

  eval::ParsedCommand fixedPart = parsed;
  fixedPart.varlist.assign(parsed.varlist.begin(), parsed.varlist.begin() + static_cast<long>(bar));
  P7MleArgv b = buildP7MleArgv("mixed", fixedPart, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  for (const auto& [k, v] : parsed.opts)
    if (k == "reml" && v.isBool && v.boolValue)
      return engineErrorBlockM4(
          "Option `reml` is a valid Stata option that this version of PIET does not implement for "
          "`mixed` (the estimator fits by maximum likelihood).",
          "unsupported_option");
  b.argv.push_back("--group");
  b.argv.push_back(grp);
  pushLevelIfGivenM4(parsed, b.argv);
  ojson raw;
  ojson block = runP7Mle("mixed", b.argv, session, &raw);
  // The random-effects stanza (var(_cons), var(Residual)) is a table Stata always
  // prints below the coefficients; like the ordered models' cutpoints it is not a
  // slope, so the mapper does not carry it and it is appended from the raw
  // payload. z/p stay null — Stata leaves those columns blank for variances.
  if (raw.contains("random_effects") && raw["random_effects"].is_array()) {
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    for (const auto& re : raw["random_effects"]) {
      ojson row;
      row["name"] = re.value("name", "");
      row["b"] = re.contains("b") ? re["b"] : ojson(nullptr);
      row["se"] = re.contains("se") ? re["se"] : ojson(nullptr);
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = re.contains("ci_lo") ? re["ci_lo"] : ojson(nullptr);
      row["ci_hi"] = re.contains("ci_hi") ? re["ci_hi"] : ojson(nullptr);
      block["coeftable"].push_back(row);
    }
  }
  return block;
}

// ===========================================================================
// P7 — survey design: `svyset` and the `svy:` prefix
//
// The design is SESSION state (as it is in Stata), so the engine stays
// stateless and each `svy:` command is rewritten into the ordinary estimation
// call plus --svy --psu --strata and the design's pweight.
//
// The inner-command whitelist fails CLOSED. An unrecognised inner command must
// reject, never fall through to a plain unweighted dispatch — that would return
// a confident number computed under no survey design at all, which is precisely
// the silent-wrong shape this codebase refuses to ship.


}  // namespace piet::handlers
