// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Descriptive statistics.
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

namespace fit_detail {
ojson runTabstatCore(const std::vector<std::string>& varlist,
                      const std::optional<std::string>& byVal,
                      const std::optional<std::string>& statsVal, const std::string& dataPath,
                      bool wantGroups, bool noTotal, const std::string& weightFlag = "",
                      const std::string& weightVar = "") {
  const std::string* dataPathOpt = &dataPath;
  std::vector<std::string> args = {"tabstat", "--vars", joinCommaM4(varlist)};
  if (byVal.has_value() && !byVal->empty()) {
    args.push_back("--by");
    args.push_back(*byVal);
  }
  // BUG-1014: [aw=]/[fw=] reach the engine, whose tabstat is summarize per group
  // with the same weight conventions (w2as sx_tabstat_aw, sx_tabstat_fw).
  if (!weightFlag.empty() && !weightVar.empty()) {
    args.push_back(weightFlag);
    args.push_back(weightVar);
  }
  if (statsVal.has_value() && !statsVal->empty()) {
    // space/comma -> comma-joined.
    std::string joined;
    std::string cur;
    for (char c : *statsVal) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) {
          if (!joined.empty()) joined += ",";
          joined += cur;
          cur.clear();
        }
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) {
      if (!joined.empty()) joined += ",";
      joined += cur;
    }
    args.push_back("--stats");
    args.push_back(joined);
  } else {
    args.push_back("--stats");
    args.push_back("mean,sd,min,max,n");
  }
  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = cobraToEstimationM4(raw, "tabstat");
  // ------------------------------------------------------------------
  // B4-W3b. The engine has ALWAYS emitted a `groups` array under `--by`
  // (verified on this build: `cobra tabstat --by year` returns seven group
  // objects), and cobraToEstimationM4 has never carried it — so `tabstat y x1,
  // by(region)` rendered the TOTAL row only. That gap is NOT closed here, and
  // deliberately: four freeze fixtures (tabstat/corr/pwcorr/levelsof.json)
  // byte-pin the group-less block for exactly that command, and re-pinning
  // them is a main-checkout edit outside this branch's manifest.
  //
  // What IS closed is the half `save`/`nototal` reach: both spellings answered
  // r(198) until this delta, so no captured fixture can contain either, and
  // carrying `groups` only when one of them is present cannot move a pin.
  if (wantGroups && raw.contains("groups") && raw["groups"].is_array()) {
    block["groups"] = raw["groups"];
  }
  if (noTotal) block.erase("total");
  return block;
}
}  // namespace

ojson runTabstat(const std::vector<std::string>& varlist, const std::optional<std::string>& byVal,
                  const std::optional<std::string>& statsVal, SessionState& session,
                  const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");

  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!dataPathOpt.has_value()) return ifErr;
  return runTabstatCore(varlist, byVal, statsVal, *dataPathOpt, wantGroups, noTotal);
}

// BUG-1014: the weighted form of runTabstat. Not in piet/handlers.h (outside
// this lane's manifest); forward-declared in dispatcher.cpp like
// runSummarizeAweight. `weightFlag` is "--aweight" or "--fweight".
ojson runTabstatWeighted(const std::vector<std::string>& varlist,
                         const std::optional<std::string>& byVal,
                         const std::optional<std::string>& statsVal, SessionState& session,
                         const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal,
                         const std::string& weightFlag, const std::string& weightVar) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!dataPathOpt.has_value()) return ifErr;
  return runTabstatCore(varlist, byVal, statsVal, *dataPathOpt, wantGroups, noTotal, weightFlag,
                        weightVar);
}

ojson runTabstatOnData(const std::vector<std::string>& varlist,
                        const std::optional<std::string>& statsVal, SessionState& session,
                        const std::string& dataPath) {
  (void)session;  // slice already materialized by the by-prefix driver
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  return runTabstatCore(varlist, std::nullopt, statsVal, dataPath, /*wantGroups=*/false,
                        /*noTotal=*/false);
}

