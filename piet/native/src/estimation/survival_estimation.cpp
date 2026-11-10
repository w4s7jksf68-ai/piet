// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Survival estimation.
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

ojson runStset(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() != 1)
    return engineErrorBlockM4("syntax_error: stset requires exactly one time variable",
                              "syntax_error");
  std::string failvar, idvar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "failure" && !v.isBool) {
      // Stata allows `failure(fail)`, `failure(fail==1)` and `failure(fail!=0)`;
      // only the bare-variable form is implemented, and the comparison forms
      // reject rather than being silently read as the bare one (they select a
      // DIFFERENT failure event).
      std::string fv = trimM4(v.strValue);
      if (fv.find_first_of("=<>!") != std::string::npos)
        return engineErrorBlockM4(
            "Option `failure(" + fv + ")` is a valid Stata option that this version of PIET does "
            "not implement for `stset` — only the bare `failure(varname)` form is available.",
            "unsupported_option");
      failvar = fv;
    } else if (k == "id" && !v.isBool) {
      idvar = trimM4(v.strValue);
    }
  }
  if (failvar.empty())
    return engineErrorBlockM4("syntax_error: stset requires failure(varname)", "syntax_error");

  std::vector<std::string> argv = {"stset", "--data", *session.currentHbin(), "--time",
                                   parsed.varlist[0], "--failure", failvar};
  if (!idvar.empty()) {
    argv.push_back("--id");
    argv.push_back(idvar);
  }
  std::string outPath = nextPathM4(session.workdir(), "stset");
  argv.push_back("--out");
  argv.push_back(outPath);

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  session.load(outPath);
  SessionState::StContext ctx;
  ctx.timevar = parsed.varlist[0];
  ctx.failvar = failvar;
  ctx.idvar = idvar;
  session.stContext = ctx;

  ojson block = raw;
  block["type"] = "stset";
  return block;
}

namespace fit_detail {

// Shared entry checks + argv prefix for stcox/streg.
P7MleArgv buildStArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  P7MleArgv r;
  if (!session.stContext.has_value()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        cobraCmd + ": the data have not been `stset`; run `stset TIMEVAR, failure(FAILVAR)` first.",
        "unsupported_option");
    return r;
  }
  if (parsed.varlist.empty()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4("syntax_error: " + cobraCmd + " requires at least one regressor",
                                      "syntax_error");
    return r;
  }
  r.argv = {cobraCmd, "--x", joinCommaM4(parsed.varlist)};
  if (parsed.ifExpr.has_value()) {
    r.argv.push_back("--keepif");
    r.argv.push_back(*parsed.ifExpr);
  }
  // vce. Stata's own `vce(robust)` on multi-record st data with an id() is
  // subject-clustered, so PIET supplies the id here rather than letting the
  // engine compute a record-level sandwich that would miss the oracle on the
  // second digit.
  if (auto shapeErr = vceOptionShapeErrorM4(parsed)) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(*shapeErr, "unsupported_option");
    return r;
  }
  std::optional<std::string> vceVal, clusterOpt;
  bool robustFlag = false;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "vce" || k == "vcov") && !vceVal.has_value())
      vceVal = v.isBool ? std::string() : v.strValue;
    else if (k == "robust" && v.isBool && v.boolValue)
      robustFlag = true;
    else if (k == "cluster" && !v.isBool)
      clusterOpt = v.strValue;
  }
  std::string want, cvar = clusterOpt.value_or("");
  if (vceVal.has_value() && !vceVal->empty()) {
    std::string lv = lowerM4(*vceVal);
    if (lv.rfind("cluster", 0) == 0) {
      want = "cluster";
      if (lv.size() > 7) {
        std::string rest = trimM4(lv.substr(7));
        if (!rest.empty()) cvar = rest;
      }
    } else {
      want = lv;
    }
  } else if (!cvar.empty()) {
    // BUG-537: cluster() beside `robust` wins (w2_pweight.log R5), as in
    // vcovFromOpts / buildP7MleArgv.
    want = "cluster";
  } else if (robustFlag) {
    want = "robust";
  }
  if (want.empty()) return r;
  if (want != "robust" && want != "cluster" && want != "iid") {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
        "implement for `" + cobraCmd + "`.", "unsupported_option");
    return r;
  }
  if (want == "cluster" && cvar.empty()) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4("Cluster variable required: vce(cluster varname)",
                                      "unsupported_option");
    return r;
  }
  r.argv.push_back("--vce");
  r.argv.push_back(want);
  if (want == "cluster") {
    r.argv.push_back("--cluster");
    r.argv.push_back(cvar);
  } else if (want == "robust" && !session.stContext->idvar.empty()) {
    r.argv.push_back("--cluster");
    r.argv.push_back(session.stContext->idvar);
  }
  return r;
}

}  // namespace

