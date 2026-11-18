// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// System estimation.
#include "piet/handlers/estimation.h"
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
#include "core/distributions.h"  // z_critical (shared with cobra)
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

ojson runReg3(const eval::ParsedCommand& parsed, SessionState& session,
              const std::string& stataCmd) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // sureg is `reg3 <eqns> ..., sure` with e(cmd)
  // set to sureg; its isure/dfk/small/corr/constraints are not
  // forwarded by this build, so they are refused by name below, never ignored.
  const bool isSureg = stataCmd == "sureg";
  if (isSureg) {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSureg.known,
                                              kVocabSureg.unimplemented)) {
      return *err;
    }
  }

  // splitWs() already broke each parenthesised equation into separate
  // varlist tokens; rejoin with single spaces and re-split on the paren
  // structure (same rationale as parseIvParenM4 above — Stata's own
  // whitespace inside the parens carries no meaning).
  std::string joined = joinSpaceM4(parsed.varlist);
  std::vector<std::string> groups;
  bool parenOk = true;
  {
    size_t i = 0;
    while (i < joined.size()) {
      while (i < joined.size() && (joined[i] == ' ' || joined[i] == '\t')) ++i;
      if (i >= joined.size()) break;
      if (joined[i] != '(') {
        parenOk = false;
        break;
      }
      size_t close = joined.find(')', i);
      if (close == std::string::npos) {
        parenOk = false;
        break;
      }
      groups.push_back(joined.substr(i + 1, close - i - 1));
      i = close + 1;
    }
  }
  if (!parenOk) {
    return engineErrorBlockM4(
        "syntax_error: reg3 requires each equation in parentheses: (depvar regressors...)",
        "syntax_error");
  }
  if (groups.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: reg3 requires at least two equations: (y1 x1 x2) (y2 x3 y1)",
        "syntax_error");
  }

  std::vector<std::string> eqArgs;
  for (const auto& g : groups) {
    std::vector<std::string> toks = splitTokensM4(g);
    if (toks.size() < 2) {
      return engineErrorBlockM4(
          "syntax_error: malformed reg3 equation `(" + g + ")`: needs a dependent variable and "
          "at least one regressor",
          "syntax_error");
    }
    std::string depvar = toks[0];
    std::vector<std::string> regs(toks.begin() + 1, toks.end());
    eqArgs.push_back(depvar + "=" + joinCommaM4(regs));
  }

  bool sawSure = isSureg, sawExplicit3sls = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "sure" && v.isBool && v.boolValue) sawSure = true;
    if (k == "3sls" && v.isBool && v.boolValue) sawExplicit3sls = true;
  }
  if (sawSure && sawExplicit3sls) {
    return engineErrorBlockM4("syntax_error: `sure` and `3sls` may not be combined", "syntax_error");
  }

  // vce()/robust/cluster/noconstant/level — cobra reg3 has NO --vcov/
  // --no-intercept/--level surface at all (live-verified: all three hard-
  // error "unrecognized option" at cobra's own CLI layer, exit nonzero, but
  // (--no-intercept/--level only, NOT --vcov) still print a full valid
  // stdout JSON first — the SAME cobra_runner.cpp BUG-039 masking shape the
  // ivregress/liml branch above documents; --vcov itself errors cleanly
  // with empty stdout, but is rejected here anyway for a uniform, honest
  // message rather than relying on that distinction). Reject before ever
  // building the argv.
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "vce" || k == "vcov" || k == "robust" || k == "cluster") {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "` (the engine's reg3 estimator has no vcov surface yet — classical "
          "SEs only).",
          "unsupported_option");
    }
  }
  std::optional<double> sysLevel;  // level(#)
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "noconstant" || k == "nocons") && v.isBool && v.boolValue) {
      return engineErrorBlockM4(
          "Option `" + k + "` is a valid Stata option that this version of PIET does not yet "
          "implement for `" + stataCmd + "`.",
          "unsupported_option");
    }
    if (k == "level" && !v.isBool) {
      // level(#): the table's normal interval at that level (set on the block below;
      // nothing else in the fit depends on it). Stata's cilevel range is 10..99.99.
      char* end = nullptr;
      const std::string t = strutil::trim(v.strValue);
      const double lv = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0' || !(lv >= 10.0 && lv <= 99.99))
        return stataRefusalM4b("level() must be between 10 and 99.99 inclusive", 198);
      sysLevel = lv;
    }
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabReg3.known;
  if (!isSureg) {
    if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
      return *err;
    }
  }

  std::vector<std::string> argv = {"reg3"};
  for (const auto& e : eqArgs) {
    argv.push_back("--eq");
    argv.push_back(e);
  }
  // Stata's OWN default is 3SLS (sure only when explicitly requested) —
  // oracle-verified (D1g, no options, matches --method 3sls exactly) —
  // while cobra's reg3 default is the OPPOSITE (SUR unless --method 3sls is
  // given — oracle D1h/`sure` matches cobra's no-flag default exactly). So
  // --method must ALWAYS be sent explicitly; relying on cobra's own default
  // would silently swap which method every plain `reg3 (...) (...)` (no
  // options) call actually ran.
  argv.push_back("--method");
  argv.push_back(sawSure ? "sure" : "3sls");
  // sureg's isure: the FGLS iterated to convergence (reg3.ado's loop: at most
  // iterate(#) passes, c(maxiter) 300 by default; tolerance(#) on mreldif of e(b),
  // 1e-6 by default; bank 2b8fedb8 `isure it(2)`: e(ic) 2; 0e1a60d6 `i`: e(ic) 6;
  // 4b300106 `isure tol(1e-2)`: 2). An iterate(#) other than c(maxiter) (300; `set
  // maxiter` is fixed at its default in this build) switches the iteration on by
  // itself, while e(method) still says "isure" only when isure was typed.
  // tolerance() alone changes nothing.
  bool isure = false;
  bool iterated = false;
  if (isSureg) {
    std::string iterateOpt, tolOpt;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "isure" && v.isBool && v.boolValue) isure = true;
      if (k == "isure" && !v.isBool) return stataRefusalM4b("option isure() not allowed", 198);
      if (k == "iterate" || k == "tolerance") {
        char* end = nullptr;
        const std::string t = v.isBool ? std::string() : strutil::trim(v.strValue);
        const double x = std::strtod(t.c_str(), &end);
        const bool okNum = !t.empty() && end != nullptr && *end == '\0';
        if (k == "iterate") {
          if (!okNum || x != std::floor(x) || x < 0 || x > 1e9)
            return stataRefusalM4b("option iterate() incorrectly specified", 198);
          char ib[32];
          std::snprintf(ib, sizeof ib, "%.0f", x);  // `1e3` reaches the engine as 1000
          iterateOpt = ib;
        } else {
          if (!okNum || !(x > 0)) return stataRefusalM4b("option tolerance() incorrectly specified", 198);
          tolOpt = t;
        }
      }
    }
    iterated = isure || (!iterateOpt.empty() && iterateOpt != "300");
    if (iterated) {
      argv.push_back("--isure");
      if (!iterateOpt.empty()) {
        argv.push_back("--iterate");
        argv.push_back(iterateOpt);
      }
      if (!tolOpt.empty()) {
        argv.push_back("--tolerance");
        argv.push_back(tolOpt);
      }
    }
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  // e(sample): the engine's joint listwise rows (--esample-out, not recorded in
  // argv), installed before setLastModel below. StataBE: `sureg (...) (...)` then
  // `estimates store S` adds _est_S, and `count if e(sample)` is e(N).
  std::string esampleValues;
  try {
    // --emit-v: the stacked system e(V) (y:x1 ... x3:_cons) that e(b)/e(V)
    // and estout's matrix mode read; not part of argv (a refit flag).
    std::vector<std::string> fitArgv = argv;
    fitArgv.push_back("--emit-v");
    esampleValues = esampleFitArgs(session, fitArgv, std::string(), 0);
    raw = runCobraJsonM4(fitArgv);
  } catch (const CobraError& ce) {
    if (!esampleValues.empty()) cecon::native_unlink(esampleValues);
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "reg3", &session);
  // level(#): each equation's interval b -/+ z(level)*se (reg3/sureg are normal-based;
  // bank 213ec4a0 `sureg ..., level(90)`).
  if (sysLevel.has_value() && block.contains("outcomes") && block["outcomes"].is_array()) {
    const double crit = cecon::z_critical(1.0 - *sysLevel / 100.0);
    for (auto& eq : block["outcomes"]) {
      if (!eq.is_object() || !eq.contains("coeftable") || !eq["coeftable"].is_array()) continue;
      for (auto& row : eq["coeftable"]) {
        if (!row.is_object() || !row.contains("b") || !row["b"].is_number() || !row.contains("se") ||
            !row["se"].is_number())
          continue;
        const double b = row["b"].get<double>(), se = row["se"].get<double>();
        row["ci_lo"] = b - crit * se;
        row["ci_hi"] = b + crit * se;
      }
    }
  }

  LastModel lm;
  lm.cobraCmd = "reg3";
  if (isSureg) lm.stataCmd = "sureg";
  std::vector<std::string> firstToks = splitTokensM4(groups[0]);
  lm.y = firstToks.empty() ? std::string() : firstToks.front();
  // No single lm.x is fully coherent for a multi-equation system; best-
  // effort populate from the FIRST equation only (test/lincom/margins/
  // predict after reg3 inherit the SAME coarse-approximation caveat noted
  // on the IV path above — a pre-existing, system-wide LastModel
  // simplification, not new here).
  if (firstToks.size() > 1) lm.x.assign(firstToks.begin() + 1, firstToks.end());
  lm.depvar = lm.y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = *session.currentHbin();
  lm.result = raw;
  if (!esampleValues.empty() && lm.result->is_object() && lm.result->contains("esample_n"))
    (void)esampleInstallOnHead(session, esampleValues, *lm.result, lm);
  else if (!esampleValues.empty())
    cecon::native_unlink(esampleValues);
  session.setLastModel(lm);
  session.lastEstBlock = block;
  // The session block names the model's depvar (the first equation's, as
  // lm.depvar), so `estimates store` pairs block and model and writes _est_NAME
  // from e(sample). Session copy only: the emitted block is unchanged.
  if (session.lastEstBlock->is_object() &&
      session.lastEstBlock->value("depvar", std::string()).empty())
    (*session.lastEstBlock)["depvar"] = lm.depvar;
  session.attachFitCovariance(raw);  // e(V): the engine's stacked system covariance
  // sureg's own macros (bank: every `sureg (y1 x..) (y2 x..)` fit): e(depvar) and
  // e(eqnames) the equations' depvars, e(exog) the regressors in order of first
  // appearance, e(method) sure, e(corr) unstructured, e(vce) unadjusted, the
  // margins defaults and e(predict) reg3_p. Plain variable names only. `reg3 ...,
  // sure` posts the same (bank f11ab0d7 `reg3 (price mpg foreign) (trunk weight
  // length), su`).
  if (sawSure) {
    std::vector<std::string> deps, exog;
    bool plain = true;
    for (const auto& g : groups) {
      const std::vector<std::string> toks = splitTokensM4(g);
      for (const auto& t : toks)
        if (t.find_first_of(".#()*?~-") != std::string::npos) plain = false;
      if (toks.empty()) plain = false;
      else deps.push_back(toks.front());
      for (size_t i = 1; i < toks.size(); ++i)
        if (std::find(exog.begin(), exog.end(), toks[i]) == exog.end()) exog.push_back(toks[i]);
    }
    exog.erase(std::remove_if(exog.begin(), exog.end(),
                              [&deps](const std::string& v) {
                                return std::find(deps.begin(), deps.end(), v) != deps.end();
                              }),
               exog.end());
    if (plain && session.lastModel().has_value()) {
      const auto join = [](const std::vector<std::string>& v) {
        std::string o;
        for (const auto& x : v) o += (o.empty() ? "" : " ") + x;
        return o;
      };
      std::string mdef;
      for (const auto& d : deps) mdef += (mdef.empty() ? "" : " ") + ("predict(xb equation(" + d + "))");
      const std::vector<std::pair<const char*, std::string>> macros = {
          {"method", isure ? "isure" : "sure"}, {"vce", "unadjusted"}, {"marginsdefault", mdef},
          {"marginsok", "XB default"}, {"predict", "reg3_p"}, {"corr", "unstructured"},
          {"exog", join(exog)}, {"depvar", join(deps)}, {"eqnames", join(deps)}};
      for (const auto& [n, v] : macros) {
        if (v.empty()) continue;
        session.eval.setEString(n, v);
        session.noteEOnFit(n);
      }
    }
  }
  // reg3's (and sureg's) system results (reg3.ado's posting; bank 77bbda65 `sureg (price
  // mpg weight) (trunk length weight)`): per equation e(cons_#) 1, e(chi2_#)/e(p_#)/
  // e(df_m#) the Wald test of its slopes, e(rss_#) from the final coefficients,
  // e(rmse_#) = sqrt(rss/N), e(mss_#) = TSS - RSS, e(r2_# ) = mss/(mss + rss); then
  // e(k) the coefficients, e(k_eq) the equations (and the hidden e(k_eform)), e(ic) 0
  // (not iterated), e(dfk2_adj) = 1/df_adj with df_adj = 1/N kept in a macro at 15
  // significant digits (bank: N 74 -> 74.00000000000007, N 69 -> 69.00000000000003),
  // e(ll) the Gaussian system log likelihood on the final residuals (engine), e(rank)
  // the full column count, the hidden e(k_autoCns) 0, and e(Sigma) the first-step
  // residual covariance the FGLS used (rows/columns the equations' depvars).
  if (raw.is_object() && raw.contains("eq_stats") && raw["eq_stats"].is_array() &&
      raw.contains("n_obs") && raw["n_obs"].is_number() && raw.contains("ll") &&
      raw["ll"].is_number() && raw.contains("sigma") && raw["sigma"].is_array() &&
      raw["eq_stats"].size() == groups.size() && raw["sigma"].size() == groups.size()) {
    const double N = raw["n_obs"].get<double>();
    auto put = [&session](const std::string& nm, double v) {
      session.eval.setEScalar(nm, v);
      session.noteEOnFit(nm);
    };
    std::vector<std::string> deps;
    double K = 0.0;
    bool ok = N > 0;
    for (const auto& g : groups) {
      const std::vector<std::string> toks = splitTokensM4(g);
      if (toks.empty()) ok = false;
      else deps.push_back(toks.front());
      K += static_cast<double>(toks.size());  // its regressors and the constant
    }
    for (size_t i = 0; ok && i < groups.size(); ++i) {
      const ojson& st = raw["eq_stats"][i];
      for (const char* key : {"rss", "tss", "chi2", "df", "p"})
        ok = ok && st.contains(key) && st[key].is_number();
    }
    for (size_t i = 0; ok && i < deps.size(); ++i)
      ok = raw["sigma"][i].is_array() && raw["sigma"][i].size() == deps.size();
    if (ok) {
      for (size_t i = 0; i < groups.size(); ++i) {
        const ojson& st = raw["eq_stats"][i];
        const std::string j = std::to_string(i + 1);
        const double rss = st["rss"].get<double>(), mss = st["tss"].get<double>() - rss;
        put("cons_" + j, 1.0);
        put("chi2_" + j, st["chi2"].get<double>());
        put("p_" + j, st["p"].get<double>());
        put("df_m" + j, st["df"].get<double>());
        put("rss_" + j, rss);
        put("rmse_" + j, std::sqrt(rss / N));
        put("mss_" + j, mss);
        put("r2_" + j, mss / (mss + rss));
      }
      const double M = static_cast<double>(groups.size());
      put("k", K);
      put("k_eq", M);
      put("k_eform", M);
      session.eval.markEHidden("k_eform", session.eval.eScalars.generationOf("k_eform"));
      put("ic", raw.contains("ic") && raw["ic"].is_number() ? raw["ic"].get<double>() : 0.0);
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.15g", 1.0 / N);
      put("dfk2_adj", 1.0 / std::strtod(buf, nullptr));
      put("ll", raw["ll"].get<double>());
      put("rank", K);
      put("k_autoCns", 0.0);
      session.eval.markEHidden("k_autoCns", session.eval.eScalars.generationOf("k_autoCns"));
      eval::EvalMatrix sig;
      sig.rownames = deps;
      sig.colnames = deps;
      for (size_t i = 0; i < deps.size(); ++i) {
        sig.data.emplace_back();
        for (size_t c = 0; c < deps.size(); ++c)
          sig.data.back().push_back(raw["sigma"][i][c].is_number() ? raw["sigma"][i][c].get<double>()
                                                                  : std::nan(""));
      }
      auto& store = session.eval.eMatrices;
      store.erase(std::remove_if(store.begin(), store.end(),
                                 [](const auto& p) { return p.first == "Sigma"; }),
                  store.end());
      store.emplace_back("Sigma", std::move(sig));
      session.noteEOnFit("Sigma");
    }
  }
  // sureg, notable: no coefficient table is shown, and r() is left with the sample
  // count, r(N) (bank 65dfa6f2 `sureg (price mpg weight) (trunk length weight), not`:
  // r(N) 74, nothing else)
  if (isSureg && block.is_object() && raw.is_object() && raw.contains("n_obs") &&
      raw["n_obs"].is_number()) {
    bool notable = false;
    for (const auto& kv : parsed.opts) {
      const std::string k = strutil::lower(kv.first);
      notable = notable || (k.size() >= 3 && std::string("notable").compare(0, k.size(), k) == 0);
    }
    if (notable) {
      ojson ra;
      ra["scalars"] = {{"N", raw["n_obs"].get<double>()}};
      block["r_after_fit"] = ra;
      if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
        (*session.lastEstBlock)["r_after_fit"] = ra;
    }
  }
  return block;
}

// ===========================================================================
// boottest (BS-066): wild cluster bootstrap post-estimation, requires a
// preceding vce(cluster ...) fit in session.lastModel().


}  // namespace piet::handlers
