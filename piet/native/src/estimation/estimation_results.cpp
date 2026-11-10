// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Estimation results.
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
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


// ===========================================================================
// Stata's built-in multiway cluster e() (FU-388 / FU-402 / FU-406). regress
// (Stata's multiway-cluster step), areg and xtreg,fe (Stata's absorb step)
// post, after a VCE with two or more cluster variables: no e(N_clust),
// e(rank) = the rank of the clipped variance (r(rankc)), the e(kcluster) matrix
// (1 x every cluster combination, row "#Groups", columns cluster1..) and the macros
// e(cluster1..) naming each combination (`id#id2`). The engine's feols reply under
// --multiway-subset-factors carries `vcov_rank` and `kcluster` (apps/cmds/common.cpp
// run_feols). w2s_multiway_class.log:116 regress `.|6|2|180`, :133
// `30|7|180|id|id2|id#id2`, :691 xtreg `.|4|30|180|cg1 cg2|2`, :1115/:1126 three-way.
// Shared with linear_estimation.cpp (runRegress), which declares both itself.
//
// The cached reply's half: run on the reply that becomes LastModel::result, BEFORE
// setLastModel (which mirrors it into the resolver), like BUG-570's n_clusters erase.
// The emitted block keeps the engine's fields.
void applyBuiltinMultiwayReplyM4(ojson& result) {
  if (!result.is_object()) return;
  result.erase("n_clusters");
  if (result.contains("vcov_rank") && result["vcov_rank"].is_number())
    result["k_vars"] = result["vcov_rank"];
}

// The e() stores' half, AFTER setLastModel: e(kcluster) and e(cluster#). Recorded
// with noteEOnFit so the next install drops them (SessionState::dropEOnFit), as it
// does any name written on top of a fit.
void postBuiltinMultiwayEM4(SessionState& session, const ojson& raw) {
  if (!raw.is_object() || !raw.contains("kcluster") || !raw["kcluster"].is_array() ||
      raw["kcluster"].empty())
    return;
  eval::EvalMatrix m;
  m.rownames = {"#Groups"};
  m.data.emplace_back();
  size_t i = 0;
  for (const auto& c : raw["kcluster"]) {
    if (!c.is_object() || !c.contains("levels") || !c["levels"].is_number()) return;
    ++i;
    m.colnames.push_back("cluster" + std::to_string(i));
    m.data[0].push_back(c["levels"].get<double>());
  }
  auto& store = session.eval.eMatrices;
  store.erase(std::remove_if(store.begin(), store.end(),
                             [](const auto& p) { return p.first == "kcluster"; }),
              store.end());
  store.emplace_back("kcluster", std::move(m));
  session.noteEOnFit("kcluster");
  i = 0;
  for (const auto& c : raw["kcluster"]) {
    const std::string name = "cluster" + std::to_string(++i);
    session.eval.setEString(name, c.value("name", std::string()));
    session.noteEOnFit(name);
  }
}

