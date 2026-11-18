// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Default e(cmdline), e(title), e(predict), e(estat_cmd), e(marginsok) of a fit.

#include "piet/eval/tokenizer.h"
#include "vendor/nlohmann/json.hpp"

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
#include "estimation/estimation_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "dispatch/registry.h"  // resolveCanonical: the command word as Stata names it
#include "rtable_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace rtable_detail {

// e(cmdline) of a fit whose handler posted none: every estimator posts the ado's
// `"<cmd> `0'"' -- the command word as Stata names it, then the rest as typed
// (StataBE 19.5, 26 estimators in scratch-laneA/lead/cmdline/cl.do: `reg income
// beertax` posts "regress income beertax", `xtlogit hi beertax, fe` "xtlogit hi
// beertax, fe"). A handler's own posting (regress's ordered e() lists, leebounds,
// bootstrap) is kept.
void postDefaultCmdline(SessionState& session) {
  if (session.eval.eStrings.find("cmdline") != nullptr) return;
  // the leaf's own line after macro expansion (empty: none recorded, post none)
  const std::string& rawCmd = session.leafCmdExpanded;
  const std::string word = commandWord(rawCmd);
  if (word.empty()) return;
  size_t k = rawCmd.find_first_not_of(" \t");
  k = rawCmd.find_first_of(" \t,", k);
  std::string rest;
  if (k != std::string::npos) {
    k = rawCmd.find_first_not_of(" \t", k);
    if (k != std::string::npos) rest = rawCmd.substr(k);  // trailing blanks stay (BUG-1048)
  }
  const std::string canon = registry::resolveCanonical(word);
  session.eval.setEString("cmdline", rest.empty() ? canon : canon + " " + rest);
  session.noteEOnFit("cmdline");  // the next install drops it
}

// e(title), e(predict), e(estat_cmd) and e(marginsok) of a fit whose handler posted
// none. Each ado posts the same words for every variant PIET fits (StataBE 19.5,
// BUG-1492/BUG-1493, scratch-laneA2/agents/epost/s1.do: default, robust and cluster VCEs,
// weights, noconstant, or, baseoutcome(), quantile(), ul()/ll(), families and links); ""
// = Stata posts none (xtreg and qreg have no e(title), areg no e(estat_cmd)). Keyed by
// e(cmd) and, where one word names several of Stata's estimators, by the fit: xtreg's fe
// (the engine's feols) and re ("xtreg_re"), clogit and xtpoisson only as the fe fits
// (`xtlogit, fe` posts e(cmd) clogit), ivregress by method, ivreghdfe as 2SLS. A prefixed
// fit (e(prefix) svy) posts its own and is left alone.
void postStataEContract(SessionState& session);

void postDefaultEMacros(SessionState& session) {
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || session.ePostedLive || !session.eval.eStrings.get("prefix").empty())
    return;
  const std::string cmd = session.eval.eStrings.get("cmd");
  const std::string& engine = lmo->cobraCmd;
  const std::string est = lmo->result.has_value() && lmo->result->is_object()
                              ? lmo->result->value("estimator", std::string())
                              : std::string();
  struct Macros {
    const char *title, *predict, *estatCmd, *marginsok;
  };
  std::optional<Macros> m;
  if (cmd == "regress") {
    m = Macros{"Linear regression", "regres_p", "regress_estat", "XB default"};
  } else if (cmd == "areg") {
    m = Macros{"Linear regression, absorbing indicators", "areg_p", "", ""};
  } else if (cmd == "reghdfe") {
    m = Macros{"HDFE Linear regression", "reghdfe_p", "reghdfe_estat", ""};
  } else if (cmd == "logit") {
    m = Macros{"Logistic regression", "logit_p", "logit_estat", "default Pr"};
  } else if (cmd == "logistic") {
    m = Macros{"Logistic regression", "logistic_p", "logit_estat", "default Pr"};
  } else if (cmd == "probit") {
    m = Macros{"Probit regression", "probit_p", "probit_estat", "default Pr"};
  } else if (cmd == "cloglog") {
    // bank 38560e4b `cloglog foreign mpg weight` (the title's dash is U+2013)
    m = Macros{"Complementary log\u2013log regression", "clog_p", "", "default Pr"};
  } else if (cmd == "clogit" && est == "xtlogit_fe") {
    m = Macros{"Conditional fixed-effects logistic regression", "xtlogit_fe_p", "", "PU0 XB"};
  } else if (cmd == "xtpoisson" && est == "xtpoisson_fe") {
    m = Macros{"Conditional fixed-effects Poisson regression", "xtpoisson_fe_p", "", ""};
  } else if (cmd == "xtreg" && engine == "feols") {
    m = Macros{"", "xtrefe_p", "xtreg_estat", ""};
  } else if (cmd == "xtreg" && est == "xtreg_re") {
    m = Macros{"", "xtrere_p", "xtreg_estat", ""};
  } else if (cmd == "poisson") {
    m = Macros{"Poisson regression", "poisso_p", "poisson_estat", ""};
  } else if (cmd == "ppmlhdfe") {
    m = Macros{"HDFE PPML regression", "ppmlhdfe_p", "reghdfe5_estat", "default"};
  } else if (cmd == "ivregress") {
    const char* title = nullptr;
    if (engine == "liml")
      title = "Instrumental-variables LIML regression";
    else if (est == "ivgmm" || est == "ivgmm_multi")
      title = "Instrumental-variables GMM regression";
    else if (est == "iv2sls" || est == "iv2sls_multi")
      title = "Instrumental-variables 2SLS regression";
    if (title != nullptr) m = Macros{title, "ivregress_p", "ivregress_estat", "XB default"};
  } else if (cmd == "ivreghdfe" && engine == "iv" && (est == "iv2sls" || est == "iv2sls_multi")) {
    m = Macros{"IV (2SLS) estimation", "reghdfe_p", "", "XB default"};
  } else if (cmd == "tobit") {
    m = Macros{"Tobit regression", "tobit_p", "",
               "default XB Pr(passthru) E(passthru) YStar(passthru)"};
  } else if (cmd == "nbreg") {
    m = Macros{"Negative binomial regression", "nbreg_p", "", ""};
  } else if (cmd == "qreg") {
    m = Macros{"", "qreg_p", "", ""};
  } else if (cmd == "glm") {
    m = Macros{"Generalized linear models", "glim_p", "", "default"};
  } else if (cmd == "ologit") {
    m = Macros{"Ordered logistic regression", "ologit_p", "ologit_estat", ""};
  } else if (cmd == "oprobit") {
    // oprobit.ado posts no e(estat_cmd) (bank 310b9f30 `oprobit rep78 mpg weight`)
    m = Macros{"Ordered probit regression", "oprobi_p", "", ""};
  } else if (cmd == "mlogit") {
    m = Macros{"Multinomial logistic regression", "mlogit_p", "", ""};
  } else if (cmd == "newey") {
    // bank 9fc11f59 `newey price mpg weight, lag(0)` (and every newey fit): the
    // title's dash is U+2013
    m = Macros{"Regression with Newey\xE2\x80\x93West standard errors", "newey_p", "newey_estat", ""};
  }
  if (!m.has_value()) {
    postStataEContract(session);
    return;
  }
  const auto fill = [&session](const char* name, const char* v) {
    if (*v == '\0' || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);  // the next install drops it
  };
  fill("title", m->title);
  fill("predict", m->predict);
  fill("estat_cmd", m->estatCmd);
  fill("marginsok", m->marginsok);
  postStataEContract(session);
}

// ---------------------------------------------------------------------------
// The rest of an estimator's e() contract: what its ado publishes besides the
// numbers (vs-stata truth bank, StataBE 19.5; each value below is the one every
// bank cell of that estimator recorded under the stated condition):
//  * e(properties) "b V" -- every estimator in kBV (the bank: 100% of its cells);
//  * the margins contract macros e(marginsderiv) (hidden), e(marginsnotok),
//    e(marginsprop) (hidden) where the ado posts a fixed word list;
//  * for the maximum-likelihood fits logit, logistic, probit and poisson, the
//    `ml` settings e(opt) moptimize, e(which) max, e(technique) nr,
//    e(deriv_useminbound) off, e(singularHmethod) (hybrid under `difficult`),
//    e(ml_method) and e(user) (logit's ado fits with the e2 evaluator when any
//    vce(), technique() or score() is given -- a pweight implies vce(robust) --
//    and with d2 otherwise; poisson always with poiss_lf, an e2 method),
//    e(crittype) ("log pseudolikelihood" under robust/cluster/pweight), and the
//    scalars e(k_eq) e(k_dv) e(k_eq_model) 1, e(noconstant), e(consonly),
//    e(k_autoCns) (the e(b) columns that are a base or omitted factor level),
//    e(rc) 0 for a converged fit or one stopped by iterate(), e(p) the model
//    test's p-value and e(r2_p) 1 - ll/ll_0 when e(ll_0) exists.
// A name already posted (by the handler) is left alone, and a fit run with an
// option these rules do not cover (technique(), constraints(), exposure(),
// offset(), a resampling VCE) posts none of the option-dependent names.
namespace {

// Is `key` an abbreviation of option `name` (at least `min` characters)?
bool optIs(const std::string& key, const char* name, size_t min) {
  const std::string n(name);
  return key.size() >= min && key.size() <= n.size() && n.compare(0, key.size(), key) == 0;
}

struct FitOptions {
  bool parsed = false;
  bool vceGiven = false;   // vce(), robust, cluster()
  bool robustLike = false; // vce(robust|cluster), robust, cluster(), pweight
  bool resampling = false; // vce(bootstrap|jackknife)
  bool noconstant = false, technique = false, difficult = false, iterate = false;
  bool constraints = false, offset = false, exposure = false, score = false;
  std::string offsetMacro;  // e(offset): offset(v) "v", exposure(v) "ln(v)"
  size_t nIndep = 0;
};

FitOptions readFitOptions(const std::string& line) {
  FitOptions f;
  eval::ParsedCommand p;
  try {
    p = eval::tokenize(line);
  } catch (...) {
    return f;
  }
  f.parsed = true;
  if (p.varlist.size() > 1) f.nIndep = p.varlist.size() - 1;
  if (p.weightType.has_value()) {
    std::string w = *p.weightType;
    for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w.rfind("pw", 0) == 0) f.robustLike = true, f.vceGiven = true;
  }
  for (const auto& [k0, v] : p.opts) {
    std::string k = k0;
    for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (optIs(k, "vce", 3)) {
      f.vceGiven = true;
      std::string t = v.isBool ? std::string() : v.strValue;
      for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const size_t b = t.find_first_not_of(" \t");
      t = b == std::string::npos ? std::string() : t.substr(b);
      if (t.rfind("r", 0) == 0 || t.rfind("cl", 0) == 0) f.robustLike = true;
      if (t.rfind("boot", 0) == 0 || t.rfind("jack", 0) == 0) f.resampling = true;
    } else if (optIs(k, "robust", 1)) {
      f.vceGiven = f.robustLike = true;
    } else if (optIs(k, "cluster", 2)) {
      f.vceGiven = f.robustLike = true;
    } else if (optIs(k, "noconstant", 3)) {  // NOConstant
      f.noconstant = true;
    } else if (optIs(k, "technique", 4)) {
      f.technique = true;
    } else if (optIs(k, "difficult", 3)) {
      f.difficult = true;
    } else if (optIs(k, "iterate", 4)) {
      f.iterate = true;
    } else if (optIs(k, "constraints", 1)) {
      f.constraints = true;
    } else if (optIs(k, "offset", 3)) {
      f.offset = true;
      if (!v.isBool) f.offsetMacro = v.strValue;
    } else if (optIs(k, "exposure", 1)) {
      f.exposure = true;
      if (!v.isBool) f.offsetMacro = "ln(" + v.strValue + ")";
    } else if (optIs(k, "score", 2)) {
      f.score = true;
    }
  }
  return f;
}

// Upper tail of chi2(df) at x: the regularized upper incomplete gamma
// Q(df/2, x/2) (series below a+1, Lentz continued fraction above) -- poisson's
// e(p) from its model test's e(chi2) and e(df_m).
double chi2Upper(double df, double x) {
  if (std::isnan(x) || !(df > 0)) return std::numeric_limits<double>::quiet_NaN();
  if (x <= 0) return 1.0;
  const double a = df / 2.0, z = x / 2.0, gln = std::lgamma(a);
  if (z < a + 1.0) {
    double ap = a, sum = 1.0 / a, term = sum;
    for (int n = 1; n < 500; ++n) {
      ap += 1.0;
      term *= z / ap;
      sum += term;
      if (std::fabs(term) < std::fabs(sum) * 1e-16) break;
    }
    return 1.0 - sum * std::exp(-z + a * std::log(z) - gln);
  }
  double b = z + 1.0 - a, c = 1e300, d = 1.0 / b, h = d;
  for (int i = 1; i < 500; ++i) {
    const double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
    b += 2.0;
    d = an * d + b;
    if (std::fabs(d) < 1e-300) d = 1e-300;
    c = b + an / c;
    if (std::fabs(c) < 1e-300) c = 1e-300;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-16) break;
  }
  return std::exp(-z + a * std::log(z) - gln) * h;
}

// e(b)'s column names, when the e(b) name reader answers.
std::optional<std::vector<std::string>> ebColumnNames(SessionState& session) {
  const auto a = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kNames);
  if (!a.matrix.has_value()) return std::nullopt;
  return a.matrix->colnames;
}