// BUG-1520: `by g: tabstat x [aw=w]` / `[fw=w]` -- one group's slice with the
// weight. Forward-declared in dispatcher.cpp like runTabstatWeighted.
ojson runTabstatOnDataWeighted(const std::vector<std::string>& varlist,
                               const std::optional<std::string>& statsVal,
                               const std::string& dataPath, const std::string& weightFlag,
                               const std::string& weightVar) {
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  return runTabstatCore(varlist, std::nullopt, statsVal, dataPath, /*wantGroups=*/false,
                        /*noTotal=*/false, weightFlag, weightVar);
}

// BUG-1014: correlate / pwcorr as StataBE 19.5 posts them (w2as_summary_stats_family
// sx_corr_*, sx_correlate_*, sx_pwcorr_*; w2o_rnames N18 N19). Not in
// piet/handlers.h (outside this lane's manifest); forward-declared in
// dispatcher.cpp. `weightFlag` is "--aweight", "--fweight" or "".
//  correlate: r(N) (Σw under fw), r(rho) = r(C)[1,2] (0 for one variable; not
//    posted with covariance), r(C) the correlation -- or covariance -- matrix,
//    r(cov_12) r(Var_1) r(Var_2) with covariance, r(sum_w) with aweights;
//  pwcorr (pwcorr.ado): r(N) = r(Nobs)[1,2], r(rho) = r(C)[1,2], r(C), r(Nobs),
//    and r(sig) with sig.
// A string variable is dropped from the varlist first ("(s ignored because
// string variable)", sx_corr_str); fewer than two left is r(102) for pwcorr.
ojson runCorrStata(const std::vector<std::string>& varlistIn, bool pairwise, bool sig, bool covar,
                   const std::string& weightFlag, const std::string& weightVar,
                   SessionState& session, const std::optional<std::string>& ifExpr,
                   const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::vector<std::string> varlist;
  if (Datastore* ds = session.datastore()) {
    for (const auto& v : varlistIn)
      if (!(ds->has(v) && ds->dtype(v) == "factor")) varlist.push_back(v);
  } else {
    varlist = varlistIn;
  }
  if (varlist.empty() || (pairwise && varlist.size() < 2)) {
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(102)")));
  }
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr, inRange);
  if (!dataPathOpt.has_value()) return ifErr;

  std::vector<std::string> args = {"corr", "--vars", joinCommaM4(varlist)};
  if (pairwise) args.push_back("--pairwise");
  if (sig) args.push_back("--sig");
  if (covar) args.push_back("--cov");
  if (!weightFlag.empty() && !weightVar.empty()) {
    args.push_back(weightFlag);
    args.push_back(weightVar);
  }
  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  // correlate reads only the rows with every variable present: none is r(2000)
  // "no observations", one r(2001) "insufficient observations", weighted or not
  // and with covariance too (StataBE 19.5, 2026-11-03: `correlate x1 x2 if x1 >
  // 100`, `... in 1`, `... [fw=w] in 1` with w = 5). r() then holds r(N) = 0 and,
  // after an earlier correlate, that one's r(C) -- even past `return clear`.
  // pwcorr is left as it was (`pwcorr x1 x2 in 1` is rc 0 in Stata).
  // The count is of rows: under [fw=] the engine's n_obs is the weight sum, so
  // the usable rows are counted here (a present, positive weight, as the engine
  // reads them).
  std::optional<double> usableRows;
  if (!pairwise && raw.contains("n_obs") && raw["n_obs"].is_number())
    usableRows = raw["n_obs"].get<double>();
  if (usableRows.has_value() && weightFlag == "--fweight" && !weightVar.empty()) {
    try {
      Datastore wds(*dataPathOpt);
      std::vector<const double*> cols;
      for (const auto& v : varlist) cols.push_back(wds.col(v).f64());
      const double* w = wds.col(weightVar).f64();
      int64_t rows = 0;
      for (int64_t r = 0; r < wds.nRows() && rows < 2; ++r) {
        bool ok = !std::isnan(w[r]) && w[r] > 0;
        for (const double* c : cols) ok = ok && !std::isnan(c[r]);
        if (ok) ++rows;
      }
      usableRows = static_cast<double>(rows);
    } catch (const std::exception&) {
      usableRows.reset();
    }
  }
  if (usableRows.has_value() && *usableRows <= 1.0) {
    const bool none = *usableRows <= 0.0;
    eval::replaceRClass(session.eval);
    std::vector<std::string> posted = {"N"};
    session.eval.rValues.set("N", 0.0);
    if (session.lastCorrelateC.has_value()) {
      eval::EvalMatrix c = *session.lastCorrelateC;
      c.gen = eval::nextStoreGeneration();  // a write of this command's
      session.eval.rMatrices.emplace_back("C", std::move(c));
      posted.push_back("C");
    }
    eval::declareRPosted(session.eval, "correlate", std::move(posted));
    return ojson(eval::errorBlock("engine_error",
                                  none ? "no observations" : "insufficient observations",
                                  std::nullopt, std::nullopt, std::nullopt,
                                  std::string(none ? "r(2000)" : "r(2001)")));
  }
  ojson corrBlock = cobraToEstimationM4(raw, "corr");
  auto cell = [](const ojson& m, size_t i, size_t j) {
    if (m.is_array() && i < m.size() && m[i].is_array() && j < m[i].size() && m[i][j].is_number())
      return m[i][j].get<double>();
    return eval::kStataMissingSentinel;
  };
  auto toMatrix = [&](const ojson& m) {
    eval::EvalMatrix out;
    out.rownames = varlist;
    out.colnames = varlist;
    for (size_t i = 0; i < varlist.size(); ++i) {
      std::vector<double> row;
      for (size_t j = 0; j < varlist.size(); ++j) {
        const double v = cell(m, i, j);
        row.push_back(v >= eval::kStataMissingSentinel ? std::numeric_limits<double>::quiet_NaN() : v);
      }
      out.data.push_back(std::move(row));
    }
    return out;
  };
  // r() is this command's from here on (an r-class command replaces all of it).
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  auto post = [&](const char* name, double v) {
    session.eval.rValues.set(name, v);
    posted.push_back(name);
  };
  auto postMatrix = [&](const char* name, const ojson& m) {
    session.eval.rMatrices.emplace_back(name, toMatrix(m));
    posted.push_back(name);
  };
  const ojson& R = raw.contains("r") ? raw["r"] : ojson();
  if (pairwise) {
    const ojson& NB = raw.contains("nobs") ? raw["nobs"] : ojson();
    post("N", cell(NB, 0, 1));
    post("rho", cell(R, 0, 1));
    postMatrix("C", R);
    postMatrix("Nobs", NB);
    if (sig && raw.contains("p")) postMatrix("sig", raw["p"]);
  } else {
    if (raw.contains("n_obs") && raw["n_obs"].is_number()) post("N", raw["n_obs"].get<double>());
    if (covar) {
      const ojson& CV = raw.contains("cov") ? raw["cov"] : ojson();
      postMatrix("C", CV);
      post("Var_1", cell(CV, 0, 0));
      if (varlist.size() >= 2) {
        post("cov_12", cell(CV, 0, 1));
        post("Var_2", cell(CV, 1, 1));
      }
    } else {
      postMatrix("C", R);
      post("rho", varlist.size() >= 2 ? cell(R, 0, 1) : 0.0);
    }
    session.lastCorrelateC = session.eval.rMatrices.back().second;
    if (weightFlag == "--aweight" && raw.contains("sum_w") && raw["sum_w"].is_number())
      post("sum_w", raw["sum_w"].get<double>());
  }
  eval::declareRPosted(session.eval, pairwise ? "pwcorr" : "correlate", std::move(posted));
  return corrBlock;
}