// ===========================================================================
// Stata's ordered e() name lists (`: e(scalars)`, `: e(macros)`, `: e(matrices)`)
// for the fits whose whole Stata namespace PIET posts (w2s_multiway_class mw_*_scalars
// /_macros/_lists, w2v_estpost_ematrix ep15d_ematrices). Each path below posts every
// name Stata lists that PIET's lazily resolved e() did not carry, with Stata's value,
// then installs the list (SessionState::ENameContract; eNameOrderFor re-checks it
// against the live stores on every read). A kind whose names cannot all be posted
// with Stata's value is left unclaimed, so `: e(kind)' keeps its r(111) refusal.
//
// The orders are Stata's e() store order (StataBE 19.5 on the oracle machine):
// e(macros) is newest-first and a re-set name moves to the front, e(matrices) is
// `b V` then newest-first (both reproduce from the ado posting sequences, e.g.
// regress's own posts then Stata's multiway-cluster step for the multiway regress
// lists); e(scalars) follows the store's own order, which the ado sequence alone does
// not determine (areg's list, w2s_multiway_class.log mw_areg_c2_scalars, has
// `ar2 p F r2_a` after the hidden/historical posts), so each scalar list is the one
// measured for that path. Names that depend on the specification are generated:
// e(cluster1..C) for the C cluster combinations.
namespace fit_detail {

void putEScalarNL(SessionState& s, const std::string& n, double v) {
  s.eval.setEScalar(n, v);
  s.noteEOnFit(n);
}
void putEStringNL(SessionState& s, const std::string& n, const std::string& v) {
  s.eval.setEString(n, v);
  s.noteEOnFit(n);
}
void putEMatrixNL(SessionState& s, const std::string& n, eval::EvalMatrix m) {
  auto& store = s.eval.eMatrices;
  store.erase(std::remove_if(store.begin(), store.end(),
                             [&n](const auto& p) { return p.first == n; }),
              store.end());
  store.emplace_back(n, std::move(m));
  s.noteEOnFit(n);
}
std::optional<double> rawNumNL(const ojson& raw, const char* key) {
  if (!raw.is_object() || !raw.contains(key) || !raw[key].is_number()) return std::nullopt;
  return raw[key].get<double>();
}
// The regress-family log likelihood of a Gaussian fit with residual (or, for ll_0,
// total) sum of squares `ss` over N observations: regress's e(ll)/e(ll_0)
// (-N/2 (1 + ln 2pi + ln(ss/N)); StataBE regress c2 e(ll) -429.7924086070367 with
// rss 1249.565, e(ll_0) -590.9762832476949 with tss 7491.19; areg/xtreg,fe post
// r(ll0) over the within sum of squares, e(ll_0) -577.19 with tss_w 6427.50).
double gaussLogLikNL(double N, double ss) {
  return -0.5 * N * (1.0 + std::log(2.0 * 3.14159265358979323846) + std::log(ss / N));
}
// e(cmdline): the ado's `"<cmd> `0'"' -- the command word as Stata names it, then
// the arguments as typed (StataBE `reg y x1 x2 , vce(cluster id   id2)` posts
// "regress y x1 x2 , vce(cluster id   id2)").
std::string stataCmdlineNL(const std::string& word, const eval::ParsedCommand& parsed,
                           const std::string& expanded = std::string()) {
  // the dispatched line before tokenize, which keeps the trailing blanks parsed.raw drops
  const std::string& s = expanded.empty() ? parsed.raw : expanded;
  size_t i = s.find_first_not_of(" \t");
  if (i == std::string::npos) return word;
  size_t j = s.find_first_of(" \t,", i);
  if (j == std::string::npos) return word;
  size_t k = s.find_first_not_of(" \t", j);
  if (k == std::string::npos) return word;
  // Trailing blanks stay: Stata's `0` keeps those a macro or xi's substitution
  // leaves (`regress y x1 ` after `xi: regress y x1 i.one`, BUG-1048); a typed
  // line arrives here already trimmed, as Stata trims it.
  return word + " " + s.substr(k);
}
// Record the engine's e(beta) coefficients (`beta_std`) or e(V_modelbased)
// (`e_Vmb`) on the live session block, IN PLACE: the session block is mutated,
// never reassigned or swapped, so its identity (liveEstimationIdentity, the
// address r(table) and the e(b) stripe annotation key on) is the fit's own. The
// matrices are built at the moment of need by EvalSession::resolveEMatrixFor
// from a copy of this block through the e(b)/e(V) reader, after
// annotateEbStripe has recorded the base-level columns. Session block only,
// like e_V and `noconstant`: the console block (freeze-pinned) is unchanged.
bool recordOnBlockNL(SessionState& session, const ojson& raw, const char* key) {
  if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object() ||
      !raw.is_object() || !raw.contains(key) || !raw[key].is_object() || raw[key].empty())
    return false;
  (*session.lastEstBlock)[key] = raw[key];
  return true;
}
std::vector<std::string> clusterMacrosNL(size_t combos) {
  std::vector<std::string> v;
  for (size_t i = 1; i <= combos; ++i) v.push_back("cluster" + std::to_string(i));
  return v;
}
void installENameContractNL(SessionState& session, const std::string& cmd,
                            std::optional<std::vector<std::string>> scalars,
                            std::optional<std::vector<std::string>> macros,
                            std::optional<std::vector<std::string>> matrices,
                            std::vector<std::string> missingScalars = {}) {
  if (!session.lastModel().has_value()) return;
  SessionState::ENameContract c;
  c.fitSerial = session.lastModel()->fitSerial;
  c.cmd = cmd;
  c.scalars = std::move(scalars);
  c.macros = std::move(macros);
  c.matrices = std::move(matrices);
  c.missingScalars = std::move(missingScalars);
  session.eNameContract = std::move(c);
}
// The options whose presence leaves the e() namespace of a regress/areg/xtreg fit as
// measured (vce/robust/cluster pick the measured VCE path; level is display-only).
// Anything else (beta, hascons, tsscons, notable, noheader, absorb options...) is
// not measured, and the fit keeps the refusal.
bool onlyOptionsNL(const eval::ParsedCommand& parsed, std::initializer_list<const char*> allowed) {
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    bool ok = false;
    for (const char* a : allowed) ok = ok || k == a;
    if (!ok) return false;
  }
  return true;
}

}  // namespace

