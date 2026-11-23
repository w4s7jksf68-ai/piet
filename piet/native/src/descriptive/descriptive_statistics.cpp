// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Descriptive statistics.
#include "piet/handlers/descriptive.h"
#include "piet/eval/result_order.h"  // orderRResults (correlate lists r() as Stata does)
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
#include "core/distributions.h"  // t_twosided_pvalue / t_critical (ttesti)
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "matrix/lapack_dsyevr.h"  // BUG-1482: the clustered V's rank (clusteredVRankM4)
#include "estimation/estimation_internal.h"

#include "estimation/estimation_families_internal.h"
#include "estimation/estimation_options.h"

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
                  const std::optional<std::string>& ifExpr, bool wantGroups, bool noTotal,
                  const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");

  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr, inRange);
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
                         const std::string& weightFlag, const std::string& weightVar,
                         const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varlist.empty()) return engineErrorBlockM4("tabstat requires at least one variable");
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr, inRange);
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
                   const std::optional<std::string>& inRange, const CorrExtra& extra) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::vector<std::string> varlist;
  if (Datastore* ds = session.datastore()) {
    // No varlist means every variable; the string ones drop out below like any
    // string variable named (StataBE 19.5, bank bd2efe966c285304: bare
    // `correlate` on a dataset with one numeric variable is a 1x1 matrix).
    std::vector<std::string> requested = varlistIn;
    if (requested.empty())
      for (const auto& nm : ds->names()) requested.push_back(nm);
    for (const auto& v : requested)
      if (!(ds->has(v) && ds->dtype(v) == "factor")) varlist.push_back(v);
  } else {
    varlist = varlistIn;
  }
  if (varlist.empty() || (pairwise && varlist.size() < 2)) {
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(102)")));
  }
  // Time-series operators (`cor gnp96 L.gnp96 D.gnp96`, varlist(ts)): each such
  // term is generated under tsset onto a copy of the frame and read from there;
  // the matrices and the table keep the names as typed.
  const std::vector<std::string> shown = varlist;
  std::string basePath = *session.currentHbin();
  {
    ojson tsErr;
    const auto tsFrame = tsVarlistFrameM4(session, varlist, tsErr);
    if (!tsFrame.has_value()) return tsErr;
    if (!tsFrame->empty()) basePath = *tsFrame;
  }
  // pwcorr's listwise (casewise): the rows with every variable present
  // (pwcorr.ado's markout of the varlist), then the same pairwise computation.
  std::optional<std::string> ifUsed = ifExpr;
  if (pairwise && extra.listwise) {
    std::string miss = "!missing(" + joinCommaM4(varlist) + ")";
    ifUsed = ifExpr.has_value() && !ifExpr->empty() ? "(" + *ifExpr + ") & " + miss : miss;
  }
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, basePath, ifUsed, ifErr, inRange);
  if (!dataPathOpt.has_value()) return ifErr;

  // pwcorr's star() and print() read each pair's p-value whether or not `sig` is set.
  const bool wantP = pairwise && (sig || extra.star > 0 || extra.print > 0 || extra.adjust != 0);
  std::vector<std::string> args = {"corr", "--vars", joinCommaM4(varlist)};
  if (pairwise) args.push_back("--pairwise");
  if (wantP) args.push_back("--sig");
  if (covar) args.push_back("--cov");
  if (extra.means && !pairwise) args.push_back("--means");
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
  // pwcorr.ado's multiple-comparison adjustment of each pair's p-value, with nrho =
  // k(k-1)/2 pairs: bonferroni min(nrho*p, 1) (a p it could not form stays missing);
  // sidak min(1, 1 - (1-p)^nrho), Stata's min() taking 1 when the power is missing.
  // print(#) keeps only the p-values at or below # in r(sig) (the others stay missing).
  if (pairwise && raw.contains("p") && raw["p"].is_array()) {
    const double k = static_cast<double>(varlist.size());
    const double nrho = k * (k - 1.0) / 2.0;
    ojson& P = raw["p"];
    for (size_t i = 0; i < P.size(); ++i) {
      if (!P[i].is_array()) continue;
      for (size_t j = 0; j < P[i].size(); ++j) {
        if (i == j) continue;
        const bool have = P[i][j].is_number();
        const double p0 = have ? P[i][j].get<double>() : std::numeric_limits<double>::quiet_NaN();
        double p = p0;
        if (extra.adjust == 1 && have) p = std::min(nrho * p0, 1.0);
        if (extra.adjust == 2) p = have ? std::min(1.0, 1.0 - std::pow(1.0 - p0, nrho)) : 1.0;
        P[i][j] = std::isnan(p) ? ojson(nullptr) : ojson(p);
      }
    }
  }
  ojson rawSig = raw;
  if (pairwise && extra.print > 0 && rawSig.contains("p") && rawSig["p"].is_array()) {
    for (auto& row : rawSig["p"])
      for (auto& c : row)
        if (!(c.is_number() && c.get<double>() <= extra.print)) c = nullptr;
  }
  if (raw.contains("means") && raw["means"].is_array())
    for (size_t j = 0; j < raw["means"].size() && j < shown.size(); ++j)
      raw["means"][j]["name"] = shown[j];
  ojson corrBlock = cobraToEstimationM4(raw, "corr");
  if (shown != varlist) corrBlock["variables"] = shown;
  if (pairwise) {
    corrBlock["sig"] = sig;
    if (wantP && raw.contains("p")) corrBlock["p"] = raw["p"];
    if (extra.star > 0) corrBlock["star"] = extra.star;
    if (extra.print > 0) corrBlock["print"] = extra.print;
  } else if (extra.means && raw.contains("means")) {
    corrBlock["means"] = raw["means"];
  }
  auto cell = [](const ojson& m, size_t i, size_t j) {
    if (m.is_array() && i < m.size() && m[i].is_array() && j < m[i].size() && m[i][j].is_number())
      return m[i][j].get<double>();
    return eval::kStataMissingSentinel;
  };
  auto toMatrix = [&](const ojson& m) {
    eval::EvalMatrix out;
    out.rownames = shown;
    out.colnames = shown;
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
    if (sig && rawSig.contains("p")) postMatrix("sig", rawSig["p"]);
  } else {
    if (raw.contains("n_obs") && raw["n_obs"].is_number()) post("N", raw["n_obs"].get<double>());
    if (covar) {
      const ojson& CV = raw.contains("cov") ? raw["cov"] : ojson();
      postMatrix("C", CV);
      ojson displayRows = ojson::array();
      for (size_t i = 0; i < varlist.size(); ++i) {
        ojson row = ojson::array();
        for (size_t j = 0; j <= i; ++j) {
          const double value = cell(CV, i, j);
          row.push_back(value >= eval::kStataMissingSentinel ? "." : fmt::general(value, 8, 0));
        }
        displayRows.push_back(std::move(row));
      }
      corrBlock["covariance_display"] = std::move(displayRows);
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
  // Stata lists them `sum_w N rho` (`N rho` unweighted, `N cov_12 Var_2 Var_1`
  // for covariance) -- vs-stata truth bank, `: r(scalars)` after correlate/pwcorr.
  eval::orderRResults(session.eval, {"sum_w", "N", "rho", "cov_12", "Var_2", "Var_1"});
  eval::declareRPosted(session.eval, pairwise ? "pwcorr" : "correlate", std::move(posted));
  return corrBlock;
}

ojson runCorr(const std::vector<std::string>& varlist, bool pairwise, bool sig,
              SessionState& session, const std::optional<std::string>& ifExpr,
              const std::optional<std::string>& inRange) {
  return runCorrStata(varlist, pairwise, sig, /*covar=*/false, "", "", session, ifExpr, inRange,
                      CorrExtra());
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
  auto refuse198 = [](const std::string& msg) {
    return ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  std::string("r(198)")));
  };
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level") {
      levelExplicit = true;
      // ttest.ado's `Level(cilevel)`: a number from 10 to 99.99 with at most two
      // decimals, else r(198) -- level(150) ran with missing bounds, level(abc)
      // silently ran at 95. level(99.5) is a level: the engine reads --level with
      // stod, so the text goes through whole (truncating it to an int put every CI
      // at the wrong confidence, r(lb_*) r(ub_*)).
      const std::string lvs = v.isBool ? std::string() : trimM4(v.strValue);
      double lv = std::numeric_limits<double>::quiet_NaN();
      size_t pos = 0;
      try {
        lv = std::stod(lvs, &pos);
      } catch (...) {
        pos = 0;
      }
      if (lvs.empty() || pos != lvs.size() || std::isnan(lv))
        return refuse198("option level() incorrectly specified");
      if (!(lv >= 10.0 && lv <= 99.99))
        return refuse198("level() must be between 10 and 99.99 inclusive");
      if (std::fabs(lv * 100.0 - std::round(lv * 100.0)) > 1e-8)
        return refuse198("level() can have at most two digits after the decimal point");
      levelArgs = {"--level", lvs};
    }
  }
  // BS-048 (BETA A4): no explicit level() — fall back to the session's `set
  // level`/`set clevel` default. ttest is the ONE PIET-wired command whose
  // cobra CLI genuinely reads --level (cmd_ttest, apps/cmds/cmds_summary.cpp
  // — applies it to alpha/t_critical throughout); session.level==95 default
  // keeps this a no-op until `set level` actually runs. (buildEstimationArgvM4
  // / estimation/linear_estimation.cpp::runRegress deliberately do NOT carry this same fallback —
  // see their own comments: cobra's feols/xtreg/logit/probit CLIs never
  // consume --level at all, so forwarding it there would trip cobra's E6
  // unconsumed-flag net instead of doing anything useful.)
  if (!levelExplicit && session.level != 95) {
    levelArgs = {"--level", std::to_string(session.level)};
  }

  std::optional<std::string> byVal;
  bool unequal = false;
  bool unequalOpt = false;  // `unequal` as typed (unequal is also set by welch)
  bool welch = false;
  bool paired = false;
  bool reverse = false;
  bool unpaired = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
    if ((k == "unequal" || k == "welch") && v.isBool && v.boolValue) unequal = true;
    if (k == "unequal" && v.isBool && v.boolValue) unequalOpt = true;
    // welch is Welch's df, not Satterthwaite's (ttesti.ado TwoTest): the engine
    // must be told which, or every df/p/CI of a welch test is Satterthwaite's.
    if (k == "welch" && v.isBool && v.boolValue) welch = true;
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
  std::string lhsVar = varlist[0];
  {
    std::string joined;
    for (const auto& t : varlist) joined += t + " ";
    const size_t eq = joined.find('=');
    if (eq != std::string::npos) {
      // `ttest rep78==3` arrives as one token: the variable is what precedes the `=`.
      lhsVar = trimM4(joined.substr(0, eq));
      std::string r = joined.substr(eq + 1);
      while (!r.empty() && (r.front() == '=' || std::isspace(static_cast<unsigned char>(r.front()))))
        r.erase(r.begin());
      eqRhs = trimM4(r);
    }
  }
  // ttest.ado: with `= exp` and no `unpaired` the test is one-sample or paired,
  // where unequal and welch mean nothing -- r(198), checked before the right-hand
  // side is read. They used to be ignored and the paired test ran at rc 0.
  if (eqRhs.has_value() && !unpaired) {
    if (unequalOpt) return refuse198("unequal option invalid for one-sample or paired tests");
    if (welch) return refuse198("welch option invalid for one-sample or paired tests");
  }
  std::vector<std::string> args;
  if (byVal.has_value() && !byVal->empty()) {
    args = {"ttest", "--var", varlist[0], "--by", *byVal};
    if (unequal) args.push_back("--unequal");
    if (welch) args.push_back("--welch");
    if (reverse) args.push_back("--reverse");
    for (const auto& a : levelArgs) args.push_back(a);
  } else {
    if (eqRhs.has_value() && !eqRhs->empty()) {
      std::string rhs = *eqRhs;
      try {
        size_t pos;
        std::stod(rhs, &pos);  // Python's float(rhs) — value unused, only validity matters
        if (pos == rhs.size()) {
          args = {"ttest", "--var", lhsVar, "--against", rhs};
          if (paired) args.push_back("--paired");
          for (const auto& a : levelArgs) args.push_back(a);
        } else {
          throw std::invalid_argument("not fully numeric");
        }
      } catch (...) {
        // rhs is a variable name -> paired/two-sample.
        args = {"ttest", "--vars", lhsVar + "," + rhs};
        if (paired) args.push_back("--paired");
        if (unpaired) args.push_back("--unpaired");
        if (unpaired && unequal) args.push_back("--unequal");
        if (unpaired && welch) args.push_back("--welch");
        if (reverse) args.push_back("--reverse");
        for (const auto& a : levelArgs) args.push_back(a);
      }
    } else if (varlist.size() >= 2) {
      args = {"ttest", "--vars", varlist[0] + "," + varlist[1]};
      if (paired) args.push_back("--paired");
      if (unpaired) args.push_back("--unpaired");
      if (unpaired && unequal) args.push_back("--unequal");
      if (unpaired && welch) args.push_back("--welch");
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
    // ttesti's sample checks carry Stata's rc: r(416) "standard deviation is
    // missing" (values whose variance overflows), r(2000) for an empty sample.
    static const std::regex kRc(R"(^r\((\d+)\)$)");
    if (std::regex_match(ce.detail, kRc))
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, ce.detail));
    static const std::regex kGroups(R"(must define exactly 2 groups \(found (\d+)\))");
    std::smatch gm;
    if (std::regex_search(msg, gm, kGroups)) {
      const int found = std::stoi(gm[1].str());
      // no row with both the variable and the group: _ttest's by() finds no
      // observations, r(2000)
      if (found == 0)
        return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                      std::nullopt, std::string("r(2000)")));
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
      // null = a statistic Stata posts as missing (n = 1 has no sd; sd = 0
      // leaves t and its p-values missing): posted as `.`, not dropped.
      if (it.value().is_null()) {
        session.eval.rValues.set(it.key(), std::numeric_limits<double>::quiet_NaN());
        posted.push_back(it.key());
        continue;
      }
      if (!it.value().is_number()) continue;
      session.eval.rValues.set(it.key(), it.value().get<double>());
      posted.push_back(it.key());
    }
  }
  eval::declareRPosted(session.eval, "ttest", std::move(posted));
  ttBlock.erase("r");  // r() carriage, not part of the displayed block
  return ttBlock;
}

