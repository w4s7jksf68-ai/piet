// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// oaxaca: Blinder-Oaxaca decomposition (Ben Jann's oaxaca 4.1.1).

#include "piet/handlers/estimation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "statistics_command_support.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace statcmd_detail;

// ===========================================================================
// `oaxaca` (Ben Jann, SSC 4.1.1): the Blinder-Oaxaca decomposition.
//
//   oaxaca depvar [indepvars | (name: vars) | normalize(dummies [# var)]] [if] [in] [weight],
//          by(groupvar) [swap linear logit probit threefold[(reverse)] weight(numlist) split
//          pooled omega adjust(vars) nodetail detail(name: vars, ...) categorical(...)
//          nocheck x1(var # ...) x2(...) fixed[(vars)] vce(robust|cluster v) cluster(v)
//          robust nose suest nosuest model1(regress[, vce(robust)]) model2(...) relax
//          noisily level(#) eform xb nolegend nodefinitions notable noheader]
//
// The handler reads the varlist and the options the way the ado's syntax does
// (parenthesised detail groups, normalize() dummy sets, the old categorical()
// and detail() spellings), decides which variance construction the ado uses
// (the models' own e(V), robust regressions, or suest -- implied by pooled,
// omega and a cluster VCE), and hands one spec to the engine (cobra oaxaca,
// src/estimators/oaxaca.cpp: the models, the means and the decomposition with
// its Jacobian). e() is the ado's PostResults: b/V, b0/V0, N N_1 N_2 k_eq
// k_eform, and the macros of every option that shaped the table.
//
// Named refusals (not computed here): svy, reference(), model1()/model2()
// beyond `regress [, vce(robust)]`, bootstrap/jackknife VCEs, logit/probit
// with weights or suest, weights with suest. e(b)'s column names are the
// joined `eq:name` spellings (PIET's matrix store keeps no separate equation
// stripe), so `_b[overall:difference]` resolves by exact match.
namespace {
const eval::OptVocab& kVocabOaxaca = eval::registerOptVocab(
    {"oaxaca"},
    {{"by", "pooled", "relax", "noisily", "level", "linear", "nolegend", "swap", "logit", "probit",
      "nodetail", "adjust", "threefold", "weight", "omega", "split", "vce", "cluster", "fixed",
      "suest", "nosuest", "nose", "model1", "model2", "x1", "x2", "xb", "eform", "categorical",
      "detail", "nocheck", "robust", "nodefinitions", "notable", "noheader", "reference",
      "noisily2"},
     {"svy"},
     // oaxaca.ado's syntax line declares one-letter stems the 2-char prefix
     // rule cannot reach: b:y p:ooled l:evel a:djust w:eights o:mega r:obust.
     {{"b", "by"},
      {"p", "pooled"},
      {"l", "level"},
      {"a", "adjust"},
      {"w", "weight"},
      {"weights", "weight"},
      {"o", "omega"},
      {"r", "robust"},
      {"d", "detail"},
      {"model", "model1"},
      // the undocumented Noisily2 (show the suest and mean output): N is its stem,
      // so `n` and `no` are it (vs-stata bank 12e43f9a, 04ad3d96: rc 0)
      {"n", "noisily2"},
      {"no", "noisily2"}}});

ojson oaxacaEngineError(const CobraError& ce) {
  const std::string msg = cobraErrorMessage(ce);
  static const std::regex reRc(R"(r\((\d+)\))");
  std::smatch m;
  if (std::regex_search(ce.detail, m, reRc)) return aerErrorReason(msg, std::stoi(m.str(1)));
  if (ce.code == "no_obs") return aerErrorReason(msg, 2000);
  if (ce.code == "unsupported") return aerError(msg, "unsupported_option");
  if (ce.code == "bad_input" || ce.code == "collinear") return aerErrorReason(msg, 198);
  return aerError(msg, ce.code);
}

// Stata's numlist / levelsof spelling of a number: integers plainly, else %.15g
// with the leading zero dropped (".5").
std::string oxNum(double v) {
  char buf[64];
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  std::string s(buf);
  if (s.rfind("0.", 0) == 0) s = s.substr(1);
  else if (s.rfind("-0.", 0) == 0) s = "-" + s.substr(2);
  return s;
}

// Split at top-level blanks, keeping a parenthesised group (and `name(...)`) one token.
std::vector<std::string> bindTokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : s) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Split at top-level commas.
std::vector<std::string> splitCommas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  for (char c : s) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (depth == 0 && c == ',') {
      out.push_back(strutil::trim(cur));
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (!strutil::trim(cur).empty()) out.push_back(strutil::trim(cur));
  return out;
}

bool hasOperator(const std::string& t) {
  return t.find('.') != std::string::npos || t.find('#') != std::string::npos;
}

struct OxVarParse {
  std::vector<std::string> vars, xvars;                                           // ParseVar's r(vars) / r(xvars)
  std::vector<std::pair<std::string, std::vector<std::string>>> normalize;        // cons, dummies
  std::vector<std::pair<std::string, std::vector<std::string>>> vgroups;          // name, vars
};

