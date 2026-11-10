// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Survey commands.
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

ojson runSvyset(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty() && !parsed.weightVar.has_value()) {
    // Bare `svyset` re-displays the current design (Stata's own behaviour).
    if (!session.svyDesign.has_value())
      return engineErrorBlockM4("svyset: no survey design is currently set.", "unsupported_option");
  }
  for (const auto& [k, v] : parsed.opts) {
    if (k == "clear" && v.isBool && v.boolValue) {
      session.svyDesign.reset();
      ojson b;
      b["type"] = "svyset";
      b["cleared"] = true;
      return b;
    }
  }
  if (!parsed.varlist.empty() || parsed.weightVar.has_value()) {
    if (parsed.varlist.size() != 1)
      return engineErrorBlockM4("syntax_error: svyset takes exactly one sampling-unit variable",
                                "syntax_error");
    if (!parsed.weightVar.has_value())
      return engineErrorBlockM4(
          "svyset: this version of PIET requires a probability weight, e.g. "
          "`svyset PSU [pweight=W], strata(S)`.",
          "unsupported_option");
    std::string wt = parsed.weightType.value_or("pweight");
    if (wt != "pweight" && wt != "pw")
      return engineErrorBlockM4(
          "svyset: only [pweight=] is supported for a survey design (got [" + wt + "=]).",
          "unsupported_option");
    std::string strata;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "strata" && !v.isBool) strata = trimM4(v.strValue);
      else if ((k == "fpc" || k == "vce" || k == "singleunit" || k == "poststrata") && !v.isBool)
        return engineErrorBlockM4(
            "Option `" + k + "()` is a valid Stata option that this version of PIET does not "
            "implement for `svyset`.",
            "unsupported_option");
    }
    // No strata() is ONE stratum, as in Stata (w2t_boottest_family.log:2455 H1:
    // `svyset id [pweight=w]` prints "Strata 1: <one>" and `svy: regress` fits
    // with "Number of strata = 1"). d.strata stays empty; runSvyPrefix then
    // sends no --strata and the engine's feols uses the constant stratum.
    SessionState::SvySettings d;
    d.psu = parsed.varlist[0];
    d.pweight = *parsed.weightVar;
    d.strata = strata;
    session.svyDesign = d;
  }
  const auto& d = *session.svyDesign;
  ojson b;
  b["type"] = "svyset";
  b["pweight"] = d.pweight;
  b["vce"] = "linearized";
  b["strata1"] = d.strata.empty() ? std::string("<one>") : d.strata;
  b["su1"] = d.psu;
  b["fpc1"] = nullptr;
  return b;
}

