// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Model comparison.
#include "piet/handlers/estimation.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>
#include "piet/cobra_runner.h"
#include "piet/cobra_session.h"
#include "piet/display_hardening.h"  // exprUnboundNameRefusal (predict, BUG-1248)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/settings.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"
#include "io/win_compat.h"  // cecon::native_unlink (BUG-050 wide unlink)
#include "piet/translate.h"
#include "piet/scratch_seq.h"
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation_internal.h"
#include "piet/eval/macros.h"  // resolveEstScalarRefs (hausman e(sigma))
#include "matrix/lapack_dsyevr.h"  // hausman: (V_b-V_B)'s eigendecomposition
#include "estimation/estimation_options.h"

namespace piet::handlers {
double chi2UpperTailIntegerDf(double df, double x);  // postestimation/likelihood_ratio_test.cpp
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

ojson runSdtest(const std::string& varName, const std::optional<std::string>& againstVal,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                const std::optional<std::string>& ifExpr, SessionState& session,
                const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (varName.empty()) {
    return engineErrorBlockM4("syntax_error: `sdtest` requires a variable", "syntax_error");
  }
  auto refuse = [](const std::string& msg, int rc) {
    return ojson(eval::errorBlock(rc == 111 ? "missing_column" : "syntax_error", msg, std::nullopt,
                                  std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
  };

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabSdtest.known;
  if (auto err = eval::rejectUnknownOptions(opts, kKnown)) {
    return *err;
  }

  std::optional<std::string> byVal;
  std::optional<std::string> levelVal;
  for (const auto& [k, v] : opts) {
    if (k == "by" && !v.isBool) byVal = trimM4(v.strValue);
    if (k == "level") levelVal = v.isBool ? std::string() : trimM4(v.strValue);
  }
  // sdtest.ado: `==` is `=` (turned into it before syntax), so the right-hand
  // side is what follows the last `=`: a number is the one-sample chi-squared
  // test, a variable the variance-ratio test of two variables.
  std::string rhs = againstVal.has_value() ? trimM4(*againstVal) : std::string();
  while (!rhs.empty() && rhs.front() == '=') rhs = trimM4(rhs.substr(1));
  const bool hasRhs = againstVal.has_value();
  // Level(cilevel): 10 to 99.99, at most two decimals, else r(198).
  if (levelVal.has_value()) {
    double lv = std::numeric_limits<double>::quiet_NaN();
    size_t pos = 0;
    try {
      lv = std::stod(*levelVal, &pos);
    } catch (...) {
      pos = 0;
    }
    if (levelVal->empty() || pos != levelVal->size() || std::isnan(lv))
      return refuse("option level() incorrectly specified", 198);
    if (!(lv >= 10.0 && lv <= 99.99))
      return refuse("level() must be between 10 and 99.99 inclusive", 198);
  }
  Datastore* ds = session.datastore();
  if (ds != nullptr && !ds->has(varName)) return refuse("variable " + varName + " not found", 111);
  if (hasRhs && byVal.has_value()) return refuse("may not combine = and by()", 198);
  if (!hasRhs && !byVal.has_value()) return refuse("by() option required", 100);
  if (hasRhs && rhs.empty()) return refuse("invalid syntax", 198);

  bool rhsNumber = false;
  if (hasRhs) {
    try {
      size_t pos = 0;
      std::stod(rhs, &pos);
      rhsNumber = pos == rhs.size();
    } catch (...) {
      rhsNumber = false;
    }
    if (!rhsNumber && ds != nullptr && !ds->has(rhs))
      return refuse("variable " + rhs + " not found", 111);
  }
  if (byVal.has_value() && ds != nullptr && !ds->has(*byVal))
    return refuse("variable " + *byVal + " not found", 111);
  // A string variable has no observations for summarize: the sample check's r(2000).
  if (ds != nullptr && ds->dtype(varName) == "factor")
    return ojson(eval::errorBlock("engine_error",
                                  rhsNumber ? "no observations" : "first sample has no observations",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(2000)")));
  if (hasRhs && !rhsNumber && ds != nullptr && ds->dtype(rhs) == "factor")
    return ojson(eval::errorBlock("engine_error", "second sample has no observations", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(2000)")));

  std::vector<std::string> argv = {"sdtest"};
  if (byVal.has_value()) {
    argv.insert(argv.end(), {"--var", varName, "--by", *byVal});
  } else if (rhsNumber) {
    argv.insert(argv.end(), {"--var", varName, "--against", rhs});
  } else {
    argv.insert(argv.end(), {"--vars", varName + "," + rhs});
  }
  if (levelVal.has_value()) {
    argv.push_back("--level");
    argv.push_back(*levelVal);
  }

  // `if` and `in` select the sample (sdtest.ado's touse): the shared row filter.
  ojson ifErr;
  auto dataPathOpt = applyIfFilterM4(session, *session.currentHbin(), ifExpr, ifErr, inRange);
  if (!dataPathOpt.has_value()) return ifErr;
  argv.push_back("--data");
  argv.push_back(*dataPathOpt);
  argv.push_back("--tidy");

  ojson raw;
  try {
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    // The sample checks carry Stata's rc (r(2000) empty sample, r(416) an sd
    // summarize cannot form); by() must name exactly two groups, r(420).
    static const std::regex kRc(R"(^r\((\d+)\)$)");
    if (std::regex_match(ce.detail, kRc))
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, ce.detail));
    const std::string msg = cobraErrorStrM4(ce);
    static const std::regex kGroups(R"(must define exactly 2 groups \(found (\d+)\))");
    std::smatch gm;
    if (std::regex_search(msg, gm, kGroups)) {
      const int found = std::stoi(gm[1].str());
      if (found == 0)
        return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                      std::nullopt, std::string("r(2000)")));
      return ojson(eval::errorBlock(
          "engine_error",
          found > 2 ? "more than 2 groups found, only 2 allowed" : "1 group found, 2 required",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(420)")));
    }
    return engineErrorBlockM4(msg, ce.code);
  }

  ojson block = cobraToEstimationM4(raw, "sdtest", &session);
  block["estimator"] = "sdtest";
  static const char* kSdtestFields[] = {
      "F", "df1", "df2", "p_lower", "p_upper", "mode", "sd", "sd_1", "sd_2",
      "groups", "combined", "level", "chi2", "sd0", "se", "ci_lo", "ci_hi", "var", "var2",
  };
  for (const char* k : kSdtestFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  // r(): the names sdtesti stores for the form that ran (one-sample: N df chi2 sd
  // p_u p_l p; two-sample: N df_1 df_2 F sd_1 sd_2 p_u p_l p sd). A statistic the
  // sample cannot form (one observation) is posted as `.`.
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (raw.contains("r") && raw["r"].is_object()) {
    for (auto it = raw["r"].begin(); it != raw["r"].end(); ++it) {
      if (it.value().is_null()) {
        session.eval.rValues.set(it.key(), std::numeric_limits<double>::quiet_NaN());
      } else if (it.value().is_number()) {
        session.eval.rValues.set(it.key(), it.value().get<double>());
      } else {
        continue;
      }
      posted.push_back(it.key());
    }
  }
  eval::declareRPosted(session.eval, "sdtest", std::move(posted));
  return block;
}

// hausman: Stata's test of two fitted models from their stored e(b) and e(V)
// ([R] hausman, Methods and formulas; the steps and their order as hausman.ado
// takes them). b is the always-consistent fit (name1), B the efficient one
// (name2, `.` = the active results, by their stored name when they are a
// stored model). Per fit: a robust/cluster VCE or pweights are r(198) unless
// force; e(b)/e(V) are read through the macro layer's own resolver (a stored
// model's e() through a probe session holding only its stores, as lrtest and
// suest read them). Then: columns whose variance is 0 are dropped (omitted and
// base levels; nothing left is r(498) "nothing to test"); skipeqs() drops the
// listed equations, equations(#:#) pairs equations by number, the default
// keeps the first equation, alleqs all; _cons goes unless constant;
// sigmamore/sigmaless rescale V_b / V_B by the squared ratio of the fits'
// sigma (xtreg fe/ml: e(sigma_e), xtreg otherwise e(rmse), other commands
// e(sigma), else e(rmse)); b's columns are matched by equation:name in B (no
// match at all is r(498)). chi2 = (b-B)' (V_b-V_B)^+ (b-B) with the
// Moore-Penrose inverse from the eigendecomposition of the symmetric
// difference (singular values |lambda|, tolerance max|lambda| * k * 1e-9),
// df = its rank unless df(#), p = chi2tail(df, chi2); r(chi2) r(df) r(p)
// r(rank). V_b-V_B "not positive definite" when its smallest eigenvalue is
// not > 0. Verified on xtreg fe/re against the bank (ff3148eb: chi2
// 1.153823266788476, rank 2).
namespace {

struct HausFit {
  std::string name;  // as given ("." stays ".")
  std::string cmd;   // e(cmd2) when set, else e(cmd)
  std::vector<std::string> eq, col;
  std::vector<double> b;
  std::vector<std::vector<double>> V;
  double sigma = std::numeric_limits<double>::quiet_NaN();
};

double hausScalar(const eval::EvalSession& ev, const std::string& name) {
  std::string t = strutil::trim(eval::resolveEstScalarRefs("e(" + name + ")", ev, nullptr));
  if (t.size() >= 2 && t.front() == '(' && t.back() == ')') t = strutil::trim(t.substr(1, t.size() - 2));
  if (t.empty() || t[0] == '"' || t[0] == '.') return std::numeric_limits<double>::quiet_NaN();
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  return (end != nullptr && *end == '\0') ? v : std::numeric_limits<double>::quiet_NaN();
}

ojson hausErr(const std::string& msg, int rc) {
  return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

std::optional<ojson> hausLoad(const std::string& name, const eval::EvalSession& ev, bool force, HausFit& f) {
  f.name = name;
  f.cmd = ev.eStrings.get("cmd2");
  if (f.cmd.empty()) f.cmd = ev.eStrings.get("cmd");
  if (!force && (!ev.eStrings.get("clustvar").empty() || ev.eStrings.get("vce") == "robust" ||
                 ev.eStrings.get("vcetype") == "Robust" || ev.eStrings.get("wtype") == "pweight"))
    return hausErr("hausman cannot be used with vce(robust), vce(cluster cvar), or p-weighted data", 198);
  using Reader = eval::EvalSession::EReader;
  const auto eb = ev.resolveEMatrixFor("b", Reader::kNames);
  const auto eV = ev.resolveEMatrixFor("V", Reader::kNames);
  if (!eb.matrix.has_value()) return hausErr("e(b) not found in " + name, 198);
  if (!eV.matrix.has_value()) return hausErr("e(V) not found in " + name, 198);
  const eval::EvalMatrix& B = *eb.matrix;
  const eval::EvalMatrix& V = *eV.matrix;
  const size_t k = B.colnames.size();
  if (B.data.size() != 1 || B.data[0].size() != k || V.data.size() != k)
    return ojson(eval::refusedCommand("hausman", "stored_e_b_V_shape"));
  for (size_t i = 0; i < k; ++i) {
    const std::string& full = B.colnames[i];
    const size_t c = full.find(':');
    f.eq.push_back(c == std::string::npos ? std::string("_") : full.substr(0, c));
    f.col.push_back(c == std::string::npos ? full : full.substr(c + 1));
    if (V.data[i].size() != k) return ojson(eval::refusedCommand("hausman", "stored_e_b_V_shape"));
  }
  f.b = B.data[0];
  f.V = V.data;
  if (f.cmd == "xtreg") {
    const std::string model = ev.eStrings.get("model");
    f.sigma = hausScalar(ev, (model == "fe" || model == "ml") ? "sigma_e" : "rmse");
  } else {
    f.sigma = hausScalar(ev, "sigma");
    if (std::isnan(f.sigma)) f.sigma = hausScalar(ev, "rmse");
  }
  return std::nullopt;
}

void hausKeep(HausFit& f, const std::vector<bool>& keep) {
  HausFit g = f;
  g.eq.clear();
  g.col.clear();
  g.b.clear();
  g.V.clear();
  std::vector<size_t> idx;
  for (size_t i = 0; i < keep.size(); ++i)
    if (keep[i]) idx.push_back(i);
  for (size_t i : idx) {
    g.eq.push_back(f.eq[i]);
    g.col.push_back(f.col[i]);
    g.b.push_back(f.b[i]);
    std::vector<double> row;
    for (size_t j : idx) row.push_back(f.V[i][j]);
    g.V.push_back(std::move(row));
  }
  f = std::move(g);
}

// The equations of a fit in their order of first appearance.
std::vector<std::string> hausEquations(const HausFit& f) {
  std::vector<std::string> out;
  for (const auto& e : f.eq)
    if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
  return out;
}

}  // namespace

ojson runHausman(const std::string& name1In, const std::string& name2In,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  // hausman is r-class: whatever it ends with, the previous r() is gone.
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "hausman", {});
  // syntax [, Alleqs Constant SIGmamore SIGMALess FORCE SKipeqs() SYMINV
  // EQuations() REVEQ TCONsistent() TEFFicient() df()]: `sig`, `sigm` and
  // `sigma` are sigmamore, the first option they abbreviate.
  bool alleqs = false, constant = false, sigmamore = false, sigmaless = false, force = false;
  std::optional<std::string> skipeqs, equations, dfText;
  std::vector<std::pair<std::string, eval::OptValue>> rest;
  for (const auto& [k, v] : opts) {
    const std::string lk = lowerM4(k);
    if (lk == "alleqs" && v.isBool) alleqs = true;
    else if (lk == "constant" && v.isBool) constant = true;
    else if ((lk == "sigmamore" || lk == "sig" || lk == "sigm" || lk == "sigma") && v.isBool) sigmamore = true;
    else if (lk == "sigmaless" && v.isBool) sigmaless = true;
    else if (lk == "force" && v.isBool) force = true;
    else if (lk == "skipeqs" && !v.isBool) skipeqs = v.strValue;
    else if (lk == "equations" && !v.isBool) equations = v.strValue;
    else if (lk == "df" && !v.isBool) dfText = v.strValue;
    else rest.emplace_back(k, v);
  }
  // tconsistent()/tefficient() only retitle the printed table, which the
  // result block does not draw yet: they keep the named refusal.
  static const std::unordered_set<std::string> kNone;
  static const std::unordered_set<std::string> kNotBuilt = {"tconsistent", "tefficient", "syminv", "reveq"};
  if (auto err = eval::rejectUnknownOptions(rest, kNone, kNotBuilt)) return *err;
  std::optional<double> dfOpt;
  if (dfText.has_value()) {
    const std::string t = trimM4(*dfText);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (t.empty() || end == nullptr || *end != '\0' || v != std::floor(v) || !(v > 0) || !(v < 11000))
      return hausErr("option df() invalid; numlist must be integer between 1 and 10999", 125);
    dfOpt = v;
  }
  // equations(#:# [, #:# ...]): the pairs of equation numbers to compare.
  std::vector<std::pair<int, int>> eqPairs;
  if (equations.has_value()) {
    std::string t = *equations;
    for (char& c : t)
      if (c == ',') c = ' ';
    std::istringstream in(t);
    std::string pair;
    while (in >> pair) {
      const size_t colon = pair.find(':');
      try {
        if (colon == std::string::npos) return hausErr("':' found where ':' expected", 198);
        eqPairs.emplace_back(std::stoi(pair.substr(0, colon)), std::stoi(pair.substr(colon + 1)));
      } catch (...) {
        return hausErr("'" + pair + "' found where integer expected", 198);
      }
    }
    alleqs = false;
    skipeqs.reset();
  }

  std::string name1 = name1In, name2 = name2In.empty() ? std::string(".") : name2In;
  if (name1 == name2) return hausErr("the two models need to be different", 198);
  const std::string activeName = session.activeStoredName();
  HausFit fits[2];
  for (int m = 0; m < 2; ++m) {
    const std::string& nm = m == 0 ? name1 : name2;
    if (nm == ".") {
      if (session.eval.eStrings.get("cmd").empty())
        return hausErr("last estimates not found", 301);
      if (auto e = hausLoad(activeName.empty() ? "." : activeName, session.eval, force, fits[m])) return *e;
      continue;
    }
    const auto it = session.estimatesModels.find(nm);
    if (it == session.estimatesModels.end() || !it->second) {
      if (session.estimatesGet(nm) != nullptr)
        return ojson(eval::refusedCommand("hausman " + nm, "stored_results_without_e"));
      return hausErr("estimation result " + nm + " not found", 111);
    }
    const auto& sm = *it->second;
    eval::EvalSession probe;
    probe.lastModelResult = sm.estBlock.has_value() ? sm.estBlock : sm.lastModelResult;
    probe.eScalars = sm.eScalars;
    probe.eStrings = sm.eStrings;
    probe.eMatrices = sm.eMatrices;
    if (auto e = hausLoad(nm, probe, force, fits[m])) return *e;
  }
  if (fits[0].name == fits[1].name) return hausErr("the two models need to be different", 198);

  // Strik0: drop the columns whose variance is 0.
  for (HausFit& f : fits) {
    std::vector<bool> keep(f.b.size());
    size_t kept = 0;
    for (size_t i = 0; i < f.b.size(); ++i) kept += (keep[i] = f.V[i][i] != 0.0) ? 1 : 0;
    if (kept == 0) return hausErr("nothing to test", 498);
    hausKeep(f, keep);
  }
  // Equations.
  for (int m = 0; m < 2; ++m) {
    HausFit& f = fits[m];
    const std::vector<std::string> eqs = hausEquations(f);
    std::vector<bool> keep(f.b.size(), true);
    if (skipeqs.has_value()) {
      std::istringstream in(*skipeqs);
      std::set<std::string> skip;
      std::string w;
      while (in >> w) skip.insert(w);
      bool anyKept = false;
      for (size_t i = 0; i < f.b.size(); ++i) {
        keep[i] = skip.count(f.eq[i]) == 0;
        anyKept = anyKept || keep[i];
      }
      if (!anyKept) std::fill(keep.begin(), keep.end(), true);  // SkipEqs keeps M when it would empty it
    } else if (!eqPairs.empty()) {
      // Select: the listed equation numbers, renamed comp1, comp2, ... in
      // their order in the fit (a fit without equation names is comp1).
      std::set<int> wanted;
      for (const auto& pr : eqPairs) wanted.insert(m == 0 ? pr.first : pr.second);
      for (size_t i = 0; i < f.b.size(); ++i) {
        const size_t pos = static_cast<size_t>(std::find(eqs.begin(), eqs.end(), f.eq[i]) - eqs.begin()) + 1;
        keep[i] = eqs.size() <= 1 || wanted.count(static_cast<int>(pos)) != 0;
      }
    } else if (!alleqs) {
      for (size_t i = 0; i < f.b.size(); ++i) keep[i] = f.eq[i] == f.eq.front();
    }
    hausKeep(f, keep);
    if (!eqPairs.empty()) {
      std::string prev;
      int j = 0;
      for (auto& e : f.eq) {
        if (e != prev) {
          ++j;
          prev = e;
        }
        e = "comp" + std::to_string(j);
      }
    }
  }
  // Rmcons.
  if (!constant) {
    for (HausFit& f : fits) {
      std::vector<bool> keep(f.b.size(), true);
      size_t kept = 0;
      for (size_t i = 0; i < f.b.size(); ++i) kept += (keep[i] = f.col[i] != "_cons") ? 1 : 0;
      if (kept == 0) return hausErr("no common coefficients names (eq:coef), nothing to test", 498);
      hausKeep(f, keep);
    }
  }
  // sigmamore / sigmaless.
  if (sigmamore || sigmaless) {
    if (std::isnan(fits[0].sigma) || std::isnan(fits[1].sigma)) {
      static const std::set<std::string> kPostsSigma = {"xtreg", "regress", "ivregress", "areg", "newey"};
      const HausFit& lacking = std::isnan(fits[0].sigma) ? fits[0] : fits[1];
      if (kPostsSigma.count(lacking.cmd))
        return ojson(eval::engineError("hausman, sigmamore/sigmaless: the e(sigma)/e(rmse) Stata's `" +
                                           lacking.cmd + "` posts is not available for " + lacking.name +
                                           " in this version of PIET; nothing was computed.",
                                       "unsupported_option"));
      return hausErr("Estimators do not save e(sigma) or e(rmse),\nsigma option not allowed", 198);
    }
    HausFit& scaled = sigmamore ? fits[0] : fits[1];
    const double ratio = sigmamore ? fits[1].sigma / fits[0].sigma : fits[0].sigma / fits[1].sigma;
    for (auto& row : scaled.V)
      for (double& x : row) x *= ratio * ratio;
  }
  // MatConf: b's columns found in B by equation:name, in b's order.
  std::vector<size_t> map;
  {
    std::vector<bool> keep(fits[0].b.size(), false);
    for (size_t i = 0; i < fits[0].b.size(); ++i) {
      for (size_t j = 0; j < fits[1].b.size(); ++j)
        if (fits[1].eq[j] == fits[0].eq[i] && fits[1].col[j] == fits[0].col[i]) {
          keep[i] = true;
          map.push_back(j);
          break;
        }
    }
    if (map.empty())
      return hausErr("no coefficients in common; specify equations(matchlist)\nfor problems with different "
                     "equation names.",
                     498);
    hausKeep(fits[0], keep);
  }
  const size_t k = map.size();
  std::vector<double> d(k), D(k * k);
  std::vector<double> bB(k);
  for (size_t i = 0; i < k; ++i) {
    bB[i] = fits[1].b[map[i]];
    d[i] = fits[0].b[i] - bB[i];
    for (size_t j = 0; j < k; ++j)
      D[i + j * k] = 0.5 * ((fits[0].V[i][j] - fits[1].V[map[i]][map[j]]) +
                            (fits[0].V[j][i] - fits[1].V[map[j]][map[i]]));
  }
  std::vector<double> w, z;
  if (piet::lapack::dsyevrVAL(static_cast<int>(k), D, 0.0, w, z) != 0 || w.size() != k)
    return ojson(eval::refusedCommand("hausman", "eigendecomposition_failed"));
  const bool posdef = w.front() > 0;  // ascending: the smallest
  double wmax = 0;
  for (double x : w) wmax = std::max(wmax, std::fabs(x));
  const double tol = wmax * static_cast<double>(k) * 1.0e-9;
  double chi2 = 0;
  int rank = 0;
  for (size_t c = 0; c < k; ++c) {
    if (!(std::fabs(w[c]) > tol)) continue;
    ++rank;
    double proj = 0;
    for (size_t i = 0; i < k; ++i) proj += z[i + c * k] * d[i];
    chi2 += proj * proj / w[c];
  }
  const double df = dfOpt.value_or(static_cast<double>(rank));
  const double p = chi2UpperTailIntegerDf(df, chi2);

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "hausman";
  block["depvar"] = "";
  block["n_obs"] = 0;
  block["chi2"] = chi2;
  block["p"] = p;
  block["dof"] = df;
  block["rank"] = rank;
  ojson coefs = ojson::array();
  for (size_t i = 0; i < k; ++i) {
    const double vd = fits[0].V[i][i] - fits[1].V[map[i]][map[i]];
    coefs.push_back(ojson{{"b_fe", fits[0].b[i]},
                          {"b_re", bB[i]},
                          {"diff", d[i]},
                          {"name", fits[0].col[i]},
                          {"se_diff", vd >= 0 ? ojson(std::sqrt(vd)) : ojson(nullptr)}});
  }
  block["coefs"] = coefs;
  block["not_pd"] = !posdef;
  std::vector<std::string> notes;
  if (rank != static_cast<int>(k))
    notes.push_back("Note: the rank of the differenced variance matrix (" + std::to_string(rank) +
                    ") does not equal the number of coefficients being tested (" + std::to_string(k) +
                    "); be sure this is what you expect, or there may be problems computing the test.  "
                    "Examine the output of your estimators for anything unexpected and possibly consider "
                    "scaling your variables so that the coefficients are on a similar scale.");
  if (chi2 >= 0 && !posdef) notes.push_back("(V_b-V_B is not positive definite)");
  if (chi2 < 0)
    notes.push_back("chi2<0 ==> model fitted on these data fails to meet the asymptotic "
                    "assumptions of the Hausman test; see suest for a generalized test");
  if (!notes.empty()) block["notes"] = notes;

  session.eval.rValues.set("rank", static_cast<double>(rank));
  session.eval.rValues.set("chi2", chi2);
  session.eval.rValues.set("df", df);
  session.eval.rValues.set("p", p);
  eval::declareRPosted(session.eval, "hausman", {"rank", "chi2", "df", "p"});
  return block;
}

// chow (BS-084): `chow depvar indepvars [if], by(groupvar)` — a PIET-ONLY
// convenience wrapper; real Stata has NO `chow` command at all (the
// oracle's own D5f section computes the classic Chow F test by hand: fit
// pooled + both subsamples, chowF = ((rss_pooled - rss_0 - rss_1)/k) /
// ((rss_0+rss_1)/(n-2k)) — see d45-oracle-stata.do's own comment "Chow test
// income~beertax split by high: pooled + subsamples"). cobra's own `chow`
// CLI computes the IDENTICAL classic homoskedastic SSR-based F test as its
// "classic" block (live-verified: engine classic.F=28.07417107825576,
// df1=2, df2=332 — byte-for-byte the oracle's own manually-computed
// chowF=28.074171, df1=2, df2=332) — plus a bonus heteroskedasticity-robust
// Wald variant (raw.robust, no oracle coverage, not selftest-pinned) this
// handler also surfaces. vce()/cluster() are NOT forwarded this batch (no
// oracle coverage for cobra chow's own --vcov/--cluster surface) — left as
// honest rejects via the unknown-option net.
ojson runChow(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.size() < 2) {
    return engineErrorBlockM4(
        "syntax_error: `chow` requires a dependent variable and at least one regressor",
        "syntax_error");
  }
  std::string y = parsed.varlist[0];
  std::vector<std::string> xs(parsed.varlist.begin() + 1, parsed.varlist.end());

  std::optional<std::string> byVal;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by" && !v.isBool) byVal = v.strValue;
  }
  if (!byVal.has_value() || trimM4(*byVal).empty()) {
    return engineErrorBlockM4("syntax_error: `chow` requires by(groupvar)", "syntax_error");
  }

  std::vector<std::string> argv = {"chow", "--y", y, "--x", joinCommaM4(xs), "--by",
                                    trimM4(*byVal)};
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }

  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabChow.known;
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

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "chow";
  block["depvar"] = y;
  block["n_obs"] = raw.value("n_obs", 0);
  static const char* kChowFields[] = {"G", "by", "group_sizes", "classic", "robust"};
  for (const char* k : kChowFields) {
    if (raw.contains(k)) block[k] = raw[k];
  }
  block["note"] =
      "Chow test (real Stata has no built-in `chow` command — this is a PIET convenience wrapper "
      "reproducing the classic manual F-test: pooled vs split-sample SSR).";
  return block;
}

// mkspline (BS-087, part): `mkspline stub = varname [if], cubic [nknots(#) |
// knots(v1,v2,...)]` -> cobra eval --mkspline "stub=var, cubic[, nknots(N)|,
// knots(...)]" --out new.hbin (a data-mutation verb, like egen — adds
// stub1..stubK-1 columns). Oracle D5g: nknots(4) gives 3 columns
// (inc_sp1/2/3, pinned means/sds) — see the fix disclosure below;
// only the CUBIC form is implemented. Real Stata's LINEAR/piecewise-linear
// default (mkspline with NO `cubic` keyword) has NO engine equivalent —
// live-verified the engine's `eval --mkspline` op ALWAYS computes a
// restricted cubic spline basis regardless of whether `cubic` was typed in
// the spec string (omitting it does NOT fall back to a linear basis, it
// silently produces the SAME cubic one). Rather than silently answer
// linear-mkspline with a cubic result, PIET requires `cubic` to be typed
// and has no linear mkspline of its own.
//
// ENGINE FIX (cobra 0.25.0): `eval --mkspline`'s own `nknots(#)` clause
// now works. LOWERCASE `nknots(N)` — the engine's own documented spelling,
// per `cobra eval`'s usage banner — places knots at Harrell's percentile
// method (e.g. nknots(4) -> the 5/35/65/95 percentiles of the source
// variable) and returns the correct N-1 basis columns, verified live
// against StataBE (byte-for-byte knot positions and basis-column summary
// stats). The old failure mode was a paren_arg substring collision at the
// CLI arg-parsing layer, present through 0.24.0: LOWERCASE `nknots(N)`
// hard-errored "mkspline: need at least 3 knots" for EVERY N tried from 1
// to 10 (the value never actually parsed), while UPPERCASE `NKNOTS(N)` was
// silently ACCEPTED but the value N was DISCARDED — it always fell back to
// the same hardcoded 5-knot/4-column default regardless of N. PIET forwards
// `nknots(N)` in LOWERCASE, per the engine's own documented spelling — the
// D5g oracle's own exact invocation (`nknots(4)`) now succeeds cleanly and
// is oracle-pinned in D5g's own selftest leg (selftest.cpp). The explicit
// `knots(v1,v2,...)` form (caller-supplied exact knot positions, bypassing
// nknots() entirely) also works mechanically (live-verified) and is
// independently wired/checked in its own selftest leg (a fresh stub name,
// since it now runs alongside the fixed nknots() leg rather than as its
// sole proof of mechanical wiring).

}  // namespace piet::handlers