// regress (one-way: OLS, vce(robust), vce(cluster v); built-in multiway) and areg
// (built-in multiway), unweighted, with the constant; and a weighted or noconstant
// one-way regress (BUG-1542), whose values are posted without the ordered name lists.
// Called by runRegress right after the multiway e() hook. `raw` is the engine reply as
// the fit received it.
void postENameListsRegressM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed, const std::string& stataCmdIn,
                             bool builtinMultiway, bool noConstant) {
  session.eNameContract.reset();
  const std::string stataCmd = stataCmdIn == "reg" ? std::string("regress") : stataCmdIn;
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || !raw.is_object()) return;
  const bool weighted = !lmo->fitWeight.empty();
  if ((weighted || noConstant) && stataCmd != "regress") return;
  const LastModel& lm = *lmo;
  const bool multiway = builtinMultiway && (lm.vcov == "twoway" || lm.vcov == "multiway") &&
                        raw.contains("kcluster") && raw["kcluster"].is_array() &&
                        !raw["kcluster"].empty();
  const auto N = rawNumNL(raw, "n_obs");
  const auto rss = rawNumNL(raw, "rss");
  const auto mss = rawNumNL(raw, "mss");
  if (!N || !rss || !mss || *N <= 0 || *rss <= 0) return;
  const std::string cmdline = stataCmdlineNL(stataCmd, parsed, session.leafCmdExpanded);

  if (stataCmd == "regress") {
    if (!lm.fe.empty() ||
        !onlyOptionsNL(parsed, {"vce", "robust", "cluster", "level", "noconstant", "nocons"}))
      return;
    enum class Path { kOls, kRobust, kCluster1, kMultiway } path;
    if (multiway) path = Path::kMultiway;
    else if (lm.vcov == "iid") path = Path::kOls;
    else if (lm.vcov == "hc1") path = Path::kRobust;
    else if (lm.vcov == "cluster" && lm.cluster.size() == 1 &&
             lm.cluster[0].find(',') == std::string::npos)
      path = Path::kCluster1;
    else return;
    // [R] regress Stored results and regress's posting: e(sum_w) is N unweighted,
    // e(ll)/e(ll_0) the Gaussian log likelihoods of the fit and of the constant-only fit.
    // Weighted (BUG-1542, StataBE 19.5, 12 rows, sum(wf) 28.5): e(sum_w) is the weight
    // total and e(ll)/e(ll_0) the same Gaussian over e(N) and e(rss)/e(mss) -- [fw=] N 30,
    // ll -17.670289377592514; [iw=] N 28, -21.142846032770578, or N 12 on the normalized
    // weights under vce(robust)/vce(cluster) (-8.9550222657338434, the [pw=] value) --
    // plus, for [aw=] only, 0.5 * sum(ln(w_i * N / sum w)) over e(sample), each weight
    // read as a precision (-10.210580476229087 under every VCE). noconstant posts no
    // e(ll_0) and the same e(ll) over its own e(rss) (StataBE `regress y x, noconstant`
    // -25.243130074332818 under every VCE, [aw=] -27.799597115109073, [fw=]
    // -65.078391062216852, [iw=] -62.18388485682388). Not measured weighted or
    // noconstant: the built-in multiway path.
    if ((weighted || noConstant) && path == Path::kMultiway) return;
    double sumW = *N, llShift = 0.0;
    if (weighted) {
      if (!raw.contains("weight") || !raw["weight"].is_object()) return;
      const auto w = rawNumNL(raw["weight"], "sum_w");
      if (!w || !(*w > 0)) return;
      sumW = *w;
      if (lm.fitWtype == "aw") {
        llShift = std::numeric_limits<double>::quiet_NaN();
        const auto rows = esampleRowsM4(session, lm);
        const double* wv = f64ColumnM4(session, lm.fitWeight);
        if (rows.has_value() && wv != nullptr && static_cast<double>(rows->size()) == *N) {
          double s = 0.0;
          for (const int64_t i : *rows) s += std::log(wv[i] * *N / sumW);
          if (std::isfinite(s)) llShift = 0.5 * s;
        }
      }
    }
    putEScalarNL(session, "sum_w", sumW);
    if (!std::isnan(llShift)) {
      putEScalarNL(session, "ll", gaussLogLikNL(*N, *rss) + llShift);
      if (!noConstant) putEScalarNL(session, "ll_0", gaussLogLikNL(*N, *mss + *rss) + llShift);
    }
    // regress's own macros; the coefficient-table step posts the hidden
    // e(_r_z__CL)/e(_r_z_abs__CL) when the table is shown (StataBE `ereturn list, all`).
    putEStringNL(session, "cmdline", cmdline);
    putEStringNL(session, "title", "Linear regression");
    putEStringNL(session, "marginsprop", "minus");
    putEStringNL(session, "marginsok", "XB default");
    putEStringNL(session, "properties", "b V");
    putEStringNL(session, "predict", "regres_p");
    putEStringNL(session, "model", "ols");
    putEStringNL(session, "estat_cmd", "regress_estat");
    putEStringNL(session, "_r_z__CL", "t");
    putEStringNL(session, "_r_z_abs__CL", "|t|");
    // A weighted or noconstant fit's e(beta), e(V_modelbased) and ordered name lists
    // are not measured.
    if (weighted || noConstant) return;
    // e(beta) is posted by regress for every fit except a one-way cluster VCE
    // (StataBE `: e(matrices)`: OLS/robust/hc3/aw/fv `b V beta...`, vce(cluster id)
    // `b V V_modelbased`); the multiway path's inner regress runs without the cluster.
    // Both are recorded on the block and resolved when read (recordOnBlockNL);
    // the matrix list below names them only when recorded, and eNameOrderFor
    // re-checks on every read that each one resolves.
    const bool beta = path != Path::kCluster1 && recordOnBlockNL(session, raw, "beta_std");
    // e(V_modelbased) is (X'X)^-1 = V/rmse^2 under every VCE the engine sends it for
    // (BUG-1043: one-way robust/cluster too, unweighted or fweighted).
    const bool vmb = path != Path::kOls && recordOnBlockNL(session, raw, "e_Vmb");
    const std::vector<std::string> base = {"cmdline", "title", "marginsprop", "marginsok",
                                           "vce", "_r_z_abs__CL", "_r_z__CL", "depvar", "cmd",
                                           "properties", "predict", "model", "estat_cmd"};
    const std::vector<std::string> olsScalars = {"N",   "sum_w", "df_m", "df_r", "F",
                                                 "r2",  "rmse",  "mss",  "rss",  "r2_a",
                                                 "ll",  "ll_0",  "rank"};
    std::optional<std::vector<std::string>> S, M, X;
    switch (path) {
      case Path::kOls:
        S = olsScalars;
        M = base;
        if (beta) X = std::vector<std::string>{"b", "V", "beta"};
        break;
      case Path::kRobust:
        // StataBE `: e(matrices)` after vce(robust): `b V beta V_modelbased`.
        S = olsScalars;
        M = base;
        M->push_back("vcetype");
        if (beta && vmb) X = std::vector<std::string>{"b", "V", "beta", "V_modelbased"};
        break;
      case Path::kCluster1: {
        S = std::vector<std::string>{"N_clust"};
        S->insert(S->end(), olsScalars.begin(), olsScalars.end());
        M = base;
        M->push_back("vcetype");
        M->push_back("clustvar");
        if (vmb) X = std::vector<std::string>{"b", "V", "V_modelbased"};  // StataBE `b V V_modelbased`
        break;
      }
      case Path::kMultiway: {
        // Stata's multiway-cluster step: df_r and rank re-set, then
        // the model test's df_m, F, p.
        S = std::vector<std::string>{"N",   "sum_w", "r2",   "rmse", "mss",  "rss", "r2_a",
                                     "ll",  "ll_0",  "df_r", "rank", "df_m", "F",   "p"};
        M = std::vector<std::string>{"_r_z_abs__CL", "_r_z__CL", "clustvar", "vce", "vcetype"};
        for (const auto& c : clusterMacrosNL(raw["kcluster"].size())) M->push_back(c);
        for (const char* n : {"cmdline", "title", "marginsprop", "marginsok", "depvar", "cmd",
                              "properties", "predict", "model", "estat_cmd"})
          M->push_back(n);
        if (beta && vmb)
          X = std::vector<std::string>{"b", "V", "kcluster", "V_modelbased", "beta"};
        break;
      }
    }
    installENameContractNL(session, session.eval.eStrings.get("cmd"), std::move(S), std::move(M), std::move(X));
    return;
  }

  if (stataCmd == "areg") {
    // The MAP path with one absorbed variable and the built-in multiway VCE
    // (Stata's absorb step, areg's ado).
    if (!multiway || lm.fe.size() != 1 || !onlyOptionsNL(parsed, {"absorb", "vce", "cluster", "level"}))
      return;
    const auto tss = rawNumNL(raw, "tss_total");
    const auto tssW = rawNumNL(raw, "tss_within");
    const auto r2a = rawNumNL(raw, "adj_r2");
    if (!tss || !tssW || !r2a || !raw.contains("e_V") || !raw["e_V"].is_object() ||
        !raw["e_V"].contains("names") || !raw["e_V"]["names"].is_array() ||
        !raw.contains("absorbed") || !raw["absorbed"].is_object() ||
        !raw["absorbed"].contains(lm.fe[0]) || !raw["absorbed"][lm.fe[0]].is_number())
      return;
    const double levels = raw["absorbed"][lm.fe[0]].get<double>();
    const double rankMb = static_cast<double>(raw["e_V"]["names"].size());
    putEScalarNL(session, "rc", 0.0);
    // r(dfe), the model-based residual dof (areg charges the absorbed levels).
    putEScalarNL(session, "df_r_mb", *N - (levels - 1.0) - rankMb);
    putEScalarNL(session, "rank_mb", rankMb);
    putEScalarNL(session, "ll", gaussLogLikNL(*N, *rss));
    putEScalarNL(session, "ll_0", gaussLogLikNL(*N, *tssW));
    putEScalarNL(session, "tss", *tss);
    putEScalarNL(session, "adjdf", 0.0);
    putEScalarNL(session, "mss_w", *tssW - *rss);
    putEScalarNL(session, "tss_w", *tssW);
    putEScalarNL(session, "k_absorb", levels);
    putEScalarNL(session, "ar2", *r2a);  // areg's ado historical(9.2) ar2 = e(r2_a)
    std::optional<std::vector<std::string>> M, X;
    // Macros (Stata's absorb step, areg, the estimation-sample signature)
    // and matrices. Only a plain varlist (no factor or
    // time-series operators, which unopvarlist would rewrite) is claimed.
    const auto plain = [](const std::string& v) {
      return !v.empty() && v.find_first_of(".#()") == std::string::npos;
    };
    bool plainVars = plain(lm.y);
    for (const auto& x : lm.x) plainVars = plainVars && plain(x);
    const std::string absvar = lm.fe[0];
    const std::string clustvar = session.eval.eStrings.get("clustvar");
    std::vector<std::string> sigVars;
    if (plainVars && !clustvar.empty()) {
      sigVars.push_back(lm.y);
      for (const auto& x : lm.x) sigVars.push_back(x);
      sigVars.push_back(absvar);
      std::istringstream cs(clustvar);
      for (std::string c; cs >> c;) sigVars.push_back(c);
    }
    std::string signature;
    if (!sigVars.empty() && lm.esample == LastModel::EsampleState::kColumn &&
        session.currentHbin().has_value()) {
      // e(datasignature): `_datasignature <vars>, esample nonames` over e(sample).
      // Stata's checksum algorithm is unpublished, so this is the engine's own
      // signature in Stata's nonames shape (src/data/datasignature.h Q9 ruling,
      // the same divergence `datasignature` itself carries).
      std::string csv;
      for (const auto& v : sigVars) csv += (csv.empty() ? "" : ",") + v;
      CobraRunResult cr = runCobra({"datasignature", "--data", *session.currentHbin(), "--vars",
                                    csv, "--keepif",
                                    std::string(kEsampleColumn) + "==" +
                                        std::to_string(lm.esampleSerial)});
      if (cr.ok) {
        signature = cr.stdout_text;
        while (!signature.empty() && (signature.back() == '\n' || signature.back() == '\r' ||
                                      signature.back() == ' '))
          signature.pop_back();
      }
    }
    if (!signature.empty()) {
      std::string sv;
      for (const auto& v : sigVars) sv += (sv.empty() ? "" : " ") + v;
      putEStringNL(session, "cmdline", cmdline);
      putEStringNL(session, "_r_z__CL", "t");
      putEStringNL(session, "_r_z_abs__CL", "|t|");
      putEStringNL(session, "estmethod", "MAP");
      putEStringNL(session, "footnote", "areg_footnote");
      putEStringNL(session, "title2", "Absorbed variable: " + absvar);
      putEStringNL(session, "title", "Linear regression, absorbing indicators");
      putEStringNL(session, "predict", "areg_p");
      putEStringNL(session, "marginsprop", "nolinearize");
      putEStringNL(session, "marginsnotok", "Residuals SCore D XBD");
      putEStringNL(session, "datasignaturevars", sv);
      putEStringNL(session, "datasignature", signature);
      putEStringNL(session, "absvar", absvar);
      putEStringNL(session, "apm", "projection");
      putEStringNL(session, "map_method", "projection");
      putEStringNL(session, "properties", "b V");
      M = std::vector<std::string>{"cmdline", "_r_z_abs__CL", "_r_z__CL", "estmethod", "cmd",
                                   "footnote", "title2", "title", "predict", "marginsprop",
                                   "marginsnotok", "datasignaturevars", "datasignature", "vce"};
      for (const auto& c : clusterMacrosNL(raw["kcluster"].size())) M->push_back(c);
      for (const char* n : {"absvar", "depvar", "vcetype", "clustvar", "apm", "map_method",
                            "properties"})
        M->push_back(n);
    }
    // e(ksingle) counts the absorbed singletons; claimed only when there are none.
    const bool noSingletons = raw.contains("fe_group_sizes") && raw["fe_group_sizes"].is_object() &&
                              raw["fe_group_sizes"].contains("min") &&
                              raw["fe_group_sizes"]["min"].is_number() &&
                              raw["fe_group_sizes"]["min"].get<double>() > 1.0;
    const bool vmb = recordOnBlockNL(session, raw, "e_Vmb");  // resolved when read
    if (noSingletons && vmb) {
      const auto one = [&absvar](const char* row, double v) {
        eval::EvalMatrix m;
        m.rownames = {row};
        m.colnames = {absvar};
        m.data = {{v}};
        return m;
      };
      putEMatrixNL(session, "ksingle", one("singletons", 0.0));
      putEMatrixNL(session, "dfabsorb", one("df", levels - 1.0));
      putEMatrixNL(session, "kabsorb", one("absvar", levels));
      X = std::vector<std::string>{"b",       "V",       "kcluster",    "ksingle",
                                   "dfabsorb", "kabsorb", "V_modelbased"};
    }
    installENameContractNL(
        session, session.eval.eStrings.get("cmd"),
        std::vector<std::string>{"rc",   "N",      "df_a",     "df_r", "df_r_mb", "rank",
                                 "rank_mb", "ll",  "ll_0",     "rss",  "tss",     "adjdf",
                                 "rmse", "mss",    "r2_w",     "r2",   "mss_w",   "tss_w",
                                 "k_absorb", "df_m", "ar2",    "p",    "F",       "r2_a"},
        std::move(M), std::move(X));
  }
}