ojson runCorr(const std::vector<std::string>& varlist, bool pairwise, bool sig,
              SessionState& session, const std::optional<std::string>& ifExpr,
              const std::optional<std::string>& inRange) {
  return runCorrStata(varlist, pairwise, sig, /*covar=*/false, "", "", session, ifExpr, inRange);
}

ojson runTtest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const std::vector<std::string>& varlist = parsed.varlist;
  const std::string& raw = parsed.raw;
  if (varlist.empty()) {
    return engineErrorBlockM4("syntax_error: ttest requires a variable", "syntax_error");
  }

  std::vector<std::string> levelArgs;
  bool levelExplicit = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      levelExplicit = true;
      try {
        int lv = std::stoi(v.strValue);
        levelArgs = {"--level", std::to_string(lv)};
      } catch (...) {
      }
    }
  }
  // BS-048 (BETA A4): no explicit level() — fall back to the session's `set
  // level`/`set clevel` default. ttest is the ONE PIET-wired command whose
  // cobra CLI genuinely reads --level (cmd_ttest, apps/cmds/cmds_summary.cpp
  // — applies it to alpha/t_critical throughout); session.level==95 default
  // keeps this a no-op until `set level` actually runs. (buildEstimationArgvM4
  // / handlers.cpp::runRegress deliberately do NOT carry this same fallback —
  // see their own comments: cobra's feols/xtreg/logit/probit CLIs never
  // consume --level at all, so forwarding it there would trip cobra's E6
  // unconsumed-flag net instead of doing anything useful.)
  if (!levelExplicit && session.level != 95) {
    levelArgs = {"--level", std::to_string(session.level)};
  }

  std::optional<std::string> byVal;
  bool unequal = false;
  bool paired = false;
  bool reverse = false;
  bool unpaired = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
    if ((k == "unequal" || k == "welch") && v.isBool && v.boolValue) unequal = true;
    if (k == "paired" && v.isBool && v.boolValue) paired = true;
    if (k == "reverse" && v.isBool && v.boolValue) reverse = true;
    if (k == "unpaired" && v.isBool && v.boolValue) unpaired = true;
  }

  // BS-059/BS-089: level/by/unequal/welch/paired/reverse/unpaired are the
  // only ttest options this handler reads — a leftover key reads as an
  // honest reject.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kTtestKnownOpts = kVocabTtest.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kTtestKnownOpts)) {
    return *err;
  }

  // A string variable has no observations: _ttest's summarize finds none,
  // r(2000) (w2as sx_ttest_ref_str; it was a factor_misuse engine error, rc 1).
  if (Datastore* tds = session.datastore()) {
    if (tds->has(varlist[0]) && tds->dtype(varlist[0]) == "factor")
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
  }
  // `ttest x = y` is `ttest x == y` (ttest.ado turns "==" into "=" before
  // syntax). The right-hand side is read from the varlist tokens, never from the
  // raw text: a regex over the raw line found the `==` of an `if` clause, so
  // `ttest pre = post if d == 1` ran a one-sample test of pre against 1 and
  // posted its names (sx_ttest_paired_eq).
  std::optional<std::string> eqRhs;
  {
    std::string joined;
    for (const auto& t : varlist) joined += t + " ";
    const size_t eq = joined.find('=');
    if (eq != std::string::npos) {
      std::string r = joined.substr(eq + 1);
      while (!r.empty() && (r.front() == '=' || std::isspace(static_cast<unsigned char>(r.front()))))
        r.erase(r.begin());
      eqRhs = trimM4(r);
    }
  }
  std::vector<std::string> args;
  if (byVal.has_value() && !byVal->empty()) {
    args = {"ttest", "--var", varlist[0], "--by", *byVal};
    if (unequal) args.push_back("--unequal");
    if (reverse) args.push_back("--reverse");
    for (const auto& a : levelArgs) args.push_back(a);
  } else {
    if (eqRhs.has_value() && !eqRhs->empty()) {
      std::string rhs = *eqRhs;
      try {
        size_t pos;
        std::stod(rhs, &pos);  // Python's float(rhs) — value unused, only validity matters
        if (pos == rhs.size()) {
          args = {"ttest", "--var", varlist[0], "--against", rhs};
          if (paired) args.push_back("--paired");
          for (const auto& a : levelArgs) args.push_back(a);
        } else {
          throw std::invalid_argument("not fully numeric");
        }
      } catch (...) {
        // rhs is a variable name -> paired/two-sample.
        args = {"ttest", "--vars", varlist[0] + "," + rhs};
        if (paired) args.push_back("--paired");
        if (unpaired) args.push_back("--unpaired");
        if (unpaired && unequal) args.push_back("--unequal");
        if (reverse) args.push_back("--reverse");
        for (const auto& a : levelArgs) args.push_back(a);
      }
    } else if (varlist.size() >= 2) {
      args = {"ttest", "--vars", varlist[0] + "," + varlist[1]};
      if (paired) args.push_back("--paired");
      if (unpaired) args.push_back("--unpaired");
      if (unpaired && unequal) args.push_back("--unequal");
      if (reverse) args.push_back("--reverse");
      for (const auto& a : levelArgs) args.push_back(a);
    } else {
      args = {"ttest", "--var", varlist[0], "--against", "0"};
      for (const auto& a : levelArgs) args.push_back(a);
    }
  }

  // BUG-042: `ttest ... if COND` — ttest's own cobra CLI has no --keepif;
  // pre-filter via the shared row-filter infra (same helper tabstat/corr use
  // above). `parsed.ifExpr`/`parsed.inRange` are already available on this
  // handler's own argument (no signature change needed, unlike tabstat/corr).
  // BS-090 (BETA Phase G): `in` now composes with `if` via AND here too.
  ojson ifErr;
  auto dataPathOpt =
      applyIfFilterM4(session, *session.currentHbin(), parsed.ifExpr, ifErr, parsed.inRange);
  if (!dataPathOpt.has_value()) return ifErr;

  args.push_back("--data");
  args.push_back(*dataPathOpt);
  args.push_back("--tidy");

  ojson raw2;
  try {
    raw2 = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    // by() must name exactly two groups: Stata's r(420) (sx_ttest_ref_by3). The
    // engine's two_groups() says how many it found.
    const std::string msg = cobraErrorStrM4(ce);
    static const std::regex kGroups(R"(must define exactly 2 groups \(found (\d+)\))");
    std::smatch gm;
    if (std::regex_search(msg, gm, kGroups)) {
      const int found = std::stoi(gm[1].str());
      return ojson(eval::errorBlock(
          "engine_error",
          found > 2 ? "more than 2 groups found, only 2 allowed"
                    : std::to_string(found) + " group" + (found == 1 ? "" : "s") + " found, 2 required",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(420)")));
    }
    if (msg.find("no observations") != std::string::npos)
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
    return engineErrorBlockM4(msg, ce.code);
  }
  ojson ttBlock = cobraToEstimationM4(raw2, "ttest");
  // BUG-165 (W:P5): the per-group dispersions Stata prints in the group rows
  // ("Std. dev."). Forwarded HERE rather than from the shared passthrough
  // list, because `sdtest` emits the same two key names and already forwards
  // them from its own field list — adding them to the shared list moved them
  // earlier in sdtest's key order, a byte change to an output this fix has no
  // business touching.
  for (const char* k : {"sd_1", "sd_2"}) {
    if (raw2.contains(k)) ttBlock[k] = raw2[k];
  }
  // BUG-1015: r() is the engine's `r` object -- every name Stata posts for the
  // form that ran (one-sample 12, two-sample 28, paired 23; ttest.sthlp Stored
  // results) and no other: r(sd) after a one-sample test reads `.` (FU-269,
  // w2o_rnames N07), the two-sample and paired names are posted (FU-270, N09 N10).
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (raw2.contains("r") && raw2["r"].is_object()) {
    for (auto it = raw2["r"].begin(); it != raw2["r"].end(); ++it) {
      if (!it.value().is_number()) continue;
      session.eval.rValues.set(it.key(), it.value().get<double>());
      posted.push_back(it.key());
    }
  }
  eval::declareRPosted(session.eval, "ttest", std::move(posted));
  ttBlock.erase("r");  // r() carriage, not part of the displayed block
  return ttBlock;
}


}  // namespace piet::handlers
