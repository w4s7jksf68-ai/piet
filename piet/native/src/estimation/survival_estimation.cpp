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
#include "estimation_sample_internal.h"     // esampleFitArgs / esampleInstallOnHead
#include "postestimation/rtable_internal.h"  // stataColumns: a factor fit's e(b) columns

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
  // e(N_sub) counts the stset id()'s subjects
  if (!session.stContext->idvar.empty()) {
    r.argv.push_back("--id");
    r.argv.push_back(session.stContext->idvar);
  }
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

namespace {

// The base variable names of a varlist term (`i.drug` -> drug, `c.age##i.drug`
// -> age drug, `ib2.x` -> x), in order of appearance.
void stBaseVarsM4(const std::string& term, std::vector<std::string>& out) {
  size_t i = 0;
  while (i <= term.size()) {
    size_t j = term.find('#', i);
    if (j == std::string::npos) j = term.size();
    std::string part = term.substr(i, j - i);
    const size_t dot = part.rfind('.');
    if (dot != std::string::npos) part = part.substr(dot + 1);
    if (!part.empty() && std::find(out.begin(), out.end(), part) == out.end()) out.push_back(part);
    i = j + 1;
    while (i < term.size() && term[i] == '#') ++i;
  }
}

// One e() string / scalar written on top of the installed fit (dropped by the
// next model install).
void stSetS(SessionState& session, const std::string& name, const std::string& v) {
  session.eval.setEString(name, v);
  session.noteEOnFit(name);
}
void stSetN(SessionState& session, const std::string& name, double v) {
  session.eval.setEScalar(name, v);
  session.noteEOnFit(name);
}
std::optional<double> stNum(const ojson& raw, const char* key) {
  if (!raw.is_object() || !raw.contains(key) || !raw[key].is_number()) return std::nullopt;
  return raw[key].get<double>();
}

// Stata's e(b) columns of an st fit, in r(table)'s layout (base levels a
// structural 0), each with the engine's name for it ("" for a base column).
// `eq` prefixes every column (`_t:` for streg; "" for stcox). False when a column
// cannot be placed (an engine-omitted regressor): nothing is posted then.
bool stEbColumnsM4(SessionState& session, const ojson& block, const std::string& eq,
                   std::vector<AncillaryEbColM4>& cols) {
  if (!session.lastModel().has_value() || !block.contains("coeftable") ||
      !block["coeftable"].is_array())
    return false;
  std::vector<rtable_detail::RtCol> rc;
  try {
    if (!rtable_detail::stataColumns(session, *session.lastModel(), block["coeftable"], rc))
      return false;
  } catch (const std::exception&) {
    return false;
  }
  for (const auto& c : rc) {
    const std::string nm = eq.empty() ? c.name : eq + ":" + c.name;
    if (c.row == nullptr) {
      cols.push_back({nm, std::string(), 0.0});
      continue;
    }
    std::string en = c.row->value("name", std::string());
    const ojson& bv = c.row->contains("b") ? (*c.row)["b"] : ojson();
    const ojson& sv = c.row->contains("se") ? (*c.row)["se"] : ojson();
    if (en.empty() || !bv.is_number() ||
        (bv.get<double>() == 0.0 && sv.is_number() && sv.get<double>() == 0.0))
      return false;
    if (en == "(intercept)") en = "_cons";
    cols.push_back({nm, en, bv.get<double>()});
  }
  return !cols.empty();
}

// A covariance reply (`e_V`-shaped: names + matrix) laid out on `cols`, a base
// column's row and column 0. nullopt when a column is not in the reply.
std::optional<eval::EvalMatrix> stVOnColsM4(const ojson& ev,
                                           const std::vector<AncillaryEbColM4>& cols) {
  if (!ev.is_object() || !ev.contains("names") || !ev.contains("matrix")) return std::nullopt;
  const ojson& vn = ev["names"];
  const ojson& vm = ev["matrix"];
  if (!vn.is_array() || !vm.is_array() || vm.size() != vn.size()) return std::nullopt;
  const size_t k = cols.size();
  std::vector<long> at(k, -1);
  for (size_t a = 0; a < k; ++a) {
    if (cols[a].engine.empty()) continue;
    for (size_t j = 0; j < vn.size(); ++j)
      if (vn[j].is_string() && vn[j].get<std::string>() == cols[a].engine) at[a] = static_cast<long>(j);
    if (at[a] < 0) return std::nullopt;
  }
  eval::EvalMatrix m;
  m.data.assign(k, std::vector<double>(k, 0.0));
  for (size_t a = 0; a < k; ++a)
    for (size_t c = 0; c < k; ++c) {
      if (at[a] < 0 || at[c] < 0) continue;
      const ojson& r1 = vm[static_cast<size_t>(at[a])];
      const ojson& r2 = vm[static_cast<size_t>(at[c])];
      if (!r1.is_array() || !r2.is_array() || !r1[static_cast<size_t>(at[c])].is_number() ||
          !r2[static_cast<size_t>(at[a])].is_number())
        return std::nullopt;
      m.data[a][c] = 0.5 * (r1[static_cast<size_t>(at[c])].get<double>() +
                            r2[static_cast<size_t>(at[a])].get<double>());
    }
  for (const auto& c : cols) {
    m.rownames.push_back(c.stata);
    m.colnames.push_back(c.stata);
  }
  return m;
}

void stPutEMatrixM4(SessionState& session, const std::string& name, eval::EvalMatrix m) {
  auto& store = session.eval.eMatrices;
  store.erase(std::remove_if(store.begin(), store.end(),
                             [&name](const auto& p) { return p.first == name; }),
              store.end());
  store.emplace_back(name, std::move(m));
  session.noteEOnFit(name);
}

// e(sample) of an st fit: the stset sample (_st == 1) with every model variable
// (covariates, strata, cluster) nonmissing, under `if`. The engine's row count
// (n_obs) is the handshake (esampleInstallOnHead).
void installStEsampleM4(SessionState& session, const eval::ParsedCommand& parsed,
                        const std::vector<std::string>& extraVars, ojson raw) {
  if (!session.lastModel().has_value() || !session.currentHbin().has_value()) return;
  LastModel lm = *session.lastModel();
  std::vector<std::string> xs(parsed.varlist.begin(), parsed.varlist.end());
  for (const auto& v : extraVars)
    if (std::find(xs.begin(), xs.end(), v) == xs.end()) xs.push_back(v);
  std::string keep = "_st==1";
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) keep += " & (" + *parsed.ifExpr + ")";
  std::vector<std::string> aux = {"feols", "--y", "_t", "--x", joinCommaM4(xs), "--keepif", keep,
                                  "--vcov", "hc1", "--data", *session.currentHbin()};
  const std::string values = esampleFitArgs(session, aux, std::string(), 0);
  ojson auxRaw;
  try {
    auxRaw = runCobraJsonM4(aux);
  } catch (const CobraError&) {
    cecon::native_unlink(values);
    return;
  }
  if (!auxRaw.is_object() || !auxRaw.contains("esample_n") || !auxRaw["esample_n"].is_number()) {
    cecon::native_unlink(values);
    return;
  }
  raw["esample_n"] = auxRaw["esample_n"];
  lm.dataPath = *session.currentHbin();
  if (esampleInstallOnHead(session, values, raw, lm))
    session.setLastModelEsample(lm.esample, lm.esampleSerial, lm.esampleInstallPath, lm.dataPath);
}

