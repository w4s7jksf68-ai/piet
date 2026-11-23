// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Selection estimation.
#include "piet/handlers/estimation.h"
#include "piet/handlers/postestimation.h"  // runHeckmanMillsVar
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/settings.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "piet/stata_format.h"
#include "estimation_internal.h"
#include "estimation_families_internal.h"  // cacheLastModelM4
#include "estimation_sample_internal.h"  // esampleFitArgs
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

// heckman's default fit, full maximum likelihood (heckman.ado: ml with heck_d2,
// started at the two-step estimates, search(off)): the engine's `heckman --ml`
// (vs-stata bank fdeed49b `heckman lw grade age, select(grade age married south)`:
// iterations -2409.0108 .. -2404.7667, e(chi2_c) 60.844821). e(b) is `y:x .. y:_cons
// S:z .. S:_cons /athrho /lnsigma` (S the selection equation's depvar, `select`
// when select() names none).
// `twostep`: heckman.ado's Heck2 (engine `heckman` without --ml): e(b) `y:x .. y:_cons
// S:z .. S:_cons /mills:lambda`, e(V) block-diagonal (vs-stata bank b6e7e050
// `select(grade age married south) twostep`).
ojson runHeckmanFit(const eval::ParsedCommand& parsed, SessionState& session, const std::string& y,
                    const std::vector<std::string>& xs, const std::string& selectRaw,
                    bool twostep) {
  static const std::regex rePlain(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  if (selectRaw.find(',') != std::string::npos)
    return engineErrorBlockM4(
        "Options inside select() (noconstant, offset()) are valid Stata that this version of "
        "PIET does not implement for `heckman`.",
        "unsupported_option");
  const size_t eq = selectRaw.find('=');
  if (eq != std::string::npos && selectRaw.find('=', eq + 1) != std::string::npos)
    return engineErrorBlockM4("syntax_error: malformed select(): only one '=' is allowed",
                              "syntax_error");
  std::string selDep;
  std::vector<std::string> selVars;
  if (eq != std::string::npos) {
    const auto d = splitTokensM4(selectRaw.substr(0, eq));
    if (d.size() != 1)
      return engineErrorBlockM4(
          "syntax_error: malformed select(): need exactly one selection-equation dependent "
          "variable",
          "syntax_error");
    selDep = d[0];
    selVars = splitTokensM4(selectRaw.substr(eq + 1));
  } else {
    selVars = splitTokensM4(selectRaw);
  }
  if (selVars.empty())
    return engineErrorBlockM4("syntax_error: select() requires at least one variable",
                              "syntax_error");
  std::vector<std::string> all(xs);
  all.insert(all.end(), selVars.begin(), selVars.end());
  if (!selDep.empty()) all.push_back(selDep);
  for (const auto& v : all)
    if (!std::regex_match(v, rePlain))
      return engineErrorBlockM4(
          "Factor variables and time-series operators (`" + v + "`) in `heckman`'s maximum-"
          "likelihood fit are valid Stata that this version of PIET does not implement.",
          "unsupported_option");
  if (parsed.weightType.has_value())
    return engineErrorBlockM4(
        "Weights in `heckman`'s maximum-likelihood fit are valid Stata that this version of PIET "
        "does not implement.",
        "unsupported_option");

  std::vector<std::string> argv = {"heckman", "--y", y, "--x", joinCommaM4(xs),
                                    "--select",
                                    selDep.empty() ? joinCommaM4(selVars)
                                                   : selDep + "=" + joinCommaM4(selVars)};
  if (!twostep) argv.insert(argv.begin() + 1, "--ml");
  // the two-step estimator: Stata's conventional VCE and rhosigma rule only
  if (twostep) {
    for (const auto& [k, v] : parsed.opts) {
      if (k == "noconstant" || k == "iterate" || k == "vce" || k == "robust")
        return engineErrorBlockM4("Option `" + k + "` with `heckman, twostep` is valid Stata "
                                  "that this version of PIET does not implement.",
                                  "unsupported_option");
    }
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  bool robust = false;
  std::string clusterVar;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
    } else if (k == "noconstant" && !twostep) {
      argv.push_back("--no-intercept");
    } else if (k == "iterate" && !twostep) {
      const std::string val = trimM4(v.isBool ? std::string() : v.strValue);
      char* end = nullptr;
      const double d = std::strtod(val.c_str(), &end);
      if (val.empty() || end == nullptr || *end != '\0' || !(d >= 0 && d <= 16000) ||
          d != std::floor(d))
        return engineErrorBlockM4("option iterate() must be an integer from 0 to 16000 in this "
                                  "build (got `" + val + "`)",
                                  "unsupported_option");
      argv.push_back("--iterate");
      argv.push_back(std::to_string(static_cast<int>(d)));
    } else if (k == "robust" && !twostep) {
      robust = true;
    } else if (k == "vce" && !twostep) {
      const std::vector<std::string> w = splitTokensM4(v.isBool ? std::string() : v.strValue);
      const std::string w0 = w.empty() ? std::string() : lowerM4(w[0]);
      if (w.size() == 1 && std::string("robust").compare(0, w0.size(), w0) == 0 && w0.size() >= 1) {
        robust = true;
      } else if (w.size() == 2 && w0.size() >= 2 && std::string("cluster").compare(0, w0.size(), w0) == 0) {
        clusterVar = w[1];
      } else if (w.size() == 1 && w0 == "oim") {
      } else {
        return engineErrorBlockM4("vce(" + (v.isBool ? std::string() : v.strValue) +
                                      ") is valid Stata that this version of PIET does not "
                                      "implement for `heckman`'s maximum-likelihood fit.",
                                  "unsupported_option");
      }
    }
  }
  if (!clusterVar.empty()) {
    argv.push_back("--cluster");
    argv.push_back(clusterVar);
  } else if (robust) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }
  std::unordered_set<std::string> kKnown = kVocabHeckman.known;
  std::unordered_set<std::string> kUnimpl = kVocabHeckman.unimplemented;
  // mills(newvar) / nshazard(newvar): the nonselection hazard (nshazard wins when
  // both are given, heckman.ado); the name must be new (confirm new var, r(110))
  std::string millsVar;
  for (const char* key : {"mills", "nshazard"}) {
    kKnown.insert(key);
    kUnimpl.erase(key);
    for (const auto& [k, v] : parsed.opts)
      if (k == key) {
        if (v.isBool || trimM4(v.strValue).empty())
          return stataRefusalM4b("option " + std::string(key) + "() incorrectly specified", 198);
        millsVar = trimM4(v.strValue);
      }
  }
  if (!millsVar.empty()) {
    static const std::regex reName(R"(^[A-Za-z_][A-Za-z0-9_]{0,31}$)");
    if (!std::regex_match(millsVar, reName)) return stataRefusalM4b(millsVar + " invalid name", 198);
    Datastore* live = session.datastore();
    if (live != nullptr && live->has(millsVar))
      return stataRefusalM4b("variable " + millsVar + " already defined", 110);
  }
  // rhosigma is the two-step default rule (heckman.ado: rhometh 3); the others are not
  if (twostep) {
    kKnown.insert("rhosigma");
    kUnimpl.erase("rhosigma");
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimpl)) return *err;
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");
  ojson raw;
  std::string esampleValues;
  try {
    std::vector<std::string> fitArgs = argv;
    esampleValues = esampleFitArgs(session, fitArgs, std::string(), 0);
    raw = runCobraJsonM4(fitArgs);
  } catch (const CobraError& ce) {
    if (!esampleValues.empty()) cecon::native_unlink(esampleValues);
    if (std::string(ce.what()).find("never censored") != std::string::npos)
      return stataRefusalM4b("Dependent variable never censored because of selection: model "
                             "would simplify to OLS regression", 498);
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (!raw.is_object() || !raw.contains("esample_n")) {
    if (!esampleValues.empty()) cecon::native_unlink(esampleValues);
    esampleValues.clear();
  }
  // Stata's scalar names, from this fit's values.
  raw["N_selected"] = raw.value("n_selected", 0.0);
  raw["N_nonselected"] = raw.value("n_obs", 0.0) - raw.value("n_selected", 0.0);
  raw["selambda"] = raw.value("se_lambda", 0.0);

  if (twostep && raw.contains("converged") && raw["converged"].is_boolean())
    raw["fconverged"] = raw["converged"].get<bool>() ? 1.0 : 0.0;

  ojson block = cobraToEstimationM4(raw, "heckman", &session);
  block["depvar"] = y;
  block["estimator"] = "heckman";
  // the fit's scalars as the block has always carried them
  for (const char* k : {"lambda", "se_lambda", "rho", "sigma", "n_selected", "select_coefs"})
    if (raw.contains(k)) block[k] = raw[k];
  const std::string seleq = raw.value("select_eq", std::string("select"));
  const double level = [&] {
    for (const auto& [k, v] : parsed.opts)
      if (k == "level" && !v.isBool) return std::strtod(v.strValue.c_str(), nullptr);
    return 95.0;
  }();
  const double zc = cecon::z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
  // z, p and the interval of a row from its b and se (the two-step rows carry t only)
  const auto zrow = [zc](ojson c) {
    const double b = c.value("b", 0.0), se = c.value("se", 0.0);
    if (se > 0) {
      c["z"] = b / se;
      c["t"] = b / se;
      c["p"] = std::erfc(std::fabs(b / se) / std::sqrt(2.0));
      c["ci_lo"] = b - zc * se;
      c["ci_hi"] = b + zc * se;
    }
    return c;
  };
  ojson coefs = ojson::array(), selCoefs = ojson::array(), aux = ojson::array();
  for (const auto& c : raw.value("coefs", ojson::array())) coefs.push_back(twostep ? zrow(c) : c);
  for (const auto& c : raw.value("select_coefs", ojson::array()))
    selCoefs.push_back(twostep ? zrow(c) : c);
  if (twostep) {
    aux.push_back(zrow(ojson{{"name", "/mills:lambda"}, {"b", raw.value("lambda", 0.0)},
                             {"se", raw.value("se_lambda", 0.0)}}));
  } else {
    aux = raw.value("aux_coefs", ojson::array());
  }
  // The table as Stata prints it: the outcome equation, the selection equation,
  // then /athrho /lnsigma and the transformed rho, sigma and lambda (ML) or
  // /mills:lambda followed by rho, sigma (two-step).
  {
    auto eqOf = [](const std::string& dv, const ojson& rows) {
      ojson e;
      e["depvar"] = dv;
      e["coeftable"] = rows;
      e["r2"] = nullptr;
      e["r2_within"] = nullptr;
      e["dof_resid"] = nullptr;
      return e;
    };
    ojson tail = aux;
    if (twostep) {
      for (const char* k : {"rho", "sigma"})
        tail.push_back(ojson{{"name", k}, {"b", raw.value(k, 0.0)}, {"se", nullptr}, {"t", nullptr},
                             {"p", nullptr}, {"ci_lo", nullptr}, {"ci_hi", nullptr}});
    } else {
      for (const auto& d : raw.value("diparm", ojson::array())) tail.push_back(d);
    }
    ojson outcomes = ojson::array();
    outcomes.push_back(eqOf(y, coefs));
    outcomes.push_back(eqOf(seleq, selCoefs));
    outcomes.push_back(eqOf(twostep ? "/mills" : "/athrho /lnsigma", tail));
    block["outcomes"] = outcomes;
  }
  // r(table) laid out as e(b) (normal reference).
  ojson rcols = ojson::array();
  std::vector<AncillaryEbColM4> cols;
  const auto addEq = [&](const ojson& rows, const std::string& eqn) {
    for (const auto& c : rows) {
      std::string nm = c.value("name", std::string());
      std::string full;
      if (!eqn.empty()) full = eqn + ":" + (nm == "(intercept)" ? std::string("_cons") : nm);
      else full = nm;
      ojson rc = c;
      rc["name"] = full;
      rcols.push_back(rc);
      cols.push_back({full, full, c.value("b", 0.0)});
    }
  };
  addEq(coefs, y);
  addEq(selCoefs, seleq);
  addEq(aux, std::string());
  block["rtable_cols"] = rcols;
  if (!twostep) {
    ojson dp = ojson::array();
    for (const auto& d : raw.value("diparm", ojson::array())) {
      ojson c = d;
      c["name"] = "_diparm1:" + d.value("name", std::string());
      dp.push_back(c);
    }
    block["rtable_diparm"] = dp;
  }
  if (!millsVar.empty()) raw["mills"] = millsVar;
  fit_detail::cacheLastModelM4(session, twostep ? "heckman" : "heckman_ml", argv, block, raw,
                               esampleValues, "heckman");
  session.lastEstBlock = block;
  // _b[] / _se[] read the live block's coeftable: every e(b) column by its full
  // name (`lw:grade`, `select:_cons`, `/athrho`, `/mills:lambda`), as the two-step
  // path always posted it; the returned block keeps the outcome table for display.
  (*session.lastEstBlock)["coeftable"] = rcols;
  (void)postAncillaryEbM4(session, raw, cols);
  if (!millsVar.empty()) {
    const ojson w = runHeckmanMillsVar(session, millsVar);
    if (w.value("type", std::string()) == "error") return w;
  }
  return block;
}