// xtreg, fe under the built-in multiway VCE, unweighted (Stata's absorb step with
// cmd xtreg,fe). Called after postBuiltinMultiwayEM4.
void postENameListsXtregFeM4(SessionState& session, const ojson& raw,
                             const eval::ParsedCommand& parsed) {
  session.eNameContract.reset();
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || !lmo->fitWeight.empty() || !raw.is_object() ||
      !onlyOptionsNL(parsed, {"fe", "vce", "cluster", "level"}))
    return;
  const auto N = rawNumNL(raw, "n_obs");
  const auto rss = rawNumNL(raw, "rss");
  const auto tss = rawNumNL(raw, "tss_total");
  const auto tssW = rawNumNL(raw, "tss_within");
  const auto su = rawNumNL(raw, "sigma_u");
  const auto se = rawNumNL(raw, "sigma_e");
  const auto ng = rawNumNL(raw, "N_g");
  if (!N || !rss || !tss || !tssW || !su || !se || !ng || !raw.contains("fe_group_sizes") ||
      !raw["fe_group_sizes"].is_object() || !raw.contains("e_V") || !raw["e_V"].is_object() ||
      !raw["e_V"].contains("names") || !raw["e_V"]["names"].is_array() ||
      !raw.contains("kcluster") || !raw["kcluster"].is_array() || raw["kcluster"].empty())
    return;
  const auto& gs = raw["fe_group_sizes"];
  if (!gs.contains("min") || !gs.contains("max") || !gs.contains("avg")) return;
  const double gmin = gs["min"].get<double>(), gmax = gs["max"].get<double>(),
               gavg = gs["avg"].get<double>();
  const double rankMb = static_cast<double>(raw["e_V"]["names"].size());
  putEScalarNL(session, "rc", 0.0);
  putEScalarNL(session, "df_ax", 0.0);  // Stata's absorb step (xtreg,fe)
  // r(dfe): the panels nest in every cluster (xtreg requires it, _xtreg_chk_cl2), so
  // the absorbed panel means are not charged (StataBE e(df_r_mb) 177 = 180 - 3).
  putEScalarNL(session, "df_r_mb", *N - rankMb);
  putEScalarNL(session, "rank_mb", rankMb);
  putEScalarNL(session, "ll", gaussLogLikNL(*N, *rss));
  putEScalarNL(session, "ll_0", gaussLogLikNL(*N, *tssW));
  putEScalarNL(session, "tss", *tss);
  putEScalarNL(session, "adjdf", 0.0);
  putEScalarNL(session, "k_absorb", *ng);
  // e(df_b): the dof of the model test of the slopes
  // (Stata's absorb step), the reply's df_b (runXtreg: the slope count,
  // bounded by the clipped V's rank).
  putEScalarNL(session, "df_b", rawNumNL(raw, "df_b").value_or(rankMb - 1.0));
  putEScalarNL(session, "sigma", std::sqrt(*su * *su + *se * *se));  // xtreg,fe
  putEScalarNL(session, "ui", *su);
  putEScalarNL(session, "g_max", gmax);
  putEScalarNL(session, "g_avg", gavg);
  putEScalarNL(session, "g_min", gmin);
  putEScalarNL(session, "Tcon", gmin == gmax ? 1.0 : 0.0);
  putEScalarNL(session, "Tbar", gavg);
  putEScalarNL(session, "tss_w", *tssW);
  putEScalarNL(session, "mss_w", *tssW - *rss);
  putEScalarNL(session, "tss_xt", *tssW);
  installENameContractNL(
      session, "xtreg",
      std::vector<std::string>{"rc",      "N",     "df_a",  "df_ax",  "df_r",  "df_r_mb", "rank",
                               "rank_mb", "ll",    "ll_0",  "rss",    "tss",   "adjdf",   "rmse",
                               "mss",     "r2",    "k_absorb", "df_b", "df_m", "r2_w",    "p",
                               "rho",     "sigma", "sigma_e", "r2_b", "r2_o",  "corr",    "sigma_u",
                               "ui",      "N_g",   "g_max", "g_avg",  "g_min", "Tcon",    "Tbar",
                               "F",       "r2_a",  "tss_w", "mss_w",  "tss_xt"},
      std::nullopt, std::nullopt);
}