std::vector<std::string> listMinus(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  std::vector<std::string> o;
  for (const auto& x : a)
    if (std::find(b.begin(), b.end(), x) == b.end()) o.push_back(x);
  return o;
}
std::vector<std::string> listAnd(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  std::vector<std::string> o;
  for (const auto& x : a)
    if (std::find(b.begin(), b.end(), x) != b.end()) o.push_back(x);
  return o;
}
bool inList(const std::vector<std::string>& a, const std::string& x) {
  return std::find(a.begin(), a.end(), x) != a.end();
}

}  // namespace

ojson runOaxaca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("oaxaca")) return engineTooOld("oaxaca", "oaxaca");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  std::string argText = strutil::trim(parsed.argText);
  // Replay (`oaxaca` with no varlist): the ado redisplays the last oaxaca fit;
  // e(cmd) is compared as a string, which is a type mismatch when no e() exists.
  if (argText.empty()) {
    const std::string cmd = session.eval.eStrings.get("cmd");
    if (cmd.empty()) return aerErrorReason("type mismatch", 109);
    if (cmd != "oaxaca") return aerErrorReason("last estimates not found", 301);
    if (session.lastEstBlock.has_value()) return *session.lastEstBlock;
    return aerErrorReason("last estimates not found", 301);
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOaxaca.known, kVocabOaxaca.unimplemented)) {
    return ojson(*err);
  }

  // ---- options -------------------------------------------------------------
  std::string by, level, threefoldArg, weightsArg, adjustArg, detailArg, categoricalArg, x1Arg, x2Arg,
      fixedArg, vceArg, clusterArg, model1Arg, model2Arg, referenceArg;
  bool swap = false, linear = false, logit = false, probit = false, threefold = false, split = false,
       pooled = false, omega = false, nodetail = false, nocheck = false, fixed = false, robust = false,
       nose = false, suestOpt = false, nosuest = false, relax = false, noisily = false, eform = false,
       xb = false, nolegend = false, nodefinitions = false, notable = false, noheader = false,
       hasWeights = false, hasPooledArg = false, hasOmegaArg = false, detailBare = false;
  for (const auto& [k, v] : parsed.opts) {
    const std::string sv = v.isBool ? std::string() : strutil::trim(v.strValue);
    if (k == "by") by = sv;
    else if (k == "swap") swap = true;
    else if (k == "linear") linear = true;
    else if (k == "logit") logit = true;
    else if (k == "probit") probit = true;
    else if (k == "threefold") {
      threefold = true;
      threefoldArg = sv;
    } else if (k == "weight") {
      hasWeights = true;
      weightsArg = sv;
    } else if (k == "split") split = true;
    else if (k == "pooled") {
      pooled = true;
      hasPooledArg = !v.isBool && !sv.empty();
    } else if (k == "omega") {
      omega = true;
      hasOmegaArg = !v.isBool && !sv.empty();
    } else if (k == "reference") referenceArg = sv;
    else if (k == "adjust") adjustArg = sv;
    else if (k == "nodetail") nodetail = true;
    else if (k == "detail") {
      if (!v.isBool) detailArg = sv;
      else detailBare = true;
    } else if (k == "categorical") categoricalArg = sv;
    else if (k == "nocheck") nocheck = true;
    else if (k == "x1") x1Arg = sv;
    else if (k == "x2") x2Arg = sv;
    else if (k == "fixed") {
      fixed = true;
      fixedArg = sv;
    } else if (k == "vce") vceArg = sv;
    else if (k == "cluster") clusterArg = sv;
    else if (k == "robust") robust = true;
    else if (k == "nose") nose = true;
    else if (k == "suest") suestOpt = true;
    else if (k == "nosuest") nosuest = true;
    else if (k == "model1") model1Arg = sv;
    else if (k == "model2") model2Arg = sv;
    else if (k == "relax") relax = true;
    else if (k == "noisily" || k == "noisily2") noisily = true;
    else if (k == "level") level = sv;
    else if (k == "eform") eform = true;
    else if (k == "xb") xb = true;
    else if (k == "nolegend") nolegend = true;
    else if (k == "nodefinitions") nodefinitions = true;
    else if (k == "notable") notable = true;
    else if (k == "noheader") noheader = true;
  }
  // noDetail is one option: `detail nodetail` is Stata's syntax refusal (bank 3460349e)
  if (detailBare && nodetail) return aerErrorReason("option nodetail not allowed", 198);
  if (by.empty()) return aerErrorReason("option by() required", 198);
  if (hasPooledArg || hasOmegaArg)
    return ojson(eval::refusedCommand("oaxaca", "pooled()/omega() model options"));
  double levelV = 95;
  if (!level.empty()) {
    char* end = nullptr;
    levelV = std::strtod(level.c_str(), &end);
    if (end == nullptr || *end != '\0' || !(levelV >= 10 && levelV <= 99.99))
      return aerErrorReason("level() must be between 10 and 99.99 inclusive", 198);
  } else {
    levelV = session.level;
  }

  // model type
  int nModelTypes = (linear ? 1 : 0) + (logit ? 1 : 0) + (probit ? 1 : 0);
  if (nModelTypes > 1) return aerErrorReason("only one of linear, logit, and probit allowed", 198);
  const std::string model = logit ? "logit" : (probit ? "probit" : "linear");
  if (model != "linear" && (!x1Arg.empty() || !x2Arg.empty()))
    return aerErrorReason("x1()/x2() not allowed in this context", 198);
  // model1()/model2(): `[cmd] [, options]`; only regress with vce(robust)/robust is computed
  bool robust1 = false, robust2 = false;
  for (int g = 1; g <= 2; ++g) {
    const std::string& m = g == 1 ? model1Arg : model2Arg;
    if (m.empty()) continue;
    std::string cmd = m, opts;
    const auto comma = m.find(',');
    if (comma != std::string::npos) {
      cmd = strutil::trim(m.substr(0, comma));
      opts = strutil::trim(m.substr(comma + 1));
    }
    if (!cmd.empty() && cmd != "regress" && cmd != "reg" && cmd != "regr" && cmd != "regre" && cmd != "regres") {
      if (cmd == "robust" || cmd.find_first_of(" (") != std::string::npos)
        return aerErrorReason("command " + cmd.substr(0, cmd.find_first_of(" (")) + " is unrecognized", 199);
      return ojson(eval::refusedCommand("oaxaca", "model" + std::to_string(g) + "() other than regress"));
    }
    if (model != "linear" && !cmd.empty() && cmd.rfind("reg", 0) == 0)
      return ojson(eval::refusedCommand("oaxaca", "model" + std::to_string(g) + "() with logit/probit"));
    const std::string lo = strutil::lower(opts);
    if (lo.empty()) continue;
    if (lo == "vce(robust)" || lo == "vce(r)" || lo == "robust" || lo == "r" || lo == "vce(rob)" ||
        lo == "vce(robu)" || lo == "vce(robus)" || lo == "rob" || lo == "robu" || lo == "robus") {
      (g == 1 ? robust1 : robust2) = true;
      continue;
    }
    return ojson(eval::refusedCommand("oaxaca", "model" + std::to_string(g) + "() options other than vce(robust)"));
  }

  // decomposition type
  if (!threefoldArg.empty()) {
    const std::string r = "reverse";
    if (threefoldArg.size() > r.size() || r.compare(0, threefoldArg.size(), threefoldArg) != 0)
      return aerErrorReason("threefold() invalid", 198);
  }
  const int nTypes = (threefold ? 1 : 0) + (hasWeights ? 1 : 0) + (pooled ? 1 : 0) + (omega ? 1 : 0) +
                     (referenceArg.empty() ? 0 : 1);
  if (nTypes > 1)
    return aerErrorReason("only one of threefold, weight(), pooled, omega, and reference() allowed", 198);
  // reference(name): a stored `regress` (no weights, no absorbed effects, plain
  // regressors) refit on its own e(sample) (the `_est_<name>` marker the store wrote)
  std::string refY, refSample;
  std::vector<std::string> refX;
  if (!referenceArg.empty()) {
    const auto hit = session.estimatesModels.find(referenceArg);
    if (hit == session.estimatesModels.end())
      return aerErrorReason("estimation result " + referenceArg + " not found", 111);
    const auto& st = hit->second;
    const bool plain = st && st->lastModel.has_value() && st->lastModel->stataCmd == "regress" &&
                       st->lastModel->fe.empty() && st->lastModel->fitWeight.empty() && st->esampleVar &&
                       !(parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) &&
                       !(parsed.inRange.has_value() && !parsed.inRange->empty());
    if (!plain) return ojson(eval::refusedCommand("oaxaca", "reference() of a model other than a plain stored regress"));
    for (const auto& v : st->lastModel->x)
      if (hasOperator(v)) return ojson(eval::refusedCommand("oaxaca", "reference() of a model with operators"));
    refY = st->lastModel->y;
    refX = st->lastModel->x;
    refSample = "_est_" + referenceArg;
  }
  int tf = 1;
  std::string ref;
  if (threefold) tf = threefoldArg.empty() ? 1 : 2;
  else if (pooled) { tf = 0; ref = "pooled"; }
  else if (omega) { tf = 0; ref = "omega"; }
  else if (hasWeights) { tf = 0; ref = "weights"; }
  else if (!referenceArg.empty()) { tf = 0; ref = "reference"; }
  std::vector<double> weights;
  std::string weightsMacro;
  if (hasWeights) {
    std::vector<std::string> el;
    if (weightsArg.empty() || !loops::expandNumlist(weightsArg, &el) || el.empty())
      return aerErrorReason("invalid numlist", 121);
    for (auto& e : el) {
      const std::string t = strutil::trim(e);
      weights.push_back(std::strtod(t.c_str(), nullptr));
      weightsMacro += (weightsMacro.empty() ? "" : " ") + t;
    }
  }

  // vce / suest
  const bool se = !nose;
  std::string clusterVar;
  bool vceRobust = false, vceCluster = false;
  if (!vceArg.empty()) {
    std::istringstream is(vceArg);
    std::string w0, w1, extra;
    is >> w0 >> w1 >> extra;
    const std::string l0 = strutil::lower(w0);
    auto isPrefix = [](const std::string& t, const std::string& full, size_t minLen) {
      return t.size() >= minLen && t.size() <= full.size() && full.compare(0, t.size(), t) == 0;
    };
    if (isPrefix(l0, "robust", 1) && w1.empty()) vceRobust = true;
    else if (isPrefix(l0, "cluster", 2) && !w1.empty() && extra.empty()) {
      vceCluster = true;
      clusterVar = w1;
    } else if (isPrefix(l0, "bootstrap", 4) || isPrefix(l0, "jackknife", 4))
      return ojson(eval::refusedCommand("oaxaca", "vce(" + w0 + ")"));
    else return aerErrorReason("vcetype '" + w0 + "' not allowed", 198);
  }
  if (!clusterArg.empty()) {
    if (!clusterVar.empty() && clusterVar != clusterArg) return aerErrorReason("option vce() misspecified", 198);
    clusterVar = clusterArg;
  }
  const bool regRobust = vceRobust || robust;
  if (suestOpt && nosuest) return aerErrorReason("suest and nosuest not both allowed", 198);
  bool suest = suestOpt || pooled || omega || !referenceArg.empty() || !clusterArg.empty() || vceCluster;
  if (!se || nosuest) suest = false;
  const bool clusterMeans = !clusterVar.empty();

  // weights
  std::string wtype, wvar, wexp;
  if (parsed.weightType.has_value()) {
    const std::string w = strutil::lower(*parsed.weightType);
    if (w == "fw" || w == "fweight") wtype = "fw";
    else if (w == "aw" || w == "aweight") wtype = "aw";
    else if (w == "pw" || w == "pweight") wtype = "pw";
    else if (w == "iw" || w == "iweight") wtype = "iw";
    wvar = strutil::trim(parsed.weightVar.value_or(""));
    wexp = "= " + strutil::trim(parsed.weightExp.value_or(wvar));
    if (wvar.empty() || wvar.find_first_of(" ()*+-/") != std::string::npos)
      return ojson(eval::refusedCommand("oaxaca", "weight expressions other than a variable"));
    if (suest) return ojson(eval::refusedCommand("oaxaca", "weights with suest"));
    if (model != "linear") return ojson(eval::refusedCommand("oaxaca", "weights with logit/probit"));
    if (regRobust && wtype != "pw") return ojson(eval::refusedCommand("oaxaca", "robust with [" + wtype + "]"));
    if (clusterMeans) return ojson(eval::refusedCommand("oaxaca", "weights with a cluster VCE"));
  }
  if (model != "linear" && suest) return ojson(eval::refusedCommand("oaxaca", "logit/probit with suest"));
  if ((robust1 || robust2) && (suest || regRobust))
    return ojson(eval::refusedCommand("oaxaca", "model1()/model2() with suest or robust"));

  // ---- the varlist (the ado's ParseVar loop) --------------------------------
  auto unab = [&](const std::string& spec, std::vector<std::string>& outv) -> std::optional<ojson> {
    for (const auto& t : bindTokens(spec))
      if (hasOperator(t)) return aerErrorReason("factor-variable and time-series operators not allowed", 101);
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    std::vector<std::string> toks = ex.tokens;
    if (auto e = loops::checkKnownVarlist(toks, session)) return *e;
    outv.insert(outv.end(), toks.begin(), toks.end());
    return std::nullopt;
  };
  OxVarParse vp;
  std::vector<std::string> toks = bindTokens(argText);
  if (toks.empty()) return aerErrorReason("too few variables specified", 198);
  std::string depvar;
  {
    std::vector<std::string> dv;
    if (toks[0].front() == '(') return aerErrorReason("too few variables specified", 198);
    if (auto e = unab(toks[0], dv)) return *e;
    if (dv.size() != 1) return aerErrorReason("too many variables specified", 103);
    depvar = dv[0];
  }
  auto parseVar = [&](const std::string& t, std::vector<std::string>& vars,
                      std::vector<std::string>& xvars) -> std::optional<ojson> {
    const std::string lt = strutil::lower(t);
    const auto par = t.find('(');
    std::string nm = par == std::string::npos ? std::string() : strutil::lower(t.substr(0, par));
    const std::string normalizeW = "normalize";
    if (par != std::string::npos && t.back() == ')' && !nm.empty() && nm.size() <= normalizeW.size() &&
        normalizeW.compare(0, nm.size(), nm) == 0) {
      std::string inside = t.substr(par + 1, t.size() - par - 2);
      std::string dummies = inside, cons = "_cons";
      const auto hash = inside.find('#');
      if (hash != std::string::npos) {
        dummies = inside.substr(0, hash);
        std::vector<std::string> cv;
        if (auto e = unab(strutil::trim(inside.substr(hash + 1)), cv)) return *e;
        if (cv.size() != 1) return aerErrorReason("too many variables specified", 103);
        cons = cv[0];
      }
      std::vector<std::string> dv;
      std::string base;
      for (const auto& tok : bindTokens(dummies)) {
        std::string v = tok;
        bool isBase = false;
        if (v.size() > 2 && strutil::lower(v.substr(0, 2)) == "b.") {
          if (!base.empty()) return aerErrorReason(v + " not allowed", 198);
          v = v.substr(2);
          isBase = true;
        }
        std::vector<std::string> u;
        if (auto e = unab(v, u)) return *e;
        if (isBase && !u.empty()) base = u[0];
        dv.insert(dv.end(), u.begin(), u.end());
      }
      if (base.empty() && !dv.empty()) base = dv[0];
      vars = dv;
      xvars = listMinus(dv, {base});
      vp.normalize.push_back({cons, dv});
      return std::nullopt;
    }
    std::vector<std::string> u;
    if (auto e = unab(t, u)) return *e;
    vars = u;
    xvars = u;
    return std::nullopt;
  };
  for (size_t i = 1; i < toks.size(); ++i) {
    const std::string& t = toks[i];
    if (t.front() == '(' && t.back() == ')') {
      std::string inner = t.substr(1, t.size() - 2);
      std::string gname;
      const auto colon = inner.find(':');
      if (colon != std::string::npos) {
        gname = strutil::trim(inner.substr(0, colon));
        inner = inner.substr(colon + 1);
      }
      std::vector<std::string> varsi;
      for (const auto& vt : bindTokens(inner)) {
        std::vector<std::string> a, b;
        if (auto e = parseVar(vt, a, b)) return *e;
        varsi.insert(varsi.end(), a.begin(), a.end());
        vp.xvars.insert(vp.xvars.end(), b.begin(), b.end());
      }
      if (gname.empty() && !varsi.empty()) gname = varsi[0];
      vp.vars.insert(vp.vars.end(), varsi.begin(), varsi.end());
      vp.vgroups.push_back({gname, varsi});
    } else {
      std::vector<std::string> a, b;
      if (auto e = parseVar(t, a, b)) return *e;
      vp.vars.insert(vp.vars.end(), a.begin(), a.end());
      vp.xvars.insert(vp.xvars.end(), b.begin(), b.end());
    }
  }
  {
    std::set<std::string> seen;
    for (const auto& v : vp.vars)
      if (!seen.insert(v).second) return aerErrorReason("invalid varlist; duplicate varnames not allowed", 198);
  }
  if (vp.normalize.empty() && !categoricalArg.empty()) {
    // the old syntax: categorical(varlist [, varlist ...])
    std::vector<std::string> vars0 = vp.vars;
    std::vector<std::string> vars = vars0, xvars = vp.xvars, cvars;
    for (const auto& grp : splitCommas(categoricalArg)) {
      std::string g = grp, cons = "_cons";
      const auto lp = g.find('(');
      if (lp != std::string::npos) {
        std::string c = g.substr(lp + 1);
        if (!c.empty() && c.back() == ')') c.pop_back();
        g = g.substr(0, lp);
        std::vector<std::string> cv;
        if (auto e = unab(strutil::trim(c), cv)) return *e;
        if (cv.size() != 1) return aerErrorReason("invalid categorical() option", 198);
        cons = cv[0];
      }
      std::vector<std::string> one;
      if (auto e = unab(strutil::trim(g), one)) return *e;
      if (one.empty() || !listAnd(cvars, one).empty()) return aerErrorReason("invalid categorical() option", 198);
      std::vector<std::string> tmp = listAnd(vars0, one);
      if (tmp.empty()) {
        vars.insert(vars.end(), one.begin(), one.end());
        xvars.insert(xvars.end(), one.begin() + 1, one.end());
      } else {
        tmp = listMinus(one, vars0);
        if (tmp.empty()) {
          xvars = listMinus(xvars, {one[0]});
        } else {
          const std::string base = tmp[0];
          tmp = listAnd(one, vars0);
          const std::string first = tmp[0];
          tmp.erase(tmp.begin());
          vars = listMinus(vars, tmp);
          xvars = listMinus(xvars, tmp);
          std::vector<std::string> nv;
          for (const auto& v : vars) {
            if (v == first) nv.insert(nv.end(), one.begin(), one.end());
            else nv.push_back(v);
          }
          vars = nv;
          std::vector<std::string> rest = listMinus(one, {base}), nx;
          for (const auto& v : xvars) {
            if (v == first) nx.insert(nx.end(), rest.begin(), rest.end());
            else nx.push_back(v);
          }
          xvars = nx;
        }
      }
      cvars.insert(cvars.end(), one.begin(), one.end());
      vp.normalize.push_back({cons, one});
    }
    vp.vars = vars;
    vp.xvars = xvars;
  }
  for (const auto& [cons, dums] : vp.normalize) {
    if (cons == "_cons") continue;
    if (!inList(vp.vars, cons)) {
      vp.vars.push_back(cons);
      vp.xvars.push_back(cons);
    }
  }
  if (vp.vgroups.empty() && !detailArg.empty()) {
    if (nodetail) return aerErrorReason("nodetail and detail() not both allowed", 198);
    for (const auto& grp : splitCommas(detailArg)) {
      const auto c = grp.find_first_of(":=");
      const std::string gname = c == std::string::npos ? std::string() : strutil::trim(grp.substr(0, c));
      const std::string vs = c == std::string::npos ? std::string() : strutil::trim(grp.substr(c + 1));
      if (gname.empty() || vs.empty()) return aerErrorReason("invalid detail() option", 198);
      std::vector<std::string> u;
      for (const auto& t : bindTokens(vs)) {
        std::vector<std::string> one;
        if (auto e = unab(t, one)) {
          u.push_back(t);  // the ado's Unab keeps a name that is no variable as typed
          continue;
        }
        u.insert(u.end(), one.begin(), one.end());
      }
      vp.vgroups.push_back({gname, u});
    }
  }
  if (nodetail) vp.vgroups.clear();
  for (const auto& v : vp.vars)
    if (v == by) return ojson(eval::refusedCommand("oaxaca", "the group variable among the regressors"));
  if (vp.vars.empty()) {
    // a constant-only model: the decomposition is the mean difference
    return ojson(eval::refusedCommand("oaxaca", "a model without regressors"));
  }

  // adjust(), fixed(), x1()/x2() name the coefficients
  std::vector<std::string> coefs = vp.vars;
  coefs.push_back("_cons");
  std::vector<std::string> adjustList, fixedVars;
  if (!adjustArg.empty()) {
    for (const auto& t : bindTokens(adjustArg)) {
      std::vector<std::string> one;
      if (unab(t, one)) one = {t};
      adjustList.insert(adjustList.end(), one.begin(), one.end());
    }
    const auto nf = listMinus(adjustList, coefs);
    if (!nf.empty()) {
      std::string s;
      for (const auto& x : nf) s += (s.empty() ? "" : " ") + x;
      return aerErrorReason("adjust(): " + s + " not found in models", 111);
    }
  }
  if (fixed && !fixedArg.empty()) {
    for (const auto& t : bindTokens(fixedArg)) {
      std::vector<std::string> one;
      if (unab(t, one)) one = {t};
      const auto hit = listAnd(one, coefs);
      if (hit.empty()) return aerErrorReason(t + " not found", 111);
      for (const auto& h : hit)
        if (!inList(fixedVars, h)) fixedVars.push_back(h);
    }
  }
  std::vector<std::pair<std::string, double>> xvals[2];
  for (int g = 0; g < 2; ++g) {
    const std::string& a = g == 0 ? x1Arg : x2Arg;
    if (a.empty()) continue;
    std::string s = a;
    for (char& c : s)
      if (c == '=' || c == ',') c = ' ';
    std::istringstream is(s);
    std::string var, val;
    std::vector<std::string> left = coefs;
    while (is >> var) {
      if (!(is >> val)) return aerErrorReason("invalid x" + std::to_string(g + 1) + "() option", 198);
      char* end = nullptr;
      const double num = std::strtod(val.c_str(), &end);
      if (end == nullptr || *end != '\0') return aerErrorReason("invalid x" + std::to_string(g + 1) + "() option", 198);
      std::vector<std::string> one;
      if (unab(var, one)) one = {var};
      const auto hit = listAnd(left, one);
      if (hit.empty()) return aerErrorReason("x" + std::to_string(g + 1) + "(): " + var + " not found", 111);
      left = listMinus(left, hit);
      for (const auto& h : hit) xvals[g].push_back({h, num});
    }
  }

  // ---- sample: if / in -----------------------------------------------------
  std::optional<std::string> cond = parsed.ifExpr;
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    const InRangeBounds rb = parseInRangeBounds(*parsed.inRange, session.macroObsCount());
    if (!rb.ok) {
      InRangeFilterResult r;
      r.ok = false;
      r.refusal = rb.refusal;
      return inRangeRejectError(r, *parsed.inRange, "oaxaca");
    }
    const std::string range = "(_n >= " + std::to_string(rb.a) + " & _n <= " + std::to_string(rb.b) + ")";
    cond = (cond.has_value() && !cond->empty()) ? "(" + *cond + ") & " + range : range;
  }

  ojson spec = {{"y", depvar}, {"by", by}, {"swap", swap}, {"model", model}, {"tf", tf},
                {"split", split}, {"detail", !nodetail}, {"se", se}, {"suest", suest},
                {"robust", regRobust}, {"relax", relax}, {"check", !nocheck},
                {"cluster_means", clusterMeans}, {"level", levelV}};
  spec["vars"] = vp.vars;
  {
    std::vector<int> reg;
    for (const auto& v : vp.vars) reg.push_back(inList(vp.xvars, v) ? 1 : 0);
    spec["regressor"] = reg;
  }
  if (!ref.empty()) spec["ref"] = ref;
  if (ref == "reference") {
    spec["ref_y"] = refY;
    spec["ref_x"] = refX;
    spec["ref_sample"] = refSample;
  }
  if (!weights.empty()) spec["weights"] = weights;
  if (!vp.vgroups.empty()) {
    ojson g = ojson::array();
    for (const auto& [n, vs] : vp.vgroups) g.push_back(ojson::array({n, vs}));
    spec["vgroups"] = g;
  }
  if (!vp.normalize.empty()) {
    ojson g = ojson::array();
    for (const auto& [n, vs] : vp.normalize) g.push_back(ojson::array({n, vs}));
    spec["normalize"] = g;
  }
  if (!adjustList.empty()) spec["adjust"] = adjustList;
  if (fixed) {
    if (fixedArg.empty()) spec["fixed"] = true;
    else spec["fixed"] = fixedVars;
  }
  for (int g = 0; g < 2; ++g)
    if (!xvals[g].empty()) {
      ojson a = ojson::array();
      for (const auto& [v, x] : xvals[g]) a.push_back(ojson::array({v, x}));
      spec[g == 0 ? "x1" : "x2"] = a;
    }
  if (!wtype.empty()) {
    spec["wvar"] = wvar;
    spec["wtype"] = wtype;
  }
  if (!clusterVar.empty()) {
    std::vector<std::string> cv;
    if (auto e = unab(clusterVar, cv)) return *e;
    clusterVar = cv.empty() ? clusterVar : cv[0];
    spec["cluster"] = clusterVar;
  }
  if (robust1 || robust2) spec["robust_models"] = ojson::array({robust1, robust2});

  std::vector<std::string> args = {"oaxaca", "--data", *session.currentHbin(), "--spec", spec.dump()};
  if (cond.has_value() && !cond->empty()) {
    args.push_back("--keepif");
    args.push_back(*cond);
  }
  const std::vector<std::string> fitArgs = args;
  const std::string esampleValues = esampleFitArgs(session, args, std::string(), 0);
  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    cecon::native_unlink(esampleValues);
    return oaxacaEngineError(ce);
  }

  // ---- e() (the ado's PostResults) -----------------------------------------
  const std::vector<std::string> names = raw["e_b"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta = jsonDoubles(raw["e_b"]["values"]);
  const std::vector<std::string> names0 = raw["e_b0"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta0 = jsonDoubles(raw["e_b0"]["values"]);
  const bool robustModels = raw.value("robust_models", false);
  std::string eVce, eVcetype;
  if (robustModels) {
    eVce = "robust";
    eVcetype = "Robust";
  }
  if (suest) {
    eVce = clusterVar.empty() ? "robust" : "cluster";
    eVcetype = "Robust";
  }
  if (se) {
    // the means' own VCE (mean ..., over() vce() cluster()) has the last word
    if (clusterMeans) {
      eVce = "cluster";
      eVcetype = "Robust";
    } else if (wtype == "pw") {
      eVce = "robust";
      eVcetype.clear();
    }
  }

  LastModel lm;
  lm.cobraCmd = "oaxaca";
  lm.y = depvar;
  lm.depvar = depvar;
  lm.x = vp.vars;
  lm.vcov = "";
  lm.dataPath = *session.currentHbin();
  est_detail::captureFitFilterM4(lm, fitArgs);
  ojson rawO = ojson(raw);
  (void)esampleInstallOnHead(session, esampleValues, rawO, lm);
  lm.result = rawO;
  session.setLastModel(lm);

  auto& ev = session.eval;
  ev.setEScalar("N", raw.value("n_obs", 0.0));
  ev.setEScalar("N_1", raw.value("N_1", 0.0));
  ev.setEScalar("N_2", raw.value("N_2", 0.0));
  const double kEq = raw.value("k_eq", 0.0);
  ev.setEScalar("k_eq", kEq);
  ev.setEScalar("k_eform", kEq);
  if (clusterMeans && raw.contains("N_clust")) ev.setEScalar("N_clust", raw["N_clust"].get<double>());
  ev.setEString("cmd", "oaxaca");
  ev.setEString("title", "Blinder-Oaxaca decomposition");
  ev.setEString("by", by);
  ev.setEString("group_1", oxNum(raw.value("group_1", 0.0)));
  ev.setEString("group_2", oxNum(raw.value("group_2", 0.0)));
  ev.setEString("depvar", depvar);
  ev.setEString("model", model);
  ev.setEString("weights", weightsMacro);
  ev.setEString("refcoefs", ref == "pooled" || ref == "omega" ? ref : (ref == "reference" ? referenceArg : std::string()));
  ev.setEString("threefold", tf == 1 ? "threefold" : (tf == 2 ? "threefold(reverse)" : ""));
  ev.setEString("split", split ? "split" : "");
  {
    std::string adj;
    for (const auto& a : adjustList) adj += (adj.empty() ? "" : " ") + a;
    ev.setEString("adjust", adj);
  }
  {
    std::string nz;
    for (const auto& [cons, dums] : vp.normalize) {
      std::string g;
      for (const auto& d : dums) g += (g.empty() ? "" : " ") + d;
      if (cons != "_cons") g += " # " + cons;
      nz += (nz.empty() ? "" : " ") + std::string("\"") + g + "\"";
    }
    ev.setEString("normalized", nz);
  }
  ev.setEString("legend", raw.value("legend", std::string()));
  {
    std::string fx;
    if (fixed && fixedArg.empty()) fx = "fixed";
    else if (fixed && !fixedVars.empty()) {
      fx = "fixed(";
      for (size_t i = 0; i < fixedVars.size(); ++i) fx += (i ? " " : "") + fixedVars[i];
      fx += ")";
    }
    ev.setEString("fixed", se ? fx : std::string());
  }
  ev.setEString("suest", suest ? "suest" : "");
  ev.setEString("vce", eVce);
  ev.setEString("vcetype", eVcetype);
  ev.setEString("clustvar", clusterMeans && se ? clusterVar : std::string());
  ev.setEString("wtype", wtype.empty() ? std::string() : wtype == "fw" ? "fweight" : wtype == "aw" ? "aweight"
                                                         : wtype == "pw" ? "pweight" : "iweight");
  ev.setEString("wexp", wtype.empty() ? std::string() : wexp);
  ev.setEString("properties", se ? "b V" : "b");
  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) {
        kv.second = std::move(m);
        return;
      }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  auto rowMat = [](const std::vector<std::string>& nm, const std::vector<double>& v, const char* row) {
    eval::EvalMatrix b;
    b.data.push_back(v);
    b.rownames = {row};
    b.colnames = nm;
    return b;
  };
  auto sqMat = [](const std::vector<std::string>& nm, const nlohmann::json& rows) {
    eval::EvalMatrix V;
    for (const auto& row : rows) V.data.push_back(jsonDoubles(row));
    V.rownames = nm;
    V.colnames = nm;
    return V;
  };
  postM("b", rowMat(names, beta, "y1"));
  if (se) postM("V", sqMat(names, raw["e_V"]["matrix"]));
  postM("b0", rowMat(names0, beta0, "r1"));  // the ado builds b0 by `matrix rename`: row r1
  if (se) postM("V0", sqMat(names0, raw["e_V0"]["matrix"]));

  // ---- the estimation block ---------------------------------------------------
  ojson block = est_detail::cobraToEstimationM4(ojson(raw), "oaxaca", &session);
  block["estimator"] = "oaxaca";
  block["depvar"] = depvar;
  block["level"] = levelV;
  block["by"] = by;
  block["group_1"] = oxNum(raw.value("group_1", 0.0));
  block["group_2"] = oxNum(raw.value("group_2", 0.0));
  block["N_1"] = raw.value("N_1", 0.0);
  block["N_2"] = raw.value("N_2", 0.0);
  block["model"] = model;
  block["tf"] = tf;
  block["ref"] = ref;
  block["split"] = split;
  block["adjusted"] = !adjustList.empty();
  {
    std::string adj;
    for (const auto& a : adjustList) adj += (adj.empty() ? "" : " ") + a;
    block["adjust"] = adj;
  }
  block["weights_macro"] = weightsMacro;
  block["refname"] = referenceArg;
  block["vcetype"] = eVcetype;
  block["se"] = se;
  block["eform"] = eform;
  block["show_header"] = !noheader;
  block["show_table"] = !notable;
  block["show_definitions"] = !nodefinitions;
  block["show_legend"] = !nolegend;
  if (!vp.vgroups.empty() && !nodetail) block["legend"] = raw.value("legend", std::string());
  if (xb) {
    // the models' coefficients and the means (e(b0)), shown under the table
    ojson b0 = ojson::array();
    const auto se0 = se ? raw["e_V0"]["matrix"] : nlohmann::json();
    for (size_t j = 0; j < names0.size(); ++j) {
      ojson r = {{"name", names0[j]}, {"b", beta0[j]}};
      if (se) {
        const double s0 = std::sqrt(std::max(0.0, se0[j][j].get<double>()));
        const double zc = cecon::z_critical(1 - levelV / 100);
        r["se"] = s0;
        r["t"] = s0 > 0 ? ojson(beta0[j] / s0) : ojson(nullptr);
        r["p"] = s0 > 0 ? ojson(std::erfc(std::fabs(beta0[j] / s0) / std::sqrt(2.0))) : ojson(nullptr);
        r["ci_lo"] = s0 > 0 ? ojson(beta0[j] - zc * s0) : ojson(nullptr);
        r["ci_hi"] = s0 > 0 ? ojson(beta0[j] + zc * s0) : ojson(nullptr);
      }
      b0.push_back(r);
    }
    block["xb"] = b0;
  }
  // r(table): the last table shown -- the decomposition, or with xb the models'
  // coefficients and means (e(b0), bank 9a6960b8). Normal reference, eform if
  // asked; without a variance (nose) a one-row table of b (bank adaef5f7).
  {
    ojson rc = ojson::array();
    const ojson& src = xb ? block["xb"] : block["coeftable"];
    for (const auto& row : src) {
      ojson c = row;
      c["eform"] = eform;
      if (row.contains("t")) c["z"] = row["t"];
      if (se && row.contains("se") && row["se"].is_number() && row["se"].get<double>() == 0.0) {
        // a coefficient held at 0 with variance 0 (relax): Stata's "(omitted)" column;
        // any other estimate with variance 0 (a V the post screen zeroed, a fixed
        // mean such as x1:_cons) keeps b with every inferential cell missing
        if (row.value("b", 1.0) == 0.0 && !xb) c["omitted"] = true;
        else c["se_missing"] = true;
      }
      rc.push_back(c);
    }
    block["rtable_cols"] = rc;
    if (xb) {
      block["rtable_of"] = "b0";
      block["rtable_mcmethod"] = "noadjust";  // the b0 table is an `ereturn display`
    }
    if (!se) block["rtable_b_only"] = true;
  }
  session.lastEstBlock = block;

  ojson blocks = ojson::array();
  if (noisily) {
    blocks.push_back(displayBlock(
        "(oaxaca noisily: the underlying group regressions are not displayed in PIET -- "
        "decomposition unaffected)"));
  }
  if (raw.contains("notes") && raw["notes"].is_array())
    for (const auto& w : raw["notes"]) blocks.push_back(displayBlock(w.get<std::string>()));
  blocks.push_back(block);
  return blocks;
}

}  // namespace piet::handlers