// e(datasignaturevars) of an st fit: _t _t0 _d, then the model's variables.
std::string stDatasignatureVarsM4(const eval::ParsedCommand& parsed,
                                  const std::vector<std::string>& extraVars) {
  std::vector<std::string> vs;
  for (const auto& t : parsed.varlist) stBaseVarsM4(t, vs);
  for (const auto& v : extraVars) stBaseVarsM4(v, vs);
  std::string out = "_t _t0 _d";
  for (const auto& v : vs) out += " " + v;
  return out;
}

// stcox's e() contract (stcox.ado and _coxph's posting; vs-stata bank fb464d8b
// `stcox age i.drug`, 3e5d296a `, vce(robust)`, b5c81a48 `, strata(drug)`,
// 5bdbc48b `efron`, 2a3864a7 `exactm`, a5cb266b `exactp`, d43b9032 `c.age##i.drug`):
// e(cmd) cox (set by the fit), e(cmd2) stcox; e(b)/e(V) unnamed by equation with
// the factor base levels; the st macros; e(N_sub), e(N_fail), e(risk), e(ll_0),
// e(r2_p) = 1 - ll/ll_0, e(df_m) = e(rank) the estimated columns.
void postCoxEContractM4(SessionState& session, const eval::ParsedCommand& parsed, const ojson& raw,
                        const ojson& block, const std::string& tiesWord,
                        const std::vector<std::string>& strataVars) {
  const std::string vce = raw.value("vce", std::string("iid"));
  const bool sandwich = vce == "robust" || vce == "cluster";
  std::vector<AncillaryEbColM4> cols;
  if (stEbColumnsM4(session, block, std::string(), cols) && postAncillaryEbM4(session, raw, cols)) {
    if (sandwich && raw.contains("e_Vmb"))
      if (auto vm = stVOnColsM4(raw["e_Vmb"], cols)) stPutEMatrixM4(session, "V_modelbased", *vm);
  }
  size_t rank = 0;
  for (const auto& c : cols)
    if (!c.engine.empty()) ++rank;
  stSetS(session, "cmd2", "stcox");
  stSetS(session, "depvar", "_t");
  stSetS(session, "t0", "_t0");
  stSetS(session, "estat_cmd", "stcox_estat");
  stSetS(session, "predict", "stcox_p");
  stSetS(session, "footnote", "stcox_footnote");
  stSetS(session, "marginsfootnote", "_multirecordcheck");
  stSetS(session, "marginsnotok",
         "CSNell DEViance DFBeta ESR LDisplace LMax MGale SCAledsch SCHoenfeld SCores");
  stSetS(session, "marginsprop", "addcons allcons");
  stSetS(session, "properties", "b V");
  stSetS(session, "k_eform", "1");
  stSetS(session, "method", tiesWord);
  stSetS(session, "ties", tiesWord);
  stSetS(session, "datasignaturevars", stDatasignatureVarsM4(parsed, strataVars));
  stSetS(session, "crittype", sandwich ? "log pseudolikelihood" : "log likelihood");
  if (raw.contains("chi2type") && raw["chi2type"].is_string())
    stSetS(session, "chi2type", raw["chi2type"].get<std::string>());
  else
    stSetS(session, "chi2type", "Wald");
  if (vce == "cluster") {
    stSetS(session, "vce", "cluster");
    stSetS(session, "vcetype", "Robust");
    stSetS(session, "clustvar", raw.value("cluster_var", std::string()));
    if (auto g = stNum(raw, "n_clusters")) stSetN(session, "N_clust", *g);
  } else if (vce == "robust") {
    stSetS(session, "vce", "robust");
    stSetS(session, "vcetype", "Robust");
  } else {
    stSetS(session, "vce", "oim");
  }
  if (!strataVars.empty()) {
    std::string sv;
    for (const auto& v : strataVars) sv += (sv.empty() ? "" : " ") + v;
    stSetS(session, "strata", sv);
    if (auto ns = stNum(raw, "n_strata")) stSetN(session, "N_strata", *ns);
  }
  if (auto nf = stNum(raw, "n_failures")) stSetN(session, "N_fail", *nf);
  if (auto ns = stNum(raw, "n_subjects")) stSetN(session, "N_sub", *ns);
  if (auto r = stNum(raw, "time_at_risk")) stSetN(session, "risk", *r);
  const auto ll = stNum(raw, "ll"), ll0 = stNum(raw, "ll_null");
  if (ll0) stSetN(session, "ll_0", *ll0);
  if (ll && ll0 && *ll0 != 0) stSetN(session, "r2_p", 1.0 - *ll / *ll0);
  if (!cols.empty()) {
    stSetN(session, "rank", static_cast<double>(rank));
    stSetN(session, "df_m", static_cast<double>(rank));
  }
}

}  // namespace