// ttesti: the immediate form of ttest, from summary statistics. One sample:
// `ttesti #obs #mean #sd #val`; two samples: `ttesti #obs1 #mean1 #sd1 #obs2
// #mean2 #sd2 [, unequal welch]`; both take level() and xname()/yname() (the
// row labels). Stata's own method (ttesti's OneTest/TwoTest with _ttest's
// table rows): se = sd/sqrt(n), t, df (n-1; n1+n2-2 pooled; Satterthwaite's or
// Welch's under unequal/welch), p = 2 ttail(df,|t|), the one-sided p's as
// p/2 and 1-p/2, and each row's interval mean -/+ invt(df_row, level) se.
// Bank: 3a8d742de94ef851 `ttesti 24 62.6 15.8 75`, 9bc8a7490c905276 level(90),
// 19147700e0806102 welch, 5ae23a055c3c4d91 unequal.
ojson runTtesti(const eval::ParsedCommand& parsed, SessionState& session) {
  auto rc = [](const std::string& msg, int code) {
    return ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + std::to_string(code) + ")"));
  };
  const std::vector<std::string>& a = parsed.varlist;
  if (a.size() != 4 && a.size() != 6)
    return rc("either four arguments for a one-sample test or six arguments for a two-sample "
              "test must be specified", 198);
  double level = static_cast<double>(session.level);
  bool unequal = false, welch = false;
  std::string xname = "x", yname = "y";
  bool xnameGiven = false;
  for (const auto& [k, v] : parsed.opts) {
    const std::string lk = strutil::lower(k);
    auto prefixOf = [&lk](const char* full, size_t minLen) {
      const std::string f(full);
      return lk.size() >= minLen && lk.size() <= f.size() && f.compare(0, lk.size(), lk) == 0;
    };
    if (prefixOf("level", 1) && !v.isBool) {
      const std::string t = trimM4(v.strValue);
      char* end = nullptr;
      const double lv = std::strtod(t.c_str(), &end);
      if (t.empty() || end == nullptr || *end != '\0') return rc("option level() incorrectly specified", 198);
      if (!(lv >= 10.0 && lv <= 99.99)) return rc("level() must be between 10 and 99.99 inclusive", 198);
      level = lv;
    } else if (prefixOf("xname", 1) && !v.isBool) {
      xname = trimM4(v.strValue);
      xnameGiven = !xname.empty();
      if (!xnameGiven) xname = "x";
    } else if (prefixOf("yname", 1) && !v.isBool) {
      yname = trimM4(v.strValue);
      if (yname.empty()) yname = "y";
    } else if (a.size() == 6 && prefixOf("unequal", 3) && v.isBool) {
      unequal = true;
    } else if (a.size() == 6 && prefixOf("welch", 1) && v.isBool) {
      welch = true;
    } else if (prefixOf("reverse", 3) || (a.size() == 6 && lk == "cate")) {
      return ojson(eval::unsupportedOption(k, "ttesti's " + k + " option is not implemented in this "
                                              "version of PIET."));
    } else {
      return rc("option " + k + " not allowed", 198);
    }
  }
  // _ttest check: each number as Stata confirms it.
  std::vector<double> x(a.size());
  for (size_t i = 0; i < a.size(); ++i) {
    const std::string t = trimM4(a[i]);
    if (t == ".") {
      x[i] = std::numeric_limits<double>::quiet_NaN();
      continue;
    }
    char* end = nullptr;
    x[i] = std::strtod(t.c_str(), &end);
    if (t.empty() || end == nullptr || *end != '\0' || !std::isfinite(x[i]))
      return rc("'" + t + "' found where number expected", 7);
  }
  auto checkSample = [&](size_t at, const std::string& which) -> std::optional<ojson> {
    const double n = x[at], m = x[at + 1], sd = x[at + 2];
    const std::string pre = which.empty() ? std::string() : which + " sample ";
    if (std::isnan(n))
      return rc(which.empty() ? "number of observations is missing"
                              : "number of observations of " + which + " sample is missing", 416);
    if (n != std::floor(n)) return rc("'" + trimM4(a[at]) + "' found where integer expected", 7);
    if (n < 0)
      return rc(which.empty() ? "number of observations is negative"
                              : "number of observations of " + which + " sample is negative", 411);
    if (n == 0) return rc(which.empty() ? "no observations" : which + " sample has no observations", 2000);
    if (std::isnan(m))
      return rc(which.empty() ? "mean is missing" : "mean of " + which + " sample is missing", 416);
    if (std::isnan(sd)) {
      if (n != 1)
        return rc(which.empty() ? "standard deviation is missing"
                                : "standard deviation of " + which + " sample is missing", 416);
    } else {
      if (sd < 0)
        return rc(which.empty() ? "standard deviation is negative"
                                : which + " sample has a negative standard deviation", 411);
      if (n == 1 && sd != 0)
        return rc(which.empty() ? "only one observation, but positive standard deviation"
                                : which + " sample has only one observation, but positive standard "
                                          "deviation", 499);
    }
    (void)pre;
    return std::nullopt;
  };
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  const double alpha = level == 95.0 ? 0.05 : 1.0 - level / 100.0;
  auto pvals = [](double t, double df, double& p, double& pl, double& pu) {
    p = cecon::t_twosided_pvalue(t, df);
    if (t < 0) {
      pl = p / 2.0;
      pu = 1.0 - pl;
    } else {
      pu = p / 2.0;
      pl = 1.0 - pu;
    }
  };
  auto rowCi = [&](double n, double m, double sd, double& se, double& lb, double& ub) {
    if (n == 1) sd = kNaN;
    se = sd / std::sqrt(n);
    const double q = n > 1 ? cecon::t_critical(alpha, n - 1.0) : kNaN;
    lb = m - q * se;
    ub = m + q * se;
  };
  auto& R = session.eval.rValues;
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "ttest";
  block["depvar"] = "";
  block["immediate"] = true;
  // What the renderer draws (estimation-format.js formatTtestTable), Stata's
  // one- and two-sample table: the title, a first column headed "Variable" when
  // xname() names a variable of the data, "   Group" when it names anything
  // else, nothing without xname(); one row per sample (Obs, Mean, Std. err.,
  // Std. dev., interval), Combined and diff rows for two samples; the t line,
  // H0 with the degrees of freedom, the three alternatives' p-values (bank
  // 3a8d742de94ef851, 5ae23a055c3c4d91, 19147700e0806102).
  ojson tt;
  tt["level"] = level;
  {
    std::string col1;
    if (xnameGiven) {
      Datastore* ds = session.datastore();
      col1 = ds != nullptr && ds->has(xname) ? "Variable" : "   Group";
    }
    tt["col1"] = col1;
  }
  auto ttRow = [](const std::string& name, double n, double m, double se, double sd, double lb,
                  double ub) {
    return ojson{{"name", name}, {"n", n}, {"mean", m}, {"se", se}, {"sd", sd}, {"lb", lb}, {"ub", ub}};
  };
  if (a.size() == 4) {
    if (auto e = checkSample(0, "")) return *e;
    if (std::isnan(x[3])) return rc("hypothesized value is missing", 416);
    const double n = x[0], m = x[1], m0 = x[3];
    const double sd = n == 1 ? kNaN : x[2];
    const double t = (m - m0) * std::sqrt(n) / sd;
    double se, lb, ub, p, pl, pu;
    rowCi(n, m, sd, se, lb, ub);
    pvals(t, n - 1.0, p, pl, pu);
    eval::replaceRClass(session.eval);
    R.set("N_1", n);
    R.set("mu_1", m);
    R.set("df_t", n - 1.0);
    R.set("t", t);
    R.set("p", p);
    R.set("p_l", pl);
    R.set("p_u", pu);
    R.set("se", se);
    R.set("sd_1", sd);
    R.set("lb_1", lb);
    R.set("ub_1", ub);
    eval::declareRPosted(session.eval, "ttesti",
                         {"N_1", "mu_1", "df_t", "t", "p", "p_l", "p_u", "se", "sd_1", "lb_1", "ub_1"});
    block["n_obs"] = n;
    ojson row = {{"name", xname}, {"b", m}, {"se", se}, {"t", t}, {"p", p}, {"ci_lo", lb}, {"ci_hi", ub}};
    block["coeftable"] = ojson::array({row});
    block["coefs"] = block["coeftable"];
    block["df"] = n - 1.0;
    block["p"] = p;
    block["h0"] = m0;
    tt["form"] = "one";
    tt["title"] = "One-sample t test";
    tt["xname"] = xname;
    tt["rows"] = ojson::array({ttRow(xname, n, m, se, sd, lb, ub)});
    tt["h0"] = m0;
    tt["h0_text"] = trimM4(a[3]);  // the value as typed (ttesti's H0 line)
    tt["t"] = t;
    tt["df"] = n - 1.0;
    tt["p_l"] = pl;
    tt["p"] = p;
    tt["p_u"] = pu;
    block["ttest_table"] = std::move(tt);
    return block;
  }
  if (auto e = checkSample(0, "first")) return *e;
  if (auto e = checkSample(3, "second")) return *e;
  const double n1 = x[0], m1 = x[1], n2 = x[3], m2 = x[4];
  const double s1 = n1 == 1 ? kNaN : x[2], s2 = n2 == 1 ? kNaN : x[5];
  double var, df;
  if (unequal || welch) {
    var = s1 * s1 / n1 + s2 * s2 / n2;
    const double a1 = s1 * s1 / n1, a2 = s2 * s2 / n2;
    df = welch ? -2.0 + var * var / (a1 * a1 / (n1 + 1.0) + a2 * a2 / (n2 + 1.0))
               : var * var / (a1 * a1 / (n1 - 1.0) + a2 * a2 / (n2 - 1.0));
  } else {
    df = n1 + n2 - 2.0;
    var = (((n1 - 1.0) * s1 * s1 + (n2 - 1.0) * s2 * s2) / df) * (1.0 / n1 + 1.0 / n2);
  }
  const double t = (m1 - m2) / std::sqrt(var);
  const double se = std::sqrt(var);
  double p, pl, pu;
  pvals(t, df, p, pl, pu);
  double se1, lb1, ub1, se2, lb2, ub2, sec, lbc, ubc;
  rowCi(n1, m1, s1, se1, lb1, ub1);
  rowCi(n2, m2, s2, se2, lb2, ub2);
  const double n = n1 + n2;
  const double m = (n1 * m1 + n2 * m2) / n;
  const double sc = std::sqrt(((n1 - 1.0) * s1 * s1 + n1 * (m - m1) * (m - m1) +
                               (n2 - 1.0) * s2 * s2 + n2 * (m - m2) * (m - m2)) / (n - 1.0));
  rowCi(n, m, sc, sec, lbc, ubc);
  const double qd = cecon::t_critical(alpha, df);
  const double lbd = (m1 - m2) - qd * se, ubd = (m1 - m2) + qd * se;
  eval::replaceRClass(session.eval);
  const std::vector<std::pair<const char*, double>> items = {
      {"N_1", n1},         {"mu_1", m1},        {"N_2", n2},         {"mu_2", m2},
      {"df_t", df},        {"t", t},            {"p", p},            {"p_l", pl},
      {"p_u", pu},         {"se", se},          {"sd_1", s1},        {"sd_2", s2},
      {"sd", sc},          {"se_1", se1},       {"lb_1", lb1},       {"ub_1", ub1},
      {"se_2", se2},       {"lb_2", lb2},       {"ub_2", ub2},       {"N_combined", n},
      {"mu_combined", m},  {"se_combined", sec}, {"lb_combined", lbc}, {"ub_combined", ubc},
      {"mu_diff", m1 - m2}, {"lb_diff", lbd},   {"ub_diff", ubd}};
  std::vector<std::string> posted;
  for (const auto& [k, v] : items) {
    R.set(k, v);
    posted.push_back(k);
  }
  eval::declareRPosted(session.eval, "ttesti", std::move(posted));
  block["n_obs"] = n;
  ojson row = {{"name", "diff"}, {"b", m1 - m2}, {"se", se}, {"t", t}, {"p", p}, {"ci_lo", lbd}, {"ci_hi", ubd}};
  block["coeftable"] = ojson::array({row});
  block["coefs"] = block["coeftable"];
  block["df"] = df;
  block["p"] = p;
  block["mean_1"] = m1;
  block["mean_2"] = m2;
  block["n_1"] = n1;
  block["n_2"] = n2;
  block["sd_1"] = s1;
  block["sd_2"] = s2;
  block["by"] = xname + " vs " + yname;
  tt["form"] = "two";
  tt["title"] = std::string("Two-sample t test with ") + (unequal || welch ? "unequal" : "equal") +
                " variances";
  tt["xname"] = xname;
  tt["yname"] = yname;
  tt["rows"] = ojson::array({ttRow(xname, n1, m1, se1, s1, lb1, ub1), ttRow(yname, n2, m2, se2, s2, lb2, ub2)});
  tt["combined"] = ttRow("Combined", n, m, sec, sc, lbc, ubc);
  tt["diff"] = ojson{{"name", "diff"}, {"mean", m1 - m2}, {"se", se}, {"lb", lbd}, {"ub", ubd}};
  tt["df_kind"] = welch ? "welch" : unequal ? "satterthwaite" : "pooled";
  tt["t"] = t;
  tt["df"] = df;
  tt["p_l"] = pl;
  tt["p"] = p;
  tt["p_u"] = pu;
  block["ttest_table"] = std::move(tt);
  return block;
}


}  // namespace piet::handlers
