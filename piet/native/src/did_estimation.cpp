// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Did estimation.
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

namespace extended_fit_detail {

ojson coefRowM4(const std::string& name, const ojson& att, const ojson& se) {
  ojson r;
  r["name"] = name;
  if (att.is_null() || se.is_null()) {
    r["b"] = att;
    r["se"] = se;
    r["t"] = nullptr;
    r["p"] = nullptr;
    r["ci_lo"] = nullptr;
    r["ci_hi"] = nullptr;
    return r;
  }
  try {
    double attF = att.get<double>();
    double seF = se.get<double>();
    double z = (seF != 0.0) ? attF / seF : std::nan("");
    r["b"] = attF;
    r["se"] = seF;
    r["t"] = z;
    r["p"] = seF != 0.0 ? ojson(normalPvalue(z)) : ojson(nullptr);
    r["ci_lo"] = attF - kZ975 * seF;
    r["ci_hi"] = attF + kZ975 * seF;
  } catch (...) {
    r["b"] = att;
    r["se"] = se;
    r["t"] = nullptr;
    r["p"] = nullptr;
    r["ci_lo"] = nullptr;
    r["ci_hi"] = nullptr;
  }
  return r;
}

// _csdid_to_estimation (dispatcher.py) — normalise a csdid result JSON into
// an estimation block for the requested aggregate.
// csdidToEstimationM4: hoisted below the anonymous block (R2) — TU c consumes it.

void cacheCsdidModel(SessionState& session, const std::vector<std::string>& argv, const ojson& raw) {
  auto optFor = [&](const std::string& flag) -> std::optional<std::string> {
    for (size_t i = 0; i < argv.size(); ++i) {
      if (argv[i] == flag && i + 1 < argv.size()) return argv[i + 1];
    }
    return std::nullopt;
  };
  LastModel lm;
  lm.cobraCmd = "csdid";
  lm.y = optFor("--y").value_or("");
  std::string xStr = optFor("--x").value_or("");
  if (!xStr.empty()) {
    std::string cur;
    for (char c : xStr) {
      if (c == ',') {
        if (!cur.empty()) lm.x.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) lm.x.push_back(cur);
  }
  lm.depvar = lm.y;
  captureFitFilterM4(lm, argv);
  lm.dataPath = session.currentHbin().value_or("");
  lm.ivar = optFor("--ivar");
  lm.timevar = optFor("--time");
  lm.gvar = optFor("--gvar");
  lm.method = optFor("--method");
  lm.comparison = optFor("--comparison");
  lm.pscoretrim = optFor("--pscoretrim");
  auto clusterVar = optFor("--cluster");
  if (clusterVar.has_value()) lm.cluster = {*clusterVar};
  lm.long2 = std::find(argv.begin(), argv.end(), "--long2") != argv.end();
  // csdid posts no e(vce)/e(vcetype) (csdid; w2ag log:167
  // ag_def_vce `||||.|`, w2l_vce_shape_det csdid_* `csdid|||.|180`): an empty
  // vcov makes setLastModel erase both.
  lm.vcov = "";
  lm.result = raw;
  session.setLastModel(lm);
}

// FU-23 (BUG-756): the SHAPE of the VCE options, read once for the four
// handlers below (csdid, ivprobit/ivtobit, boottest, xtpoisson/xtlogit ,fe).
// Each of them used to read `cluster` only when it carried a value
// (`k == "cluster" && !v.isBool`), so a bare `cluster` never reached their
// checks: xtpoisson/xtlogit ,fe and ivprobit/ivtobit fitted the iid model at
// rc 0, and boottest ran with the fit's clusters. A valued `robust(x)` and a
// trailing token in `vce(robust x)` were skipped the same way. What each
// command does with each shape differs and is the oracle's to say
// (tests/eval/golden/aer_w2/w2l_vce_shape.log), so this only classifies; the
// handlers map a shape to Stata's answer.




OptShapeM4b shapeOfM4b(const eval::OptValue& v, std::vector<std::string>* toks) {
  if (v.isBool) return OptShapeM4b::kBare;
  *toks = splitTokensM4(v.strValue);
  if (toks->empty()) return OptShapeM4b::kEmpty;
  return toks->size() == 1 ? OptShapeM4b::kOne : OptShapeM4b::kMany;
}

VceShapesM4b readVceShapesM4b(const eval::ParsedCommand& parsed) {
  VceShapesM4b s;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "cluster") {
      s.cluster = shapeOfM4b(v, &s.clusterVars);
      s.clusterRaw = v.isBool ? std::string() : trimM4(v.strValue);
    } else if (k == "robust") {
      std::vector<std::string> toks;
      s.robust = shapeOfM4b(v, &toks);
      s.robustRaw = v.isBool ? std::string() : trimM4(v.strValue);
    } else if ((k == "vce" || k == "vcov") && !s.sawVce) {
      s.sawVce = true;
      s.vceRaw = v.isBool ? std::string() : trimM4(v.strValue);
      std::string rest;
      s.vceType = canonicalVceType(s.vceRaw, &rest);
      s.vceRest = splitTokensM4(rest);
    }
  }
  return s;
}