// A base (`1b.`), omitted (`o.`, `2o.`) or empty factor level in any part of a
// column name: one automatically created constraint each (ml's e(k_autoCns)).
bool isAutoCnsColumn(const std::string& col) {
  std::string name = col.substr(col.find(':') == std::string::npos ? 0 : col.find(':') + 1);
  size_t i = 0;
  while (i <= name.size()) {
    const size_t j = std::min(name.find('#', i), name.size());
    const std::string part = name.substr(i, j - i);
    const size_t dot = part.find('.');
    if (dot != std::string::npos) {
      const std::string op = part.substr(0, dot);
      if (!op.empty() && (op.back() == 'b' || op.find('o') != std::string::npos) &&
          op.find_first_not_of("0123456789bno") == std::string::npos)
        return true;
    }
    i = j + 1;
  }
  return false;
}

void putEMatrix(SessionState& session, const std::string& name, eval::EvalMatrix m) {
  auto& store = session.eval.eMatrices;
  store.erase(std::remove_if(store.begin(), store.end(),
                             [&name](const auto& p) { return p.first == name; }),
              store.end());
  store.emplace_back(name, std::move(m));
  session.noteEOnFit(name);
}

// See postStataEContract. Nothing is posted when a column's regressor cannot be
// read as a plain variable, a factor level of one, or an interaction of those.
void postLogitMnsCns(SessionState& session, const LastModel& lm,
                     const std::vector<std::string>& cols, bool withMns = true) {
  Datastore* ds = session.datastore();
  if (ds == nullptr || cols.empty()) return;
  try {
    const int64_t n = ds->nRows();
    if (!ds->has(kEsampleColumn)) return;
    const double* es = ds->col(kEsampleColumn).f64();
    if (es == nullptr) return;
    const double serial = static_cast<double>(lm.esampleSerial);
    const double* w = nullptr;
    std::string wexp = session.eval.eStrings.get("wexp");
    if (!wexp.empty()) {
      const size_t eq = wexp.find('=');
      std::string wv = eq == std::string::npos ? std::string() : wexp.substr(eq + 1);
      wv.erase(0, wv.find_first_not_of(" \t"));
      wv.erase(wv.find_last_not_of(" \t") + 1);
      if (wv.empty() || !ds->has(wv) || (w = ds->col(wv).f64()) == nullptr) return;
    }
    const auto column = [&ds](const std::string& v) -> const double* {
      if (v.empty() || isHiddenColumn(v) || !ds->has(v)) return nullptr;
      return ds->col(v).f64();
    };
    eval::EvalMatrix mns, cns;
    mns.rownames = {"r1"};
    mns.data.assign(1, {});
    std::vector<size_t> autoCols;
    for (size_t j = 0; j < cols.size(); ++j) {
      const std::string& full = cols[j];
      const std::string name = full.substr(full.find(':') == std::string::npos ? 0 : full.find(':') + 1);
      mns.colnames.push_back(name);
      if (name == "_cons") {
        mns.data[0].push_back(1.0);
        continue;
      }
      if (isAutoCnsColumn(full)) autoCols.push_back(j);
      // a base level's mean is 0; an omitted (`o.`) column keeps its regressor's
      // mean (bank b5c084a3: `5o.rep78` .0466, `2o.rep78` 0 -- no rep78==2 row
      // is left in the sample)
      bool base = false;
      // the regressor as a product of parts: `x`, `c.x` (the value) or `k.x` (x == k)
      std::vector<std::pair<const double*, double>> parts;  // (column, level or NaN)
      size_t i = 0;
      while (i <= name.size()) {
        const size_t e = std::min(name.find('#', i), name.size());
        const std::string part = name.substr(i, e - i);
        i = e + 1;
        const size_t dot = part.find('.');
        std::string var = dot == std::string::npos ? part : part.substr(dot + 1);
        const std::string op = dot == std::string::npos ? std::string() : part.substr(0, dot);
        double level = std::numeric_limits<double>::quiet_NaN();
        std::string num = op;
        if (!num.empty() && num.find('b') != std::string::npos) base = true;
        num.erase(std::remove_if(num.begin(), num.end(), [](char ch) { return ch == 'b' || ch == 'o'; }),
                  num.end());
        if (!num.empty() && num != "c") {
          if (num.find_first_not_of("0123456789") != std::string::npos) return;
          level = std::stod(num);
        }
        const double* c = column(var);
        if (c == nullptr) return;
        parts.emplace_back(c, level);
      }
      if (base) {
        mns.data[0].push_back(0.0);
        continue;
      }
      double sw = 0.0, sx = 0.0;
      for (int64_t r = 0; r < n; ++r) {
        if (es[r] != serial) continue;
        double x = 1.0;
        for (const auto& [c, level] : parts) x *= std::isnan(level) ? c[r] : (c[r] == level ? 1.0 : 0.0);
        const double wr = w == nullptr ? 1.0 : w[r];
        sw += wr;
        sx += wr * x;
      }
      if (!(sw > 0) || !std::isfinite(sx)) return;
      mns.data[0].push_back(sx / sw);
    }
    if (withMns) putEMatrix(session, "mns", std::move(mns));
    if (!autoCols.empty()) {
      cns.colnames = cols;
      cns.colnames.push_back("_Cns:_r");
      for (size_t k = 0; k < autoCols.size(); ++k) {
        cns.rownames.push_back("r" + std::to_string(k + 1));
        std::vector<double> row(cols.size() + 1, 0.0);
        row[autoCols[k]] = 1.0;
        cns.data.push_back(std::move(row));
      }
      putEMatrix(session, "Cns", std::move(cns));
    }
  } catch (...) {
    return;
  }
}

// e(gradient) of an ml fit whose reply carries `e_gradient` ({names, values}:
// the gradient of the log likelihood at the posted iterate, one value per engine
// coefficient, apps/cmds/cmds_mle.cpp add_e_gradient): laid on e(b)'s own stripe
// by reading a copy of the live block with each coefficient replaced by its
// gradient element through the e(b) reader, so the base and omitted columns
// e(b) carries are 0 and the names are e(b)'s; row r1 (StataBE 19.5 `logistic
// foreign mpg weight`: e(gradient) 1 x 3 `foreign:mpg foreign:weight
// foreign:_cons`, row r1; bank 506b9692 `1b.rep78` and `2o.rep78` 0).
std::optional<eval::EvalMatrix> gradientOnEbStripe(SessionState& session, const ojson& raw) {
  if (!raw.contains("e_gradient") || !raw["e_gradient"].is_object()) return std::nullopt;
  const ojson& eg = raw["e_gradient"];
  if (!eg.contains("names") || !eg.contains("values") || !eg["names"].is_array() ||
      !eg["values"].is_array() || eg["names"].size() != eg["values"].size())
    return std::nullopt;
  std::map<std::string, double> byName;
  for (size_t i = 0; i < eg["names"].size(); ++i) {
    if (!eg["names"][i].is_string() || !eg["values"][i].is_number()) return std::nullopt;
    byName[eg["names"][i].get<std::string>()] = eg["values"][i].get<double>();
  }
  const ojson* live = session.eval.coefSource();
  if (live == nullptr || !live->is_object() || !live->contains("coeftable") ||
      !(*live)["coeftable"].is_array())
    return std::nullopt;
  ojson copy = *live;
  for (auto& r : copy["coeftable"]) {
    if (!r.is_object() || !r.contains("name") || !r["name"].is_string()) return std::nullopt;
    const std::string nm = r["name"].get<std::string>();
    auto it = byName.find(nm);
    if (it == byName.end() && (nm == "_cons" || nm == "(intercept)"))
      it = byName.find(nm == "_cons" ? "(intercept)" : "_cons");
    if (it == byName.end()) return std::nullopt;
    r["b"] = it->second;
  }
  eval::EvalSession::EMatrixAnswer a =
      session.eval.resolveFitEMatrixFrom("b", eval::EvalSession::EReader::kCopy, &copy);
  if (!a.matrix.has_value() || a.matrix->data.size() != 1) return std::nullopt;
  // the same columns as the live e(b), or nothing is posted
  const auto cols = ebColumnNames(session);
  if (!cols.has_value() || *cols != a.matrix->colnames) return std::nullopt;
  a.matrix->rownames = {"r1"};
  return a.matrix;
}

// e(V_modelbased) of a sandwich ml fit whose reply carries `e_Vmb` ({names,
// matrix}: the inverse negative Hessian at the posted iterate, the default
// VCE's matrix): read through the e(V) reader from a copy of the live block
// with that matrix as its covariance, so it carries e(V)'s stripe (StataBE
// 19.5 `logit foreign mpg weight, vce(robust)`: e(V_modelbased) 3 x 3 on
// `foreign:mpg foreign:weight foreign:_cons`, the plain fit's e(V)).
std::optional<eval::EvalMatrix> modelBasedVOnEbStripe(SessionState& session, const ojson& raw) {
  if (!raw.contains("e_Vmb") || !raw["e_Vmb"].is_object()) return std::nullopt;
  const ojson* live = session.eval.coefSource();
  if (live == nullptr || !live->is_object()) return std::nullopt;
  ojson copy = *live;
  copy["e_V"] = raw["e_Vmb"];
  eval::EvalSession::EMatrixAnswer a =
      session.eval.resolveFitEMatrixFrom("V", eval::EvalSession::EReader::kCopy, &copy);
  if (!a.matrix.has_value() || a.diagonalOnly) return std::nullopt;
  const auto cols = ebColumnNames(session);
  if (!cols.has_value() || *cols != a.matrix->colnames || *cols != a.matrix->rownames)
    return std::nullopt;
  return a.matrix;
}