// ivregress 2sls under the built-in multiway VCE, asymptotic (no `small`),
// unweighted, with the constant (ivregress.ado / ivregress 2sls). Called after
// runIvRegress has posted e(mss)/e(tss)/e(rss)/e(r2)/e(r2_a)/e(df_m).
void postENameListsIvregressM4(SessionState& session, const ojson& raw, size_t kEndog) {
  session.eNameContract.reset();
  if (!session.lastModel().has_value() || !raw.is_object()) return;
  const auto p = rawNumNL(raw, "chi2_p");
  if (!p || kEndog == 0) return;
  for (const char* n : {"mss", "tss", "rss", "r2", "r2_a", "df_m"})
    if (session.eval.eScalars.find(n) == nullptr) return;
  putEScalarNL(session, "kappa", 1.0);
  putEScalarNL(session, "p", *p);
  putEScalarNL(session, "k_endog", static_cast<double>(kEndog));
  putEScalarNL(session, "iterations", 0.0);
  installENameContractNL(
      session, "ivregress",
      std::vector<std::string>{"N",    "kappa",   "mss",  "tss",  "rss",  "rmse",
                               "r2",   "r2_a",    "df_m", "p",    "rank_mb", "rank",
                               "chi2", "df_r",    "k_endog", "iterations"},
      std::nullopt, std::nullopt, {"rank_mb", "df_r"});
}

// ===========================================================================
// tabstat / correlate / pwcorr / ttest — all route through cobraToEstimationM4
// (dispatcher.py's "4b. Cobra estimator" argv-Dispatch path; no native
// handler in the oracle, just an argv builder + the standard estimation
// wrapper).

// W:P5: tabstat's body, split at the row-filter boundary. runTabstat (below)
// does the filtering and calls this; the `by G: tabstat` driver in
// dispatcher.cpp has ALREADY materialized its group slice — with the
// sub-command's own `if` folded in — and calls runTabstatOnData, which lands
// here without repeating the filter. This split is what lets the by-prefix
// driver REUSE tabstat rather than grow a second copy of it (the P4
// `by: regress` precedent).

}  // namespace piet::handlers
