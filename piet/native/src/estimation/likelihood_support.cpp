// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Likelihood support.
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

namespace est_detail {  // R2: reopened — these cross the TU cut (runP7Mle & friends)



// Builds `CMD --y Y --x X1,X2 [--keepif E] [--vce ... [--cluster C]]`.
// `vceAllowed` names the vce spellings this verb actually implements; anything
// else is an honest reject rather than a silent downgrade to iid.
// BUG-333: forward Stata's `level(#)` to the engine. All six commands wired
// through this (nl/gmm, mixed, fracreg, ivprobit/ivtobit) register AND consume
// `--level` in cobra today (options_table.cpp:110-118); PIET was simply not
// passing it, so the option was an honest-but-unnecessary reject. Kept as a
// free function rather than a branch inside buildP7MleArgv because that builder
// is shared with commands whose engine half has NO --level (xtlogit), and a
// blanket push there would turn a working call into an E6 unconsumed-flag error.
// Same idiom as runTobit/runQreg/runNbreg: forward the string, let the engine
// own the [10, 99.99] range check and surface it through CobraError.
void pushLevelIfGivenM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == "level" && !v.isBool) {
      argv.push_back("--level");
      argv.push_back(v.strValue);
      return;
    }
  }
}

P7MleArgv buildP7MleArgv(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                         const std::vector<std::string>& vceAllowed) {
  P7MleArgv r;
  if (parsed.varlist.size() < 2) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "syntax_error: " + cobraCmd + " requires a dependent variable and at least one regressor",
        "syntax_error");
    return r;
  }
  const std::string y = parsed.varlist[0];
  const std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());
  r.argv = {cobraCmd, "--y", y, "--x", joinCommaM4(xs)};
  if (parsed.ifExpr.has_value()) {
    r.argv.push_back("--keepif");
    r.argv.push_back(*parsed.ifExpr);
  }

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
  std::string want;
  std::string cvar = clusterOpt.value_or("");
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
    // Stata's legacy bare `cluster(v)` is a synonym for vce(cluster v) — the
    // BUG-055 class. Honouring it here keeps these verbs from silently running
    // iid while the user believes they clustered. BUG-537: tested BEFORE the
    // bare `robust` flag -- `robust cluster(v)` together is cluster-robust in
    // Stata (w2_pweight.log R5_pw_robust_AND_cluster_equals_R4); robust-first
    // dropped the cluster. Same ordering as vcovFromOpts above.
    want = "cluster";
  } else if (robustFlag) {
    want = "robust";
  }
  if (want.empty()) return r;  // engine default (iid)

  bool allowed = false;
  for (const auto& s : vceAllowed)
    if (s == want) { allowed = true; break; }
  if (!allowed) {
    r.errored = true;
    r.errorBlock = engineErrorBlockM4(
        "Option `vce(" + want + ")` is a valid Stata option that this version of PIET does not "
        "implement for `" + cobraCmd + "`.",
        "unsupported_option");
    return r;
  }
  if (want == "cluster") {
    // A second cluster variable would be two-way clustering, which none of these
    // ML estimators implements — rejecting is parity, quietly using the first
    // variable is not.
    std::istringstream iss(cvar);
    std::vector<std::string> toks;
    std::string t;
    while (iss >> t) toks.push_back(t);
    if (toks.empty()) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4("Cluster variable required: vce(cluster varname)",
                                        "unsupported_option");
      return r;
    }
    if (toks.size() > 1) {
      r.errored = true;
      r.errorBlock = engineErrorBlockM4(
          "Two-way clustering is not implemented for `" + cobraCmd + "`.", "unsupported_option");
      return r;
    }
    r.argv.push_back("--vce");
    r.argv.push_back("cluster");
    r.argv.push_back("--cluster");
    r.argv.push_back(toks[0]);
  } else {
    r.argv.push_back("--vce");
    r.argv.push_back(want);
  }
  return r;
}

