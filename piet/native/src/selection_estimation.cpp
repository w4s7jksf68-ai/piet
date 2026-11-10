// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Selection estimation.
#include "piet/handlers.h"
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
#include "estimators/posthest.h"  // cecon::margins_parse_at (header-inline)

#include "extended_estimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace extended_fit_detail {}
using namespace extended_fit_detail;
using namespace extended_estimation_vocab;

ojson runHeckman(const eval::ParsedCommand& parsed, SessionState& session) {
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
  size_t eq = selectRaw->find('=');
  if (eq == std::string::npos) {
    return engineErrorBlockM4("syntax_error: malformed select(): missing '='", "syntax_error");
  }
  if (selectRaw->find('=', eq + 1) != std::string::npos) {
    return engineErrorBlockM4("syntax_error: malformed select(): only one '=' is allowed",
                              "syntax_error");
  }
  std::vector<std::string> selDep = splitTokensM4(selectRaw->substr(0, eq));
  std::vector<std::string> selVars = splitTokensM4(selectRaw->substr(eq + 1));
  if (selDep.size() != 1 || selVars.empty()) {
    return engineErrorBlockM4(
        "syntax_error: malformed select(): need exactly one selection-equation dependent "
        "variable and at least one identifying variable",
        "syntax_error");
  }

  if (!sawTwostep) {
    return engineErrorBlockM4(
        "Full maximum-likelihood `heckman` (Stata's default when `twostep` is omitted) is not "
        "supported by this version of PIET — the engine's heckman estimator only implements the "
        "two-step Heckit method. Use `heckman ..., select(...) twostep`.",
        "unsupported_option");
  }

  std::vector<std::string> argv = {"heckman", "--y", y, "--x", joinCommaM4(xs), "--select",
                                    selDep[0] + "=" + joinCommaM4(selVars)};
  // Deliberately no --twostep (see the function-header comment: the flag
  // itself triggers a cobra CLI masking bug; the engine's bare default
  // already computes the two-step estimate the oracle pins).
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-274: `level()` used to fall through to the generic reject net below
  // ("option `level` not allowed" — `level` was simply absent from kKnown).
  // BUG-202 gave cobra's heckman a real `--level` (ci_lo/ci_hi on BOTH the
  // outcome coeftable and select_coefs, asymptotic z — live-verified on this
  // tree's engine), so it is now in kKnown and forwards, same idiom as
  // runTobit/runNbreg. The engine validates the range itself ([10, 99.99]).
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
      continue;
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabHeckman.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
    return *err;
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  // Stata's scalar names, from the values this fit actually computed.
  raw["N_selected"] = raw.at("n_selected");
  raw["N_nonselected"] = raw.at("n_obs").get<double>() - raw.at("n_selected").get<double>();
  raw["selambda"] = raw.at("se_lambda");
  if (raw.contains("converged") && raw["converged"].is_boolean())
    raw["fconverged"] = raw["converged"].get<bool>() ? 1.0 : 0.0;
  ojson block = cobraToEstimationM4(raw, "heckman", &session);
  block["depvar"] = y;  // cobra heckman's raw JSON carries no "depvar" field at all (live-verified)
  block["estimator"] = "heckman";  // Stata's own display name is "heckman" regardless of twostep
  static const char* kHeckmanFields[] = {
      "lambda", "se_lambda", "rho", "sigma", "n_selected", "select_coefs",
  };
  for (const char* k : kHeckmanFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // Multi-equation shape (outcome + selection): reuse the SAME
  // {depvar, coeftable, r2, r2_within, dof_resid}-per-entry "outcomes" array
  // shape D1's reg3 adapter already established (cobraToEstimationM4's
  // "equations"->"outcomes" remap above), so any future "iterate
  // block.outcomes[]" frontend renderer picks up heckman's two equations
  // for free. cobra's select_coefs rows carry name/b/se/t always, and — since
  // BUG-202 gave heckman `--level` — ci_lo/ci_hi too WHEN level() was
  // forwarded (live-verified both ways on this tree's engine: absent without
  // --level, populated with it). Carry what the engine sent; p is genuinely
  // never emitted on select rows and stays null, not fabricated (BUG-274
  // un-staled the old "no p/CI ever" wording here).
  if (raw.contains("select_coefs") && raw["select_coefs"].is_array()) {
    ojson selCt = ojson::array();
    for (const auto& c : raw["select_coefs"]) {
      ojson r;
      r["name"] = c.value("name", std::string());
      r["b"] = c.contains("b") ? c["b"] : ojson(nullptr);
      r["se"] = c.contains("se") ? c["se"] : ojson(nullptr);
      r["t"] = c.contains("t") ? c["t"] : ojson(nullptr);
      r["p"] = ojson(nullptr);
      r["ci_lo"] = c.contains("ci_lo") ? c["ci_lo"] : ojson(nullptr);
      r["ci_hi"] = c.contains("ci_hi") ? c["ci_hi"] : ojson(nullptr);
      selCt.push_back(r);
    }
    ojson outcomeEq;
    outcomeEq["depvar"] = y;
    outcomeEq["coeftable"] = block.value("coeftable", ojson::array());
    outcomeEq["r2"] = ojson(nullptr);
    outcomeEq["r2_within"] = ojson(nullptr);
    outcomeEq["dof_resid"] = ojson(nullptr);
    ojson selectEq;
    selectEq["depvar"] = selDep[0];
    selectEq["coeftable"] = selCt;
    selectEq["r2"] = ojson(nullptr);
    selectEq["r2_within"] = ojson(nullptr);
    selectEq["dof_resid"] = ojson(nullptr);
    ojson outcomes = ojson::array();
    outcomes.push_back(outcomeEq);
    outcomes.push_back(selectEq);
    block["outcomes"] = outcomes;
  }

  LastModel lm;
  lm.cobraCmd = "heckman";  // added to runMargins's kLinearCmds below — heckman's outcome
                            // equation is a plain linear fit, so dydx is equally meaningless.
  lm.y = y;
  lm.x = xs;
  lm.depvar = y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // Both fitted equations and the Mills coefficient are part of e(b), even
  // though the console presents the equations separately. Preserve their
  // actual SEs for qualified _se[]; a full cross-equation e(V) is not invented.
  ojson full = ojson::array();
  auto appendEq = [&full](const ojson& rows, const std::string& eq) {
    for (const auto& c : rows) {
      ojson row = c;
      std::string name = row.value("name", std::string());
      if (name == "(intercept)") name = "_cons";
      row["name"] = eq + ":" + name;
      full.push_back(std::move(row));
    }
  };
  appendEq(block.value("coeftable", ojson::array()), y);
  appendEq(raw.value("select_coefs", ojson::array()), selDep[0]);
  full.push_back(ojson{{"name", "/mills:lambda"}, {"b", raw["lambda"]},
                       {"se", raw["se_lambda"]}});
  eval::EvalMatrix eb;
  eb.rownames = {"y1"};
  eb.data.emplace_back();
  for (const auto& row : full) {
    eb.colnames.push_back(row["name"].get<std::string>());
    eb.data[0].push_back(row["b"].get<double>());
  }
  (*session.lastEstBlock)["coeftable"] = std::move(full);
  session.eval.eMatrices.emplace_back("b", std::move(eb));
  return block;
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