// ologit / oprobit (ologit.ado, oprobit.ado and ml's posting; vs-stata bank
// d8de89e3 `ologit rep78 mpg weight`, 310b9f30 `oprobit rep78 mpg weight`,
// 8127bef1 / 3f3d419e `, vce(robust)`, 9375308e `, iterate(2)`): the ml settings
// (e(user) mopt__<cmd>_d2(), the e2 evaluator when a vce() or score() is given),
// e(depvar), e(depvar_outcomes) and e(marginsdefault) from the outcome values,
// the counts e(k) e(k_eq) (the depvar equation plus one per cutpoint) e(k_aux)
// e(k_cat) e(k_dv) e(k_eq_model) e(noconstant) 1 e(consonly) 0 e(version) 3,
// e(rank), e(k_autoCns), e(ic) e(rc) e(df_m) e(p) e(r2_p), and the matrices
// e(cat) (the outcome values, r1 x c1..), e(b_pclass) (0 for a coefficient, 2 for
// a cutpoint), e(ilog), e(gradient) and, under a sandwich, e(V_modelbased) -- all
// from the engine's reply for this fit. e(N_cd) is posted 0 only when no
// observation's own outcome has a fitted probability within 1e-6 of 1.
void postOrderedEContract(SessionState& session, const LastModel& lm, const ojson& raw,
                          const std::string& cmd, bool e2, bool robustLike, bool iterate,
                          bool difficult) {
  const auto ebNames = ebColumnNames(session);
  if (!ebNames.has_value() || ebNames->empty()) return;  // e(b) was not posted: nothing to name
  const auto fillS = [&session](const char* name, const std::string& v) {
    if (v.empty() || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillN = [&session](const char* name, double v) {
    if (session.eval.eScalars.find(name) != nullptr) return;
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const auto num = [&raw](const char* key) -> std::optional<double> {
    if (!raw.contains(key) || !raw[key].is_number()) return std::nullopt;
    return raw[key].get<double>();
  };
  const std::vector<std::string>& cols = *ebNames;
  const size_t k = cols.size();
  size_t nCut = 0, nAuto = 0;
  for (const auto& c : cols) {
    if (c.rfind("/cut", 0) == 0) ++nCut;
    if (isAutoCnsColumn(c)) ++nAuto;
  }
  if (!raw.contains("cat_values") || !raw["cat_values"].is_array()) return;
  const ojson& cv = raw["cat_values"];
  if (nCut == 0 || cv.size() != nCut + 1) return;

  fillS("opt", "moptimize");
  fillS("which", "max");
  fillS("technique", "nr");
  fillS("deriv_useminbound", "off");
  fillS("singularHmethod", difficult ? "hybrid" : "m-marquardt");
  fillS("ml_method", e2 ? "e2" : "d2");
  fillS("user", "mopt__" + cmd + (e2 ? "_e2()" : "_d2()"));
  fillS("crittype", robustLike ? "log pseudolikelihood" : "log likelihood");
  if (raw.contains("chi2type") && raw["chi2type"].is_string())
    fillS("chi2type", raw["chi2type"].get<std::string>());
  fillS("depvar", lm.y);
  if (lm.vcov == "iid" && session.eval.eStrings.get("vce") == "iid")
    session.eval.setEString("vce", "oim");
  // The outcome values: an integer value is its own outcome(), any other is
  // outcome(#i) (the ado's rule); depvar_outcomes lists the values as displayed.
  std::string outcomes, mdflt;
  eval::EvalMatrix cat;
  cat.rownames = {"r1"};
  cat.data.assign(1, {});
  for (size_t i = 0; i < cv.size(); ++i) {
    if (!cv[i].is_number()) return;
    const double v = cv[i].get<double>();
    if (!(v == std::floor(v)) || std::fabs(v) >= 1e15) return;  // display spelling not measured
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    outcomes += (outcomes.empty() ? "" : " ") + std::string(buf);
    mdflt += (mdflt.empty() ? "" : " ") + std::string("predict(pr outcome(") + buf + "))";
    cat.colnames.push_back("c" + std::to_string(i + 1));
    cat.data[0].push_back(v);
  }
  fillS("depvar_outcomes", outcomes);
  fillS("marginsdefault", mdflt);
  if (session.eval.findClassMatrix('e', "cat") == nullptr) putEMatrix(session, "cat", std::move(cat));

  fillN("k", static_cast<double>(k));
  fillN("k_eq", static_cast<double>(1 + nCut));
  fillN("k_aux", static_cast<double>(nCut));
  fillN("k_cat", static_cast<double>(nCut + 1));
  fillN("k_dv", 1.0);
  fillN("k_eq_model", 1.0);
  fillN("noconstant", 1.0);
  fillN("consonly", 0.0);
  fillN("version", 3.0);
  fillN("k_autoCns", static_cast<double>(nAuto));
  fillN("rank", static_cast<double>(k - nAuto));
  if (const auto it = num("iters")) fillN("ic", *it);
  bool conv = raw.contains("converged") && raw["converged"].is_boolean() &&
              raw["converged"].get<bool>();
  if (conv || iterate) fillN("rc", 0.0);
  fillN("converged", conv ? 1.0 : 0.0);
  if (const auto df = num("df_chi2")) fillN("df_m", *df);
  if (const auto p = num("p_chi2")) fillN("p", *p);
  if (num("ll_0").has_value())
    if (const auto r2 = num("r2_pseudo")) fillN("r2_p", *r2);
  if (const auto pm = num("max_p_own"))
    if (*pm < 1.0 - 1e-6) fillN("N_cd", 0.0);

  // e(b_pclass): the coefficient class of each e(b) column (_b_pclass: default 0,
  // aux 2 -- the cutpoints).
  if (session.eval.findClassMatrix('e', "b_pclass") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"y1"};
    m.colnames = cols;
    m.data.assign(1, {});
    for (const auto& c : cols) m.data[0].push_back(c.rfind("/cut", 0) == 0 ? 2.0 : 0.0);
    putEMatrix(session, "b_pclass", std::move(m));
  }
  // e(ilog): the log likelihood at iterations 0..e(ic), zeros after (1 x 20).
  if (raw.contains("ilog") && raw["ilog"].is_array() && !raw["ilog"].empty() &&
      raw["ilog"].size() <= 20 && session.eval.findClassMatrix('e', "ilog") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    bool ok = true;
    size_t i = 0;
    for (const auto& v : raw["ilog"]) {
      if (!v.is_number()) ok = false;
      else m.data[0][i] = v.get<double>();
      ++i;
    }
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    if (ok) putEMatrix(session, "ilog", std::move(m));
  }
  // e(gradient): the engine's gradient (slopes, then cutpoints) laid on e(b)'s
  // columns; a base or omitted column has none (0).
  if (raw.contains("gradient") && raw["gradient"].is_array() &&
      raw["gradient"].size() == k - nAuto && session.eval.findClassMatrix('e', "gradient") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.colnames = cols;
    m.data.assign(1, {});
    size_t g = 0;
    bool ok = true;
    for (const auto& c : cols) {
      if (isAutoCnsColumn(c)) {
        m.data[0].push_back(0.0);
        continue;
      }
      const ojson& v = raw["gradient"][g++];
      if (!v.is_number()) ok = false;
      m.data[0].push_back(v.is_number() ? v.get<double>() : 0.0);
    }
    if (ok) putEMatrix(session, "gradient", std::move(m));
  }
  // e(V_modelbased): the observed-information variance under a sandwich, on e(V)'s
  // stripe (the reply names its rows as e_V does).
  if (robustLike && raw.contains("e_Vmb") && raw["e_Vmb"].is_object() &&
      session.eval.findClassMatrix('e', "V_modelbased") == nullptr) {
    const ojson& vn = raw["e_Vmb"].value("names", ojson::array());
    const ojson& vm = raw["e_Vmb"].value("matrix", ojson::array());
    // each e(b) column's row of the reply by its bare name; a base or omitted
    // column has none (a structural 0)
    std::vector<long> at(k, -1);
    bool ok = vn.size() == k - nAuto && vm.size() == vn.size();
    for (size_t a = 0; a < k && ok; ++a) {
      if (isAutoCnsColumn(cols[a])) continue;
      const size_t colon = cols[a].find(':');
      const std::string bare = colon == std::string::npos ? cols[a] : cols[a].substr(colon + 1);
      for (size_t j = 0; j < vn.size(); ++j)
        if (vn[j].is_string() && vn[j].get<std::string>() == bare) at[a] = static_cast<long>(j);
      if (at[a] < 0) ok = false;
    }
    eval::EvalMatrix m;
    m.rownames = cols;
    m.colnames = cols;
    m.data.assign(k, std::vector<double>(k, 0.0));
    for (size_t a = 0; a < k && ok; ++a) {
      for (size_t c = 0; c < k && ok; ++c) {
        if (at[a] < 0 || at[c] < 0) continue;
        const ojson& ra = vm[static_cast<size_t>(at[a])];
        const ojson& rc = vm[static_cast<size_t>(at[c])];
        if (!ra.is_array() || !rc.is_array() || ra.size() != vn.size() || rc.size() != vn.size() ||
            !ra[static_cast<size_t>(at[c])].is_number() || !rc[static_cast<size_t>(at[a])].is_number()) {
          ok = false;
          break;
        }
        m.data[a][c] = 0.5 * (ra[static_cast<size_t>(at[c])].get<double>() +
                              rc[static_cast<size_t>(at[a])].get<double>());
      }
    }
    if (ok) putEMatrix(session, "V_modelbased", std::move(m));
  }
  // e(Cns): one row per base or omitted e(b) column (1 there), and a last column
  // _Cns:_r of 0 (bank d37aa219: `rep78:0b.foreign`, 1 x 8).
  if (nAuto > 0 && session.eval.findClassMatrix('e', "Cns") == nullptr) {
    eval::EvalMatrix cns;
    cns.colnames = cols;
    cns.colnames.push_back("_Cns:_r");
    for (size_t j = 0; j < k; ++j) {
      if (!isAutoCnsColumn(cols[j])) continue;
      cns.rownames.push_back("r" + std::to_string(cns.rownames.size() + 1));
      std::vector<double> row(k + 1, 0.0);
      row[j] = 1.0;
      cns.data.push_back(std::move(row));
    }
    putEMatrix(session, "Cns", std::move(cns));
  }
}

// mlogit (mlogit.ado and ml's posting; vs-stata bank e9b3681e `mlogit race age
// grade wage`, 28c04502 `, baseoutcome(2)`, 4b460c6a `, vce(robust)`, 0c684b5b
// `, baseoutcome(1) rrr vce(cluster industry)`, d678752f `, iterate(2)`): the ml
// settings, e(depvar_outcomes) / e(marginsdefault) and the matrix e(out) from the
// outcome values, e(baseout) (the base outcome's value) and its position
// e(ibaseout) = e(k_eq_base) (macro e(k_eq_model_skip)), e(k_eq) = e(k_eq_model) =
// e(k_out) = e(k_eform) the number of outcomes, e(k) = e(rank) the estimated
// coefficients (the base equation's `o.` columns are not counted, e(k_autoCns) 0),
// e(ilog), e(gradient) (zeros on the base equation) and, under a sandwich,
// e(V_modelbased).
void postMlogitEContract(SessionState& session, const ojson& raw, const LastModel& lm, bool e2,
                         bool robustLike, bool iterate, bool difficult, bool noconstant) {
  const auto ebNames = ebColumnNames(session);
  if (!ebNames.has_value() || ebNames->empty()) return;
  const auto fillS = [&session](const char* name, const std::string& v) {
    if (v.empty() || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillN = [&session](const char* name, double v) {
    if (session.eval.eScalars.find(name) != nullptr) return;
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const auto num = [&raw](const char* key) -> std::optional<double> {
    if (!raw.contains(key) || !raw[key].is_number()) return std::nullopt;
    return raw[key].get<double>();
  };
  if (!raw.contains("cat_values") || !raw["cat_values"].is_array()) return;
  const ojson& cv = raw["cat_values"];
  const auto baseVal = num("base_outcome");
  const size_t K = cv.size();
  if (K < 2 || !baseVal.has_value()) return;
  const std::vector<std::string>& cols = *ebNames;
  const auto isO = [](const std::string& c) {
    const size_t colon = c.find(':');
    return c.compare(colon == std::string::npos ? 0 : colon + 1, 2, "o.") == 0;
  };
  size_t nFree = 0;
  for (const auto& c : cols) nFree += isO(c) ? 0 : 1;

  std::string outcomes, mdflt;
  eval::EvalMatrix out;
  out.rownames = {"r1"};
  out.data.assign(1, {});
  size_t ibase = 0;
  for (size_t i = 0; i < K; ++i) {
    if (!cv[i].is_number()) return;
    const double v = cv[i].get<double>();
    if (!(v == std::floor(v)) || std::fabs(v) >= 1e15) return;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    outcomes += (outcomes.empty() ? "" : " ") + std::string(buf);
    mdflt += (mdflt.empty() ? "" : " ") + std::string("predict(pr outcome(") + buf + "))";
    out.colnames.push_back("c" + std::to_string(i + 1));
    out.data[0].push_back(v);
    if (v == *baseVal) ibase = i + 1;
  }
  if (ibase == 0) return;

  fillS("opt", "moptimize");
  fillS("which", "max");
  fillS("technique", "nr");
  fillS("deriv_useminbound", "off");
  fillS("singularHmethod", difficult ? "hybrid" : "m-marquardt");
  fillS("ml_method", e2 ? "e2" : "d2");
  fillS("user", e2 ? "mopt__mlogit_e2()" : "mopt__mlogit_d2()");
  fillS("crittype", robustLike ? "log pseudolikelihood" : "log likelihood");
  if (raw.contains("chi2type") && raw["chi2type"].is_string())
    fillS("chi2type", raw["chi2type"].get<std::string>());
  fillS("depvar", lm.y);
  if (lm.vcov == "iid" && session.eval.eStrings.get("vce") == "iid")
    session.eval.setEString("vce", "oim");
  fillS("depvar_outcomes", outcomes);
  fillS("marginsdefault", mdflt);
  fillS("k_eq_model_skip", std::to_string(ibase));
  if (session.eval.findClassMatrix('e', "out") == nullptr) putEMatrix(session, "out", std::move(out));

  fillN("k", static_cast<double>(nFree));
  fillN("rank", static_cast<double>(nFree));
  fillN("k_eq", static_cast<double>(K));
  fillN("k_eq_model", static_cast<double>(K));
  fillN("k_out", static_cast<double>(K));
  fillN("k_eform", static_cast<double>(K));
  fillN("k_eq_base", static_cast<double>(ibase));
  fillN("ibaseout", static_cast<double>(ibase));
  fillN("baseout", *baseVal);
  fillN("k_dv", 1.0);
  fillN("k_autoCns", 0.0);
  fillN("noconstant", noconstant ? 1.0 : 0.0);
  fillN("consonly", 0.0);
  if (const auto it = num("iters")) fillN("ic", *it);
  const bool conv = raw.contains("converged") && raw["converged"].is_boolean() &&
                    raw["converged"].get<bool>();
  if (conv || iterate) fillN("rc", 0.0);
  fillN("converged", conv ? 1.0 : 0.0);
  if (const auto df = num("df_chi2")) fillN("df_m", *df);
  if (const auto p = num("p_chi2")) fillN("p", *p);
  if (num("ll_0").has_value())
    if (const auto r2 = num("r2_pseudo")) fillN("r2_p", *r2);
  if (const auto pm = num("max_p_own"))
    if (*pm < 1.0 - 1e-6) fillN("N_cd", 0.0);

  if (raw.contains("ilog") && raw["ilog"].is_array() && !raw["ilog"].empty() &&
      raw["ilog"].size() <= 20 && session.eval.findClassMatrix('e', "ilog") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    bool ok = true;
    size_t i = 0;
    for (const auto& v : raw["ilog"]) {
      if (!v.is_number()) ok = false;
      else m.data[0][i] = v.get<double>();
      ++i;
    }
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    if (ok) putEMatrix(session, "ilog", std::move(m));
  }
  // the engine's gradient and e_Vmb run over the non-base equations in e(b)'s order
  if (raw.contains("gradient") && raw["gradient"].is_array() && raw["gradient"].size() == nFree &&
      session.eval.findClassMatrix('e', "gradient") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.colnames = cols;
    m.data.assign(1, {});
    size_t g = 0;
    bool ok = true;
    for (const auto& c : cols) {
      if (isO(c)) {
        m.data[0].push_back(0.0);
        continue;
      }
      const ojson& v = raw["gradient"][g++];
      if (!v.is_number()) ok = false;
      m.data[0].push_back(v.is_number() ? v.get<double>() : 0.0);
    }
    if (ok) putEMatrix(session, "gradient", std::move(m));
  }
  if (robustLike && raw.contains("e_Vmb") && raw["e_Vmb"].is_object() &&
      session.eval.findClassMatrix('e', "V_modelbased") == nullptr) {
    const ojson& vm = raw["e_Vmb"].value("matrix", ojson::array());
    if (vm.is_array() && vm.size() == nFree) {
      std::vector<long> at;
      long next = 0;
      for (const auto& c : cols) at.push_back(isO(c) ? -1 : next++);
      const size_t k = cols.size();
      // Its stripe has no equations and no `o.` marks (bank 0c684b5b: rows and
      // columns `age grade _cons age grade _cons ...`).
      std::vector<std::string> bare;
      for (const auto& c : cols) {
        const size_t colon = c.find(':');
        std::string b = colon == std::string::npos ? c : c.substr(colon + 1);
        if (b.compare(0, 2, "o.") == 0) b = b.substr(2);
        bare.push_back(b);
      }
      eval::EvalMatrix m;
      m.rownames = bare;
      m.colnames = bare;
      m.data.assign(k, std::vector<double>(k, 0.0));
      bool ok = true;
      for (size_t a = 0; a < k && ok; ++a)
        for (size_t c = 0; c < k && ok; ++c) {
          if (at[a] < 0 || at[c] < 0) continue;
          const ojson& ra = vm[static_cast<size_t>(at[a])];
          const ojson& rc = vm[static_cast<size_t>(at[c])];
          if (!ra.is_array() || !rc.is_array() || ra.size() != nFree || rc.size() != nFree ||
              !ra[static_cast<size_t>(at[c])].is_number() || !rc[static_cast<size_t>(at[a])].is_number()) {
            ok = false;
            break;
          }
          m.data[a][c] = 0.5 * (ra[static_cast<size_t>(at[c])].get<double>() +
                                rc[static_cast<size_t>(at[a])].get<double>());
        }
      if (ok) putEMatrix(session, "V_modelbased", std::move(m));
    }
  }
}

// xtpoisson's random-effects (gamma) model (xtpoisson.ado / xtps_ren and ml's
// posting; vs-stata bank f8d5160a `xtpoisson fatal beertax unemp`, 8c1f9870
// `, vce(robust)`, bee652c3 `, vce(cluster sid)`, ee596daf `, noconstant`,
// eacdec67 `, exposure(pop)`): the model macros, the comparison model
// (e(ll_c), e(chi2_c) = 2(ll - ll_c), e(chi2_ct) LR), the panel counts, e(alpha),
// the ml settings (xtps_lf, d2, hybrid), e(b_pclass) (2 on /lnalpha), e(ilog),
// e(gradient) and, under the sandwich, e(V_modelbased); e(vce) robust under both
// vce(robust) and vce(cluster panelvar), with e(clustvar) / e(N_clust) for the
// latter only.
void postXtpoisReEContract(SessionState& session, const LastModel& lm, const ojson& raw,
                           bool robustLike, bool clusterGiven, bool iterate, bool noconstant) {
  const auto ebNames = ebColumnNames(session);
  if (!ebNames.has_value() || ebNames->empty()) return;
  const auto fillS = [&session](const char* name, const std::string& v) {
    if (v.empty() || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillN = [&session](const char* name, double v) {
    if (session.eval.eScalars.find(name) != nullptr) return;
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const auto num = [&raw](const char* key) -> std::optional<double> {
    if (!raw.contains(key) || !raw[key].is_number()) return std::nullopt;
    return raw[key].get<double>();
  };
  const std::vector<std::string>& cols = *ebNames;
  const size_t k = cols.size();
  size_t nAuto = 0;
  for (const auto& c : cols) nAuto += isAutoCnsColumn(c) ? 1 : 0;
  const std::string ivar = lm.ivar.has_value() && !lm.ivar->empty()
                               ? *lm.ivar
                               : (session.panelvar.has_value() ? *session.panelvar : std::string());
  // model macros
  fillS("_coef_table_props", "pushtitle");
  fillS("model", "re");
  fillS("diparm1", "lnalpha, exp label(\"alpha\")");
  fillS("predict", "xtpoisson_re_p");
  fillS("distrib", "Gamma");
  fillS("method", "ml");
  fillS("ivar", ivar);
  fillS("chi2_ct", "LR");
  fillS("depvar", lm.y);
  fillS("chi2type", "Wald");
  fillS("title", "Random-effects Poisson regression");
  if (raw.contains("offset") && raw["offset"].is_string()) fillS("offset", raw["offset"].get<std::string>());
  fillS("opt", "moptimize");
  fillS("which", "max");
  fillS("technique", "nr");
  fillS("deriv_useminbound", "off");
  fillS("singularHmethod", "hybrid");
  fillS("ml_method", "d2");
  fillS("user", "xtps_lf");
  fillS("crittype", robustLike ? "log pseudolikelihood" : "log likelihood");
  if (robustLike) {
    session.eval.setEString("vce", "robust");
    session.eval.setEString("vcetype", "Robust");
    session.noteEOnFit("vcetype");
    if (clusterGiven) {
      session.eval.setEString("clustvar", ivar);
      if (const auto g = num("n_clusters")) fillN("N_clust", *g);
    } else {
      session.eval.setEString("clustvar", "");
    }
  } else if (session.eval.eStrings.get("vce") == "iid") {
    session.eval.setEString("vce", "oim");
  }
  // scalars
  fillN("k", static_cast<double>(k));
  fillN("rank", static_cast<double>(k - nAuto));
  fillN("k_eq", 2.0);
  fillN("k_eq_model", 1.0);
  fillN("k_aux", 1.0);
  fillN("k_dv", 1.0);
  fillN("noconstant", noconstant ? 1.0 : 0.0);
  fillN("consonly", 0.0);
  fillN("k_autoCns", static_cast<double>(nAuto));
  if (const auto it = num("iters")) fillN("ic", *it);
  const bool conv = raw.contains("converged") && raw["converged"].is_boolean() &&
                    raw["converged"].get<bool>();
  if (conv || iterate) fillN("rc", 0.0);
  fillN("converged", conv ? 1.0 : 0.0);
  if (const auto v = num("ll")) fillN("ll", *v);
  if (const auto v = num("chi2")) fillN("chi2", *v);
  if (const auto v = num("df_chi2")) fillN("df_m", *v);
  if (const auto v = num("p_chi2")) fillN("p", *v);
  if (const auto v = num("ll_c")) fillN("ll_c", *v);
  if (const auto v = num("chi2_c")) fillN("chi2_c", *v);
  if (const auto v = num("n_groups")) fillN("N_g", *v);
  if (const auto v = num("g_min")) fillN("g_min", *v);
  if (const auto v = num("g_avg")) fillN("g_avg", *v);
  if (const auto v = num("g_max")) fillN("g_max", *v);
  if (raw.contains("alpha") && raw["alpha"].is_object() && raw["alpha"].contains("b"))
    fillN("alpha", raw["alpha"]["b"].get<double>());
  // matrices
  if (session.eval.findClassMatrix('e', "b_pclass") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"y1"};
    m.colnames = cols;
    m.data.assign(1, {});
    for (const auto& c : cols) m.data[0].push_back(c == "/lnalpha" ? 2.0 : 0.0);
    putEMatrix(session, "b_pclass", std::move(m));
  }
  if (raw.contains("ilog") && raw["ilog"].is_array() && !raw["ilog"].empty() &&
      raw["ilog"].size() <= 20 && session.eval.findClassMatrix('e', "ilog") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    bool ok = true;
    size_t i = 0;
    for (const auto& v : raw["ilog"]) {
      if (!v.is_number()) ok = false;
      else m.data[0][i] = v.get<double>();
      ++i;
    }
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    if (ok) putEMatrix(session, "ilog", std::move(m));
  }
  if (raw.contains("gradient") && raw["gradient"].is_array() && raw["gradient"].size() == k - nAuto &&
      session.eval.findClassMatrix('e', "gradient") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.colnames = cols;
    m.data.assign(1, {});
    size_t g = 0;
    for (const auto& c : cols) {
      if (isAutoCnsColumn(c)) {
        m.data[0].push_back(0.0);
        continue;
      }
      const ojson& v = raw["gradient"][g++];
      m.data[0].push_back(v.is_number() ? v.get<double>() : 0.0);
    }
    putEMatrix(session, "gradient", std::move(m));
  }
  // e(Cns): one row per base or omitted column (bank ae7df0d2 `xtpoisson fatal
  // beertax i.year`: `fatal:1982b.year`).
  if (nAuto > 0 && session.eval.findClassMatrix('e', "Cns") == nullptr) {
    eval::EvalMatrix cns;
    cns.colnames = cols;
    cns.colnames.push_back("_Cns:_r");
    for (size_t j = 0; j < k; ++j) {
      if (!isAutoCnsColumn(cols[j])) continue;
      cns.rownames.push_back("r" + std::to_string(cns.rownames.size() + 1));
      std::vector<double> row(k + 1, 0.0);
      row[j] = 1.0;
      cns.data.push_back(std::move(row));
    }
    putEMatrix(session, "Cns", std::move(cns));
  }
  if (robustLike && raw.contains("e_Vmb") && raw["e_Vmb"].is_object() &&
      session.eval.findClassMatrix('e', "V_modelbased") == nullptr) {
    const ojson& vm = raw["e_Vmb"].value("matrix", ojson::array());
    if (vm.is_array() && vm.size() == k - nAuto) {
      std::vector<long> at;
      long next = 0;
      for (const auto& c : cols) at.push_back(isAutoCnsColumn(c) ? -1 : next++);
      eval::EvalMatrix m;
      m.rownames = cols;
      m.colnames = cols;
      m.data.assign(k, std::vector<double>(k, 0.0));
      bool ok = true;
      for (size_t a = 0; a < k && ok; ++a)
        for (size_t c = 0; c < k && ok; ++c) {
          if (at[a] < 0 || at[c] < 0) continue;
          const ojson& ra = vm[static_cast<size_t>(at[a])];
          const ojson& rc = vm[static_cast<size_t>(at[c])];
          if (!ra.is_array() || !rc.is_array() || !ra[static_cast<size_t>(at[c])].is_number() ||
              !rc[static_cast<size_t>(at[a])].is_number()) {
            ok = false;
            break;
          }
          m.data[a][c] = 0.5 * (ra[static_cast<size_t>(at[c])].get<double>() +
                                rc[static_cast<size_t>(at[a])].get<double>());
        }
      if (ok) putEMatrix(session, "V_modelbased", std::move(m));
    }
  }
}

// xtlogit, fe (clogit's posting; vs-stata bank 1ce53089 `xtlogit hi beertax
// unemp, fe`, 0c54ed4e `if year > 1983`, 6da9267d `i.year`): the model macros,
// the ml settings (clogit_lf, e2), e(ll) / e(ll_0) (the log likelihood at
// beta = 0) and the LR model test, e(r2_p), the counts, e(ilog), e(gradient) and
// e(Cns). e(N_drop) / e(N_group_drop) are posted 0 only when no panel was dropped.
void postXtlogitFeEContract(SessionState& session, const ojson& raw, bool iterate) {
  const auto ebNames = ebColumnNames(session);
  if (!ebNames.has_value() || ebNames->empty()) return;
  const auto fillS = [&session](const char* name, const std::string& v) {
    if (v.empty() || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillN = [&session](const char* name, double v) {
    if (session.eval.eScalars.find(name) != nullptr) return;
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const auto num = [&raw](const char* key) -> std::optional<double> {
    if (!raw.contains(key) || !raw[key].is_number()) return std::nullopt;
    return raw[key].get<double>();
  };
  const auto ll = num("loglik");
  const auto ll0 = num("ll_0");
  if (!ll.has_value() || !ll0.has_value()) return;
  const std::vector<std::string>& cols = *ebNames;
  const size_t k = cols.size();
  size_t nAuto = 0;
  for (const auto& c : cols) nAuto += isAutoCnsColumn(c) ? 1 : 0;
  const std::string ivar = session.eval.eStrings.get("ivar").empty() && session.panelvar.has_value()
                               ? *session.panelvar
                               : session.eval.eStrings.get("ivar");
  fillS("ivar", ivar);
  fillS("group", ivar);
  fillS("model", "fe");
  fillS("marginsnotok", "default Pc1 STDP DBeta DX2 GDBeta GDX2 Hat Residuals RSTAndard SCore");
  fillS("marginsdefault", "predict(pu0)");
  fillS("_coef_table_props", "pushtitle");
  fillS("marginsprop", "addcons");
  fillS("ml_method", "e2");
  fillS("multiple", "multiple");
  fillS("chi2type", "LR");
  fillS("deriv_useminbound", "off");
  fillS("opt", "moptimize");
  fillS("user", "clogit_lf");
  fillS("crittype", "log likelihood");
  fillS("singularHmethod", "m-marquardt");
  fillS("technique", "nr");
  fillS("which", "max");
  const double rank = static_cast<double>(k - nAuto);
  fillN("rank", rank);
  fillN("k", static_cast<double>(k));
  fillN("k_eq", 1.0);
  fillN("k_eq_model", 1.0);
  fillN("k_dv", 1.0);
  fillN("noconstant", 1.0);
  fillN("consonly", 0.0);
  fillN("k_autoCns", static_cast<double>(nAuto));
  if (const auto it = num("iters")) fillN("ic", *it);
  const bool conv = raw.contains("converged") && raw["converged"].is_boolean() &&
                    raw["converged"].get<bool>();
  if (conv || iterate) fillN("rc", 0.0);
  fillN("ll", *ll);
  fillN("ll_0", *ll0);
  const double chi2 = 2.0 * (*ll - *ll0);
  fillN("chi2", chi2);
  fillN("df_m", rank);
  if (rank > 0) fillN("p", chi2Upper(rank, chi2));
  if (*ll0 != 0) fillN("r2_p", 1.0 - *ll / *ll0);
  // the panels with all-positive or all-negative outcomes (a single-row panel is
  // one) and their rows (bank 0c54ed4e: 4 groups, 20 obs)
  if (const auto nd = num("n_dropped_obs")) {
    const auto gc = num("n_dropped_const"), gs = num("n_dropped_size");
    if (gc && gs) {
      fillN("N_drop", *nd);
      fillN("N_group_drop", *gc + *gs);
    }
  }
  if (raw.contains("ilog") && raw["ilog"].is_array() && !raw["ilog"].empty() &&
      raw["ilog"].size() <= 20 && session.eval.findClassMatrix('e', "ilog") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    size_t i = 0;
    for (const auto& v : raw["ilog"]) m.data[0][i++] = v.is_number() ? v.get<double>() : 0.0;
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    putEMatrix(session, "ilog", std::move(m));
  }
  if (raw.contains("gradient") && raw["gradient"].is_array() && raw["gradient"].size() == k - nAuto &&
      session.eval.findClassMatrix('e', "gradient") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.colnames = cols;
    m.data.assign(1, {});
    size_t g = 0;
    for (const auto& c : cols) {
      if (isAutoCnsColumn(c)) {
        m.data[0].push_back(0.0);
        continue;
      }
      const ojson& v = raw["gradient"][g++];
      m.data[0].push_back(v.is_number() ? v.get<double>() : 0.0);
    }
    putEMatrix(session, "gradient", std::move(m));
  }
  if (nAuto > 0 && session.eval.findClassMatrix('e', "Cns") == nullptr) {
    eval::EvalMatrix cns;
    cns.colnames = cols;
    cns.colnames.push_back("_Cns:_r");
    for (size_t j = 0; j < k; ++j) {
      if (!isAutoCnsColumn(cols[j])) continue;
      cns.rownames.push_back("r" + std::to_string(cns.rownames.size() + 1));
      std::vector<double> row(k + 1, 0.0);
      row[j] = 1.0;
      cns.data.push_back(std::move(row));
    }
    putEMatrix(session, "Cns", std::move(cns));
  }
}


// tobit (Stata 19's tobit fits gsem; tobit.ado posts gsem's e() and its own;
// vs-stata bank 8328f8b5 `tobit mpg weight, ll(17)`, fa235e5d `tobit mpg weight`
// (no limit: gsem's gaussian `regress` model), a375d11c `, vce(robust)`, 87c721c6
// `[pweight=w]`, dda72913 `i.rep78`, 04017236 `, iterate(2)`): the ml settings,
// the limits as typed or as the depvar's extreme for a bare ll / ul, the model's
// family and link macros, gsem's bookkeeping scalars (every one a constant of a
// one-equation model without latent variables), e(ic) e(converged) e(ilog)
// e(gradient) from the engine's ml iteration, e(_N), e(yinfo1_cens_info) (the
// censoring counts) and e(b_sd) / e(V_sd) (the variance's square root, by the
// delta method), the coefficient classes (1, the variance 4) and e(Cns) for base
// levels.
void postTobitEContract(SessionState& session, const LastModel& lm, const ojson& raw,
                        bool robustLike, bool iterate) {
  const auto ebNames = ebColumnNames(session);
  if (!ebNames.has_value() || ebNames->size() < 2) return;
  const std::vector<std::string>& cols = *ebNames;
  const std::string y = lm.y;
  const std::string varCol = "/var(e." + y + ")";
  if (cols.back() != varCol) return;
  const auto ebm = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  const auto evm = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
  if (!ebm.matrix.has_value() || !evm.matrix.has_value()) return;
  const size_t k = cols.size();
  if (ebm.matrix->data.size() != 1 || ebm.matrix->data[0].size() != k || evm.matrix->data.size() != k)
    return;
  const auto setS = [&session](const char* name, const std::string& v) {
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillS = [&session](const char* name, const std::string& v) {
    if (v.empty() || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillN = [&session](const char* name, double v) {
    if (session.eval.eScalars.find(name) != nullptr) return;
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const auto num = [&raw](const char* key) -> std::optional<double> {
    if (!raw.contains(key) || !raw[key].is_number()) return std::nullopt;
    return raw[key].get<double>();
  };
  // a limit as Stata displays a number (`17`, `4000`, `.5`)
  const auto numText = [](double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    std::string s = buf;
    if (s.rfind("0.", 0) == 0) s.erase(0, 1);
    else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
    return s;
  };
  const auto llLim = num("ll_limit"), ulLim = num("ul_limit");
  const bool censored = llLim.has_value() || ulLim.has_value();

  // the regressors as e(b) names them (base and interaction columns included)
  std::string xvars;
  size_t nAuto = 0;
  for (size_t j = 0; j + 1 < k; ++j) {
    if (isAutoCnsColumn(cols[j])) ++nAuto;
    const size_t colon = cols[j].find(':');
    const std::string bare = colon == std::string::npos ? cols[j] : cols[j].substr(colon + 1);
    if (bare == "_cons") continue;
    xvars += (xvars.empty() ? "" : " ") + bare;
  }

  fillS("opt", "moptimize");
  fillS("which", "max");
  fillS("technique", "nr");
  fillS("deriv_useminbound", "off");
  fillS("singularHmethod", "m-marquardt");
  fillS("ml_method", "gf2");
  fillS("user", "_gsem_eval_iid()");
  fillS("method", "ml");
  fillS("eqnames", "gsem_footnote");
  fillS("footnote", "gsem_footnote");
  fillS("link1", "identity");
  fillS("m1_offset", "\"\"");
  fillS("crittype", robustLike ? "log pseudolikelihood" : "log likelihood");
  fillS("depvar", y);
  fillS("atvars_nochainrule", xvars);
  if (robustLike && raw.value("vce", std::string()) == "cluster") {
    // vce(cluster V) (vs-stata bank ddab9f0e `tobit mpg weight, ll(17) vce(cluster cl)`)
    setS("vce", "cluster");
    fillS("vcetype", "Robust");
    setS("clustvar", raw.value("clustvar", std::string()));
    if (const auto g = num("n_clusters")) fillN("N_clust", *g);
  } else if (robustLike) {
    setS("vce", "robust");
    fillS("vcetype", "Robust");
  } else {
    setS("vce", "oim");
    if (raw.contains("chi2type") && raw["chi2type"].is_string())
      fillS("chi2type", raw["chi2type"].get<std::string>());
  }
  fillS("limit_l", llLim.has_value() ? numText(*llLim) : std::string("-inf"));
  fillS("limit_u", ulLim.has_value() ? numText(*ulLim) : std::string("+inf"));
  if (llLim.has_value()) fillS("llopt", numText(*llLim));
  if (ulLim.has_value()) fillS("ulopt", numText(*ulLim));
  std::string fargs;
  if (llLim.has_value()) fargs += " lcensored(" + numText(*llLim) + ")";
  if (ulLim.has_value()) fargs += " rcensored(" + numText(*ulLim) + ")";
  if (censored) {
    fillS("family1", "gaussian ," + fargs);
    fillS("model1", "tobit");
    fillS("cens_info1", "e(yinfo1_cens_info)");
    fillS("yinfo1_finfo_family", "gaussian");
    fillS("yinfo1_finfo_fargs", "," + fargs);
    fillS("yinfo1_finfo_link", "identity");
    fillS("yinfo1_name", y);
    fillS("yinfo1_xvars", xvars);
    fillN("k_gauss", 0.0);
    fillN("k_yinfo", 1.0);
    fillN("yinfo1_cons", 1.0);
    fillN("yinfo1_yidx", 1.0);
  } else {
    fillS("family1", "gaussian");
    fillS("model1", "regress");
    fillS("gauss1_name", y);
    fillS("gauss1_xvars", xvars);
    fillN("k_gauss", 1.0);
    fillN("k_yinfo", 0.0);
    fillN("gauss1_cons", 1.0);
    fillN("gauss1_yidx", 1.0);
    fillN("gauss_nyvars", 1.0);
  }

  const auto nUnc = num("n_uncensored"), nLc = num("n_left_censored"), nRc = num("n_right_censored");
  if (nUnc) fillN("N_unc", *nUnc);
  if (nLc) fillN("N_lc", *nLc);
  if (nRc) fillN("N_rc", *nRc);
  for (const char* zero : {"consonly", "fmmcmd", "gsem_crossdim", "gsem_hascross", "gsem_ncross",
                           "gsem_nlevels", "irtcmd", "k_hinfo", "k_mult", "k_rc", "mecmd",
                           "noconstant", "xtcmd"})
    fillN(zero, 0.0);
  for (const char* one : {"N_groups", "estimates", "k_aux", "k_dv", "k_eq_model", "k_rs", "listwise"})
    fillN(one, 1.0);
  fillN("gsem_vers", 3.0);
  fillN("version", 3.0);
  fillN("k_eq", 2.0);
  fillN("k", static_cast<double>(k));
  fillN("k_autoCns", static_cast<double>(nAuto));
  fillN("rank", static_cast<double>(k - nAuto));
  if (const auto it = num("iters")) fillN("ic", *it);
  const bool conv = raw.contains("converged") && raw["converged"].is_boolean() &&
                    raw["converged"].get<bool>();
  fillN("converged", conv ? 1.0 : 0.0);
  if (conv || iterate) fillN("rc", 0.0);
  if (const auto r2 = num("r2_pseudo")) fillN("r2_p", *r2);
  if (robustLike) {
    if (const auto F = num("F")) fillN("F", *F);
    if (const auto p = num("p_F")) fillN("p", *p);
  } else if (const auto p = num("p_chi2")) {
    fillN("p", *p);
  }

  const auto rowVec = [](const std::vector<std::string>& cn, std::vector<double> v) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.colnames = cn;
    m.data = {std::move(v)};
    return m;
  };
  std::vector<std::string> cnums;
  for (size_t j = 1; j <= k; ++j) cnums.push_back("c" + std::to_string(j));
  std::vector<double> pclass(k, 1.0), seq(k);
  pclass[k - 1] = 4.0;
  for (size_t j = 0; j < k; ++j) seq[j] = static_cast<double>(j + 1);
  putEMatrix(session, "b_pclass", rowVec(cols, pclass));
  putEMatrix(session, "b_keep", rowVec(cols, std::vector<double>(k, 1.0)));
  putEMatrix(session, "b_idx", rowVec(cnums, seq));
  putEMatrix(session, "b_fitorder", rowVec(cnums, seq));
  // the residual standard deviation's metric: sd = sqrt(var), d sd / d var = 1 / (2 sd)
  {
    std::vector<std::string> sdCols(cols);
    sdCols.back() = "/sd(e." + y + ")";
    const double var = ebm.matrix->data[0][k - 1];
    if (var > 0) {
      const double sd = std::sqrt(var), d = 1.0 / (2.0 * sd);
      std::vector<double> bsd(ebm.matrix->data[0]);
      bsd.back() = sd;
      putEMatrix(session, "b_sd", rowVec(sdCols, bsd));
      eval::EvalMatrix vsd = *evm.matrix;
      vsd.rownames = sdCols;
      vsd.colnames = sdCols;
      for (size_t i = 0; i < k; ++i) {
        vsd.data[i][k - 1] *= d;
        vsd.data[k - 1][i] *= d;
      }
      putEMatrix(session, "V_sd", std::move(vsd));
      putEMatrix(session, "b_sd_pclass", rowVec(sdCols, pclass));
    }
  }
  if (const auto nObs = num("n_obs")) {
    eval::EvalMatrix m = rowVec({y}, {*nObs});
    putEMatrix(session, "_N", std::move(m));
  }
  if (censored && nUnc && nLc && nRc) {
    eval::EvalMatrix m;
    m.rownames = {y};
    m.colnames = {"uncensored", "left", "right", "interval"};
    m.data = {{*nUnc, *nLc, *nRc, 0.0}};
    putEMatrix(session, "yinfo1_cens_info", std::move(m));
  }
  if (!censored) {
    putEMatrix(session, "gauss_V", rowVec({"c1"}, {1.0}));
    putEMatrix(session, "gauss_hasbeta", rowVec({"c1"}, {0.0}));
  }
  if (raw.contains("ilog") && raw["ilog"].is_array() && !raw["ilog"].empty() &&
      raw["ilog"].size() <= 20) {
    std::vector<std::string> c20;
    std::vector<double> v20(20, 0.0);
    for (int c = 1; c <= 20; ++c) c20.push_back("c" + std::to_string(c));
    size_t i = 0;
    for (const auto& v : raw["ilog"]) v20[i++] = v.is_number() ? v.get<double>() : 0.0;
    putEMatrix(session, "ilog", rowVec(c20, v20));
  }
  // e(gradient): the engine's (β, var) gradient on e(b)'s columns, a base column 0
  if (raw.contains("gradient") && raw["gradient"].is_array() &&
      raw["gradient"].size() == k - nAuto) {
    std::vector<double> g;
    size_t gi = 0;
    for (const auto& c : cols) {
      if (isAutoCnsColumn(c)) {
        g.push_back(0.0);
        continue;
      }
      const ojson& v = raw["gradient"][gi++];
      g.push_back(v.is_number() ? v.get<double>() : 0.0);
    }
    putEMatrix(session, "gradient", rowVec(cols, g));
  }
  // e(V_modelbased) under a sandwich: the observed-information variance on e(V)'s stripe
  if (robustLike && raw.contains("e_Vmb") && raw["e_Vmb"].is_object()) {
    const ojson& vm = raw["e_Vmb"].value("matrix", ojson::array());
    if (vm.is_array() && vm.size() == k - nAuto) {
      eval::EvalMatrix m;
      m.rownames = cols;
      m.colnames = cols;
      m.data.assign(k, std::vector<double>(k, 0.0));
      std::vector<long> at(k, -1);
      long next = 0;
      for (size_t j = 0; j < k; ++j)
        if (!isAutoCnsColumn(cols[j])) at[j] = next++;
      bool ok = true;
      for (size_t a = 0; a < k && ok; ++a)
        for (size_t c = 0; c < k && ok; ++c) {
          if (at[a] < 0 || at[c] < 0) continue;
          const ojson& ra = vm[static_cast<size_t>(at[a])];
          if (!ra.is_array() || ra.size() != k - nAuto || !ra[static_cast<size_t>(at[c])].is_number()) {
            ok = false;
            break;
          }
          m.data[a][c] = ra[static_cast<size_t>(at[c])].get<double>();
        }
      if (ok) putEMatrix(session, "V_modelbased", std::move(m));
    }
  }
  if (nAuto > 0) {
    eval::EvalMatrix cns;
    cns.colnames = cols;
    cns.colnames.push_back("_Cns:_r");
    for (size_t j = 0; j < k; ++j) {
      if (!isAutoCnsColumn(cols[j])) continue;
      cns.rownames.push_back("r" + std::to_string(cns.rownames.size() + 1));
      std::vector<double> row(k + 1, 0.0);
      row[j] = 1.0;
      cns.data.push_back(std::move(row));
    }
    putEMatrix(session, "Cns", std::move(cns));
  }
}


// heckman's maximum-likelihood fit (heckman.ado and ml's posting; vs-stata bank
// fdeed49b `heckman lw grade age, select(grade age married south)`, 3e7eef81
// `, vce(robust)`, 797ed381 `, vce(cluster industry)`, f6b9aa69 `, noconstant`,
// cd02251b `select(s = ...)`): the ml settings (heck_d2, the e2 method), the three
// diparm() transforms, the predict/margins lists, e(depvar) (the outcome, then the
// selection equation's depvar when select() names one), the counts, the
// independence test (e(chi2_c), LR or under a sandwich Wald) and the slopes' Wald
// test, e(ilog) and e(gradient) from the engine's ml iteration.
void postHeckmanMlEContract(SessionState& session, const LastModel& lm, const ojson& raw,
                            bool robustLike, bool clusterGiven, bool iterate, bool noconstant) {
  const auto ebNames = ebColumnNames(session);
  if (!ebNames.has_value() || ebNames->size() < 4) return;
  const std::vector<std::string>& cols = *ebNames;
  const size_t k = cols.size();
  const auto setS = [&session](const char* name, const std::string& v) {
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillS = [&session](const char* name, const std::string& v) {
    if (v.empty() || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto setN = [&session](const char* name, double v) {
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const auto num = [&raw](const char* key) -> std::optional<double> {
    if (!raw.contains(key) || !raw[key].is_number()) return std::nullopt;
    return raw[key].get<double>();
  };
  setS("title", "Heckman selection model");
  setS("title2", "(regression model with sample selection)");
  setS("predict", "heckma_p");
  setS("method", "ml");
  if (raw.contains("mills") && raw["mills"].is_string()) setS("mills", raw["mills"].get<std::string>());
  fillS("opt", "moptimize");
  fillS("which", "max");
  fillS("technique", "nr");
  fillS("deriv_useminbound", "off");
  fillS("singularHmethod", "m-marquardt");
  fillS("ml_method", "e2");
  fillS("user", "heck_d2");
  fillS("chi2type", "Wald");
  fillS("chi2_ct", raw.value("chi2_ct", std::string(robustLike ? "Wald" : "LR")));
  fillS("crittype", robustLike ? "log pseudolikelihood" : "log likelihood");
  fillS("diparm1", "athrho, tanh label(\"rho\")");
  fillS("diparm2", "lnsigma, exp label(\"sigma\")");
  fillS("diparm3",
        "athrho lnsigma, func(exp(@2)*(exp(@1)-exp(-@1))/(exp(@1)+exp(-@1)) ) der( "
        "exp(@2)*(1-((exp(@1)-exp(-@1))/(exp(@1)+exp(-@1)))^2) "
        "exp(@2)*(exp(@1)-exp(-@1))/(exp(@1)+exp(-@1)) ) label(\"lambda\")");
  fillS("marginsderiv", "PSel YCond YExpected Mills NShazard");
  fillS("marginsnomarkout", "XBSel PSel");
  fillS("marginsnotok", "SCores SCORESEL stdp stdf stdpsel");
  fillS("marginsok", "default XB PSel YCond YExpected Mills NShazard");
  const std::string seleq = raw.value("select_eq", std::string("select"));
  setS("depvar", seleq == "select" ? lm.y : lm.y + " " + seleq);
  if (clusterGiven) {
    setS("vce", "cluster");
    fillS("vcetype", "Robust");
    setS("clustvar", raw.value("clustvar", std::string()));
  } else if (robustLike) {
    setS("vce", "robust");
    fillS("vcetype", "Robust");
  } else {
    setS("vce", "oim");
  }
  size_t nAuto = 0;
  for (const auto& c : cols)
    if (isAutoCnsColumn(c)) ++nAuto;
  setN("k", static_cast<double>(k));
  setN("rank", static_cast<double>(k - nAuto));
  setN("k_autoCns", static_cast<double>(nAuto));
  setN("k_eq", 4.0);
  setN("k_aux", 2.0);
  setN("k_dv", 2.0);
  setN("k_eq_model", 1.0);
  setN("consonly", 0.0);
  setN("noconstant", noconstant ? 1.0 : 0.0);
  if (const auto it = num("iters")) setN("ic", *it);
  const bool conv = raw.contains("converged") && raw["converged"].is_boolean() &&
                    raw["converged"].get<bool>();
  setN("converged", conv ? 1.0 : 0.0);
  if (conv || iterate) setN("rc", 0.0);
  if (const auto v = num("chi2")) setN("chi2", *v);
  if (const auto v = num("df_chi2")) setN("df_m", *v);
  if (const auto v = num("p_chi2")) setN("p", *v);
  if (const auto v = num("chi2_c")) setN("chi2_c", *v);
  if (const auto v = num("p_c")) setN("p_c", *v);
  for (const char* key : {"rho", "sigma", "lambda"})
    if (const auto v = num(key)) setN(key, *v);
  if (const auto v = num("se_lambda")) setN("selambda", *v);
  if (const auto v = num("n_selected")) {
    setN("N_selected", *v);
    setN("N_unc", *v);
    if (const auto n = num("n_obs")) {
      setN("N_nonselected", *n - *v);
      setN("N_cens", *n - *v);
    }
  }
  if (const auto v = num("n_clusters")) setN("N_clust", *v);
  if (raw.contains("ilog") && raw["ilog"].is_array() && !raw["ilog"].empty() &&
      raw["ilog"].size() <= 20) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    size_t i = 0;
    for (const auto& v : raw["ilog"]) m.data[0][i++] = v.is_number() ? v.get<double>() : 0.0;
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    putEMatrix(session, "ilog", std::move(m));
  }
  if (raw.contains("gradient") && raw["gradient"].is_array() && raw["gradient"].size() == k) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.colnames = cols;
    m.data.assign(1, {});
    for (const auto& v : raw["gradient"]) m.data[0].push_back(v.is_number() ? v.get<double>() : 0.0);
    putEMatrix(session, "gradient", std::move(m));
  }
  if (robustLike && raw.contains("e_Vmb") && raw["e_Vmb"].is_object()) {
    const ojson& vm = raw["e_Vmb"].value("matrix", ojson::array());
    if (vm.is_array() && vm.size() == k) {
      eval::EvalMatrix m;
      m.rownames = cols;
      m.colnames = cols;
      m.data.assign(k, std::vector<double>(k, 0.0));
      bool ok = true;
      for (size_t a = 0; a < k && ok; ++a) {
        if (!vm[a].is_array() || vm[a].size() != k) {
          ok = false;
          break;
        }
        for (size_t c = 0; c < k; ++c) m.data[a][c] = vm[a][c].is_number() ? vm[a][c].get<double>() : 0.0;
      }
      if (ok) putEMatrix(session, "V_modelbased", std::move(m));
    }
  }
}

// heckman's two-step fit (heckman.ado's Heck2; vs-stata bank b6e7e050 `heckman lw
// grade age, select(grade age married south) twostep`, c1488541 `twostep rhosigma`):
// the method, the rhosigma rule, the conventional VCE, the predict/margins lists,
// e(depvar), the counts, rho/sigma/lambda (e(rho1) when the rule moved rho), the Wald
// test of the outcome slopes and the first-step probit's convergence.
void postHeckmanTwostepEContract(SessionState& session, const LastModel& lm, const ojson& raw) {
  const auto ebNames = ebColumnNames(session);
  if (!ebNames.has_value()) return;
  const auto setS = [&session](const char* name, const std::string& v) {
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto setN = [&session](const char* name, double v) {
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const auto num = [&raw](const char* key) -> std::optional<double> {
    if (!raw.contains(key) || !raw[key].is_number()) return std::nullopt;
    return raw[key].get<double>();
  };
  setS("title", "Heckman selection model -- two-step estimates");
  setS("title2", "(regression model with sample selection)");
  setS("predict", "heckma_p");
  setS("method", "two-step");
  if (raw.contains("mills") && raw["mills"].is_string()) setS("mills", raw["mills"].get<std::string>());
  setS("rhometh", "rhosigma");
  setS("vce", "conventional");
  setS("chi2type", "Wald");
  setS("margins_prolog", "heckman_fix_stripe");
  setS("margins_epilog", "heckman_restore_stripe");
  setS("marginsok", "XB XBSel Mills NShazard PSel E(passthru) Pr(passthru) YStar(passthru) default");
  setS("marginsnotok", "SCores SCORESEL stdp stdf stdpsel YCond YExpected");
  setS("marginsnomarkout", "XBSel PSel");
  setS("marginsderiv", "PSel Mills NShazard");
  const std::string seleq = raw.value("select_eq", std::string("select"));
  setS("depvar", seleq == "select" ? lm.y : lm.y + " " + seleq);
  setN("rank", static_cast<double>(ebNames->size()));
  if (const auto v = num("chi2")) setN("chi2", *v);
  if (const auto v = num("df_chi2")) setN("df_m", *v);
  if (const auto v = num("p_chi2")) setN("p", *v);
  for (const char* key : {"rho", "sigma", "lambda", "rho1", "fconverged"})
    if (const auto v = num(key)) setN(key, *v);
  if (const auto v = num("se_lambda")) setN("selambda", *v);
  if (const auto v = num("n_selected")) {
    setN("N_selected", *v);
    setN("N_unc", *v);
    if (const auto n = num("n_obs")) {
      setN("N_nonselected", *n - *v);
      setN("N_cens", *n - *v);
    }
  }
}

}  // namespace

void postStataEContract(SessionState& session) {
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || session.ePostedLive || !session.eval.eStrings.get("prefix").empty())
    return;
  const std::string cmd = session.eval.eStrings.get("cmd");
  if (cmd.empty()) return;
  // A handler that published its fit's whole ordered namespace (regress, areg,
  // ivregress, pca: estimation/estimation_results.cpp) owns it: nothing is added.
  if (session.eNameContract.has_value() && session.eNameContract->fitSerial == lmo->fitSerial)
    return;
  const auto fillS = [&session](const char* name, const std::string& v) {
    if (v.empty() || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);
  };
  const auto fillN = [&session](const char* name, double v) {
    if (session.eval.eScalars.find(name) != nullptr) return;
    session.eval.setEScalar(name, v);
    session.noteEOnFit(name);
  };
  const ojson* raw = lmo->result.has_value() && lmo->result->is_object() ? &*lmo->result : nullptr;
  const auto rawNum = [raw](const char* key) -> std::optional<double> {
    if (raw == nullptr || !raw->contains(key) || !(*raw)[key].is_number()) return std::nullopt;
    return (*raw)[key].get<double>();
  };
  const FitOptions fo = readFitOptions(session.leafCmdExpanded);

  // e(properties): Stata's word list for a fit whose e(b) and e(V) both exist.
  static const std::set<std::string> kBV = {
      "logit",  "logistic", "probit", "poisson", "cloglog",  "nbreg",   "glm",    "ppmlhdfe",
      "ivprobit", "ivtobit", "tobit", "ologit",  "oprobit",  "mlogit",  "qreg",   "xtreg",
      "sureg",  "reg3",     "var",    "acreg",   "fracreg",  "zip",     "zinb",   "xtlogit",
      "xtpoisson", "clogit", "heckman", "newey", "prais",    "areg",    "ivregress", "gmm",
      "nl",     "xtabond",  "xtgee",  "mixed"};
  if (kBV.count(cmd) > 0 && session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy)
                                .matrix.has_value() &&
      session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy).matrix.has_value())
    fillS("properties", "b V");

  // newey: e(vcetype) Newey–West and e(rank) the coefficients that are not base or
  // omitted levels (bank 9fc11f59: rank 3 for mpg weight _cons).
  if (cmd == "newey") {
    fillS("vcetype", "Newey\xE2\x80\x93West");
    if (const auto cols = ebColumnNames(session)) {
      double k = 0;
      for (const auto& c : *cols) k += isAutoCnsColumn(c) ? 0.0 : 1.0;
      fillN("rank", k);
    }
  }
  // regress's fixed macros on the fits whose ordered e() lists are not claimed
  // (estimation_results.cpp posts them on the others): an exact fit (bank 513fdb79
  // `regress y mpg` with y constant, 7317c331 rss 0) posts them as every fit does.
  if (cmd == "regress" && lmo->fe.empty()) {
    fillS("properties", "b V");
    fillS("model", "ols");
    fillS("marginsprop", "minus");
  }
  // The margins contract macros (fixed per estimator).
  struct MarginsMacros {
    const char *cmd, *deriv, *notok, *prop;
  };
  static const MarginsMacros kMargins[] = {
      {"logit", "default Pr",
       "stdp DBeta DEviance DX2 DDeviance Hat Number Residuals RStandard SCore", ""},
      {"logistic", "default Pr",
       "stdp DBeta DEviance DX2 DDeviance Hat Number Residuals RStandard SCore", ""},
      {"probit", "default Pr", "stdp DEviance SCore", ""},
      {"cloglog", "default Pr", "stdp SCore", ""},
      {"poisson", "default N IR", "", ""},
      {"nbreg", "default N IR", "", ""},
      {"ologit", "default Pr OUTcome(passthru)", "", "addcons"},
      {"oprobit", "default Pr OUTcome(passthru)", "", "addcons"},
      {"mlogit", "default Pr OUTcome(passthru)", "stdp stddp SCores", ""},
      {"glm", "",
       "stdp Anscombe Cooksd Deviance Hat Likelihood Pearson Response Score Working ADJusted "
       "STAndardized STUdentized MODified",
       ""},
      {"ppmlhdfe", "",
       "stdp Anscombe Cooksd Deviance Hat Likelihood Pearson Response Score Working ADJusted "
       "STAndardized STUdentized MODified",
       ""},
      {"qreg", "", "stdp stddp residuals", ""},
      {"fracreg", "", "stdp scores", ""},
      {"sureg", "", "stdp Residuals stddp", ""},
      {"reg3", "", "stdp Residuals stddp", ""},
      {"var", "", "stdp Residuals", ""},
      {"reghdfe", "", "Residuals SCore", ""},
      {"ivprobit", "", "", "nochainrule"},
      {"ivtobit", "", "", "nochainrule"},
  };
  for (const auto& mm : kMargins) {
    if (cmd != mm.cmd) continue;
    fillS("marginsderiv", mm.deriv);
    fillS("marginsnotok", mm.notok);
    fillS("marginsprop", mm.prop);
  }
  // ivregress's instrument macros (bank e711c736 and every ivregress fit): e(instd) =
  // e(endog) the instrumented regressors, e(exogr) the included exogenous regressors as
  // e(b) names them (`weight 0b.foreign 1.foreign`: d21497fc; `o.weight` when
  // collinear: 18eb9e3d), e(insts) = e(exog) those followed by the excluded
  // instruments, e(idtype) `justidentified` / `overidentified`. Posted only when every
  // excluded instrument is a plain variable and e(b) leads with the instrumented
  // regressors as typed.
  if (cmd == "ivregress" && !session.eval.eStrings.get("estimator").empty() &&
      !lmo->ivEndog.empty()) {
    if (const auto cols = ebColumnNames(session)) {
      std::vector<std::string> c;
      for (const auto& s : *cols)
        if (s != "_cons") c.push_back(s.substr(s.find(':') == std::string::npos ? 0 : s.find(':') + 1));
      bool ok = c.size() >= lmo->ivEndog.size() && lmo->ivInstruments.size() >= lmo->ivEndog.size();
      std::string instd, exogr, excluded;
      for (size_t j = 0; ok && j < lmo->ivEndog.size(); ++j) {
        const std::optional<std::string> full = resolveVarName(lmo->ivEndog[j], session);
        ok = full.has_value() && *full == c[j];
        instd += (instd.empty() ? "" : " ") + c[j];
      }
      for (size_t j = lmo->ivEndog.size(); ok && j < c.size(); ++j)
        exogr += (exogr.empty() ? "" : " ") + c[j];
      for (size_t j = 0; ok && j < lmo->ivInstruments.size(); ++j) {
        const std::string& z = lmo->ivInstruments[j];
        ok = !z.empty() && z.find_first_of(".#()*?~- ") == std::string::npos;
        const std::optional<std::string> full = ok ? resolveVarName(z, session) : std::nullopt;
        ok = full.has_value();
        if (ok) excluded += (excluded.empty() ? "" : " ") + *full;
      }
      if (ok) {
        const std::string exog = exogr.empty() ? excluded : exogr + " " + excluded;
        fillS("instd", instd);
        fillS("endog", instd);
        fillS("exogr", exogr);
        fillS("insts", exog);
        fillS("exog", exog);
        fillS("idtype", lmo->ivInstruments.size() == lmo->ivEndog.size() ? "justidentified"
                                                                          : "overidentified");
      }
    }
  }
  // reghdfe's e(indepvars): e(b)'s columns as the regressor list reghdfe expanded
  // before the fit -- a base factor level left out, a factor level written `#bn.`,
  // an omitted (collinear) regressor under its plain name (bank b3afd3a0 `0b.foreign
  // 1.foreign` -> `1bn.foreign`, d8b4c167 `1982b.year 1983.year` -> `1983bn.year`,
  // bff6620b `o.ti` -> `ti`; `c.weight#c.weight` as is). Posted for the fits whose
  // absorbed-effect bookkeeping was posted (e(dofmethod)); a column outside these
  // shapes (a factor interaction, an omitted level, a time-series operator) leaves
  // it unposted.
  if (cmd == "reghdfe" && !session.eval.eStrings.get("dofmethod").empty()) {
    if (const auto cols = ebColumnNames(session)) {
      std::string iv;
      bool ok = true;
      const auto plainName = [](const std::string& v) {
        return !v.empty() && (std::isalpha(static_cast<unsigned char>(v[0])) || v[0] == '_') &&
               std::all_of(v.begin(), v.end(), [](char ch) {
                 return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
               });
      };
      for (const std::string& col : *cols) {
        std::string out;
        if (plainName(col)) {
          out = col;
        } else if (col.rfind("o.", 0) == 0 && plainName(col.substr(2))) {
          out = col.substr(2);
        } else if (col.find('#') != std::string::npos) {
          size_t i = 0;  // continuous interactions only: every part `c.var`
          while (ok && i <= col.size()) {
            const size_t j = std::min(col.find('#', i), col.size());
            const std::string part = col.substr(i, j - i);
            ok = part.rfind("c.", 0) == 0 && plainName(part.substr(2));
            i = j + 1;
          }
          out = col;
        } else {
          const size_t dot = col.find('.');
          const std::string op = dot == std::string::npos ? std::string() : col.substr(0, dot);
          const std::string var = dot == std::string::npos ? std::string() : col.substr(dot + 1);
          const size_t digits = op.find_first_not_of("0123456789");
          if (op.size() > 1 && digits == op.size() - 1 && op.back() == 'b' && plainName(var))
            continue;  // a base level
          // a time-series operator stays as typed (bank 0fd9f334 `L.beertax D.unemp _cons`)
          const bool tsop = !op.empty() && std::string("LDFS").find(op[0]) != std::string::npos &&
                            op.find_first_not_of("LDFS0123456789") == std::string::npos;
          ok = !op.empty() && (digits == std::string::npos || tsop) && plainName(var);
          out = tsop ? col : op + "bn." + var;
        }
        if (!ok) break;
        iv += (iv.empty() ? "" : " ") + out;
      }
      if (ok && !iv.empty()) fillS("indepvars", iv);
    }
  }
  // xtreg: every model posts the same list; fe adds XBD D and signs its sample
  // (e(datasignaturevars): the depvar, the regressors' variables, the panel
  // variable and absorb(); e(datasignature) itself is not reproduced -- its
  // checksum is not PIET's).
  if (cmd == "xtreg") {
    const std::string model = session.eval.eStrings.get("model");
    const bool fe = model == "fe";
    fillS("marginsnotok", fe ? "E U UE SCore STDP XBU XBD D" : "E U UE SCore STDP XBU");
    fillS("marginsprop", "nolinearize");
    if (model == "be") fillS("predict", "xtrefe_p");
    // re/be: e(depvar), e(ivar) (every xtreg posts the panel variable), e(vce)
    // "conventional" for the default VCE, and re's Wald model test (bank: 100 of
    // 100 default re fits `conventional`, `Wald`, e(ivar) set).
    if (model == "re" || model == "be") {
      fillS("depvar", lmo->y);
      if (session.eval.eStrings.get("vce") == "iid") session.eval.setEString("vce", "conventional");
      if (model == "re") {
        fillS("chi2type", "Wald");
        // the Wald model test's df and p-value, as the reply carries them
        if (raw != nullptr && raw->contains("df_chi2") && (*raw)["df_chi2"].is_number() &&
            raw->contains("p_chi2") && (*raw)["p_chi2"].is_number() &&
            !raw->contains("n_clusters")) {
          fillN("df_m", (*raw)["df_chi2"].get<double>());
          fillN("p", (*raw)["p_chi2"].get<double>());
        }
      }
    }
    // every model posts the panel variable
    const std::string ivar = lmo->ivar.has_value() && !lmo->ivar->empty()
                                 ? *lmo->ivar
                                 : (session.panelvar.has_value() ? *session.panelvar : std::string());
    fillS("ivar", ivar);
    // fe's absorb step (bank: every fe fit): e(absvar) the panel variable (and
    // absorb()'s), e(estmethod) xtreg_fe (MAP under absorb()), the hidden
    // e(Tbar__CL) label, e(rc) 0 and, with one absorbed variable, e(kabsorb)
    // (1 x 1, its level count; row r1, column the panel variable).
    if (fe && fo.parsed && !ivar.empty()) {
      std::string absorbVar;
      for (const auto& [k, v] : eval::tokenize(session.leafCmdExpanded).opts)
        if (optIs(k, "absorb", 1) && !v.isBool) absorbVar = v.strValue;
      absorbVar.erase(0, absorbVar.find_first_not_of(" \t"));
      absorbVar.erase(absorbVar.find_last_not_of(" \t") + 1);
      // posted under its full name (the ado's syntax expands an abbreviation)
      bool absorbKnown = true;
      if (!absorbVar.empty() && absorbVar.find_first_of(" .#(") == std::string::npos) {
        const std::optional<std::string> full = resolveVarName(absorbVar, session);
        if (full.has_value()) absorbVar = *full;
        else absorbKnown = false;
      }
      if (absorbKnown && absorbVar.find_first_of(" .#(") == std::string::npos) {
        fillS("absvar", absorbVar.empty() ? ivar : ivar + " " + absorbVar);
        // a cluster variable other than the panel variable may not nest in it,
        // which sends Stata to MAP too (bank: `vce(cluster rg)` MAP): not posted
        const std::string clv = session.eval.eStrings.get("clustvar");
        if (!absorbVar.empty()) fillS("estmethod", "MAP");
        else if (clv.empty() || clv == ivar) fillS("estmethod", "xtreg_fe");
        fillS("Tbar__CL", "Mean of group sizes");
        fillN("rc", 0.0);
        // e(sum_w): e(N) for an unweighted fit
        if (session.eval.eStrings.get("wtype").empty())
          if (const auto nObs = rawNum("n_obs")) fillN("sum_w", *nObs);
        if (absorbVar.empty() && raw != nullptr && raw->contains("absorbed") &&
            (*raw)["absorbed"].is_object() && (*raw)["absorbed"].contains(ivar) &&
            (*raw)["absorbed"][ivar].is_number() &&
            session.eval.findClassMatrix('e', "kabsorb") == nullptr) {
          eval::EvalMatrix m;
          m.rownames = {"r1"};
          m.colnames = {ivar};
          m.data = {{(*raw)["absorbed"][ivar].get<double>()}};
          putEMatrix(session, "kabsorb", std::move(m));
        }
      }
    }
    if (fe && fo.parsed && !ivar.empty()) {
      // The bank's lists: the depvar and the plain (or c.) regressors as typed,
      // then the panel variable, then each i. factor's variable and absorb()'s
      // (`xtreg frate beertax unemp i.year, fe` -> "frate beertax unemp sid
      // year"; `, fe absorb(year)` the same). Interactions and time-series
      // operators are not posted. Each name is posted in full: the ado's
      // `syntax varlist` expands an abbreviation before the signature is taken
      // (`xtreg fr beert unemp, fe` posts "fr beertax unemp sid").
      const eval::ParsedCommand p = eval::tokenize(session.leafCmdExpanded);
      std::vector<std::string> head, tail;
      bool plain = !p.varlist.empty();
      const auto add = [](std::vector<std::string>& to, const std::string& v) {
        if (std::find(to.begin(), to.end(), v) == to.end()) to.push_back(v);
      };
      for (size_t i = 0; i < p.varlist.size() && plain; ++i) {
        std::string v = p.varlist[i];
        const bool factor = v.rfind("i.", 0) == 0;
        if (factor || v.rfind("c.", 0) == 0) v = v.substr(2);
        if (v.empty() || v.find_first_of(".#()*?~-") != std::string::npos ||
            (factor && i == 0)) {
          plain = false;
          break;
        }
        const std::optional<std::string> full = resolveVarName(v, session);
        if (!full.has_value()) {
          plain = false;
          break;
        }
        v = *full;
        if (i == 0) head.push_back(v);
        else add(factor ? tail : head, v);
      }
      for (const auto& [k, v] : p.opts) {
        if (!optIs(k, "absorb", 1) || v.isBool) continue;
        std::string a = v.strValue;
        const size_t b0 = a.find_first_not_of(" \t");
        a = b0 == std::string::npos ? std::string() : a.substr(b0, a.find_last_not_of(" \t") - b0 + 1);
        std::optional<std::string> full;
        if (!a.empty() && a.find_first_of(" .#") == std::string::npos)
          full = resolveVarName(a, session);
        if (!full.has_value()) plain = false;
        else add(tail, *full);
      }
      if (plain) {
        std::string out;
        for (const auto& v : head) out += (out.empty() ? "" : " ") + v;
        out += " " + ivar;
        for (const auto& v : tail)
          if (v != ivar) out += " " + v;
        fillS("datasignaturevars", out);
      }
    }
  }

  // fracreg's ml record, model test, e(mns) and macros (bank 2bef1615, 952f78c4)
  if (cmd == "fracreg") {
    if (raw != nullptr) est_detail::postFracregContractM4(session, *raw);
    return;
  }
  // ologit / oprobit: the rest of the ado's posting (ologit.ado / oprobit.ado
  // Estimate, then ml's own e()). Only for the forms the handler fits and posts
  // e(b) for (no technique(), constraints(), offset(), resampling VCE).
  if ((cmd == "ologit" || cmd == "oprobit") && raw != nullptr && fo.parsed && !fo.technique &&
      !fo.resampling && !fo.constraints && !fo.exposure) {
    // e(offset): the variable offset() named (bank 13941585 "off")
    if (fo.offset && raw->contains("offset") && (*raw)["offset"].is_string())
      fillS("offset", (*raw)["offset"].get<std::string>());
    postOrderedEContract(session, *lmo, *raw, cmd, fo.vceGiven || fo.score, fo.robustLike,
                         fo.iterate, fo.difficult);
  }
  if (cmd == "xtpoisson" && lmo->cobraCmd == "xtpois_re" && raw != nullptr && fo.parsed &&
      !fo.technique && !fo.resampling && !fo.constraints) {
    const bool clusterGiven = lmo->vcov == "cluster" || !lmo->cluster.empty();
    postXtpoisReEContract(session, *lmo, *raw, fo.robustLike, clusterGiven, fo.iterate,
                          fo.noconstant);
  }
  if (cmd == "clogit" && raw != nullptr && raw->value("estimator", std::string()) == "xtlogit_fe" &&
      fo.parsed && !fo.technique && !fo.resampling && !fo.constraints && !fo.vceGiven) {
    if (session.eval.eStrings.get("vce") == "iid") session.eval.setEString("vce", "oim");
    postXtlogitFeEContract(session, *raw, fo.iterate);
  }
  if (cmd == "heckman" && lmo->cobraCmd == "heckman" && raw != nullptr &&
      raw->value("method", std::string()) == "twostep")
    postHeckmanTwostepEContract(session, *lmo, *raw);
  if (cmd == "heckman" && lmo->cobraCmd == "heckman_ml" && raw != nullptr && fo.parsed) {
    const bool clusterGiven = raw->value("vce", std::string()) == "cluster";
    postHeckmanMlEContract(session, *lmo, *raw, fo.robustLike, clusterGiven, fo.iterate,
                           fo.noconstant);
  }
  if (cmd == "tobit" && raw != nullptr && fo.parsed && !fo.technique && !fo.resampling &&
      !fo.constraints && !fo.offset)
    postTobitEContract(session, *lmo, *raw, fo.robustLike, fo.iterate);
  if (cmd == "mlogit" && raw != nullptr && fo.parsed && !fo.technique && !fo.resampling &&
      !fo.constraints) {
    postMlogitEContract(session, *raw, *lmo, fo.vceGiven || fo.score, fo.robustLike, fo.iterate,
                        fo.difficult, fo.noconstant);
  }

  // The `ml` settings and scalars of the four maximum-likelihood fits.
  const bool logitLike = cmd == "logit" || cmd == "logistic" || cmd == "probit";
  // cloglog: the same ml settings, always the e2 evaluator clog_lf (bank 38560e4b)
  const bool clog = cmd == "cloglog";
  if (!(logitLike || clog || cmd == "poisson") || !fo.parsed || fo.technique || fo.resampling ||
      fo.constraints || (fo.exposure && cmd != "poisson"))
    return;
  // e(offset) as the ado posts it (bank: `offset(lpop)` "lpop", `exposure(pop)`
  // "ln(pop)"); a blank or compound spelling is not posted.
  std::string offsetMacro = fo.offsetMacro;
  offsetMacro.erase(0, offsetMacro.find_first_not_of(" \t"));
  offsetMacro.erase(offsetMacro.find_last_not_of(" \t") + 1);
  const bool offsetUsed = fo.offset || fo.exposure;
  if (offsetUsed) {
    if (offsetMacro.empty() || offsetMacro.find_first_of(" ,") != std::string::npos) return;
    fillS("offset", offsetMacro);
  }
  const bool e2 = cmd == "poisson" || clog || fo.vceGiven || fo.score;
  fillS("opt", "moptimize");
  fillS("which", "max");
  fillS("technique", "nr");
  fillS("deriv_useminbound", "off");
  fillS("singularHmethod", fo.difficult ? "hybrid" : "m-marquardt");
  fillS("ml_method", e2 ? "e2" : "d2");
  if (cmd == "poisson") fillS("user", "poiss_lf");
  else if (clog) fillS("user", "clog_lf");
  else fillS("user", std::string("mopt__") + (cmd == "probit" ? "probit" : "logit") +
                         (e2 ? "_e2()" : "_d2()"));
  fillS("crittype", fo.robustLike ? "log pseudolikelihood" : "log likelihood");
  if (cmd == "poisson") {
    fillS("gof", "poiss_g");
    // poisson's model test: LR, Wald under robust/cluster/pweight or noconstant.
    fillS("chi2type", fo.robustLike || fo.noconstant ? "Wald" : "LR");
    // e(vce) "oim": the ML default VCE (as logit's, binary_estimation.cpp).
    if (lmo->vcov == "iid" && session.eval.eStrings.get("vce") == "iid")
      session.eval.setEString("vce", "oim");
  }

  fillN("k_eq", 1.0);
  fillN("k_dv", 1.0);
  fillN("k_eq_model", 1.0);
  fillN("noconstant", fo.noconstant ? 1.0 : 0.0);
  if (fo.nIndep > 0) fillN("consonly", 0.0);
  if (const auto cols = ebColumnNames(session)) {
    double n = 0;
    for (const auto& c : *cols) n += isAutoCnsColumn(c) ? 1.0 : 0.0;
    fillN("k_autoCns", n);
    if (cmd == "poisson") fillN("k", static_cast<double>(cols->size()));
  }
  const std::optional<double> conv =
      raw != nullptr && raw->contains("converged") && (*raw)["converged"].is_boolean()
          ? std::optional<double>((*raw)["converged"].get<bool>() ? 1.0 : 0.0)
          : rawNum("converged");
  if ((conv.has_value() && *conv == 1.0) || fo.iterate) fillN("rc", 0.0);
  // the model test's p-value
  if (logitLike || clog) {
    if (const auto p = rawNum("p_chi2")) fillN("p", *p);
  } else {
    const auto chi2 = rawNum("chi2");
    const auto df = rawNum("chi2_df");
    // With fewer clusters than coefficients the clustered V is short of rank and
    // Stata's model test drops constraints (bank 03ca3e32: 2 clusters, 3
    // coefficients, Wald chi2(0) = .); the reply's df is then not Stata's.
    const auto G = rawNum("n_clusters");
    const auto cols = ebColumnNames(session);
    const bool shortRank = G.has_value() && *G > 0 &&
                           (!cols.has_value() || *G - 1.0 < static_cast<double>(cols->size()));
    if (df && !shortRank) fillN("df_m", *df);
    if (chi2 && df && *df > 0 && !shortRank) fillN("p", chi2Upper(*df, *chi2));
  }
  const auto ll0 = rawNum("ll_0");
  if (ll0) {
    if (const auto r2 = rawNum("r2_pseudo")) fillN("r2_p", *r2);
  }
  // e(ilog): ml's 1 x 20 iteration log, the log likelihood at iterations
  // 0..ic and zeros after (row r1, columns c1..c20), from the reply's `ilog`.
  // The ado starts Newton-Raphson at the constant-only fit (iteration 0 is
  // e(ll_0)); under offset() at that constant less the mean offset, and under
  // noconstant at zero, which are the engine's starts too (binary_ml_fit; bank
  // 4ab82d44 probit offset iteration 0 -57.490097, 515ed8d0 logit noconstant
  // -51.292891 = 74 ln .5). With a constant and no offset the log is posted
  // only when its iteration 0 is e(ll_0).
  const auto ilog0 = [raw]() -> std::optional<double> {
    if (raw == nullptr || !raw->contains("ilog") || !(*raw)["ilog"].is_array() ||
        (*raw)["ilog"].empty() || !(*raw)["ilog"][0].is_number())
      return std::nullopt;
    return (*raw)["ilog"][0].get<double>();
  }();
  const auto llZero = rawNum("ll_0");
  // cloglog starts at the 3-iteration logit fit, as the engine does (cloglog_ml).
  // poisson's reply carries `ilog` only from Stata's own ml path (fepois.cpp).
  if (((logitLike || clog) || cmd == "poisson") && ilog0 &&
      (clog || cmd == "poisson" || offsetUsed || fo.noconstant ||
       (llZero && std::fabs(*ilog0 - *llZero) <= 1e-9 * std::fabs(*llZero))) &&
      (*raw)["ilog"].size() <= 20 && session.eval.findClassMatrix('e', "ilog") == nullptr) {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    bool ok = true;
    size_t i = 0;
    for (const auto& v : (*raw)["ilog"]) {
      if (!v.is_number()) ok = false;
      else m.data[0][i] = v.get<double>();
      ++i;
    }
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    if (ok) {
      session.eval.eMatrices.emplace_back("ilog", std::move(m));
      session.noteEOnFit("ilog");
    }
  }
  if ((logitLike || clog || cmd == "poisson") && raw != nullptr &&
      session.eval.findClassMatrix('e', "gradient") == nullptr) {
    if (auto g = gradientOnEbStripe(session, *raw)) putEMatrix(session, "gradient", std::move(*g));
  }
  if ((logitLike || clog || cmd == "poisson") && raw != nullptr &&
      session.eval.findClassMatrix('e', "V_modelbased") == nullptr) {
    if (auto v = modelBasedVOnEbStripe(session, *raw)) putEMatrix(session, "V_modelbased", std::move(*v));
  }
  // e(mns) and e(Cns) of logit/logistic/probit (the ado's _rmcoll results): mns
  // is the weighted mean over e(sample) of each e(b) column's regressor -- 0 for
  // a base or omitted level, 1 for _cons (bank: `[fweight=w]`, `[pweight=w]` and
  // `[iweight=w]` all weight it); Cns has one row per base/omitted column, 1 in
  // that column, and a last column _Cns:_r of 0 (posted only when there is one).
  if (logitLike && lmo->esample == LastModel::EsampleState::kColumn &&
      session.eval.findClassMatrix('e', "mns") == nullptr) {
    if (const auto cols = ebColumnNames(session)) postLogitMnsCns(session, *lmo, *cols);
  }
  // cloglog and poisson post e(Cns) and no e(mns) (bank 21336c67, cf47e05a)
  if ((clog || cmd == "poisson") && lmo->esample == LastModel::EsampleState::kColumn &&
      session.eval.findClassMatrix('e', "Cns") == nullptr) {
    if (const auto cols = ebColumnNames(session)) postLogitMnsCns(session, *lmo, *cols, false);
  }
  // e(converged) as a stored scalar: the reply carries it as a boolean, which
  // the ordered-list check below (SessionState::eNameOrderFor) reads only from
  // a number.
  if (conv.has_value()) fillN("converged", *conv);

  if (clog) return;  // no ordered-name contract claimed for cloglog
  // `: e(scalars)` / `: e(macros)` (SessionState::ENameContract): Stata's
  // ordered lists of a fresh fit (the bank's epilogue enumerates them), claimed
  // only for the names this path posts; eNameOrderFor re-checks on every read
  // that each listed name resolves and that no posted name is unlisted, so a
  // list that does not match the live e() keeps the r(111) refusal. The order is
  // the fresh fit's only: a later lincom, margins, `estimates store`, ... that
  // Stata runs by holding the estimates withdraws it (dispatcher.cpp
  // applyEOrderRewrite). e(matrices) is not claimed: its members (e(rules),
  // e(b), e(V), e(mns), e(Cns), e(ilog), e(gradient)) depend on what each fit
  // could post above.
  const bool cluster = !session.eval.eStrings.get("clustvar").empty();
  const bool weighted = !session.eval.eStrings.get("wtype").empty();
  const bool hasLl0 = ll0.has_value() && std::isfinite(*ll0);
  std::vector<std::string> S = {"rank", "N",        "ic",        "k",         "k_eq", "noconstant",
                                "consonly", "k_dv", "converged", "rc",        "k_autoCns"};
  if (cluster) S.push_back("N_clust");
  S.push_back("ll");
  S.push_back("k_eq_model");
  if (hasLl0) S.push_back("ll_0");
  for (const char* n : {"df_m", "chi2", "p"}) S.push_back(n);
  if (logitLike) {
    S.push_back("N_cdf");
    S.push_back("N_cds");
  }
  if (hasLl0 || cmd == "poisson") S.push_back("r2_p");
  std::vector<std::string> M = {"cmdline", "cmd"};
  const std::vector<std::string> wmac =
      weighted ? std::vector<std::string>{"wexp", "wtype"} : std::vector<std::string>{};
  if (cmd == "poisson") {
    for (const char* n : {"predict", "marginsderiv", "estat_cmd"}) M.push_back(n);
    if (offsetUsed) M.push_back("offset");
    M.push_back("gof");
    M.insert(M.end(), wmac.begin(), wmac.end());
    for (const char* n : {"chi2type", "deriv_useminbound", "opt"}) M.push_back(n);
  } else {
    if (cmd == "logistic") {
      M.push_back("predict");
      M.insert(M.end(), wmac.begin(), wmac.end());
      M.push_back("estat_cmd");
    } else {
      M.push_back("estat_cmd");
      M.push_back("predict");
    }
    for (const char* n : {"marginsderiv", "marginsok", "marginsnotok", "title"}) M.push_back(n);
    if (cmd != "logistic") M.insert(M.end(), wmac.begin(), wmac.end());
    if (offsetUsed) M.push_back("offset");
    for (const char* n : {"chi2type", "deriv_useminbound", "opt"}) M.push_back(n);
  }
  if (!session.eval.eStrings.get("vcetype").empty()) M.push_back("vcetype");
  if (cluster) M.push_back("clustvar");
  M.push_back("vce");
  if (cmd == "poisson") M.push_back("title");
  for (const char* n : {"user", "crittype", "ml_method", "singularHmethod", "technique", "which",
                        "depvar", "properties"})
    M.push_back(n);
  SessionState::ENameContract c;
  c.fitSerial = lmo->fitSerial;
  c.cmd = cmd;
  // poisson's e(ic) (the iteration count) is not in the engine's reply.
  if (logitLike) c.scalars = std::move(S);
  c.macros = std::move(M);
  session.eNameContract = std::move(c);
}

}  // namespace rtable_detail

}  // namespace piet::handlers