// rawOut, when given, receives the engine payload BEFORE cobraToEstimationM4
// narrows it to the known block keys — the ordered models need it because their
// cutpoints ride in a key the mapper does not carry through.
// `stataCmd` is the e(cmd) word LastModel records (cacheLastModelM4); empty
// publishes cobraCmd, as every runP7Mle caller does.
ojson runP7MleAsM4(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
                          SessionState& session, ojson* rawOut, const std::string& stataCmd) {
  cobraArgs.push_back("--data");
  cobraArgs.push_back(*session.currentHbin());
  cobraArgs.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(cobraArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  if (rawOut) *rawOut = raw;
  ojson block = cobraToEstimationM4(raw, cobraCmd, &session);
  fillCoeftableStatsM4(block);
  cacheLastModelM4(session, cobraCmd, cobraArgs, block, raw, std::string(), stataCmd);
  return block;
}

ojson runP7Mle(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
               SessionState& session, ojson* rawOut) {
  return runP7MleAsM4(cobraCmd, std::move(cobraArgs), session, rawOut, std::string());
}

// BUG-1196: the depvar equation of an engine coefficient array as Stata names
// it -- `depvar:name`, `(intercept)` spelled `_cons` and placed last. False when
// a name is not a plain variable name (a factor level, an interaction, a
// time-series operator): Stata's columns there (base levels, `o.` marks) are
// not the engine's, and nothing is posted.
bool depvarEqColsM4(const std::string& depvar, const ojson& coefs, const char* bKey,
                    std::vector<AncillaryEbColM4>& out) {
  static const std::regex rePlain(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  if (!std::regex_match(depvar, rePlain) || !coefs.is_array() || coefs.empty()) return false;
  std::vector<AncillaryEbColM4> cols;
  std::optional<AncillaryEbColM4> cons;
  for (const auto& c : coefs) {
    if (!c.is_object() || !c.contains("name") || !c["name"].is_string() || !c.contains(bKey) ||
        !c[bKey].is_number())
      return false;
    const std::string nm = c["name"].get<std::string>();
    AncillaryEbColM4 col{std::string(), nm, c[bKey].get<double>()};
    if (nm == "(intercept)" || nm == "_cons") {
      col.stata = depvar + ":_cons";
      cons = col;
      continue;
    }
    if (!std::regex_match(nm, rePlain)) return false;
    col.stata = depvar + ":" + nm;
    cols.push_back(std::move(col));
  }
  if (cons.has_value()) cols.push_back(*cons);
  out.insert(out.end(), cols.begin(), cols.end());
  return true;
}

// BUG-1196: e(b)/e(V) of a fit whose Stata stripe has equations or ancillary
// parameters that the coefficient table does not carry (tobit, ologit/oprobit,
// nbreg, mlogit), posted as e() matrices, which every e(b)/e(V) reader answers
// before the stripe table's refusal (EvalSession::resolveEMatrixFor). `cols`
// are Stata's columns in order: its name, the engine's `e_V` name for it ("" for
// a column Stata posts as a structural 0 -- mlogit's base outcome), and b. e(V)
// is the engine's `--emit-v` covariance, made exactly symmetric as
// canonicalEMatrix makes it. Posts nothing -- the refusal stands -- unless the
// reply's `e_V` names every column with finite cells.
bool postAncillaryEbM4(SessionState& session, const ojson& raw,
                       const std::vector<AncillaryEbColM4>& cols) {
  const auto it = raw.find("e_V");
  if (cols.empty() || it == raw.end() || !it->is_object() || !it->contains("names") ||
      !it->contains("matrix"))
    return false;
  const ojson& vn = (*it)["names"];
  const ojson& vm = (*it)["matrix"];
  if (!vn.is_array() || !vm.is_array() || vm.size() != vn.size()) return false;
  const size_t k = cols.size();
  std::vector<long> at(k, -1);
  for (size_t a = 0; a < k; ++a) {
    if (!std::isfinite(cols[a].b)) return false;
    if (cols[a].engine.empty()) continue;
    for (size_t j = 0; j < vn.size(); ++j)
      if (vn[j].is_string() && vn[j].get<std::string>() == cols[a].engine) {
        at[a] = static_cast<long>(j);
        break;
      }
    if (at[a] < 0) return false;
  }
  const auto cell = [&vm, &vn](long r, long c, double* v) {
    const ojson& row = vm[static_cast<size_t>(r)];
    if (!row.is_array() || row.size() != vn.size() || !row[static_cast<size_t>(c)].is_number())
      return false;
    *v = row[static_cast<size_t>(c)].get<double>();
    return std::isfinite(*v);
  };
  eval::EvalMatrix mb, mv;
  mb.rownames = {"y1"};
  mb.data.emplace_back();
  mv.data.assign(k, std::vector<double>(k, 0.0));
  for (size_t a = 0; a < k; ++a) {
    mb.colnames.push_back(cols[a].stata);
    mb.data[0].push_back(cols[a].b);
    for (size_t c = 0; c < k; ++c) {
      if (at[a] < 0 || at[c] < 0) continue;
      double v1 = 0, v2 = 0;
      if (!cell(at[a], at[c], &v1) || !cell(at[c], at[a], &v2)) return false;
      mv.data[a][c] = 0.5 * (v1 + v2);
    }
  }
  mv.rownames = mb.colnames;
  mv.colnames = mb.colnames;
  for (auto* m : {&mb, &mv}) {
    const std::string nm = m == &mb ? "b" : "V";
    bool put = false;
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = *m;
        put = true;
      }
    if (!put) session.eval.eMatrices.emplace_back(nm, *m);
  }
  return true;
}

}  // namespace est_detail

}  // namespace piet::handlers
