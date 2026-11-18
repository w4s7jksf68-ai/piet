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
  // The instrument-set options change the GMM-type and standard instruments (and
  // noconstant the regressors), which the engine's fixed one-step instrument matrix
  // does not take: fitting without them posted the default model's numbers under
  // the option (bank 36f2518c `xtabond frate unemp, endogenous(beertax)` e(sig2)
  // .0232823 against the default fit's .0202885; c9003abe noconstant, 9c25f989
  // maxlags(), 5f740234 maxldep(), cea8c63d inst(), b07c2ad3 diffvars(), f2f364a6
  // pre()). Refused by name until the engine builds those instrument sets.
  //
  // Every option is read by xtabond's own names and minimum abbreviations (LAgs,
  // Robust, TWOstep, noCONStant, ARtests, MAXLDep, DIFFvars, INST, MAXLAGs, SMall,
  // Level, VCE, vsquish, COEFLegend, selegend, NOLSTRETCH, ENDogenous, pre), so
  // `endog(beertax)` is endogenous() and refused with it; a spelling matching none
  // is "option ... not allowed with xtabond" r(198), as the parser's leftover check
  // reports it (`en(beertax)`: below ENDogenous's three letters).
  static const std::pair<const char*, size_t> kXtabondOpts[] = {
      {"lags", 2},     {"robust", 1},     {"twostep", 3},  {"noconstant", 6}, {"artests", 2},
      {"maxldep", 5},  {"diffvars", 4},   {"inst", 4},     {"maxlags", 6},    {"small", 2},
      {"level", 1},    {"vce", 3},        {"vsquish", 7},  {"coeflegend", 5}, {"selegend", 8},
      {"nolstretch", 10}, {"endogenous", 3}, {"pre", 3},   {"xtabond", 7},    {"xtdpdsys", 8}};
  std::vector<std::pair<std::string, eval::OptValue>> opts;
  for (const auto& [kRaw, v] : parsed.opts) {
    std::string k;
    for (const auto& [name, minLen] : kXtabondOpts) {
      const std::string full = name;
      if (kRaw.size() >= minLen && kRaw.size() <= full.size() &&
          full.compare(0, kRaw.size(), kRaw) == 0) {
        k = full;
        break;
      }
    }
    if (kRaw == "vcov") k = "vce";
    if (k.empty())
      return ojson(eval::errorBlock(
          "engine_error",
          "option " + kRaw + (v.isBool ? std::string() : "(" + v.strValue + ")") +
              " not allowed with xtabond",
          std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    opts.emplace_back(k, v);
  }
  // xtdpd's check behind xtabond: diffvars() with the constant in the model is
  // r(498) (vs-stata bank ec592263 `xtabond frate beertax, diffvars(unemp)`).
  {
    bool diffvars = false, nocons = false;
    for (const auto& [k, v] : opts) {
      if (k == "diffvars" && !v.isBool) diffvars = true;
      if (k == "noconstant" && v.isBool && v.boolValue) nocons = true;
    }
    if (diffvars && !nocons)
      return ojson(eval::errorBlock(
          "engine_error",
          "option diffvars() specified incorrectly\n    diffvars() may not be specified if the "
          "model has a constant or you specify\n    instruments for level equations.",
          std::nullopt, std::nullopt, std::nullopt, "r(498)"));
  }
  for (const auto& [k, v] : opts) {
    static const std::set<std::string> kInstrumentOpts = {
        "endogenous", "pre", "inst", "diffvars", "maxlags", "maxldep", "noconstant"};
    if (kInstrumentOpts.count(k))
      return engineErrorBlockM4("Option `" + k +
                                    "` is a valid Stata option that this version of PIET does not "
                                    "implement for `xtabond` (the engine builds only the default "
                                    "instrument set).",
                                "unsupported_option");
  }
  for (const auto& [k, v] : opts) {
    // xtabond's Level(cilevel) is the one-letter option (LAgs() takes two): `l(2)`
    // is r(198) "level() must be between 10 and 99.99 inclusive" (vs-stata bank
    // 8e734ba5b2e26756; `la(2)` .. `lags(2)` run).
    if (k == "level" && !v.isBool) {
      const std::string a = trimM4(v.strValue);
      char* end = nullptr;
      const double lv = std::strtod(a.c_str(), &end);
      if (a.empty() || end == nullptr || *end != '\0')
        return ojson(eval::errorBlock("syntax_error", a + " found where number expected",
                                      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
      if (lv < 10.0 || lv > 99.99)
        return ojson(eval::errorBlock("syntax_error",
                                      "level() must be between 10 and 99.99 inclusive",
                                      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
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
  for (const auto& [k, v] : opts) {
    // the bare `robust` flag is vce(robust) (the parser's old-style VCE spelling)
    if (k == "robust" && v.isBool && v.boolValue) {
      vceReq = "robust";
      continue;
    }
    if (k == "vce" && !v.isBool) {
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