static ojson runStcoxM4In(const eval::ParsedCommand& parsed, SessionState& session) {
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
  b.argv.push_back("--emit-v");  // e(V) (and e(V_modelbased) under a sandwich), posted below
  ojson raw;
  // e(cmd) is `cox` (stcox.ado fits through cox and reposts only e(cmd2) stcox).
  ojson block = runP7MleAsM4("stcox", b.argv, session, &raw, "cox");
  if (block.value("type", std::string()) == "error" || !raw.is_object()) return block;
  {
    // Stata's e(ties)/e(method) words: breslow, efron, partial (exactp), marginal
    // (exactm) -- the requested method, whether or not the data have ties.
    const std::string tiesWord = ties == "exactp" ? "partial" : ties == "exactm" ? "marginal" : ties;
    std::vector<std::string> extra = strataVars;
    if (raw.contains("cluster_var") && raw["cluster_var"].is_string())
      extra.push_back(raw["cluster_var"].get<std::string>());
    postCoxEContractM4(session, parsed, raw, block, tiesWord, strataVars);
    installStEsampleM4(session, parsed, extra, raw);
  }
  // `nohr` reports the coefficients themselves (Stata's r(table) b row is b, its
  // eform row 0); the hazard-ratio metric is the default.
  if (!nohr) applyHazardRatioTableM4(block, raw);
  return block;
}

// `in a/b`: folded into the fit's `if` as an observation-number condition
// (inRangeConditionM4), so every reader of the condition below sees the same rows.
ojson runStcox(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    std::optional<std::string> eff;
    if (auto err = inRangeConditionM4(session, parsed, eff, "stcox")) return *err;
    eval::ParsedCommand folded = parsed;
    folded.ifExpr = eff;
    folded.inRange.reset();
    return runStcoxM4In(folded, session);
  }
  return runStcoxM4In(parsed, session);
}