namespace fit_detail {

// Stata's stcox/streg print HAZARD RATIOS by default, with the z, p and CI still
// taken from the RAW coefficient scale. The engine keeps `b`/`se` in the raw
// metric like every other estimator and carries `hr`/`se_hr`/`hr_ci_lo`/
// `hr_ci_hi` alongside — which is right for the contract, but the shared
// tidy_coeftable only copies the seven standard fields, so the rendered table
// showed the raw coefficient where Stata shows the ratio. This re-points the
// table at the hazard-ratio columns (from the RAW payload, since the mapper does
// not carry them through) and keeps the raw coefficient available under `b_raw`.
void applyHazardRatioTableM4(ojson& block, const ojson& raw) {
  if (!raw.contains("coefs") || !raw["coefs"].is_array()) return;
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  std::map<std::string, const ojson*> byName;
  for (const auto& c : raw["coefs"])
    if (c.contains("name") && c.contains("hr")) byName[c["name"].get<std::string>()] = &c;
  for (auto& row : block["coeftable"]) {
    auto it = byName.find(row.value("name", std::string()));
    if (it == byName.end()) continue;
    const ojson& c = *it->second;
    row["b_raw"] = row.contains("b") ? row["b"] : ojson(nullptr);
    row["se_raw"] = row.contains("se") ? row["se"] : ojson(nullptr);
    row["b"] = c["hr"];
    if (c.contains("se_hr")) row["se"] = c["se_hr"];
    if (c.contains("hr_ci_lo")) row["ci_lo"] = c["hr_ci_lo"];
    if (c.contains("hr_ci_hi")) row["ci_hi"] = c["hr_ci_hi"];
  }
  block["coef_metric"] = "hazard_ratio";
}

}  // namespace

ojson runStcox(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildStArgv("stcox", parsed, session);
  if (b.errored) return b.errorBlock;
  ojson raw;
  ojson block = runP7Mle("stcox", b.argv, session, &raw);
  applyHazardRatioTableM4(block, raw);
  return block;
}

ojson runStreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildStArgv("streg", parsed, session);
  if (b.errored) return b.errorBlock;
  std::string dist;
  for (const auto& [k, v] : parsed.opts)
    if ((k == "distribution" || k == "dist" || k == "d") && !v.isBool) dist = lowerM4(trimM4(v.strValue));
  if (dist.empty())
    return engineErrorBlockM4("syntax_error: streg requires distribution()", "syntax_error");
  if (dist != "weibull")
    return engineErrorBlockM4(
        "Option `distribution(" + dist + ")` is a valid Stata option that this version of PIET does "
        "not implement for `streg` — only `distribution(weibull)` is available.",
        "unsupported_option");
  b.argv.push_back("--dist");
  b.argv.push_back("weibull");
  ojson raw;
  ojson block = runP7Mle("streg", b.argv, session, &raw);
  applyHazardRatioTableM4(block, raw);
  // Stata prints /ln_p, p and 1/p in a stanza below the coefficients; like the
  // ordered models' cutpoints, they are not slopes and the mapper drops them, so
  // they are appended here from the raw payload.
  for (const char* key : {"ln_p", "p", "inv_p"}) {
    if (!raw.contains(key) || !raw[key].is_object()) continue;
    const ojson& s = raw[key];
    ojson row;
    row["name"] = std::string(key) == "ln_p" ? "/ln_p" : (std::string(key) == "p" ? "p" : "1/p");
    row["b"] = s.contains("b") ? s["b"] : ojson(nullptr);
    row["se"] = s.contains("se") ? s["se"] : ojson(nullptr);
    row["t"] = s.contains("z") ? s["z"] : ojson(nullptr);
    row["p"] = s.contains("p") ? s["p"] : ojson(nullptr);
    row["ci_lo"] = s.contains("ci_lo") ? s["ci_lo"] : ojson(nullptr);
    row["ci_hi"] = s.contains("ci_hi") ? s["ci_hi"] : ojson(nullptr);
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    block["coeftable"].push_back(row);
  }
  return block;
}

// ===========================================================================
// logit / probit


}  // namespace piet::handlers
