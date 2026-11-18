// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Survival estimation.
#include "piet/handlers/estimation.h"
#include <algorithm>
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
#include "core/exact_sum.h"  // stset's r(sum)
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
#include "extended_estimation_internal.h"  // stataRefusalM4b
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
  // The time variable is read as a varname: `a-b` is a variable range whose ends
  // must be names, so a non-name end is Stata's r(198) "<end> invalid name"
  // (vs-stata bank 38ba4709 `stset studytime-100, failure(died)`: "100 invalid name").
  {
    const std::string& tv = parsed.varlist[0];
    const size_t dash = tv.find('-');
    if (dash != std::string::npos && !strutil::isValidStataIdentifier(tv)) {
      const std::string lhs = tv.substr(0, dash), rhs = tv.substr(dash + 1);
      for (const std::string& end : {lhs, rhs})
        if (!strutil::isValidStataIdentifier(end))
          return extended_fit_detail::stataRefusalM4b(
              (end.empty() ? std::string("-") : end) + " invalid name", 198);
    }
  }
  // show and noshow together: Stata's own refusal (vs-stata bank c9986ff5
  // `stset studytime, failure(died) show noshow`: r(198) "option noshow not allowed").
  {
    bool show = false, noshow = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "show") show = true;
      if (k == "noshow") noshow = true;
    }
    if (show && noshow)
      return engineErrorBlockM4("option noshow not allowed", "syntax_error");
  }
  // What this build sets is the time variable, failure(varname) and id(): every
  // other part of Stata's stset syntax selects or rescales the analysis time
  // (`if`, if(), enter(), exit(), origin(), scale(), ever(), never(), after(),
  // before(), time0()) and was read by nobody, so the data came back stset as if
  // it had not been typed (`stset studytime, failure(died) exit(time 30)` left
  // _t at 39). Each is refused by name; exit(time .), Stata's default, is the
  // fit PIET makes.
  if (parsed.ifExpr.has_value())
    return engineErrorBlockM4(
        "`stset ... if` is valid Stata that this version of PIET does not implement: the "
        "observations it excludes from the analysis are not marked. Restrict the data with "
        "`keep if` before stset.",
        "unsupported_option");
  for (const auto& [k, v] : parsed.opts) {
    if (k == "failure" || k == "id" || k == "show" || k == "noshow") continue;
    if (k == "exit" && !v.isBool) {
      std::string e = trimM4(v.strValue);
      e.erase(std::remove_if(e.begin(), e.end(), [](char c) { return c == ' ' || c == '\t'; }),
              e.end());
      if (e == "time." || e == "t.") continue;
    }
    return engineErrorBlockM4("Option `" + k + "` is a valid Stata option that this version of "
                              "PIET does not implement for `stset`.",
                              "unsupported_option");
  }
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

  // r() is the last summary stset's report runs, `summarize _t if _st, meanonly`
  // (vs-stata bank 7bb6e51a `stset studytime, failure(died)`: r(N) 48, r(sum) 744,
  // r(mean) 15.5, r(min) 1, r(max) 39), with show and noshow alike.
  try {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has("_t") && ds->has("_st")) {
      const double* t = ds->col("_t").f64();
      const double* st = ds->col("_st").f64();
      if (t != nullptr && st != nullptr) {
        const int64_t n = ds->nRows();
        double cnt = 0, mn = 0, mx = 0;
        cecon::ExactSum sum;
        for (int64_t i = 0; i < n; ++i) {
          if (!(st[i] == 1.0) || std::isnan(t[i]) || t[i] >= eval::kStataMissingSentinel) continue;
          if (cnt == 0 || t[i] < mn) mn = t[i];
          if (cnt == 0 || t[i] > mx) mx = t[i];
          sum.add(t[i]);
          cnt += 1;
        }
        if (cnt > 0) {
          eval::replaceRClass(session.eval);
          session.eval.rValues.set("N", cnt);
          session.eval.rValues.set("sum_w", cnt);
          session.eval.rValues.set("sum", sum.value());
          session.eval.rValues.set("mean", sum.value() / cnt);
          session.eval.rValues.set("min", mn);
          session.eval.rValues.set("max", mx);
          eval::declareRPosted(session.eval, "summarize", {"N", "sum_w", "sum", "mean", "min", "max"});
        }
      }
    }
  } catch (const std::exception&) {
  }

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
    // Stata's st commands refuse data that are not stset with r(119) "data not st"
    // (vs-stata bank 1f5ea076 `stcox mpg`).
    r.errorBlock = extended_fit_detail::stataRefusalM4b("data not st", 119);
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
  // Ties, strata and the display metric. These were read by nobody: `efron`,
  // `exactm`, `exactp`, `strata()`, `tvc()` and `nohr` all came back as the
  // Breslow, unstratified hazard-ratio fit (bank 5bdbc48b `stcox age i.drug,
  // efron` HR 1.1218394 against PIET's Breslow 1.1183339; b5c81a48 `strata(drug)`
  // 1.1193711 against 1.0798451; ccbc78b0 `nohr` b .11184 printed as 1.11833).
  // efron, exactp, exactm and strata() are computed by the engine (risk sets within
  // stratum, Efron's downweighting of a tie set, the exact partial and marginal
  // likelihoods); time-varying covariates are refused by name rather than answered
  // with the Breslow fit.
  // stcox.ado's `syntax` stems: BREslow EFRon EXACTM EXACTP noHR STrata SHared TVC. A
  // shorter spelling the shared prefix rule would resolve (`ef`, `br`, `exa`, `no`) is
  // r(198) in Stata
  // (review cox4.do: `stcox age, ef` -> r(198); bank 6ccd1af5 `efr` is accepted), so
  // the words are checked as typed, before resolution.
  {
    const long long comma = eval::findUnquotedComma(parsed.raw);
    if (comma >= 0) {
      std::vector<std::pair<std::string, eval::OptValue>> rawOpts;
      try {
        rawOpts = eval::parseOpts(parsed.raw.substr(static_cast<size_t>(comma) + 1));
      } catch (const std::exception&) {
        rawOpts.clear();
      }
      // (bank stcox abbreviation cells: b/br, ef, e/ex/exa/exac/exact, n/no/noh, s, t/tv
      // are all r(198) in Stata; bre, efr, st, sh are accepted)
      static const std::pair<const char*, size_t> kStems[] = {
          {"breslow", 3}, {"efron", 3}, {"exactm", 6}, {"exactp", 6}, {"nohr", 4},
          {"strata", 2},  {"shared", 2}, {"tvc", 3}};
      for (const auto& [rk, rv] : rawOpts) {
        const std::string lk = lowerM4(rk);
        for (const auto& [word, minLen] : kStems) {
          const std::string w(word);
          if (lk.size() < minLen && lk.size() < w.size() && w.compare(0, lk.size(), lk) == 0)
            return extended_fit_detail::stataRefusalM4b(
                "option " + rk + (rv.isBool ? std::string() : std::string("()")) + " not allowed", 198);
        }
      }
    }
  }
  bool nohr = false;
  std::vector<std::string> tieOpts;
  std::vector<std::string> strataVars;
  for (const auto& [k, v] : parsed.opts) {
    const std::string lk = lowerM4(k);
    if (lk == "nohr" && v.isBool) {
      nohr = true;
    } else if (lk == "breslow" || lk == "efron" || lk == "exactm" || lk == "exactp") {
      tieOpts.push_back(lk);
    } else if (lk == "strata") {
      if (v.isBool || splitTokensM4(v.strValue).empty())
        return extended_fit_detail::stataRefusalM4b("option strata() requires a varlist", 198);
      for (const auto& t : splitTokensM4(v.strValue)) strataVars.push_back(t);
    } else if (lk == "tvc" || lk == "texp" || lk == "offset" || lk == "shared" ||
               lk == "frailty" || lk == "basehc" || lk == "basechazard" || lk == "basesurv" ||
               lk == "mgale" || lk == "esr" || lk == "schoenfeld" || lk == "scaledsch") {
      return engineErrorBlockM4("Option `" + lk + "` is a valid Stata option that this version of "
                                "PIET does not implement for `stcox`.",
                                "unsupported_option");
    }
  }
  if (tieOpts.size() > 1)
    return engineErrorBlockM4("Options `" + tieOpts[0] + "` and `" + tieOpts[1] +
                                  "` together are not implemented for `stcox` in this version of PIET.",
                              "unsupported_option");
  const std::string ties = tieOpts.empty() ? std::string("breslow") : tieOpts.front();
  for (const auto& sv : strataVars)
    if (sv.find_first_of(".#()") != std::string::npos)
      return engineErrorBlockM4("strata(" + sv + "): this version of PIET takes plain variable "
                                "names in strata().",
                                "unsupported_option");
  P7MleArgv b = buildStArgv("stcox", parsed, session);
  if (b.errored) return b.errorBlock;
  const bool sandwich = std::find(b.argv.begin(), b.argv.end(), "--vce") != b.argv.end() &&
                        std::find(b.argv.begin(), b.argv.end(), "iid") == b.argv.end();
  if (sandwich && (ties != "breslow" || !strataVars.empty()))
    return engineErrorBlockM4(
        "A robust or cluster VCE together with " +
            std::string(ties != "breslow" ? ties + " ties" : std::string("strata()")) +
            " is a valid Stata combination that this version of PIET does not implement for "
            "`stcox`.",
        "unsupported_option");
  if (ties != "breslow") {
    b.argv.push_back("--ties");
    // efron, exactp or exactm (bank a5cb266b exactp HR[age] 1.1263549744884696, 2a3864a7
    // exactm 1.1237896099737557)
    b.argv.push_back(ties);
  }
  if (!strataVars.empty()) {
    b.argv.push_back("--strata");
    b.argv.push_back(joinCommaM4(strataVars));
  }
  ojson raw;
  ojson block = runP7Mle("stcox", b.argv, session, &raw);
  // `nohr` reports the coefficients themselves (Stata's r(table) b row is b, its
  // eform row 0); the hazard-ratio metric is the default.
  if (!nohr) applyHazardRatioTableM4(block, raw);
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
  pushLevelIfGivenM4(parsed, b.argv);  // level(#): the reported intervals
  ojson raw;
  ojson block = runP7Mle("streg", b.argv, session, &raw);
  // `nohr` reports the coefficients themselves, as stcox's does (bank aa9b4c29
  // `streg age i.drug, distribution(weibull) nohr level(90)`: age .1172843); the
  // hazard-ratio metric is the default. It was accepted and ignored.
  bool nohr = false;
  for (const auto& [k, v] : parsed.opts)
    if (lowerM4(k) == "nohr" && v.isBool && v.boolValue) nohr = true;
  if (!nohr) applyHazardRatioTableM4(block, raw);
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