namespace {

// streg's distribution() word as streg.ado's GetCmd reads it (case as typed):
// exponential/ereg from 1 letter, weibull from 1, lognormal from 4 or lnormal
// from 2, loglogistic from 4 or llogistic from 2, gompertz from 3, ggamma from 4.
// Returns the fitting command (Stata's e(cmd)) or "" for an unknown word.
std::string stregCmdOfM4(const std::string& dist) {
  const auto is = [&dist](const char* word, size_t minLen) {
    const std::string w(word);
    return dist.size() >= minLen && dist.size() <= w.size() && w.compare(0, dist.size(), dist) == 0;
  };
  if (dist.empty()) return "";
  if (is("exponential", 1) || is("ereg", 1)) return "ereg";
  if (is("weibull", 1)) return "weibull";
  if (is("lognormal", 4) || is("lnormal", 2)) return "lnormal";
  if (is("loglogistic", 4) || is("llogistic", 2)) return "llogistic";
  if (is("gompertz", 3)) return "gompertz";
  if (is("ggamma", 4)) return "gamma";
  return "";
}

}  // namespace

// streg (streg.ado and each family's ado; vs-stata bank ed95841f weibull, 8c8f2610
// exponential, 1908f3b4 gompertz, 14037643 loglogistic, 07be5711 lognormal,
// b345526e weibull `time`, 2eb0d9ee weibull `ancillary(drug)`, aa9b4c29 `nohr
// level(90)`): the engine fits each family on Stata's ml path (constant-only model
// first; e(ic) is the full model's iteration count); this posts e(b)/e(V) on
// Stata's stripe (`_t:` equation with its factor base levels, then /ln_p,
// /gamma, /lngamma or /lnsigma, or the `ln_p:` equation of ancillary()),
// e(Cns), e(gradient), e(ilog), the family's macros and scalars, r(table) with
// the diparm columns, and e(sample).
static ojson runStregM4In(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildStArgv("streg", parsed, session);
  if (b.errored) return b.errorBlock;
  std::string dist;
  bool distGiven = false, nohr = false, timeOpt = false;
  std::vector<std::string> ancVars;
  for (const auto& [k, v] : parsed.opts) {
    const std::string lk = lowerM4(k);
    if ((lk == "distribution" || lk == "dist" || lk == "d") && !v.isBool) {
      dist = trimM4(v.strValue);
      distGiven = true;
    } else if (lk == "nohr" && v.isBool && v.boolValue) {
      nohr = true;
    } else if (lk == "time" && v.isBool && v.boolValue) {
      timeOpt = true;
    } else if (lk == "ancillary") {
      if (v.isBool) return extended_fit_detail::stataRefusalM4b("option ancillary() requires a varlist", 198);
      for (const auto& t : splitTokensM4(v.strValue)) ancVars.push_back(t);
    }
  }
  // streg.ado GetCmd: without distribution() streg refits the family of the
  // previous streg estimates (e(cmd), with e(cmd2) "streg"); only with no streg
  // estimates in memory is the option required.
  std::string ecmd;
  if (!distGiven || dist.empty()) {
    if (session.eval.eStrings.get("cmd2") != "streg")
      return extended_fit_detail::stataRefusalM4b("you must specify option distribution()", 198);
    ecmd = session.eval.eStrings.get("cmd");
    dist = ecmd;
  } else {
    ecmd = stregCmdOfM4(dist);
  }
  if (ecmd != "ereg" && ecmd != "weibull" && ecmd != "lnormal" && ecmd != "llogistic" &&
      ecmd != "gompertz" && ecmd != "gamma")
    return extended_fit_detail::stataRefusalM4b("unknown distribution " + dist, 198);
  if (ecmd == "gamma")
    return engineErrorBlockM4(
        "Option `distribution(" + dist + ")` (the generalized gamma) is a valid Stata option that "
        "this version of PIET does not implement for `streg`.",
        "unsupported_option");
  // the metric options each family takes (streg.ado Opt_1/Opt_2/Opt_3)
  const bool aftFamily = ecmd == "lnormal" || ecmd == "llogistic";
  if (ecmd == "gompertz" && timeOpt)
    return extended_fit_detail::stataRefusalM4b("option time not allowed", 198);
  if (aftFamily && nohr) return extended_fit_detail::stataRefusalM4b("option nohr not allowed", 198);
  if (timeOpt && ecmd == "ereg")
    return engineErrorBlockM4("Option `time` with `distribution(exponential)` is a valid Stata "
                              "option that this version of PIET does not implement for `streg`.",
                              "unsupported_option");
  static const std::regex rePlain(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  if (!ancVars.empty()) {
    if (ecmd == "ereg")
      return engineErrorBlockM4("Option `ancillary()` with `distribution(exponential)` is not "
                                "implemented in this version of PIET.",
                                "unsupported_option");
    if (timeOpt && !aftFamily)
      return engineErrorBlockM4("Options `ancillary()` and `time` together are a valid Stata "
                                "combination that this version of PIET does not implement for "
                                "`streg`.",
                                "unsupported_option");
    for (const auto& v : ancVars)
      if (!std::regex_match(v, rePlain))
        return engineErrorBlockM4("ancillary(" + v + "): this version of PIET takes plain variable "
                                  "names in ancillary().",
                                  "unsupported_option");
  }
  const std::string engineDist = ecmd == "ereg"        ? "exponential"
                                 : ecmd == "weibull"   ? "weibull"
                                 : ecmd == "gompertz"  ? "gompertz"
                                 : ecmd == "llogistic" ? "loglogistic"
                                                       : "lognormal";
  b.argv.push_back("--dist");
  b.argv.push_back(engineDist);
  // lognormal and loglogistic are fitted in the time metric only: `time` is
  // accepted for them and changes nothing (streg.ado Opt_3)
  if (timeOpt && !aftFamily) b.argv.push_back("--time");
  if (!ancVars.empty()) {
    b.argv.push_back("--anc");
    b.argv.push_back(joinCommaM4(ancVars));
  }
  pushLevelIfGivenM4(parsed, b.argv);  // level(#): the reported intervals
  b.argv.push_back("--emit-v");
  ojson raw;
  ojson block = runP7MleAsM4("streg", b.argv, session, &raw, ecmd);
  if (block.value("type", std::string()) == "error" || !raw.is_object()) return block;

  const bool aft = aftFamily || timeOpt;
  const bool hasAnc = ecmd != "ereg";
  const bool ancEq = !ancVars.empty();
  // the main equation's coefficients are printed as hazard ratios in the PH
  // metric unless nohr; ancillary() shuts the transform off (streg.ado noeform)
  const bool eform = !aft && !nohr && !ancEq;
  const std::string vce = raw.value("vce", std::string("iid"));
  const bool sandwich = vce == "robust" || vce == "cluster";
  // ---- e(b) / e(V): `_t:` columns (r(table)'s layout), then the ancillary ----
  std::vector<AncillaryEbColM4> cols;
  std::string ancName;  // the ancillary parameter's e(b) name without ancillary()
  if (ecmd == "weibull") ancName = "/ln_p";
  else if (ecmd == "gompertz") ancName = "/gamma";
  else if (ecmd == "llogistic") ancName = "/lngamma";
  else if (ecmd == "lnormal") ancName = "/lnsigma";
  const ojson ancRows = raw.value("anc", ojson::array());
  bool placed = stEbColumnsM4(session, block, "_t", cols);
  const size_t kMain = cols.size();
  for (auto& c : cols)
    if (c.engine == "_cons") c.engine = "(intercept)";
  if (placed && hasAnc) {
    for (const auto& r : ancRows) {
      const std::string zn = r.value("name", std::string());
      const ojson& bv = r.contains("b") ? r["b"] : ojson();
      if (!bv.is_number()) {
        placed = false;
        break;
      }
      std::string nm = ancEq ? "ln_p:" + (zn == "(intercept)" ? std::string("_cons") : zn) : ancName;
      cols.push_back({nm, "anc:" + zn, bv.get<double>()});
    }
    if (!ancEq && ancRows.size() != 1) placed = false;
  }
  if (placed) placed = postAncillaryEbM4(session, raw, cols);
  const size_t k = cols.size();
  size_t nAuto = 0;
  for (const auto& c : cols)
    if (c.engine.empty()) ++nAuto;
  if (placed) {
    if (sandwich && raw.contains("e_Vmb"))
      if (auto vm = stVOnColsM4(raw["e_Vmb"], cols)) stPutEMatrixM4(session, "V_modelbased", *vm);
    eval::EvalMatrix rb;
    rb.rownames = {"r1"};
    std::vector<std::string> names;
    for (const auto& c : cols) names.push_back(c.stata);
    // e(gradient): the engine's (fitted-metric) gradient on e(b)'s columns, a base column 0
    const ojson& g = raw.value("gradient", ojson::array());
    if (g.is_array() && g.size() == k - nAuto) {
      std::vector<double> gv;
      size_t gi = 0;
      for (const auto& c : cols) gv.push_back(c.engine.empty() ? 0.0 : g[gi++].get<double>());
      rb.colnames = names;
      rb.data = {gv};
      stPutEMatrixM4(session, "gradient", rb);
    }
    const ojson& il = raw.value("ilog", ojson::array());
    if (il.is_array() && !il.empty() && il.size() <= 20) {
      eval::EvalMatrix m;
      m.rownames = {"r1"};
      std::vector<double> v(20, 0.0);
      for (size_t i = 0; i < il.size(); ++i) v[i] = il[i].is_number() ? il[i].get<double>() : 0.0;
      for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
      m.data = {v};
      stPutEMatrixM4(session, "ilog", m);
    }
    if (nAuto > 0) {
      eval::EvalMatrix cns;
      cns.colnames = names;
      cns.colnames.push_back("_Cns:_r");
      for (size_t j = 0; j < k; ++j) {
        if (!cols[j].engine.empty()) continue;
        cns.rownames.push_back("r" + std::to_string(cns.rownames.size() + 1));
        std::vector<double> row(k + 1, 0.0);
        row[j] = 1.0;
        cns.data.push_back(std::move(row));
      }
      stPutEMatrixM4(session, "Cns", cns);
    }
  }
  // ---- macros ----
  const auto has0 = [&session]() {
    // whether any record of the sample enters late (weibull's lf program)
    Datastore* ds = session.datastore();
    if (ds == nullptr || !ds->has("_t0") || ds->dtype("_t0") != "f64") return false;
    const double* c = ds->col("_t0").f64();
    for (int64_t i = 0; i < ds->nRows(); ++i)
      if (c[i] > 0 && c[i] < eval::kStataMissingSentinel) return true;
    return false;
  };
  const std::string fam = ecmd == "ereg"       ? "Exponential"
                          : ecmd == "weibull"  ? "Weibull"
                          : ecmd == "gompertz" ? "Gompertz"
                          : ecmd == "llogistic" ? "Loglogistic"
                                               : "Lognormal";
  stSetS(session, "title", fam + (aft ? " AFT regression" : " PH regression"));
  stSetS(session, "cmd2", "streg");
  stSetS(session, "depvar", "_t");
  stSetS(session, "t0", "_t0");
  stSetS(session, "dead", "_d");
  stSetS(session, "frm2", aft ? "time" : "hazard");
  stSetS(session, "estat_cmd", "streg_estat");
  stSetS(session, "predict", "streg_p");
  stSetS(session, "predict_sub", ecmd == "ereg"       ? "ereg_p"
                                 : ecmd == "weibull"  ? "weibul_p"
                                 : ecmd == "gompertz" ? "gomper_p"
                                 : ecmd == "llogistic" ? "llogis_p"
                                                      : "lnorma_p");
  stSetS(session, "user", ecmd == "ereg"       ? "ereg_lf"
                          : ecmd == "weibull"  ? (ancEq ? "weib2_lf" : has0() ? "weib_lf" : "weib_lf0")
                          : ecmd == "gompertz" ? "gomp_lf"
                          : ecmd == "llogistic" ? "logis_lf"
                                               : "lnorm_lf");
  stSetS(session, "marginsfootnote", "_multirecordcheck");
  stSetS(session, "marginsprop", "nochainrule");
  stSetS(session, "properties", "b V");
  stSetS(session, "opt", "moptimize");
  stSetS(session, "which", "max");
  stSetS(session, "technique", "nr");
  stSetS(session, "deriv_useminbound", "off");
  stSetS(session, "singularHmethod", "m-marquardt");
  stSetS(session, "ml_method", "e2");
  stSetS(session, "crittype", sandwich ? "log pseudolikelihood" : "log likelihood");
  stSetS(session, "chi2type", raw.value("chi2type", std::string("Wald")));
  if (!ancEq) stSetS(session, "stcurve", "stcurve");
  if (ancEq) {
    stSetS(session, "noeform", "noeform");
    if (ecmd == "weibull") stSetS(session, "by_weibull_c", "by_weibull_c");
  }
  if (ecmd == "weibull" && !ancEq) {
    stSetS(session, "diparm1", "ln_p, exp label(p)");
    stSetS(session, "diparm2", "ln_p, f(exp(-@)) d(exp(-@)) label(\"1/p\")");
  } else if (ecmd == "llogistic" && !ancEq) {
    stSetS(session, "diparm1", "lngamma, exp label(gamma)");
  } else if (ecmd == "lnormal" && !ancEq) {
    stSetS(session, "diparm1", "lnsigma, exp label(sigma)");
  }
  if (vce == "cluster" || (vce == "robust" && raw.contains("cluster_var"))) {
    stSetS(session, "vce", vce == "cluster" ? "cluster" : "robust");
    stSetS(session, "vcetype", "Robust");
    stSetS(session, "clustvar", raw.value("cluster_var", std::string()));
    if (auto g = stNum(raw, "n_clusters")) stSetN(session, "N_clust", *g);
  } else if (vce == "robust") {
    stSetS(session, "vce", "robust");
    stSetS(session, "vcetype", "Robust");
  } else {
    stSetS(session, "vce", "oim");
  }
  // ---- scalars ----
  if (auto nf = stNum(raw, "n_failures")) stSetN(session, "N_fail", *nf);
  if (auto ns = stNum(raw, "n_subjects")) stSetN(session, "N_sub", *ns);
  if (auto r = stNum(raw, "time_at_risk")) stSetN(session, "risk", *r);
  if (auto l0 = stNum(raw, "ll_null")) stSetN(session, "ll_0", *l0);
  if (auto ic = stNum(raw, "iters")) stSetN(session, "ic", *ic);
  stSetN(session, "rc", 0.0);
  stSetN(session, "consonly", 0.0);
  stSetN(session, "noconstant", 0.0);
  stSetN(session, "k_eq_model", 1.0);
  stSetN(session, "k_eq", hasAnc ? 2.0 : 1.0);
  stSetN(session, "k_dv", hasAnc ? 1.0 : 2.0);
  // (llogistic.ado posts its e(k_aux) as a macro: bank 14037643 `e(k_aux) : "1"`)
  if (hasAnc && !ancEq && ecmd != "llogistic") stSetN(session, "k_aux", 1.0);
  if (ecmd == "llogistic" && !ancEq) stSetS(session, "k_aux", "1");
  if (hasAnc && raw.contains("converged_cons") && raw["converged_cons"].is_boolean())
    stSetN(session, "converged_cons", raw["converged_cons"].get<bool>() ? 1.0 : 0.0);
  if (ecmd == "gompertz" || ecmd == "llogistic" || ecmd == "lnormal" || ancEq)
    stSetN(session, "rank0", 1.0 + static_cast<double>(ancEq ? ancRows.size() : 1));
  if (placed) {
    stSetN(session, "k", static_cast<double>(k));
    stSetN(session, "k_autoCns", static_cast<double>(nAuto));
    stSetN(session, "rank", static_cast<double>(k - nAuto));
    stSetN(session, "df_m", static_cast<double>(kMain - nAuto - 1));
  }
  if (auto p = stNum(raw, "p_chi2")) stSetN(session, "p", *p);
  if (!ancEq && ancRows.size() == 1 && ancRows[0].contains("b") && ancRows[0]["b"].is_number()) {
    const double a = ancRows[0]["b"].get<double>();
    if (ecmd == "weibull") stSetN(session, "aux_p", std::exp(a));
    else if (ecmd == "gompertz") stSetN(session, "gamma", a);
    else if (ecmd == "llogistic") stSetN(session, "gamma", std::exp(a));
    else if (ecmd == "lnormal") stSetN(session, "sigma", std::exp(a));
  }

  // ---- the table: r(table) laid out as e(b), the diparm transforms after it ----
  if (placed) {
    const ojson mainRows = raw.value("coefs", ojson::array());
    std::map<std::string, const ojson*> mainRow;
    for (const auto& c : mainRows)
      if (c.contains("name")) mainRow[c["name"].get<std::string>()] = &c;
    std::map<std::string, const ojson*> ancRow;
    for (const auto& c : ancRows)
      if (c.contains("name")) ancRow["anc:" + c["name"].get<std::string>()] = &c;
    ojson rcols = ojson::array();
    for (size_t j = 0; j < k; ++j) {
      ojson rc;
      rc["name"] = cols[j].stata;
      const bool inMain = j < kMain;
      rc["eform"] = inMain && eform;
      if (cols[j].engine.empty()) {
        rc["omitted"] = true;
        rcols.push_back(rc);
        continue;
      }
      const ojson* src = nullptr;
      if (inMain) {
        const auto it = mainRow.find(cols[j].engine);
        if (it != mainRow.end()) src = it->second;
      } else {
        const auto it = ancRow.find(cols[j].engine);
        if (it != ancRow.end()) src = it->second;
      }
      if (src == nullptr) {
        rcols = ojson();
        break;
      }
      for (const char* key : {"b", "se", "z", "p", "ci_lo", "ci_hi"}) rc[key] = (*src)[key];
      rcols.push_back(rc);
    }
    if (rcols.is_array()) block["rtable_cols"] = rcols;
    ojson dp = ojson::array();
    const auto addDp = [&dp](const char* name, const ojson& o) {
      if (!o.is_object()) return;
      ojson c;
      c["name"] = std::string("_diparm1:") + name;
      for (const char* key : {"b", "se", "ci_lo", "ci_hi"}) c[key] = o.contains(key) ? o[key] : ojson();
      dp.push_back(c);
    };
    ojson ancT;  // the ancillary parameter's exp() transform (diparm)
    if (!ancEq && ancRows.size() == 1 && (ecmd == "llogistic" || ecmd == "lnormal")) {
      const ojson& a = ancRows[0];
      const double bb = a.value("b", 0.0), se = a.value("se", 0.0), lo = a.value("ci_lo", 0.0),
                   hi = a.value("ci_hi", 0.0);
      ancT = ojson{{"b", std::exp(bb)}, {"se", std::exp(bb) * se}, {"ci_lo", std::exp(lo)},
                   {"ci_hi", std::exp(hi)}};
    }
    if (ecmd == "weibull" && !ancEq) {
      addDp("p", raw.value("p", ojson()));
      addDp("1/p", raw.value("inv_p", ojson()));
    } else if (ecmd == "llogistic" && !ancT.is_null()) {
      addDp("gamma", ancT);
    } else if (ecmd == "lnormal" && !ancT.is_null()) {
      addDp("sigma", ancT);
    }
    if (!dp.empty()) block["rtable_diparm"] = dp;
    // the displayed table: the main equation (hazard ratios unless nohr / AFT),
    // then the ancillary rows and their transforms, as Stata prints them
    ojson ct = ojson::array();
    for (size_t j = 0; j < k; ++j) {
      if (!block.contains("rtable_cols")) break;
      const ojson& rc = block["rtable_cols"][j];
      if (!rc.is_object() || rc.value("omitted", false)) continue;
      // rows keep e(b)'s names (`_b[_t:age]`, `_b[/ln_p]` read them; `_b[age]`
      // finds the first equation's); the stub is what the table prints
      ojson row;
      const std::string nm = cols[j].stata;
      row["name"] = nm;
      if (nm.rfind("_t:", 0) == 0) row["stub"] = nm.substr(3);
      const double bb = rc.value("b", 0.0), se = rc.value("se", 0.0);
      const bool ef = rc.value("eform", false);
      row["b"] = ef ? std::exp(bb) : bb;
      row["se"] = ef ? std::exp(bb) * se : se;
      row["t"] = rc["z"];
      row["p"] = rc["p"];
      row["ci_lo"] = ef ? std::exp(rc.value("ci_lo", 0.0)) : rc["ci_lo"].get<double>();
      row["ci_hi"] = ef ? std::exp(rc.value("ci_hi", 0.0)) : rc["ci_hi"].get<double>();
      if (ef) {
        row["b_raw"] = bb;
        row["se_raw"] = se;
      }
      ct.push_back(row);
    }
    for (const auto& d : dp) {
      ojson row;
      const std::string nm = d.value("name", std::string());
      row["name"] = nm;
      row["stub"] = nm.substr(nm.find(':') + 1);
      row["b"] = d["b"];
      row["se"] = d["se"];
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = d["ci_lo"];
      row["ci_hi"] = d["ci_hi"];
      ct.push_back(row);
    }
    if (block.contains("rtable_cols")) {
      // The live e()'s block (what _b[]/_se[] read) keeps the coefficient metric;
      // the returned block prints hazard ratios.
      ojson coefCt = ct;
      for (auto& row : coefCt) {
        if (!row.contains("b_raw")) continue;
        const double bb = row["b_raw"].get<double>(), se = row["se_raw"].get<double>();
        const double lo = std::log(row["ci_lo"].get<double>()), hi = std::log(row["ci_hi"].get<double>());
        row["b"] = bb;
        row["se"] = se;
        row["ci_lo"] = lo;
        row["ci_hi"] = hi;
        row.erase("b_raw");
        row.erase("se_raw");
      }
      ojson live = block;
      live["coeftable"] = coefCt;
      block["coeftable"] = ct;
      if (eform) block["coef_metric"] = "hazard_ratio";
      session.lastEstBlock = live;
    }
  }
  installStEsampleM4(session, parsed, ancVars, raw);
  return block;
}

// `in a/b`: folded into the fit's `if` as an observation-number condition
// (inRangeConditionM4; vs-stata bank e42e9d5b `streg ... in 1/30`).
ojson runStreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    std::optional<std::string> eff;
    if (auto err = inRangeConditionM4(session, parsed, eff, "streg")) return *err;
    eval::ParsedCommand folded = parsed;
    folded.ifExpr = eff;
    folded.inRange.reset();
    return runStregM4In(folded, session);
  }
  return runStregM4In(parsed, session);
}

// ===========================================================================
// logit / probit


}  // namespace piet::handlers
