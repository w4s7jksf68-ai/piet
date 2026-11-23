// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Vector time series.
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
#include "estimator_command_support.h"  // e3::postBV / setE*OnFit
#include "core/distributions.h"             // t_twosided_pvalue / stata_invttail / z_critical

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

namespace {

// var.ado's e() contract (vs-stata bank 47c5c432 `var close volume, nobigf`,
// 0d830c50 `var close volume`): e(b) / e(V) are equation-major and, within an
// equation, variable-major (`close:L.close close:L2.close close:L.volume
// close:L2.volume close:_cons`); e(V) is Sigma_ml (x) (X'X)^-1; e(G) is X'X / T
// with the constant first and the lags lag-major (`_cons L.close L.volume
// L2.close L2.volume`); e(Sigma) is Sigma_ml. Every number is laid out from the
// engine's own fit (cmds_varmodel.cpp: vcov, xtx, sigma, the equations), none is
// recomputed here.
// dfk (bank 2b62f0b5 `var close volume, dfk`): Sigma divides by T - m instead
// of T, so e(Sigma) and e(V) scale by c = T / (T - m), each equation's Wald
// chi2 by 1 / c, e(detsig) by c^K; e(ll_dfk) is the likelihood at that Sigma
// and e(df_m) is m. small (bank fc4ce2a4): each equation's F_# = chi2_# / (K p)
// on (K p, T - m) degrees of freedom, e(df_r) = e(df_r#) = T - m, and the
// coefficient table is t(T - m).
void postVarEResults(SessionState& session, const ojson& raw, const eval::ParsedCommand& parsed,
                     bool nobigf, bool dfk, bool small) {
  if (!raw.is_object() || !raw.contains("vars") || !raw.contains("vcov") ||
      !raw.contains("xtx") || !raw.contains("equations") || !raw.contains("sigma") ||
      !raw.contains("lags") || !raw.contains("n_obs") || !session.lastModel().has_value())
    return;
  std::vector<std::string> vars;
  for (const auto& v : raw["vars"]) vars.push_back(v.get<std::string>());
  const size_t K = vars.size();
  const size_t p = raw["lags"].get<size_t>();
  const bool cons = raw.value("constant", true);
  const size_t Kp = K * p, m = Kp + (cons ? 1 : 0), KM = K * m;
  const double T = raw["n_obs"].get<double>();
  const ojson& eqs = raw["equations"];
  if (K < 1 || eqs.size() != K || raw["vcov"].size() != KM * KM || raw["xtx"].size() != m * m ||
      !(T > 0))
    return;
  for (const auto& eq : eqs)
    if (!eq.contains("coefs") || eq["coefs"].size() != m) return;
  if (!(T > static_cast<double>(m))) return;
  const double dfr = T - static_cast<double>(m);
  const double c = dfk ? T / dfr : 1.0;
  auto lagName = [&](size_t j, size_t c) {
    return (j == 1 ? std::string("L.") : "L" + std::to_string(j) + ".") + vars[c];
  };
  auto eqName = [&](size_t a) {
    std::string n = vars[a];
    for (char& ch : n)
      if (ch == '.') ch = '_';
    return n;
  };
  // Stata order within an equation -> the engine's regressor index.
  std::vector<size_t> order;
  std::vector<std::string> within;
  for (size_t c = 0; c < K; ++c)
    for (size_t j = 1; j <= p; ++j) {
      order.push_back((j - 1) * K + c);
      within.push_back(lagName(j, c));
    }
  if (cons) {
    order.push_back(Kp);
    within.push_back("_cons");
  }
  std::vector<std::string> names;
  std::vector<size_t> full;  // Stata position -> engine system index
  std::vector<double> b;
  for (size_t a = 0; a < K; ++a)
    for (size_t r = 0; r < m; ++r) {
      names.push_back(eqName(a) + ":" + within[r]);
      full.push_back(a * m + order[r]);
      const ojson& cell = eqs[a]["coefs"][order[r]];
      if (!cell.contains("b") || !cell["b"].is_number()) return;
      b.push_back(cell["b"].get<double>());
    }
  const ojson& vc = raw["vcov"];
  std::vector<std::vector<double>> V(KM, std::vector<double>(KM, 0.0));
  for (size_t i = 0; i < KM; ++i)
    for (size_t j = 0; j < KM; ++j) {
      const ojson& x = vc[full[i] * KM + full[j]];
      if (!x.is_number()) return;
      V[i][j] = x.get<double>() * c;
    }
  e3::postBV(session, names, b, V);
  // e(bf), the full parameter vector (zeros where a constraint removed a
  // coefficient; none here), unless nobigf (bank 0d830c50: 1 x 10, row r1).
  if (!nobigf) {
    eval::EvalMatrix bf;
    bf.rownames = {"r1"};
    bf.colnames = names;
    bf.data.push_back(b);
    bool replaced = false;
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == "bf") {
        kv.second = bf;
        replaced = true;
      }
    if (!replaced) session.eval.eMatrices.emplace_back("bf", std::move(bf));
  }

