// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Nonlinear estimation.
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

ojson runNlGmm(const std::string& cobraCmd, const std::string& rawLine,
               const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // Take the expression straight off the raw line: the tokenizer splits on
  // whitespace, so rejoining varlist tokens would silently normalise the
  // spacing inside the user's own formula.
  std::string s = trimM4(rawLine);
  size_t lp = s.find('(');
  if (lp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires a parenthesised expression",
                              "syntax_error");
  int depth = 0;
  size_t rp = std::string::npos;
  for (size_t i = lp; i < s.size(); ++i) {
    if (s[i] == '(') ++depth;
    else if (s[i] == ')' && --depth == 0) { rp = i; break; }
  }
  if (rp == std::string::npos)
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " expression has no closing ')'",
                              "syntax_error");
  std::vector<std::string> argv = {cobraCmd, "--expr", s.substr(lp, rp - lp + 1)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  if (cobraCmd == "gmm") {
    std::string instruments;
    bool onestep = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "instruments" && !v.isBool) {
        std::istringstream iss(v.strValue);
        std::vector<std::string> toks;
        std::string t;
        while (iss >> t) toks.push_back(t);
        instruments = joinCommaM4(toks);
      }
      else if (k == "onestep" && v.isBool && v.boolValue) onestep = true;
      else if (k == "twostep" && v.isBool && v.boolValue)
        return engineErrorBlockM4(
            "Option `twostep` is a valid Stata option that this version of PIET does not implement "
            "for `gmm` (only the one-step estimator is available).",
            "unsupported_option");
    }
    if (instruments.empty())
      return engineErrorBlockM4("syntax_error: gmm requires instruments()", "syntax_error");
    argv.push_back("--instruments");
    argv.push_back(instruments);
    if (!onestep)
      return engineErrorBlockM4(
          "`gmm` without `onestep` (two-step GMM) is a valid Stata command that this version of "
          "PIET does not implement.",
          "unsupported_command");
    argv.push_back("--onestep");
  }
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "robust") && cobraCmd == "nl")
      return engineErrorBlockM4(
          "Option `vce()` is a valid Stata option that this version of PIET does not implement for "
          "`nl` (the small-sample factor is not fixed by any oracle available here).",
          "unsupported_option");
  }
  // [aw=]/[pw=] (BUG-1283): the engine's weighted NLS -- aweights normalized to N
  // with the model-based VCE, pweights the robust sandwich (StataBE 19.5 _se[/b1]
  // .067819537723433465 / .071775268345285762). Stata notes the weight sum first.
  const bool weighted = cobraCmd == "nl" && parsed.weightVar.has_value() &&
                        !parsed.weightVar->empty();
  if (weighted) {
    const std::string wt = lowerM4(parsed.weightType.value_or("aw"));
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    const bool pw = wt == "pw" || wt == "pweight";
    argv.push_back("--wtype");
    argv.push_back(pw ? "pw" : "aw");
    if (pw) {  // e(vce) robust, e(vcetype) Robust, as Stata posts under a pweight
      argv.push_back("--vce");
      argv.push_back("robust");
    }
  }
  pushLevelIfGivenM4(parsed, argv);
  ojson raw;
  ojson block = runP7Mle(cobraCmd, argv, session, &raw);
  if (!weighted || block.value("type", std::string()) == "error" || !raw.contains("weight") ||
      !raw["weight"].is_object() || !raw["weight"].contains("sum_w") ||
      !raw["weight"]["sum_w"].is_number())
    return block;
  char buf[64];
  std::snprintf(buf, sizeof buf, "(sum of wgt is %.16g)", raw["weight"]["sum_w"].get<double>());
  ojson note;
  note["type"] = "display";
  note["text"] = buf;
  return ojson::array({note, block});
}

// ===========================================================================
// P7 — multilevel: `mixed depvar indepvars || GROUP:, vce(robust)`
//
// Only the random-INTERCEPT form is implemented. A random-slope list after the
// colon, or a second `||` level, is a different model and rejects rather than
// being silently fitted as an intercept-only one.


}  // namespace piet::handlers