static ojson runHeckmanM4In(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: heckman requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> selectRaw;
  bool sawTwostep = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "select" && !v.isBool) selectRaw = v.strValue;
    if (k == "twostep" && v.isBool && v.boolValue) sawTwostep = true;
  }
  if (!selectRaw.has_value() || trimM4(*selectRaw).empty()) {
    return engineErrorBlockM4("syntax_error: heckman requires select(depvar = varlist)",
                              "syntax_error");
  }
  // select([depvar =] varlist): without `depvar =` a row is selected when the outcome
  // is observed (heckman's syntax: depvar_s is optional). Full maximum likelihood by
  // default, the two-step estimator under twostep.
  return runHeckmanFit(parsed, session, y, xs, *selectRaw, sawTwostep);
}

// `in a/b`: folded into the fit's `if` as an observation-number condition
// (inRangeConditionM4), so every reader of the condition below sees the same rows.
ojson runHeckman(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    std::optional<std::string> eff;
    if (auto err = inRangeConditionM4(session, parsed, eff, "heckman")) return *err;
    eval::ParsedCommand folded = parsed;
    folded.ifExpr = eff;
    folded.inRange.reset();
    return runHeckmanM4In(folded, session);
  }
  return runHeckmanM4In(parsed, session);
}


// ===========================================================================
// qreg (BS-071, BETA D2 batch): `qreg depvar indepvars [if] [, quantile(#)
// level(#)]` -> `cobra qreg --tau --level`. Oracle: d2-oracle-stata.log D2e
// (median, no quantile() at all — cobra's own --tau default is 0.5,
// matching Stata's) / D2f (quantile(.25)).
//
// Stata's quantile() accepts EITHER a bare fraction in (0,1) exclusive
// (`.25`/`0.25`) OR a percentage in (1,100) exclusive (`25`) — normalized
// here to cobra's own `--tau` fraction convention (values > 1 are treated
// as a percentage). Unlike tobit/ivregress/reg3's `level()` (never
// forwarded — see above), qreg's own usage banner DOES list `--level`, and
// live-verified it genuinely changes the returned CI (and even the SE/t/p —
// a real per-level rank-based interval, not a cosmetic no-op: `--level 90`
// vs `--level 95` on the same fit produced different se/t/p, not just a
// wider/narrower ci_lo/ci_hi off a fixed se) — so `level()` DOES forward here.

}  // namespace piet::handlers