  auto setM = [&session](const std::string& nm, eval::EvalMatrix mat) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = std::move(mat);
        return;
      }
    session.eval.eMatrices.emplace_back(nm, std::move(mat));
  };
  {
    std::vector<size_t> g;
    std::vector<std::string> gn;
    if (cons) {
      g.push_back(Kp);
      gn.push_back("_cons");
    }
    for (size_t j = 1; j <= p; ++j)
      for (size_t c = 0; c < K; ++c) {
        g.push_back((j - 1) * K + c);
        gn.push_back(lagName(j, c));
      }
    eval::EvalMatrix G;
    G.rownames = gn;
    G.colnames = gn;
    for (size_t i = 0; i < m; ++i) {
      std::vector<double> row;
      for (size_t j = 0; j < m; ++j) row.push_back(raw["xtx"][g[i] * m + g[j]].get<double>() / T);
      G.data.push_back(std::move(row));
    }
    setM("G", std::move(G));
  }
  {
    eval::EvalMatrix S;
    for (size_t a = 0; a < K; ++a) {
      S.rownames.push_back(eqName(a));
      S.colnames.push_back(eqName(a));
      std::vector<double> row;
      for (size_t q = 0; q < K; ++q) row.push_back(raw["sigma"][a][q].get<double>() * c);
      S.data.push_back(std::move(row));
    }
    setM("Sigma", std::move(S));
  }
  for (const char* nm : {"b", "V", "bf", "G", "Sigma"}) session.noteEOnFit(nm);
  auto num = [&raw](const char* k) {
    return raw.contains(k) && raw[k].is_number() ? raw[k].get<double>()
                                                 : std::numeric_limits<double>::quiet_NaN();
  };
  auto sc = [&session](const std::string& n, double v) {
    if (std::isfinite(v)) e3::setEScalarOnFit(session, n, v);
  };
  sc("T", T);
  sc("N_gaps", num("n_gaps"));
  sc("tmin", num("t_first"));
  sc("tmax", num("t_last"));
  sc("k", static_cast<double>(KM));
  sc("k_eq", static_cast<double>(K));
  sc("k_dv", static_cast<double>(K));
  sc("neqs", static_cast<double>(K));
  sc("mlag", static_cast<double>(p));
  sc("tparms", static_cast<double>(KM));
  sc("rank", static_cast<double>(KM));
  sc("df_eq", static_cast<double>(m));
  sc("detsig", num("det_sigma") * std::pow(c, static_cast<double>(K)));
  sc("detsig_ml", num("det_sigma"));
  if (dfk) {
    sc("ll_dfk", num("ll") - 0.5 * T * static_cast<double>(K) * std::log(c));
    sc("df_m", static_cast<double>(m));
  }
  if (small) sc("df_r", dfr);
  for (size_t a = 0; a < K; ++a) {
    const ojson& eq = eqs[a];
    const std::string i = std::to_string(a + 1);
    auto f = [&eq](const char* k) {
      return eq.contains(k) && eq[k].is_number() ? eq[k].get<double>()
                                                 : std::numeric_limits<double>::quiet_NaN();
    };
    sc("k_" + i, static_cast<double>(m));
    sc("obs_" + i, T);
    sc("df_m" + i, static_cast<double>(Kp));
    sc("ll_" + i, f("ll"));
    sc("rmse_" + i, f("rmse"));
    sc("r2_" + i, f("r2"));
    // The engine computes each form's p-value with its own distribution
    // functions (cmds_varmodel.cpp: p_chi2, p_chi2_dfk, p_F, p_F_dfk).
    // Under small an equation's test is posted as F_# / df_r# / p_# only; its
    // chi2_# is not posted (bank 9fbad83d `var ..., small dfk`: F_1 F_2, no chi2_#).
    const double chi2 = dfk ? f("chi2_dfk") : f("chi2");
    if (small) {
      sc("F_" + i, chi2 / static_cast<double>(Kp));
      sc("df_r" + i, dfr);
      sc("p_" + i, dfk ? f("p_F_dfk") : f("p_F"));
    } else {
      sc("chi2_" + i, chi2);
      sc("p_" + i, dfk ? f("p_chi2_dfk") : f("p_chi2"));
    }
  }
  std::string vlist, eqnames, lags, mdef;
  for (size_t a = 0; a < K; ++a) {
    vlist += (a ? " " : "") + vars[a];
    eqnames += eqName(a) + " ";  // var.ado appends each name and a blank
    mdef += std::string(a ? " " : "") + "predict(xb equation(" + eqName(a) + "))";
  }
  for (size_t j = 1; j <= p; ++j) lags += (j > 1 ? " " : "") + std::to_string(j);
  auto mac = [&session](const std::string& n, const std::string& v) {
    e3::setEStringOnFit(session, n, v);
  };
  mac("depvar", vlist);
  mac("endog", vlist);
  mac("eqnames", eqnames);
  mac("lags", lags);
  mac("title", "Vector autoregression");
  mac("predict", "var_p");
  mac("marginsok", "xb default");
  mac("marginsdefault", mdef);
  mac("properties", "b V");
  if (session.timevar.has_value()) {
    mac("timevar", *session.timevar);
    if (Datastore* ds = session.datastore(); ds != nullptr && ds->has(*session.timevar))
      mac("tsfmt", session.displayFormat(ds->varinfo(*session.timevar)));
  }
  if (!cons) mac("nocons", "nocons");
  if (nobigf) mac("bigf", "nobigf");
  if (dfk) mac("dfk", "dfk");
  if (small) mac("small", "small");
  // e(vce) is republished by every model install (as logit's `oim`).
  session.eval.setEString("vce", "unadjusted");
  (void)parsed;
}

}  // namespace