// `svy: CMD ...` — rewrite into the ordinary engine call plus the design flags.
ojson runSvyPrefix(const std::string& innerRaw, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (!session.svyDesign.has_value())
    return engineErrorBlockM4(
        "svy: the data have not been `svyset`; run `svyset PSU [pweight=W], strata(S)` first.",
        "unsupported_option");
  const auto& d = *session.svyDesign;

  eval::ParsedCommand p;
  try {
    p = eval::tokenize(innerRaw);
  } catch (const std::exception&) {
    return engineErrorBlockM4("syntax_error: could not parse the command after `svy:`",
                              "syntax_error");
  }
  const std::string inner = lowerM4(trimM4(p.cmd));
  if (p.weightVar.has_value())
    return engineErrorBlockM4(
        "svy: weights may not be given on the command line — the design's [pweight=] applies.",
        "unsupported_option");

  std::vector<std::string> argv;
  if (inner == "regress" || inner == "reg" || inner == "r" || inner == "feols") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: regress requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"feols", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else if (inner == "mean") {
    if (p.varlist.empty())
      return engineErrorBlockM4("syntax_error: svy: mean requires at least one variable",
                                "syntax_error");
    argv = {"mean", "--vars", joinCommaM4(p.varlist)};
  } else if (inner == "logit" || inner == "logistic") {
    if (p.varlist.size() < 2)
      return engineErrorBlockM4("syntax_error: svy: logit requires a dependent variable and at "
                                "least one regressor", "syntax_error");
    argv = {"logit", "--y", p.varlist[0], "--x",
            joinCommaM4(std::vector<std::string>(p.varlist.begin() + 1, p.varlist.end()))};
  } else {
    return engineErrorBlockM4(
        "`svy: " + p.cmd + "` is a valid Stata construction that this version of PIET does not "
        "implement (the survey prefix covers regress, mean and logit).",
        "unsupported_command");
  }
  if (p.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*p.ifExpr);
  }
  argv.push_back("--weight");
  argv.push_back(d.pweight);
  argv.push_back("--wtype");
  argv.push_back("pw");
  argv.push_back("--svy");
  argv.push_back("--psu");
  argv.push_back(d.psu);
  // A design without strata() is one stratum for every svy verb the engine
  // serves (BUG-1424: its mean and logit paths take it, as feols did).
  if (!d.strata.empty()) {
    argv.push_back("--strata");
    argv.push_back(d.strata);
  }
  // e(cmd) after `svy: regress` is `regress` (w2t_boottest_family.log:2497
  // H1_svy ecmd=regress); the engine's verb stays the cobraCmd every replay
  // keys on. mean and logit keep publishing the engine's word as before.
  ojson raw;
  ojson block = runP7MleAsM4(argv[0], argv, session, &raw,
                             argv[0] == "feols" ? std::string("regress") : std::string());
  // FU-471: as in runLogitProbit, the logit reply's `df_m` stays on the cached
  // reply (e(df_m)) and off the block, which never carried it.
  if (argv[0] == "logit" && block.is_object()) block.erase("df_m");
  // The design's counts from the reply's svy block (BUG-1424: StataBE e(N_psu) 20,
  // e(N_strata) 1, e(df_r) 19 after `svy: mean` / `svy: logit` on a one-stratum design).
  if (block.value("type", std::string()) != "error" && raw.is_object() && raw.contains("svy") &&
      raw["svy"].is_object()) {
    const ojson& sv = raw["svy"];
    for (const auto& [key, name] : std::vector<std::pair<const char*, const char*>>{
             {"n_psu", "N_psu"}, {"n_strata", "N_strata"}, {"design_df", "df_r"},
             {"pop_size", "N_pop"}})
      if (sv.contains(key) && sv[key].is_number())
        putEScalarNL(session, name, sv[key].get<double>());
    // BUG-1513 (StataBE 19.5, one stratum and strata(s)): `svy: regress` e(df_m) 1, the
    // numerator dof of the design-adjusted F the reply sends; e(prefix) svy; e(title)
    // "Survey: " + the command's own title ("Linear regression", "Mean estimation",
    // "Logistic regression" after svy: logit and svy: logistic). e(vce)/e(vcetype)
    // linearized/Linearized come with the model (cacheLastModelM4, setLastModel).
    if (argv[0] == "feols" && sv.contains("f_df1") && sv["f_df1"].is_number())
      putEScalarNL(session, "df_m", sv["f_df1"].get<double>());
    putEStringNL(session, "prefix", "svy");
    putEStringNL(session, "title", argv[0] == "feols"  ? "Survey: Linear regression"
                                   : argv[0] == "mean" ? "Survey: Mean estimation"
                                                       : "Survey: Logistic regression");
  }
  return block;
}

// ===========================================================================
// P7 — survival: stset / stcox / streg
//
// `stset` is an ENGINE verb, not PIET state: it materializes `_t0/_t/_d/_st` as
// real columns, which is data computation, and every later st command then reads
// them as ordinary variables — the same arrangement Stata has. What PIET keeps is
// the DECLARATION (session.stContext), which is what lets stcox/streg reject
// honestly on un-stset data and lets `vce(robust)` cluster on the subject id.


}  // namespace piet::handlers