// A refusal Stata gives itself, carried with Stata's own rc (coarseRc reads the
// `r(NNN)` reason first) and the oracle's wording.
ojson stataRefusalM4b(const std::string& message, int rc) {
  return ojson(eval::errorBlock("syntax_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// Defined after runDrdid (below): drdid's fit with the treatment given as an
// expression; runCsdid's cluster()==ivar path re-runs csdid's last cell with it.
ojson drdidFitCoreM4(SessionState& session, const std::string& y,
                     const std::vector<std::string>& xs, const std::string& ivarName,
                     const std::string& timeVarName, const std::string& treatExpr,
                     const std::string& treatName, const std::optional<std::string>& ifExpr);

}  // namespace

// R2: hoisted from the anonymous namespace above so estimation_m4c.cpp can link it.
ojson csdidToEstimationM4(const ojson& raw, const std::string& subcmd) {
  ojson block;
  std::string estimator = raw.value("estimator", std::string("csdid"));
  block["type"] = "estimation";
  block["estimator"] = estimator;
  block["estat"] = subcmd;
  block["depvar"] = raw.value("depvar", "");
  block["n_obs"] = raw.contains("n_obs") ? raw["n_obs"] : raw.value("n_units", ojson(0));
  if (raw.contains("method") && !raw["method"].is_null() &&
      !(raw["method"].is_string() && raw["method"].get<std::string>().empty())) {
    block["method"] = raw["method"];
  }
  if (raw.contains("comparison") && !raw["comparison"].is_null() &&
      !(raw["comparison"].is_string() && raw["comparison"].get<std::string>().empty())) {
    block["comparison"] = raw["comparison"];
  }

  if (subcmd == "simple") {
    ojson att = raw.contains("simple_att") ? raw["simple_att"] : ojson(nullptr);
    ojson se = raw.contains("simple_se") ? raw["simple_se"] : ojson(nullptr);
    block["coeftable"] = ojson::array({coefRowM4("ATT", att, se)});
  } else if (subcmd == "event") {
    ojson rows = ojson::array();
    ojson eventStudy = ojson::array();
    for (const auto& ev : raw.value("events", ojson::array())) {
      ojson eVal = ev.value("e", ojson(nullptr));
      std::string name;
      if (eVal.is_number()) {
        double d = eVal.get<double>();
        if (d == std::floor(d)) {
          name = std::to_string(static_cast<long long>(d));
        } else {
          std::ostringstream oss;
          oss << d;
          name = oss.str();
        }
      } else if (eVal.is_string()) {
        name = eVal.get<std::string>();
      }
      ojson row = coefRowM4(name, ev.value("att", ojson(nullptr)), ev.value("se", ojson(nullptr)));
      rows.push_back(row);
      ojson es;
      es["e"] = name;
      es["att"] = row["b"];
      es["se"] = row["se"];
      es["ci_lo"] = row["ci_lo"];
      es["ci_hi"] = row["ci_hi"];
      eventStudy.push_back(es);
    }
    block["coeftable"] = rows;
    block["event_study"] = eventStudy;
    if (raw.contains("window") && !raw["window"].is_null() &&
        !(raw["window"].is_array() && raw["window"].empty())) {
      block["window"] = raw["window"];
    }
    if (raw.contains("pre_avg") && !raw["pre_avg"].is_null()) {
      block["pre_avg"] = raw["pre_avg"];
      block["pre_avg_se"] = raw.contains("pre_avg_se") ? raw["pre_avg_se"] : ojson(nullptr);
      block["post_avg"] = raw.contains("post_avg") ? raw["post_avg"] : ojson(nullptr);
      block["post_avg_se"] = raw.contains("post_avg_se") ? raw["post_avg_se"] : ojson(nullptr);
    }
  } else if (subcmd == "calendar" || subcmd == "group") {
    // calendar: ATT per outcome period, keyed "t". group: ATT per treatment
    // cohort, keyed "g" (distinct aggregations — the tables must differ).
    const char* arrKey = (subcmd == "group") ? "groups" : "calendar";
    const char* entKey = (subcmd == "group") ? "g" : "t";
    ojson rows = ojson::array();
    for (const auto& ent : raw.value(arrKey, ojson::array())) {
      ojson tVal = ent.value(entKey, ojson(nullptr));
      std::string name;
      if (tVal.is_number()) {
        double d = tVal.get<double>();
        if (d == std::floor(d)) {
          name = std::to_string(static_cast<long long>(d));
        } else {
          std::ostringstream oss;
          oss << d;
          name = oss.str();
        }
      } else if (tVal.is_string()) {
        name = tVal.get<std::string>();
      }
      rows.push_back(coefRowM4(name, ent.value("att", ojson(nullptr)), ent.value("se", ojson(nullptr))));
    }
    block["coeftable"] = rows;
  } else if (subcmd == "pretrend") {
    ojson pt = raw.value("pretrend", ojson::object());
    ojson pretrend;
    pretrend["chi2"] = pt.contains("chi2") ? pt["chi2"] : ojson(nullptr);
    pretrend["df"] = pt.contains("df") ? pt["df"] : ojson(nullptr);
    pretrend["p"] = pt.contains("p") ? pt["p"] : ojson(nullptr);
    block["pretrend"] = pretrend;
    block["coeftable"] = ojson::array();
  } else {
    block["coeftable"] = ojson::array();
  }
  return block;
}

// csdid-ematrix-post (w2ag_csdid_drdid): the engine already sends every posting
// Stata makes, with Stata's names (apps/cmds/cmds_panel.cpp cmd_csdid):
//   attgt          {names, b, V}  csdid's default e(b)/e(V): the K ATT(g,t) cells
//                                 `g#:t_#_#` then the K `wgt:w#_#` columns
//                                 (csdid; w2ag log:114 ag_def_names)
//   agg_<form>     {names, b, V}  agg(simple|event|group|calendar) (csdid)
//   estat_event    {e_b, e_V}     `estat event`'s block-diagonal posting
//                                 (csdid_estat.ado csdid_event; w2ag log:1384 v13 = 0)
// The V is copied verbatim: no PIET-side construction.
std::optional<CsdidPostingM4> csdidPostingM4(const ojson& raw, const std::string& key) {
  if (!raw.is_object() || !raw.contains(key) || !raw[key].is_object()) return std::nullopt;
  const ojson& p = raw[key];
  auto num = [](const ojson& x) -> double {
    return x.is_number() ? x.get<double>() : std::numeric_limits<double>::quiet_NaN();
  };
  CsdidPostingM4 P;
  const ojson* names = nullptr;
  const ojson* b = nullptr;
  const ojson* V = nullptr;
  if (key == "estat_event") {
    if (!p.contains("e_b") || !p["e_b"].is_object() || !p.contains("e_V") || !p["e_V"].is_object())
      return std::nullopt;
    names = p["e_b"].contains("names") ? &p["e_b"]["names"] : nullptr;
    b = p["e_b"].contains("values") ? &p["e_b"]["values"] : nullptr;
    V = p["e_V"].contains("matrix") ? &p["e_V"]["matrix"] : nullptr;
  } else {
    names = p.contains("names") ? &p["names"] : nullptr;
    b = p.contains("b") ? &p["b"] : nullptr;
    V = p.contains("V") ? &p["V"] : nullptr;
  }
  if (names == nullptr || b == nullptr || V == nullptr || !names->is_array() || !b->is_array() ||
      !V->is_array())
    return std::nullopt;
  const size_t K = names->size();
  if (K == 0 || b->size() != K || V->size() != K) return std::nullopt;
  for (const auto& n : *names) {
    if (!n.is_string()) return std::nullopt;
    P.names.push_back(n.get<std::string>());
  }
  for (const auto& x : *b) P.b.push_back(num(x));
  P.V.reserve(K * K);
  for (const auto& row : *V) {
    if (!row.is_array() || row.size() != K) return std::nullopt;
    for (const auto& x : row) P.V.push_back(num(x));
  }
  return P;
}

ojson csdidPostedCoeftableM4(const CsdidPostingM4& P) {
  ojson rows = ojson::array();
  const size_t K = P.names.size();
  for (size_t i = 0; i < K; ++i) {
    const double v = P.V[i * K + i];
    const double se = (std::isnan(v) || v < 0) ? std::numeric_limits<double>::quiet_NaN()
                                               : std::sqrt(v);
    rows.push_back(coefRowM4(P.names[i], std::isnan(P.b[i]) ? ojson(nullptr) : ojson(P.b[i]),
                             std::isnan(se) ? ojson(nullptr) : ojson(se)));
  }
  return rows;
}

void postCsdidPostingM4(SessionState& session, const CsdidPostingM4& P, const std::string& bName,
                        const std::string& vName) {
  auto setE = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices) {
      if (kv.first == nm) {
        kv.second = std::move(m);
        return;
      }
    }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  const size_t K = P.names.size();
  eval::EvalMatrix b;
  b.data.push_back(P.b);
  b.rownames = {"y1"};
  b.colnames = P.names;
  eval::EvalMatrix V;
  for (size_t i = 0; i < K; ++i)
    V.data.emplace_back(P.V.begin() + static_cast<std::ptrdiff_t>(i * K),
                        P.V.begin() + static_cast<std::ptrdiff_t>((i + 1) * K));
  V.rownames = P.names;
  V.colnames = P.names;
  setE(bName, std::move(b));
  setE(vName, std::move(V));
}

namespace extended_fit_detail {

std::string numTextM4b(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// csdid_ctl_cluster (w2l_vce_shape_det log:2031-2050): with cluster() naming
// the ivar, csdid.ado passes `_xtreg_chk_cl2`, runs every (g,t)
// cell as `capture drdid ... ivar() time() treatment() <method>` (no cluster is passed, each success posts e()), and only then fails r(198)
// at `collapse.., by(ivar)`, where the cluster variable is both
// a target and by(). csdid never re-posts, so e() is the LAST successful
// cell's drdid posting (e(cmd) drdid, e(N) of that cell, no vce/vcetype).
// This re-runs that cell through drdid's own fit (drdidFitCoreM4). The cell
// order is csdid.ado's: glev = the positive gvar levels, tlev = the time grid
// range(min, max, min step) without its first point (data_check);
// the short-gap base time1 starts at time0 and moves to j after every j < g,
// and long2 fixes it at g - gdelta over the whole grid. The comparison group is never-treated `inlist(gvar,0,g)` or
// not-yet-treated. Only dripw reaches the engine's drdid, so any
// other method (csdid's default is reg without covariates) leaves
// e() as it was; so does a cell sample the fit refuses (the next-to-last
// cell is tried, and so on, as csdid's `capture` skips a failed cell).
void csdidRepostLastCellM4(SessionState& session, const std::optional<std::string>& ifExpr,
                           const std::string& y, const std::vector<std::string>& xs,
                           const std::string& ivar, const std::string& time,
                           const std::string& gvar, const std::optional<std::string>& method,
                           const std::optional<std::string>& comparison, bool long2) {
  const std::string m = (method.has_value() && !method->empty()) ? *method
                        : (xs.empty() ? std::string("reg") : std::string("dripw"));
  if (m != "dripw") return;
  const bool notyet = comparison.has_value() && *comparison == "notyet";

  ojson ifErr;
  auto basePathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr);
  if (!basePathOpt.has_value()) return;
  const std::string basePath = *basePathOpt;
  const bool madeIfTemp = basePath != *session.currentHbin();
  std::optional<Datastore> tempDs;
  Datastore* ds = nullptr;
  if (madeIfTemp) {
    tempDs.emplace(basePath);
    ds = &*tempDs;
  } else {
    ds = session.datastore();
  }
  std::set<double> tSet, gSet;
  bool ok = ds != nullptr;
  if (ok) {
    std::vector<std::string> cols = {y, ivar, time, gvar};
    cols.insert(cols.end(), xs.begin(), xs.end());
    std::vector<const double*> ptr;
    int64_t n = -1;
    for (const auto& c : cols) {
      if (!ds->has(c) || ds->dtype(c) != "f64") {
        ok = false;
        break;
      }
      const cecon::Column& col = ds->col(c);
      ptr.push_back(col.f64());
      n = col.nbytes / static_cast<int64_t>(sizeof(double));
    }
    if (ok) {
      const double* tcol = ptr[2];
      const double* gcol = ptr[3];
      for (int64_t r = 0; r < n; ++r) {
        bool miss = false;
        for (const double* p : ptr) miss = miss || std::isnan(p[r]);
        if (miss) continue;
        tSet.insert(tcol[r]);
        if (gcol[r] > 0) gSet.insert(gcol[r]);
      }
    }
  }
  tempDs.reset();
  if (madeIfTemp) cecon::native_unlink(basePath);
  if (!ok || tSet.size() < 2 || gSet.empty()) return;

  const std::vector<double> tv(tSet.begin(), tSet.end());
  double dt = std::numeric_limits<double>::infinity();
  for (size_t k = 1; k < tv.size(); ++k) dt = std::min(dt, tv[k] - tv[k - 1]);
  std::vector<double> grid;  // range(min, max, dt)
  for (int64_t k = 0;; ++k) {
    const double v = tv.front() + static_cast<double>(k) * dt;
    if (v > tv.back() + 1e-9 * std::max(1.0, std::fabs(tv.back()))) break;
    grid.push_back(v);
  }
  struct Cell {
    double g, t1, j;
  };
  std::vector<Cell> cells;
  for (double g : gSet) {
    if (!long2) {
      double time1 = grid.front();
      for (size_t k = 1; k < grid.size(); ++k) {
        const double j = grid[k];
        cells.push_back({g, time1, j});
        if (j < g) time1 = j;
      }
    } else {
      const double time1 = g - dt;
      for (double j : grid)
        if (time1 != j) cells.push_back({g, time1, j});
    }
  }
  const std::string base = (ifExpr.has_value() && !trimM4(*ifExpr).empty())
                               ? "(" + *ifExpr + ") & "
                               : std::string();
  for (auto it = cells.rbegin(); it != cells.rend(); ++it) {
    const std::string g = numTextM4b(it->g), j = numTextM4b(it->j), t1 = numTextM4b(it->t1);
    const std::string gsel =
        notyet ? "(" + gvar + "==0 | " + gvar + "==" + g + " | (" + gvar + ">" + g + " & " + gvar +
                     ">" + j + "))"
               : "inlist(" + gvar + ",0," + g + ")";
    const std::string cellIf = base + gsel + " & inlist(" + time + "," + t1 + "," + j + ")";
    // The treatment is csdid's tempvar `tr` = (gvar == g); the
    // ATET column carries a tempvar-shaped name, as Stata's does.
    const ojson res = drdidFitCoreM4(session, y, xs, ivar, time, gvar + "==" + g, "__000000",
                                     cellIf);
    if (res.value("type", std::string()) != "error") return;
  }
}

}  // namespace

ojson runCsdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty()) {
    return engineErrorBlockM4("syntax_error: csdid requires a depvar", "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> ivar, time, gvar, method, comparison, pscoretrim, window;
  bool long2 = false;
  std::string agg;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "ivar" && !v.isBool) ivar = v.strValue;
    if (k == "time" && !v.isBool) time = v.strValue;
    if (k == "gvar" && !v.isBool) gvar = v.strValue;
    if (k == "method" && !v.isBool) method = v.strValue;
    if (k == "comparison" && !v.isBool) comparison = v.strValue;
    if (k == "pscoretrim" && !v.isBool) pscoretrim = v.strValue;
    if (k == "window" && !v.isBool) window = v.strValue;
    // csdid `long2` — Callaway-Sant'Anna long-difference (fixed g-1 base)
    // event-study parametrization. Bare flag; pass through to the engine.
    if (k == "long2") long2 = true;
    // csdid-notyet-agg: Stata's own spelling of the not-yet-treated comparison
    // is the bare flag `notyet` (csdid; w2ag log:451 ag_ny rc 0,
    // e(control_group) "Not yet Treated"). PIET's `comparison()` stays.
    if (k == "notyet") comparison = std::string("notyet");
    if (k == "agg" && !v.isBool) agg = trimM4(v.strValue);
  }
  // FU-23 (BUG-757): cluster() by shape, w2l_vce_shape.log. A bare `cluster`
  // and an empty `cluster()` are ignored (csdid_bare_cluster/_empty_cluster rc 0,
  // the default fit); two variables are r(103) "cluster():  too many variables
  // specified" (csdid_opt_c2), where this loop used to forward the FIRST one.
  // vce()/robust are ignored by csdid whatever they say (the vocabulary note at
  // the top of the file), so they are read nowhere.
  const VceShapesM4b vceShape = readVceShapesM4b(parsed);
  std::optional<std::string> cluster;
  if (vceShape.cluster == OptShapeM4b::kMany) {
    return stataRefusalM4b("cluster():  too many variables specified", 103);
  }
  if (vceShape.cluster == OptShapeM4b::kOne) cluster = vceShape.clusterVars[0];
  if (!ivar.has_value() || ivar->empty()) {
    return engineErrorBlockM4(
        "csdid requires `ivar(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  if (!time.has_value() || time->empty()) {
    return engineErrorBlockM4(
        "csdid requires `time(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  if (!gvar.has_value() || gvar->empty()) {
    return engineErrorBlockM4(
        "csdid requires `gvar(varname)` — e.g. `csdid y x, ivar(id) time(t) gvar(g) method(dripw)`.",
        "unsupported_option");
  }
  // csdid-notyet-agg, csdid: an empty agg() is attgt; anything but
  // the five forms is "Aggregation not Allowed", exit 10 (the ado compares the
  // text as typed, so `agg(Simple)` is refused too).
  if (agg.empty()) agg = "attgt";
  if (agg != "attgt" && agg != "simple" && agg != "group" && agg != "calendar" &&
      agg != "event") {
    return stataRefusalM4b("Aggregation not Allowed", 10);
  }

  std::vector<std::string> argv = {"csdid", "--y", y, "--ivar", *ivar, "--time", *time, "--gvar", *gvar};
  if (!xs.empty()) {
    argv.push_back("--x");
    argv.push_back(joinCommaM4(xs));
  }
  if (method.has_value() && !method->empty()) {
    argv.push_back("--method");
    argv.push_back(*method);
  }
  if (comparison.has_value() && !comparison->empty()) {
    argv.push_back("--comparison");
    argv.push_back(*comparison);
  }
  if (pscoretrim.has_value() && !pscoretrim->empty()) {
    argv.push_back("--pscoretrim");
    argv.push_back(*pscoretrim);
  }
  if (window.has_value() && !window->empty()) {
    std::istringstream iss(*window);
    std::vector<std::string> parts;
    std::string tok;
    while (iss >> tok) parts.push_back(tok);
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i) joined += ",";
      joined += parts[i];
    }
    argv.push_back("--window");
    argv.push_back(joined.empty() ? *window : joined);
  }
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
  bool clusterIsIvar = false;
  if (cluster.has_value()) {
    // csdid_ctl_cluster: `cluster(id)` with `ivar(id)` is r(198) "id may not be
    // both target and by()". The engine refuses the same literal name match
    // (apps/cmds/cmds_panel.cpp, "name the same variable"), but its error left
    // here as rc 1; the rule is on the name token, so it is checked here.
    // The refusal itself comes after the option net below, with csdid's
    // last drdid cell posted first (csdidRepostLastCellM4).
    if (*cluster == trimM4(*ivar)) {
      clusterIsIvar = true;
    } else {
      argv.push_back("--cluster");
      argv.push_back(*cluster);
    }
  }
  if (long2) argv.push_back("--long2");

  // Audit fix: every neighboring estimator in this file (ivregress/reg3/
  // etc.) closes its option parsing with eval::rejectUnknownOptions —
  // csdid never did, so a typo (e.g. `methdo(dripw)` instead of
  // `method(dripw)`) silently fell through the opts loop above (none of
  // its `if (k == ...)` arms match, so the mistyped key is simply never
  // read) instead of erroring, producing a plausible-looking but WRONG
  // result (a different, unintended specification) rather than naming the
  // bad option.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kCsdidKnownOpts = kVocabCsdid.known;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kCsdidKnownOpts)) {
    return *err;
  }
  if (clusterIsIvar) {
    csdidRepostLastCellM4(session, parsed.ifExpr, y, xs, trimM4(*ivar), trimM4(*time),
                          trimM4(*gvar), method, comparison, long2);
    return stataRefusalM4b(*cluster + " may not be both target and by()", 198);
  }

  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    // csdid_ctl_cluster_id2: a cluster variable that varies within a panel is
    // r(498) "panels are not nested within clusters". The engine detects it
    // (apps/cmds/cmds_panel.cpp, "is not nested within --ivar") but its
    // bad_input code has no rc of its own, so it became rc 1. The engine's
    // message is the only signal this side receives; it is matched on the
    // clause that names the rule.
    if (cluster.has_value() &&
        std::string(ce.what()).find("is not nested within --ivar") != std::string::npos) {
      return stataRefusalM4b("panels are not nested within clusters", 498);
    }
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson block = csdidToEstimationM4(raw, "simple");
  // csdid-ematrix-post / csdid-notyet-agg: e(b)/e(V) are the posting agg()
  // asked for (csdid `ereturn post b1 s1`), and the coefficient
  // table is its columns, as Stata prints it: `_b[g4:t_3_4]` answers after a
  // default fit and `_b[ATT]` is r(111) "[ATT] not found" (w2ag log:136, :141).
  const std::string postKey = agg == "attgt" ? std::string("attgt") : "agg_" + agg;
  const std::optional<CsdidPostingM4> posting = csdidPostingM4(raw, postKey);
  if (!posting.has_value() && agg != "attgt") {
    return engineErrorBlockM4("csdid, agg(" + agg + "): the engine reply has no `" + postKey +
                                  "` posting (cobra too old); nothing was posted.",
                              "engine_error");
  }
  if (posting.has_value()) block["coeftable"] = csdidPostedCoeftableM4(*posting);
  block["agg"] = agg;
  // eb-order T-P3 part 0: the block is written AFTER the fresh setLastModel
  // (inside cacheCsdidModel), which clears the previous fit's block.
  cacheCsdidModel(session, argv, raw);
  session.lastEstBlock = block;
  if (posting.has_value()) postCsdidPostingM4(session, *posting);
  // e(b_attgt)/e(V_attgt) carry the cells whatever agg() posted (csdid).
  if (auto cells = csdidPostingM4(raw, "attgt")) postCsdidPostingM4(session, *cells, "b_attgt", "V_attgt");
  // csdid, the e() macros (w2ag log:120 ag_def_emac
  // `attgt|reg|Never Treated|default|`, :548 ag_l2_emac `...|long2|`, :404
  // ag_cl_vce `|||cl|15|`).
  // Each is recorded on top of the fit (noteEOnFit) so the NEXT model install
  // drops it: e(agg) must not answer after a later `regress`.
  auto setOnFit = [&session](const std::string& name, const std::string& value) {
    session.eval.setEString(name, value);
    session.noteEOnFit(name);
  };
  setOnFit("estat_cmd", "csdid_estat");
  setOnFit("agg", agg);
  setOnFit("ggroup", *gvar);
  setOnFit("id", *ivar);
  setOnFit("method", raw.value("method", method.value_or(std::string())));
  setOnFit("typebase", long2 ? "long2" : "default");
  if (cluster.has_value()) setOnFit("clustvar", *cluster);
  {
    const std::string cmp = raw.value("comparison", comparison.value_or(std::string("never")));
    setOnFit("control_group", cmp == "notyet" ? "Not yet Treated" : "Never Treated");
  }
  session.noteEOnFit("b_attgt");
  session.noteEOnFit("V_attgt");
  return block;
}

// ===========================================================================
// ivregress / ivreghdfe / reg3 (BETA D1 batch: BS-063/BS-064/BS-065)


}  // namespace piet::handlers