ojson runVar(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-657: var.ado runs `tsset, noquery` -> `_ts`, whose refusal is "time
  // variable not set, use tsset varname ..." r(111) (the time-series helper); was
  // unsupported_option = r(198).
  if (!session.timevar.has_value() || session.timevar->empty())
    return ojson(eval::errorBlock("engine_error", "time variable not set, use tsset varname ...",
                                  std::nullopt, std::nullopt, std::nullopt, "r(111)"));
  if (parsed.varlist.size() < 2)
    return engineErrorBlockM4("syntax_error: var requires at least two endogenous variables",
                              "syntax_error");
  std::string lags = "1/2";
  bool lutstats = false, nobigf = false, dfk = false, small = false, noconstant = false;
  double level = static_cast<double>(session.level);
  // var's iteration controls belong to its constrained (iterated SUR) fit: noisure,
  // log / nolog, tolerance() and iterate() without constraints() are r(198), in
  // that order of checking (var's own syntax checks).
  // The keys arrive as typed: each is matched at var's own minimum abbreviation
  // (noISure, LOg, NOLOg, TOLerance, ITerate), and a shorter prefix of one of
  // them names no option of var (r(198) "option ... not allowed").
  {
    auto refuse = [](const std::string& msg) {
      return ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                    "r(198)"));
    };
    struct IterOpt { const char* full; size_t minLen; size_t fromLen; };
    // fromLen: the shortest prefix that can only mean this option among var's
    // options (`l` is Level, `la` LAgs, `lu` LUTstats; `no` starts several).
    static const IterOpt kIterOpts[] = {{"noisure", 4, 3}, {"log", 2, 2}, {"nolog", 4, 3},
                                        {"tolerance", 3, 1}, {"iterate", 2, 1}};
    bool hasConstraints = false, isure = false, logOpt = false, tol = false, iter = false;
    for (const auto& [k, v] : parsed.opts) {
      if (k == "constraints") {
        hasConstraints = true;
        continue;
      }
      for (const auto& o : kIterOpts) {
        const std::string full(o.full);
        if (k.size() < o.fromLen || k.size() > full.size() || full.compare(0, k.size(), k) != 0)
          continue;
        if (k.size() < o.minLen)
          return refuse("option " + k + (v.isBool ? std::string() : std::string("()")) +
                        " not allowed");
        if (full == "noisure") isure = true;
        else if (full == "log" || full == "nolog") logOpt = true;
        else if (full == "tolerance") tol = true;
        else iter = true;
      }
    }
    if (!hasConstraints) {
      if (isure) return refuse("noisure cannot be specified without constraints()");
      if (logOpt) return refuse("log and nolog cannot be specified without constraints()");
      if (tol) return refuse("tolerance() cannot be specified without constraints()");
      if (iter) return refuse("iterate() cannot be specified without constraints()");
    }
  }
  // vce(): var takes unadjusted (the default) and robust. Robust replaces e(V) by
  // the equation-stacked sandwich, which this build does not compute: refused by
  // name, never fitted as the default. Any other type is r(198).
  for (const auto& [k, v] : parsed.opts) {
    if (k != "vce") continue;
    std::istringstream in(v.isBool ? std::string() : trimM4(v.strValue));
    std::string word, extra;
    in >> word;
    std::getline(in, extra);
    const std::string& w = word;  // option names are case-sensitive
    const auto spells = [&w](const std::string& full, size_t minLen) {
      return w.size() >= minLen && w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
    };
    if (w.empty() || !trimM4(extra).empty())
      return ojson(eval::errorBlock("syntax_error", "option vce() incorrectly specified",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    if (spells("robust", 1))
      return engineErrorBlockM4(
          "Option `vce(robust)` is a valid Stata option that this version of PIET does not "
          "implement for `var`.",
          "unsupported_option");
    if (!spells("unadjusted", 2))
      return ojson(eval::errorBlock("syntax_error", "vcetype " + word + " not allowed",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }
  for (const auto& [k, v] : parsed.opts) {
    // var's syntax: LAgs() (two letters at least) and Level(cilevel): `l(2 4)`
    // is level(), whose argument is one number -- r(198) "2 4 found where
    // number expected" (vs-stata bank d47b32973752ac73).
    if (!k.empty() && k.size() <= 5 && std::string("level").compare(0, k.size(), k) == 0 &&
        !v.isBool) {
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
      level = lv;
    }
    if (k == "lags" && !v.isBool) {
      // lags(numlist) names the lags themselves: lags(2) is the second lag alone and
      // lags(2 4) the second and fourth (bank ec86b3b2: N 244, the fourth lag's
      // sample). The engine fits the contiguous lags 1..p only, so any other list is
      // refused rather than fitted as 1..max (it used to read `2 4` as 1..2).
      std::set<long> want;
      bool ok = true;
      std::istringstream in(trimM4(v.strValue));
      std::string tok;
      while (ok && in >> tok) {
        const size_t sl = tok.find('/');
        try {
          size_t used = 0;
          const long a = std::stol(tok.substr(0, sl), &used);
          ok = used == (sl == std::string::npos ? tok.size() : sl);
          long b = a;
          if (ok && sl != std::string::npos) {
            const std::string rest = tok.substr(sl + 1);
            b = std::stol(rest, &used);
            ok = used == rest.size();
          }
          for (long j = a; ok && j <= b; ++j) want.insert(j);
        } catch (const std::exception&) {
          ok = false;
        }
      }
      const long p = want.empty() ? 0 : *want.rbegin();
      if (ok && p >= 1 && *want.begin() == 1 && static_cast<long>(want.size()) == p) {
        lags = "1/" + std::to_string(p);
      } else if (ok && !want.empty()) {
        return engineErrorBlockM4(
            "Option `lags(" + trimM4(v.strValue) +
                ")` names a lag list other than 1/p, which this version of PIET does not "
                "implement for `var` (only the contiguous lags 1 through p are fitted).",
            "unsupported_option");
      } else {
        lags = trimM4(v.strValue);  // the engine refuses what it cannot read
      }
    } else if ((k == "exog" || k == "constraints" || k == "dfk") && !v.isBool)
      return engineErrorBlockM4(
          "Option `" + k + "()` is a valid Stata option that this version of PIET does not "
          "implement for `var`.",
          "unsupported_option");
    // noconstant drops the constant from every equation; the engine's equations
    // always carry one, so fitting without it posted the constant model's numbers
    // (bank e21e2db0 `var close volume, noconstant`: e(sbic) 26.6985 against 26.6003).
    else if (k == "noconstant" && v.isBool && v.boolValue)
      noconstant = true;
    else if (k == "lutstats" && v.isBool && v.boolValue)
      lutstats = true;
    else if (k == "nobigf" && v.isBool && v.boolValue)
      nobigf = true;
    else if (k == "dfk" && v.isBool && v.boolValue)
      dfk = true;
    else if (k == "small" && v.isBool && v.boolValue)
      small = true;
  }
  std::vector<std::string> argv = {"var", "--y", joinCommaM4(parsed.varlist), "--lags", lags,
                                   "--tsset", *session.timevar, "--irf-steps", "8"};
  if (noconstant) argv.push_back("--noconstant");
  // `in a/b` restricts the estimation rows as `if` does -- the rows outside still
  // supply lagged values (vs-stata bank c53a2733 `var close volume in 10/200`,
  // 86dd66d7 `if t>20 in 1/200`): the range joins the `if` as an observation-number
  // condition on the engine's in-sample mask.
  std::optional<std::string> ifEff = parsed.ifExpr;
  if (auto err = inRangeConditionM4(session, parsed, ifEff, "var")) return *err;
  if (ifEff.has_value() && !ifEff->empty()) {
    argv.push_back("--keepif");
    argv.push_back(*ifEff);
    // the rows `if` excludes supply lags only within the sample's own panel
    if (session.panelvar.has_value() && !session.panelvar->empty()) {
      argv.push_back("--panelvar");
      argv.push_back(*session.panelvar);
    }
  }
  // runP7Mle's fit, plus the engine's e(sample) (the lag sample's rows), which
  // `estimates store` records as _est_<name> (StataBE: `var close volume` then
  // `estimates store v1` adds a variable). The --esample-out flag is not recorded.
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
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (!raw.is_object() || !raw.contains("esample_n")) {
    cecon::native_unlink(esampleValues);
    esampleValues.clear();
  }
  ojson block = cobraToEstimationM4(raw, "var", &session);
  fillCoeftableStatsM4(block);
  cacheLastModelM4(session, "var", argv, block, raw, esampleValues);
  // lutstats: Lutkepohl's information criteria, ln|Sigma_ML| + c(T) * p K^2 / T
  // with c = 2, 2 ln(ln T), ln T for aic, hqic, sbic (p the number of lags, K the
  // equations), in place of the likelihood-based ones; e(lutstats) names it (bank
  // bc3357f0 `var close volume, lutstats`: detsig_ml 977784252.27, T 246, sbic
  // 20.8798348 = ln(detsig_ml) + ln(246)/246 * 8).
  if (lutstats && raw.is_object() && raw.contains("det_sigma") && raw["det_sigma"].is_number() &&
      raw.contains("n_obs") && raw["n_obs"].is_number() && raw.contains("lags") &&
      raw["lags"].is_number() && raw.contains("vars") && raw["vars"].is_array() &&
      session.lastModel().has_value()) {
    const double det = raw["det_sigma"].get<double>(), T = raw["n_obs"].get<double>();
    const double K = static_cast<double>(raw["vars"].size()), p = raw["lags"].get<double>();
    if (det > 0 && T > 1) {
      const double arparms = p * K * K, base = std::log(det);
      const double aic = base + 2.0 * arparms / T;
      const double hqic = base + 2.0 * std::log(std::log(T)) / T * arparms;
      const double sbic = base + std::log(T) / T * arparms;
      session.patchLastModelResult("aic", aic);
      session.patchLastModelResult("hqic", hqic);
      session.patchLastModelResult("sbic", sbic);
      raw["aic"] = aic;
      raw["hqic"] = hqic;
      raw["sbic"] = sbic;
      if (block.is_object()) {
        for (const char* key : {"aic", "hqic", "sbic"})
          if (block.contains(key)) block[key] = raw[key];
      }
      session.eval.setEString("lutstats", "lutstats");
      session.noteEOnFit("lutstats");
    }
  }
  postVarEResults(session, raw, parsed, nobigf, dfk, small);
  // The printed table follows dfk (se scaled by sqrt(T / (T - m))), small (t on
  // T - m) and level(): the engine's rows are the ML, normal, 95% ones.
  if (block.is_object() && block.contains("outcomes") && raw.contains("n_obs") &&
      raw.contains("parms_per_eq")) {
    const double T = raw["n_obs"].get<double>(), m = raw["parms_per_eq"].get<double>();
    if (T > m) {
      const double sc = dfk ? std::sqrt(T / (T - m)) : 1.0, dfr = T - m;
      const double crit = small ? cecon::stata_invttail(dfr, (1.0 - level / 100.0) / 2.0)
                                : cecon::z_critical(level == 95.0 ? 0.05 : 1.0 - level / 100.0);
      for (auto& out : block["outcomes"]) {
        if (small) out["dof_resid"] = dfr;
        if (!out.contains("coeftable")) continue;
        for (auto& row : out["coeftable"]) {
          if (!row.contains("b") || !row["b"].is_number() || !row.contains("se") ||
              !row["se"].is_number())
            continue;
          const double b = row["b"].get<double>(), se = row["se"].get<double>() * sc;
          row["se"] = se;
          if (!(se > 0)) continue;
          const double t = b / se;
          row["t"] = t;
          row["p"] = small ? cecon::t_twosided_pvalue(t, dfr) : std::erfc(std::fabs(t) / std::sqrt(2.0));
          row["ci_lo"] = b - crit * se;
          row["ci_hi"] = b + crit * se;
        }
      }
      if (small) block["var_small_df_r"] = dfr;
      session.lastEstBlock = block;
    }
  }
  // vargranger's table is `test` on the posted e(b)/e(V): its statistics follow
  // dfk (the covariance scaled by T / (T - m)) and small (F on T - m). The rows
  // in the form this fit posts are kept on the model's own payload, so
  // `vargranger` after `estimates restore` (or with estimates()) reads the
  // restored model's rows.
  if (raw.is_object() && raw.contains("granger") && raw["granger"].is_array()) {
    ojson rows = ojson::array();
    auto numOr = [](const ojson& g, const char* k) {
      return g.contains(k) && g[k].is_number() ? g[k] : ojson(nullptr);
    };
    for (const auto& g : raw["granger"]) {
      ojson r;
      r["equation"] = g.value("equation", "");
      r["excluded"] = g.value("excluded", "");
      const ojson chi2 = numOr(g, dfk ? "chi2_dfk" : "chi2");
      const ojson df = numOr(g, "df");
      if (small) {
        r["F"] = chi2.is_number() && df.is_number() && df.get<double>() > 0
                     ? ojson(chi2.get<double>() / df.get<double>())
                     : ojson(nullptr);
        r["df"] = df;
        r["df_r"] = numOr(g, "df_r");
        r["p"] = numOr(g, dfk ? "p_F_dfk" : "p_F");
      } else {
        r["chi2"] = chi2;
        r["df"] = df;
        r["p"] = numOr(g, dfk ? "p_chi2_dfk" : "p");
      }
      rows.push_back(std::move(r));
    }
    session.patchLastModelResult("granger", rows);
    session.patchLastModelResult("granger_small", small);
  }
  return block;
}

ojson runVargranger(const eval::ParsedCommand& parsed, SessionState& session) {
  // vargranger.ado's `syntax [, ESTimates(string) SEParator(integer 0)]`: the
  // capitalised stem is each option's minimum, and anything else is r(198) (bank
  // b7882e81 `e(v1)` / 39509333 `es(v1)` "option e() not allowed"; f0960560
  // `zznotanoption`). estimates(NAME) runs on the stored VAR results named, the
  // active results staying where they are (bank 599d7cb4: `var close volume`,
  // `estimates store v1`, `regress close volume`, `vargranger, estimates(v1)`
  // prints the VAR's tests and e(cmd) is still regress).
  std::optional<std::string> estName;
  for (const auto& [k, v] : parsed.opts) {
    const std::string lk = lowerM4(k);
    const auto spells = [&lk](const std::string& full, size_t minLen) {
      return lk.size() >= minLen && lk.size() <= full.size() && full.compare(0, lk.size(), lk) == 0;
    };
    if (spells("estimates", 3) && !v.isBool) {
      estName = trimM4(v.strValue);
    } else if (spells("separator", 3) && !v.isBool) {
      // a display rule only
    } else {
      return ojson(eval::errorBlock("engine_error",
                                    "option " + k + (v.isBool ? std::string() : std::string("()")) +
                                        " not allowed",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }
  const ojson* src = nullptr;
  if (estName.has_value()) {
    const auto it = session.estimatesModels.find(*estName);
    if (it == session.estimatesModels.end() || !it->second)
      return ojson(eval::errorBlock("engine_error", "estimation result " + *estName + " not found",
                                    std::nullopt, std::nullopt, std::nullopt, "r(111)"));
    const SessionState::StoredModel& sm = *it->second;
    if (sm.lastModelResult.has_value() && sm.lastModelResult->is_object() &&
        sm.lastModelResult->contains("granger"))
      src = &*sm.lastModelResult;
    else
      return engineErrorBlockM4("vargranger: the stored results " + *estName +
                                    " are not VAR results this version of PIET can read.",
                                "unsupported_option");
  } else {
    // vargranger.ado: `estimates restore .` with no active results is r(301)
    // "cannot restore estimates(.)" (vs-stata bank 6a9c01c7); results of another
    // command are r(198).
    if (!session.lastModel().has_value())
      return ojson(eval::errorBlock("engine_error", "cannot restore estimates(.)", std::nullopt,
                                    std::nullopt, std::nullopt, "r(301)"));
    if (session.lastModel()->cobraCmd != "var")
      return ojson(eval::errorBlock("engine_error",
                                    "vargranger only works with estimates from var, svar, "
                                    "ivsvar, or xtvar",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    // The ACTIVE results' tests (vargranger restores `.`), never an earlier
    // or later fit's: after `estimates restore` the restored VAR answers.
    const auto& active = session.lastModel()->result;
    if (!active.has_value() || !active->is_object() || !active->contains("granger"))
      return engineErrorBlockM4("vargranger: the active VAR results carry no Granger tests "
                                "this version of PIET can read.",
                                "unsupported_option");
    src = &*active;
  }
  const ojson& raw = *src;
  ojson rows = ojson::array();
  // r(gstats): one row per test, `equation:excluded`, columns chi2 df p (bank
  // 599d7cb4: 4 x 3, rows close:volume close:ALL volume:close volume:ALL); after
  // a small-sample fit the tests are F tests, columns F df df_r p.
  const bool small = raw.value("granger_small", false);
  const std::vector<const char*> cols =
      small ? std::vector<const char*>{"F", "df", "df_r", "p"}
            : std::vector<const char*>{"chi2", "df", "p"};
  eval::EvalMatrix gs;
  for (const char* c : cols) gs.colnames.push_back(c);
  bool complete = raw["granger"].is_array() && !raw["granger"].empty();
  for (const auto& g : raw["granger"]) {
    ojson r;
    r["equation"] = g.value("equation", "");
    r["excluded"] = g.value("excluded", "");
    for (const char* key : cols) r[key] = g.contains(key) ? g[key] : ojson(nullptr);
    rows.push_back(r);
    gs.rownames.push_back(g.value("equation", std::string()) + ":" + g.value("excluded", std::string()));
    std::vector<double> row;
    for (const char* key : cols) {
      if (g.contains(key) && g[key].is_number()) row.push_back(g[key].get<double>());
      else complete = false;
    }
    gs.data.push_back(std::move(row));
  }
  eval::replaceRClass(session.eval);
  if (complete) {
    session.eval.rMatrices.emplace_back("gstats", std::move(gs));
    eval::declareRPosted(session.eval, "vargranger", {"gstats"});
  }
  ojson b;
  b["type"] = "vargranger";
  b["title"] = "Granger causality Wald tests";
  b["small"] = small;
  b["rows"] = rows;
  return b;
}

// `irf create NAME, step(#) set("PATH", replace)` — materializes the stored IRF
// cells as a Stata `.irf` file, which is an ordinary `.dta`.
ojson runIrf(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty() || lowerM4(trimM4(parsed.varlist[0])) != "create")
    return engineErrorBlockM4(
        "`irf` subcommands other than `create` are valid Stata commands that this version of PIET "
        "does not implement.",
        "unsupported_command");
  // irf_create.ado: no active results is r(301) "estimates not found"; another
  // command's results r(198) "cannot compute IRFs after <cmd>".
  if (!session.lastModel().has_value())
    return ojson(eval::errorBlock("engine_error", "estimates not found", std::nullopt,
                                  std::nullopt, std::nullopt, "r(301)"));
  {
    const LastModel& lm = *session.lastModel();
    const std::string ecmd = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
    // Stata computes IRFs after var and svar, vec, arima / arfima, arch (with
    // condition), dsge, lpirf, ivsvar, xtvar and any e(irf_cmd); PIET builds them
    // after var only, so the others are refused as not implemented. Results of any
    // other command are r(198).
    static const std::set<std::string> kIrfFamilies = {
        "svar", "vec", "arima", "arfima", "arch", "dsge", "dsgenl", "lpirf", "ivsvar", "xtvar"};
    if (lm.cobraCmd != "var") {
      if (kIrfFamilies.count(ecmd) != 0)
        return engineErrorBlockM4("`irf create` after `" + ecmd +
                                      "` is valid Stata that this version of PIET does not "
                                      "implement (IRFs are built after `var` only).",
                                  "unsupported_command");
      return ojson(eval::errorBlock("engine_error", "cannot compute IRFs after " + ecmd,
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    }
  }
  // The active VAR's own payload: after `estimates restore` it is the restored fit's.
  const auto& activeVar = session.lastModel()->result;
  if (!activeVar.has_value() || !activeVar->is_object() || !activeVar->contains("irf"))
    return engineErrorBlockM4("irf create: the active VAR results carry no IRF cells this "
                              "version of PIET can read.",
                              "unsupported_option");
  const std::string irfname = parsed.varlist.size() > 1 ? parsed.varlist[1] : "irf1";
  // varirf_create.ado's syntax: Order() STep() replace set() BS BSP BSAving()
  // Reps(numlist integer max=1 >=50) noSE noDots ESTimates() Level(), plus
  // irf_create.ado's BAYESprefix. Anything else is r(198) (vs-stata bank aae03acc
  // `irf create order1, set(myirf) zznotanoption`); reps() below 50 is the numlist
  // range error r(125) (bank cf8d9f89 `... bs reps(20)`). Only step() and set()
  // are honoured: the others change what is computed (ordering, bootstrap
  // standard errors, their level) and are refused by name rather than ignored.
  {
    static const std::vector<std::pair<std::string, size_t>> kIrfOpts = {
        {"order", 1},  {"step", 2},  {"replace", 7}, {"set", 3},       {"bs", 2},
        {"bsp", 3},    {"bsaving", 3}, {"reps", 1},  {"nose", 4},      {"nodots", 3},
        {"estimates", 3}, {"level", 1}, {"bayesprefix", 11}};
    for (const auto& [k0, v] : parsed.opts) {
      const std::string k = lowerM4(k0);
      std::string canon;
      for (const auto& [name, minLen] : kIrfOpts)
        if (k.size() >= minLen && k.size() <= name.size() && name.compare(0, k.size(), k) == 0) {
          canon = name;
          break;
        }
      if (canon.empty())
        return ojson(eval::errorBlock("unsupported_option", "option " + k0 + " not allowed",
                                      std::nullopt, std::nullopt, std::nullopt, "r(198)"));
      if (canon == "reps" && !v.isBool) {
        const std::string a = trimM4(v.strValue);
        char* end = nullptr;
        const double r = std::strtod(a.c_str(), &end);
        if (!a.empty() && end != nullptr && *end == '\0' && r == std::floor(r) && r < 50)
          return ojson(eval::errorBlock(
              "syntax_error", "reps() invalid -- invalid number, outside of allowed range",
              std::nullopt, std::nullopt, std::nullopt, "r(125)"));
      }
    }
    for (const auto& [k0, v] : parsed.opts) {
      const std::string k = lowerM4(k0);
      if (k.rfind("st", 0) == 0 || k == "set" || k == "replace" || k == "nodots" ||
          (k.size() >= 3 && std::string("nodots").compare(0, k.size(), k) == 0))
        continue;
      return engineErrorBlockM4("Option `" + k0 +
                                    "` is a valid Stata option that this version of PIET does "
                                    "not implement for `irf create` (only step() and set() are "
                                    "honoured).",
                                "unsupported_option");
    }
  }
  std::string setPath;
  long steps = 8;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "set" && !v.isBool) {
      setPath = trimM4(v.strValue);
      // `set("path", replace)` — strip the option word and the quotes.
      size_t comma = setPath.find(',');
      if (comma != std::string::npos) setPath = trimM4(setPath.substr(0, comma));
      if (setPath.size() >= 2 && setPath.front() == '"' && setPath.back() == '"')
        setPath = setPath.substr(1, setPath.size() - 2);
    } else if (k.size() >= 2 && std::string("step").compare(0, k.size(), lowerM4(k)) == 0 &&
               !v.isBool) {
      steps = std::strtol(trimM4(v.strValue).c_str(), nullptr, 10);
    }
  }
  if (setPath.empty())
    return engineErrorBlockM4("syntax_error: irf create requires set(\"filename\")",
                              "syntax_error");
  if (setPath.size() < 4 || setPath.substr(setPath.size() - 4) != ".irf") setPath += ".irf";

  // Build the cell table as CSV, convert it, and export it as the .dta the .irf
  // format actually is. PIET has no dataset writer of its own, so this reuses the
  // engine verbs rather than growing one.
  const ojson& cells = (*activeVar)["irf"]["cells"];
  static const char* kNumCols[] = {"irf",     "cirf",     "oirf",   "coirf",  "sirf",
                                   "dm",      "cdm",      "stdirf", "stdcirf", "stdoirf",
                                   "stdcoirf", "stdsirf", "stddm",  "stdcdm", "fevd",
                                   "sfevd",   "mse",      "stdfevd", "stdsfevd"};
  std::ostringstream csv;
  csv << "irfname,step";
  for (const char* c : kNumCols) csv << "," << c;
  csv << ",response,impulse\n";
  int64_t n_written = 0;
  for (const auto& c : cells) {
    if (c.value("step", 0) > steps) continue;
    csv << irfname << "," << c.value("step", 0);
    for (const char* k : kNumCols) {
      csv << ",";
      if (c.contains(k) && !c[k].is_null()) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", c[k].get<double>());
        csv << buf;
      }
    }
    csv << "," << c.value("response", std::string()) << "," << c.value("impulse", std::string())
        << "\n";
    ++n_written;
  }
  const std::string csvPath = nextPathM4(session.workdir(), "irf_cells") + ".csv";
  {
    std::ofstream f(csvPath);
    if (!f) return engineErrorBlockM4("irf create: could not write the intermediate table",
                                      "data_io");
    f << csv.str();
  }
  const std::string hbinPath = nextPathM4(session.workdir(), "irf_frame");
  CobraRunResult cv = runCobraOneShot({"convert", "--data", csvPath, "--out", hbinPath,
                                       "--factor", "irfname,response,impulse"});
  if (!cv.ok)
    return engineErrorBlockM4("irf create: could not build the IRF table (" +
                                  (cv.stderr_text.empty() ? cv.error : cv.stderr_text) + ")",
                              "engine_error");
  CobraRunResult ex = runCobraOneShot({"export-dta", "--data", hbinPath, "--out", setPath});
  if (!ex.ok)
    return engineErrorBlockM4("irf create: could not write " + setPath + " (" +
                                  (ex.stderr_text.empty() ? ex.error : ex.stderr_text) + ")",
                              "engine_error");
  ojson b;
  b["type"] = "irf";
  b["subcommand"] = "create";
  b["irfname"] = irfname;
  b["file"] = setPath;
  b["n_rows"] = n_written;
  b["steps"] = steps;
  b["notes"] = ojson::array({"(file " + setPath + " created)", "(file " + setPath + " now active)",
                             "(file " + setPath + " updated)"});
  return b;
}

// ===========================================================================
// P7 — multiple imputation: `mi set`, `mi register`, `mi impute regress`
//
// The mlong ROWS are real data: the engine writes the whole 4,800 + M×129 frame
// itself (`--out-data`), because it already holds both the source frame and the
// draws — reassembling them here would mean round-tripping the values back
// through a CSV merge, which is more moving parts for the same result and a
// second place for the row order to drift. SessionState keeps the declaration
// only, so `mi impute` can reject honestly on un-set or unregistered data.


}  // namespace piet::handlers
