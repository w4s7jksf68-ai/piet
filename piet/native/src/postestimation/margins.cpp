// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Margins.
#include "piet/handlers/postestimation.h"
#include "piet/handlers/results.h"
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
#include "estimation/estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

namespace postest_detail {

// A margins refusal carrying an explicit Stata rc: coarseRc reads the `r(NNN)`
// reason before the code (BUG-187), so `display _rc` after `capture margins ...`
// is never the generic 1 on these paths.
ojson marginsRefusalM4(const std::string& message, int rc,
                       const std::string& code = "unsupported_option") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// What a served margins form knows about itself for r() (FU-179, FU-276).
// One engine `--at` spec (`v=val[,v2=val2]`, parseAtNumlistM4) with each numeric
// value replaced by float(value): Stata evaluates an at() value at float
// precision whatever the variable's storage type (w2v_margins_siblings
// FU668_at_dvar_atmeans_b/_rt, `margins, at(dvar=0.1) atmeans` on a double dvar
// is 4.921007387529768 at float(0.1), 4.33e-10 away at the double 0.1; the grid
// applies the same rule, posthest.h margins_grid). A value that is not a plain
// number goes through unchanged for the engine to judge.
std::string floatAtSpecM4(const std::string& spec) {
  std::string out;
  size_t pos = 0;
  while (pos <= spec.size()) {
    const size_t comma = spec.find(',', pos);
    std::string tok = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    const size_t eq = tok.find('=');
    if (eq != std::string::npos) {
      const std::string val = trimM4(tok.substr(eq + 1));
      char* end = nullptr;
      const double v = val.empty() ? 0.0 : std::strtod(val.c_str(), &end);
      if (!val.empty() && end != nullptr && *end == '\0' && std::isfinite(v)) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(static_cast<float>(v)));
        tok = tok.substr(0, eq + 1) + buf;
      }
    }
    out += (out.empty() && pos == 0 ? "" : ",") + tok;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

// BUG-1449: what margins' r() records about the FIT (_marg_work.class
// ._parse_vce :2717-2721 and its return list), read while e() is still the
// fit's: `post` replaces e() before r() is written on some paths. Measured on
// StataBE 19.5 (scratch-laneA/agents/p-margins s1.do, after `regress y x1 x2
// i.c`): r(est_cmd) regress, r(est_cmdline) "regress y x1 x2 i.c",
// r(model_vce) ols (robust/cluster after vce(robust)/vce(cluster id), with
// r(model_vcetype) Robust; oim after logit), r(predict1_label) "Linear
// prediction" ("Pr(d)" after `logit d ...`), r(cmdline) "margins , dydx(x1)" --
// the command word, a blank, then the rest as typed ("margins " when bare).
struct MarginsFitInfoM4 {
  bool captured = false;
  std::string estCmd, estCmdline, modelVce, modelVcetype;
  std::string predictLabel;          // "" = this fit's default prediction is not known here
  std::string cmdline;               // "" = the typed line is not known
  std::vector<std::string> ebNames;  // e(b)'s stripe: r(Jacobian)'s columns
  std::vector<double> ebValues;      // e(b) itself (the Jacobian's check)
};

struct MarginsRInfoM4 {
  bool leastSquares = false;  // a least-squares fit: t(e(df_r))-referenced, r(df_r) posted
  bool dydx = false;          // a dydx() form: r(derivatives)/r(xvars)
  std::string xvars;          // the dydx() varlist as given; "" = not known exactly (dydx(*))
  int kMargins = 0;           // terms in the marginlist (r(k_margins))
  int atPoints = 0;           // at() points, atmeans alone counting one (r(at), r(atstats#))
  bool cluster = false;       // a clustered fit (r(N_clust), r(clustvar))
  // BUG-1449: the rest of Stata's set, posted when `fit` was captured (the
  // forms runMargins serves; glm/qreg2 margins keep their historical set).
  MarginsFitInfoM4 fit;
  std::string title;              // r(title); "" = not known
  int kAt = 0;                    // r(k_at): at() options, atmeans alone counting one
  int kBy = 1;                    // r(k_by): over() groups
  int continuous = -1;            // r(continuous): 1 every dydx() variable continuous, 0 not, -1 unknown
  std::string bRow = "r1";        // r(b)'s row name
  std::vector<std::vector<double>> jacobian;  // r(Jacobian) rows over fit.ebNames; empty = unknown
  bool nKnown = false;            // r(_N): r(N) per column, 0 for a dydx() base level
};

// _marg_work.class :900-913 titles the results by `.noasobs` (:4001-4016): no
// covariate is left at its observed values -- atmeans, or at() fixing every
// covariate of e(b), the marginlist factor counting as fixed (StataBE 19.5,
// s1.do/s2.do: `margins, atmeans` and at(x1=1 x2=2 c=1) "Adjusted predictions",
// at(x1=(2 4)) and `margins c` "Predictive margins", `dydx(x1) atmeans`
// "Conditional marginal effects", `dydx(x1) at(x2=1)` "Average marginal
// effects"). "" when e(b) names a term that is not read here.
std::string marginsTitleM4(bool dydx, bool atmeans, const std::string& atText,
                           const std::string& marginVar, const std::vector<std::string>& ebNames) {
  bool noAsObs = atmeans;
  if (!noAsObs && !trimM4(atText).empty()) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(atText);
    if (p.rc != 0 || ebNames.empty()) return std::string();
    std::set<std::string> fixed(p.vars.begin(), p.vars.end());
    if (!marginVar.empty()) fixed.insert(marginVar);
    bool all = true;
    for (const auto& nm : ebNames) {
      const size_t colon = nm.find(':');
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(colon == std::string::npos ? nm : nm.substr(colon + 1), &ok))
        all = all && fixed.count(a.var) > 0;
      if (!ok) return std::string();
    }
    noAsObs = all;
  }
  if (dydx) return noAsObs ? "Conditional marginal effects" : "Average marginal effects";
  return noAsObs ? "Adjusted predictions" : "Predictive margins";
}

// The names Stata posts in r() for THIS margins invocation (RULING.md C.3/D.3;
// margins's help), each measured on live StataBE 19.5 (2026-10-13,
// `return list` after `margins`, `margins t`, `margins, dydx(x1)`, `margins,
// at(x1=5)`, `margins, atmeans` after `regress y x1 x2 i.t`, and `margins,
// dydx(x1)` after `logit d x1 x2`). Declared whether or not PIET posts each one:
// rTableAfterCommand turns declared-but-absent into a named r(111), never `.`.
// The cluster pair is the help file's, not measured: declaring it only makes an
// unposted read refuse.
std::vector<std::string> marginsDeclaredRM4(const MarginsRInfoM4& info) {
  std::vector<std::string> names = {
      "N",          "level",      "k_at",          "k_by",        "k_predict", "k_margins",
      "cmd",        "cmdline",    "est_cmdline",   "est_cmd",     "emptycells", "mcmethod",
      "predict1_label", "expression", "vcetype",   "vce",         "model_vce", "title",
      "table",      "chainrule",  "V",             "Jacobian",    "error",     "b",
      "_N"};
  if (info.leastSquares) names.push_back("df_r");  // posted after regress, absent after logit
  if (info.kMargins > 0) names.push_back("margins");
  if (info.dydx) {
    // r(continuous) is absent when a dydx() variable is a factor (s1.do: dydx(c),
    // dydx(x1 c), dydx(*) after `regress y x1 x2 i.c`).
    if (info.continuous != 0) names.push_back("continuous");
    names.push_back("derivatives");
    names.push_back("xvars");
  }
  // After vce(robust) / vce(cluster) (s1.do J/K); not posted when e(vcetype) is empty.
  if (!info.fit.modelVcetype.empty()) names.push_back("model_vcetype");
  if (info.atPoints > 0) {
    names.push_back("at");
    for (int i = 1; i <= info.atPoints; ++i) names.push_back("atstats" + std::to_string(i));
  }
  if (info.cluster) {
    names.push_back("N_clust");
    names.push_back("clustvar");
  }
  return names;
}

// FU-179/FU-276: margins is r-class. Every served form replaces r() wholesale --
// a stale r() from the previous command must not answer for it -- posts the
// results this handler knows exactly, and declares Stata's set for the
// invocation (marginsDeclaredRM4). Posted: r(N), r(level) (margins takes no
// level(); `set level`), r(k_margins), r(df_r) after a least-squares fit (its
// e(df_r): StataBE `margins` after `regress` posts 52 = e(df_r)), r(cmd),
// r(derivatives)/r(xvars) for dydx(), and -- when `names` is non-empty -- r(b)
// (1 x k, row `r1`), r(V) (k x k) and r(table) (9 x k; posted only when the
// printed p-values and intervals reproduce at the declared level and reference,
// handlers::buildRTableFromBlock's own check). An off-diagonal element no engine
// path computed arrives as NaN (Stata's missing), never 0: a zero covariance
// would be a number nobody measured.
void postMarginsRClassM4(SessionState& session, const ojson& block, const MarginsRInfoM4& info,
                         const std::vector<std::string>& names, const std::vector<double>& b,
                         const std::vector<std::vector<double>>& V,
                         const ojson& tableRows = ojson::array()) {
  auto& ev = session.eval;
  // e(df_r) is the fit's (the key macros.cpp's resolveEScalar reads off the
  // model result, else a posted e-scalar): read before any `post` replaces e().
  double dfR = std::numeric_limits<double>::quiet_NaN();
  if (info.leastSquares) {
    const auto& lmNow = session.lastModel();
    if (lmNow.has_value() && lmNow->result.has_value() && lmNow->result->is_object() &&
        lmNow->result->contains("df_r") && (*lmNow->result)["df_r"].is_number()) {
      dfR = (*lmNow->result)["df_r"].get<double>();
    } else if (const double* dfrE = ev.eScalars.find("df_r")) {
      dfR = *dfrE;
    }
    if (!(dfR > 0)) dfR = std::numeric_limits<double>::quiet_NaN();
  }
  eval::replaceRClass(ev);
  if (block.contains("n_obs") && block["n_obs"].is_number()) {
    ev.rValues.set("N", block["n_obs"].get<double>());
  }
  ev.rValues.set("level", static_cast<double>(session.level));
  ev.rValues.set("k_margins", static_cast<double>(info.kMargins));
  if (std::isfinite(dfR)) ev.rValues.set("df_r", dfR);
  ev.rStrings.set("cmd", "margins");
  if (info.dydx) {
    ev.rStrings.set("derivatives", "dy/dx");
    if (!info.xvars.empty()) ev.rStrings.set("xvars", info.xvars);
  }
  // BUG-1449: the rest of the set (s1.do return lists). The method macros are
  // constants of every form served here: the delta method (no vce()), no
  // multiple-comparison adjustment, emptycells(strict), the default prediction.
  if (info.fit.captured) {
    ev.rValues.set("k_at", static_cast<double>(info.kAt));
    ev.rValues.set("k_by", static_cast<double>(info.kBy));
    ev.rValues.set("k_predict", 1.0);
    ev.rStrings.set("vce", "delta");
    ev.rStrings.set("vcetype", "Delta-method");
    ev.rStrings.set("expression", "predict()");
    ev.rStrings.set("mcmethod", "noadjust");
    ev.rStrings.set("emptycells", "strict");
    if (!info.title.empty()) ev.rStrings.set("title", info.title);
    if (!info.fit.estCmd.empty()) ev.rStrings.set("est_cmd", info.fit.estCmd);
    if (!info.fit.estCmdline.empty()) ev.rStrings.set("est_cmdline", info.fit.estCmdline);
    if (!info.fit.modelVce.empty()) ev.rStrings.set("model_vce", info.fit.modelVce);
    if (!info.fit.modelVcetype.empty()) ev.rStrings.set("model_vcetype", info.fit.modelVcetype);
    if (!info.fit.predictLabel.empty()) ev.rStrings.set("predict1_label", info.fit.predictLabel);
    if (!info.fit.cmdline.empty()) ev.rStrings.set("cmdline", info.fit.cmdline);
    if (info.dydx && info.continuous == 1) ev.rStrings.set("continuous", "continuous");
  }
  if (!names.empty() && b.size() == names.size() && V.size() == names.size()) {
    eval::EvalMatrix rb;
    rb.rownames = {info.bRow};
    rb.colnames = names;
    rb.data.push_back(b);
    eval::EvalMatrix rv;
    rv.rownames = names;
    rv.colnames = names;
    rv.data = V;
    ev.rMatrices.emplace_back("b", std::move(rb));
    ev.rMatrices.emplace_back("V", std::move(rv));
    if (tableRows.is_array() && tableRows.size() == names.size()) {
      ojson tb;
      tb["estimator"] = "margins_rtable";
      if (std::isfinite(dfR)) tb["df_r"] = dfR;
      tb["coeftable"] = tableRows;
      (void)postRTableFromBlock(session, tb, static_cast<double>(session.level));
    }
    // BUG-1449 matrices (s1.do/s2.do): r(error) 0 per column (every column was
    // computed); r(_N) the observations behind each column, 0 for a dydx() base
    // level (`x1 0b.c 1.c ...` -> 60 0 60 ...); r(chainrule) 1 x 3 (1, k, k) for
    // a single-equation e(b) whose last column is _cons (7 columns after `regress
    // y x1 x2 i.c`, 3 after `logit d x1 x2`; absent after a noconstant fit);
    // r(Jacobian) d(column)/d(e(b)), one row per column over e(b)'s stripe.
    if (info.fit.captured) {
      auto row1 = [](const std::vector<std::string>& cols, std::vector<double> vals) {
        eval::EvalMatrix m;
        m.rownames = {"r1"};
        m.colnames = cols;
        m.data.push_back(std::move(vals));
        return m;
      };
      ev.rMatrices.emplace_back("error", row1(names, std::vector<double>(names.size(), 0.0)));
      if (info.nKnown && block.contains("n_obs") && block["n_obs"].is_number()) {
        const double n = block["n_obs"].get<double>();
        std::vector<double> nv;
        // A dydx() column with no at() point is r(_N)'s equation over `_cons`
        // (s1.do `matrix list r(_N)` after dydx(x1 c): `x1:_cons 0b.c:_cons ...`).
        std::vector<std::string> ncols = names;
        if (info.dydx)
          for (auto& nm : ncols)
            if (nm.find(':') == std::string::npos) nm += ":_cons";
        for (const auto& nm : names) {
          // A dydx() base level: the head's level flags carry `b` but not `bn`.
          const std::string head = nm.substr(0, nm.find(':'));
          const size_t dot = head.find('.');
          const std::string flags = dot == std::string::npos ? std::string() : head.substr(0, dot);
          const bool base = info.dydx && flags.find('b') != std::string::npos &&
                            flags.find("bn") == std::string::npos;
          nv.push_back(base ? 0.0 : n);
        }
        ev.rMatrices.emplace_back("_N", row1(ncols, std::move(nv)));
      }
      const std::vector<std::string>& eb = info.fit.ebNames;
      auto eqOf = [](const std::string& s) {
        const size_t c = s.find(':');
        return c == std::string::npos ? std::string() : s.substr(0, c);
      };
      const bool oneEq = !eb.empty() && std::all_of(eb.begin(), eb.end(), [&](const std::string& s) {
                           return eqOf(s) == eqOf(eb.front());
                         });
      if (oneEq && eb.back().substr(eb.back().find(':') == std::string::npos ? 0 : eb.back().find(':') + 1) ==
                       "_cons") {
        const double k = static_cast<double>(eb.size());
        ev.rMatrices.emplace_back("chainrule", row1({"c1", "c2", "c3"}, {1.0, k, k}));
      }
      if (!eb.empty() && info.jacobian.size() == names.size() &&
          std::all_of(info.jacobian.begin(), info.jacobian.end(),
                      [&eb](const std::vector<double>& r) { return r.size() == eb.size(); })) {
        eval::EvalMatrix jm;
        jm.rownames = names;
        jm.colnames = eb;
        jm.data = info.jacobian;
        ev.rMatrices.emplace_back("Jacobian", std::move(jm));
      }
    }
  }
  eval::declareRPosted(ev, "margins", marginsDeclaredRM4(info));
}

// BUG-1449: the fit's half of margins' r() set (MarginsFitInfoM4), captured on
// entry. r(predict1_label) is posted for the default predictions measured (s1.do,
// s3.do): the linear prediction after the least-squares family and `xtpoisson,
// fe`, Pr(depvar) after logit/probit, "Predicted number of events" after poisson.
MarginsFitInfoM4 marginsFitInfoM4(SessionState& session) {
  MarginsFitInfoM4 f;
  f.captured = true;
  const auto& ev = session.eval;
  if (const std::string* s = ev.eStrings.find("cmd")) f.estCmd = *s;
  if (const std::string* s = ev.eStrings.find("cmdline")) f.estCmdline = *s;
  if (const std::string* s = ev.eStrings.find("vce")) f.modelVce = *s;
  if (const std::string* s = ev.eStrings.find("vcetype")) f.modelVcetype = *s;
  const auto& lm = session.lastModel();
  if (lm.has_value()) {
    if (postestOlsCmdsM4().count(lm->cobraCmd) || lm->cobraCmd == "xtpois")
      f.predictLabel = "Linear prediction";
    else if ((lm->cobraCmd == "logit" || lm->cobraCmd == "probit" || lm->cobraCmd == "cloglog") &&
             !lm->depvar.empty())
      f.predictLabel = "Pr(" + lm->depvar + ")";  // cloglog: agents/post/cl.do MD "Pr(yb)"
    else if (lm->cobraCmd == "poisson")
      f.predictLabel = "Predicted number of events";
    else if (lm->cobraCmd == "xtlogit" && !lm->depvar.empty())
      f.predictLabel = "Pr(" + lm->depvar + "|fixed effect is 0)";  // clogit_p.ado's pu0
  }
  // margins' `0` with its leading blanks dropped, after "margins " (the leaf's
  // macro-expanded line, as the fit hook's e(cmdline) reads it).
  const std::string& s = session.leafCmdExpanded;
  const size_t i = s.find_first_not_of(" \t");
  if (i != std::string::npos && s.compare(i, 7, "margins") == 0 &&
      (i + 7 == s.size() || s[i + 7] == ' ' || s[i + 7] == '\t' || s[i + 7] == ',')) {
    const size_t k = s.find_first_not_of(" \t", i + 7);
    f.cmdline = "margins " + (k == std::string::npos ? std::string() : s.substr(k));
  }
  const eval::EvalSession::EMatrixAnswer eb =
      session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  if (eb.matrix.has_value() && eb.matrix->data.size() == 1 &&
      eb.matrix->data[0].size() == eb.matrix->colnames.size()) {
    f.ebNames = eb.matrix->colnames;
    f.ebValues = eb.matrix->data[0];
  }
  return f;
}

double marginsNumM4(const ojson& row, const char* key) {
  return (row.contains(key) && row[key].is_number()) ? row[key].get<double>()
                                                     : std::numeric_limits<double>::quiet_NaN();
}

// k x k with se^2 on the diagonal and NaN elsewhere (see postMarginsRClassM4).
std::vector<std::vector<double>> marginsDiagVM4(const std::vector<double>& se) {
  const size_t k = se.size();
  std::vector<std::vector<double>> V(k, std::vector<double>(k, std::numeric_limits<double>::quiet_NaN()));
  for (size_t i = 0; i < k; ++i) V[i][i] = se[i] * se[i];
  return V;
}

// r() from a margins block: its coeftable (the dydx forms and the per-level
// predictive margins), else the engine's predictive `margins` array (the at()
// forms; one point is Stata's `_cons` column, several are `#._at`). `V` is the
// engine's full covariance across points when it sent one of the right size.
// `levelBn`: the coeftable rows are the levels of a marginlist factor, and r()'s
// first column carries `bn` (BUG-1240, StataBE 19.5 `margins c`: `0bn.c 1.c 2.c
// 3.c`; after `ib2.c` still `0bn.c`, the first level, not the base) while the
// table keeps the plain level.
void postMarginsFromBlockM4(SessionState& session, const ojson& block, const ojson& raw,
                            const MarginsRInfoM4& info, bool levelBn = false) {
  std::vector<std::string> names;
  std::vector<double> b, se;
  ojson rows = ojson::array();
  auto tableRow = [](const std::string& name, const ojson& src, const char* bKey) {
    ojson r;
    r["name"] = name;
    for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
             {bKey, "b"}, {"se", "se"}, {"t", "t"}, {"z", "z"}, {"p", "p"}, {"ci_lo", "ci_lo"},
             {"ci_hi", "ci_hi"}}) {
      if (src.contains(from)) r[to] = src[from];
    }
    return r;
  };
  if (block.contains("coeftable") && block["coeftable"].is_array()) {
    for (const auto& row : block["coeftable"]) {
      names.push_back(row.value("name", std::string()));
      const size_t dot = names.back().find('.');
      if (levelBn && names.size() == 1 && dot != std::string::npos) names.back().insert(dot, "bn");
      b.push_back(marginsNumM4(row, "b"));
      se.push_back(marginsNumM4(row, "se"));
      rows.push_back(tableRow(names.back(), row, "b"));
    }
  } else if (block.contains("margins") && block["margins"].is_array()) {
    const auto& arr = block["margins"];
    for (size_t i = 0; i < arr.size(); ++i) {
      // Stata's colfullnames: the first point is `1bn._at` (w2v_margins_siblings
      // F_range_numlist_lg_cols `1bn._at 2._at 3._at`).
      names.push_back(arr.size() == 1 ? std::string("_cons")
                                      : std::to_string(i + 1) + (i == 0 ? "bn" : "") + "._at");
      b.push_back(marginsNumM4(arr[i], "margin"));
      se.push_back(marginsNumM4(arr[i], "se"));
      rows.push_back(tableRow(names.back(), arr[i], "margin"));
    }
  }
  std::vector<std::vector<double>> V = marginsDiagVM4(se);
  if (raw.is_object() && raw.contains("V") && raw["V"].is_array() && raw["V"].size() == names.size()) {
    bool ok = true;
    std::vector<std::vector<double>> full;
    for (const auto& vr : raw["V"]) {
      if (!vr.is_array() || vr.size() != names.size()) {
        ok = false;
        break;
      }
      std::vector<double> row;
      for (const auto& x : vr) row.push_back(x.is_number() ? x.get<double>()
                                                           : std::numeric_limits<double>::quiet_NaN());
      full.push_back(std::move(row));
    }
    if (ok) V = std::move(full);
  }
  postMarginsRClassM4(session, block, info, names, b, V, rows);
}

// FU-94: how the marginlist term `var` enters the fitted model. lm.x carries the
// fit's own term spellings (`x1`, `i.t`, `ib2.t`, `i.t#c.x1`, `c.x1`).
enum class MarginsTermM4 { Absent, Continuous, Factor, Other };
MarginsTermM4 marginsTermRoleM4(const LastModel& lm, const std::string& var) {
  bool factor = false, continuous = false, other = false;
  for (const std::string& term : lm.x) {
    const bool interaction = term.find('#') != std::string::npos;
    size_t pos = 0;
    while (pos <= term.size()) {
      const size_t hash = term.find('#', pos);
      const std::string seg =
          term.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.rfind('.');
      const std::string op = dot == std::string::npos ? std::string() : seg.substr(0, dot);
      const std::string name = dot == std::string::npos ? seg : seg.substr(dot + 1);
      if (name == var) {
        if (interaction) {
          other = true;
        } else if (op.empty() || op == "c") {
          continuous = true;
        } else if (op[0] == 'i') {
          factor = true;
        } else {
          other = true;  // a level spelling (`2.t`) or a time-series operator
        }
      }
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
  }
  if (other) return MarginsTermM4::Other;
  if (factor) return MarginsTermM4::Factor;
  if (continuous) return MarginsTermM4::Continuous;
  return MarginsTermM4::Absent;
}

// FU-342: does `var` appear inside a `#` term of the fit -- in its own term
// spellings (lm.x: `c.x1##c.x1`, `i.t#c.x1`) or in its coefficient names
// (`x1#x1`, `2.t#c.x1`)? Each `#` segment is compared after its operator
// prefix (`c.`, `i.`, `2.`, `ib2.`), so `x1` never matches `x10`.
bool marginsVarInInteractionM4(const LastModel& lm, const std::string& var) {
  auto mentions = [&var](const std::string& term) {
    if (term.find('#') == std::string::npos) return false;
    size_t pos = 0;
    while (pos <= term.size()) {
      const size_t hash = term.find('#', pos);
      const std::string seg =
          term.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.rfind('.');
      if ((dot == std::string::npos ? seg : seg.substr(dot + 1)) == var) return true;
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
    return false;
  };
  for (const std::string& term : lm.x) {
    if (mentions(term)) return true;
  }
  if (lm.result.has_value() && lm.result->contains("coeftable") &&
      (*lm.result)["coeftable"].is_array()) {
    for (const auto& row : (*lm.result)["coeftable"]) {
      if (row.contains("name") && row["name"].is_string() &&
          mentions(row["name"].get<std::string>())) {
        return true;
      }
    }
  }
  return false;
}

// BUG-1502: margins reads every dydx() and at() name against the fit's
// covariates before anything is computed, and a name that is none of them (not
// in the data, or in the data but not in the model) is r(322) after regress,
// logit, probit and poisson alike (StataBE 19.5, scratch-laneA/agents/file-new-1/
// i11.do and rlife/m1s.do): "invalid dydx() option;" / "nosuchvar not found in
// list of covariates", "invalid at() option;" / "variable nosuchvar not found in
// list of covariates". The covariates are the fit's term spellings (lm.x), its
// absorbed variables and its coefficient names, each `#` segment read after its
// operator and `eq:` prefixes. A keyword (`_all`), a wildcard, an operated name
// or one that abbreviates a covariate is left to the paths below. Only the
// one-equation fits measured (the least-squares family, logit, probit, poisson)
// are checked: a multi-equation fit's other equations (heckman's selection) are
// not in its coefficient table, and a fit whose table is not in memory refuses
// nothing here.
std::optional<ojson> marginsCovariateRefusalM4(const LastModel& lm,
                                               const std::optional<std::string>& dydxVal,
                                               const std::optional<std::string>& atVal) {
  if (!postestOlsCmdsM4().count(lm.cobraCmd) && !postestGlmCmdsM4().count(lm.cobraCmd))
    return std::nullopt;
  if (!lm.result.has_value() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array() || (*lm.result)["coeftable"].empty())
    return std::nullopt;
  std::vector<std::string> covariates;
  auto addTerm = [&covariates](std::string term) {
    const size_t colon = term.rfind(':');
    if (colon != std::string::npos) term = term.substr(colon + 1);
    size_t pos = 0;
    while (pos <= term.size()) {
      const size_t hash = term.find('#', pos);
      const std::string seg =
          term.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.rfind('.');
      covariates.push_back(dot == std::string::npos ? seg : seg.substr(dot + 1));
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
  };
  for (const std::string& term : lm.x) addTerm(term);
  for (const std::string& term : lm.fe) addTerm(term);
  for (const auto& row : (*lm.result)["coeftable"])
    if (row.contains("name") && row["name"].is_string()) addTerm(row["name"].get<std::string>());
  auto judgedBelow = [&covariates](const std::string& name) {
    if (name.empty() || name[0] == '_' || !strutil::isValidStataIdentifier(name)) return true;
    for (const std::string& c : covariates)
      if (c.compare(0, name.size(), name) == 0) return true;  // the covariate or its abbreviation
    return false;
  };
  if (dydxVal.has_value()) {
    for (std::string v : splitTokensM4(*dydxVal)) {
      if (v.rfind("i.", 0) == 0 || v.rfind("c.", 0) == 0) v = v.substr(2);
      if (!judgedBelow(v))
        return marginsRefusalM4("invalid dydx() option;\n" + v + " not found in list of covariates",
                                322, "engine_error");
    }
  }
  if (atVal.has_value() && !trimM4(*atVal).empty()) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
    if (p.rc == 0)
      for (const std::string& v : p.vars)
        if (!judgedBelow(v))
          return marginsRefusalM4(
              "invalid at() option;\nvariable " + v + " not found in list of covariates", 322,
              "engine_error");
  }
  return std::nullopt;
}

// BUG-1449: r(Jacobian) of a least-squares predictive margin, d(margin)/d(e(b)):
// the e(sample) mean of each e(b) column, a marginlist factor's columns set to
// the level of each row (StataBE 19.5 s1.do after `regress y x1 x2 i.c`:
// `margins` 5.25 2.31 0 .25 .25 .25 1; `margins c`, level 1: 5.25 2.31 0 1 0 0
// 1). A base or omitted column is 0. Each row must reproduce its margin (J b),
// or nothing is returned.
bool marginsLinearJacobianM4(SessionState& session, const MarginsFitInfoM4& fit,
                             const std::string& factorVar, const std::vector<double>& levels,
                             const std::vector<double>& margins,
                             std::vector<std::vector<double>>& J) {
  const std::vector<std::string>& eb = fit.ebNames;
  if (eb.empty() || fit.ebValues.size() != eb.size() || margins.size() != levels.size()) return false;
  struct Atom {
    const double* v = nullptr;
    bool factor = false;
    double level = 0;
    bool fixed = false;  // the marginlist factor: set per row
  };
  struct Col {
    bool constant = false, zero = false;
    std::vector<Atom> atoms;
  };
  Datastore* ds = nullptr;
  std::vector<int64_t> rows;
  try {
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind != SessionState::EsampleReading::Kind::kColumn) return false;
    ds = session.datastore();
    if (ds == nullptr || !ds->has(kEsampleColumn) || ds->col(kEsampleColumn).f64() == nullptr) return false;
    const double* m = ds->col(kEsampleColumn).f64();
    for (int64_t i = 0; i < ds->nRows(); ++i)
      if (m[i] == static_cast<double>(er.serial)) rows.push_back(i);
  } catch (const std::exception&) {
    return false;
  }
  if (rows.empty()) return false;
  std::vector<Col> cols;
  for (const std::string& nm : eb) {
    Col c;
    if (nm.find(':') != std::string::npos) return false;
    if (nm == "_cons") {
      c.constant = true;
      cols.push_back(std::move(c));
      continue;
    }
    size_t pos = 0;
    bool mentionsFactor = false;
    while (true) {
      const size_t hash = nm.find('#', pos);
      const std::string seg = nm.substr(pos, hash == std::string::npos ? std::string::npos : hash - pos);
      const size_t dot = seg.find('.');
      const std::string op = dot == std::string::npos ? std::string() : seg.substr(0, dot);
      const std::string var = dot == std::string::npos ? seg : seg.substr(dot + 1);
      size_t d = 0;
      while (d < op.size() && std::isdigit(static_cast<unsigned char>(op[d]))) ++d;
      const std::string flags = op.substr(d);
      Atom a;
      if (d > 0) {
        if (flags.find_first_not_of("bno") != std::string::npos) return false;
        a.factor = true;
        a.level = std::stod(op.substr(0, d));
        if (flags.find('o') != std::string::npos ||
            (flags.find('b') != std::string::npos && flags.find("bn") == std::string::npos))
          c.zero = true;
      } else if (op == "o" || op == "co" || op == "oc") {
        c.zero = true;
      } else if (!(op.empty() || op == "c")) {
        return false;  // a time-series operator: not a column read here
      }
      if (var.empty() || !ds->has(var) || ds->col(var).f64() == nullptr) return false;
      a.v = ds->col(var).f64();
      a.fixed = !factorVar.empty() && var == factorVar;
      if (a.fixed) {
        if (!a.factor) return false;
        mentionsFactor = true;
      }
      c.atoms.push_back(a);
      if (hash == std::string::npos) break;
      pos = hash + 1;
    }
    // The engine's per-level path refuses a term derived from the factor.
    if (mentionsFactor && c.atoms.size() > 1) return false;
    cols.push_back(std::move(c));
  }
  J.assign(levels.size(), std::vector<double>(eb.size(), 0.0));
  for (size_t li = 0; li < levels.size(); ++li) {
    double fitted = 0.0, scale = 0.0;
    for (size_t j = 0; j < cols.size(); ++j) {
      const Col& c = cols[j];
      double mean = 0.0;
      if (c.constant) {
        mean = 1.0;
      } else if (!c.zero) {
        double s = 0.0;
        for (int64_t r : rows) {
          double v = 1.0;
          for (const Atom& a : c.atoms) {
            const double x = a.fixed ? levels[li] : a.v[r];
            v *= a.factor ? (x == a.level ? 1.0 : 0.0) : x;
          }
          s += v;
        }
        mean = s / static_cast<double>(rows.size());
      }
      J[li][j] = mean;
      fitted += mean * fit.ebValues[j];
      scale += std::fabs(mean * fit.ebValues[j]);
    }
    if (!std::isfinite(fitted) || std::fabs(fitted - margins[li]) > 1e-9 * std::max(1.0, scale)) return false;
  }
  return true;
}

// BUG-1549: margins takes e(df_r) only when every prediction is `xb`, which the
// default prediction is when the fit's predict program declares `default_xb`
// (_marg_work.class; regres_p and areg_p do). reghdfe_p and xtreg's xtrefe_p do
// not, so after `reghdfe` (absorbed or not) and `xtreg, fe` every margins form is
// normal-referenced: no r(df_r), r(table)'s df `.` and crit 1.959964 (StataBE
// 19.5, p-margins/s4.do; scratch-laneA2/agents/post/m1.do, m2.do: `margins,
// dydx(x1)` z 45.07, interval 1.891077 2.063039, where after areg t(48) and
// 1.888854 2.065262). marginsGridFitM4's reference (t after regress and areg
// only) agrees.
bool marginsNormalRefM4(const LastModel& lm) {
  return lm.stataCmd == "reghdfe" || lm.stataCmd == "xtreg";
}

// A t-referenced margins row re-referenced to the normal, runMarginsGrid's rule:
// z = b/se, p = 2*normal(-|z|), the interval b -+ 1.959964*se, `t` mirroring `z`.
// A row with no positive se keeps its missing statistics.
void marginsNormalRowM4(ojson& row) {
  const double b = marginsNumM4(row, "b"), se = marginsNumM4(row, "se");
  if (!std::isfinite(b) || !(se > 0)) return;
  const double z = b / se, crit = cecon::z_critical(0.05);
  row["z"] = z;
  row["t"] = z;
  row["p"] = std::erfc(std::fabs(z) / std::sqrt(2.0));
  row["ci_lo"] = b - crit * se;
  row["ci_hi"] = b + crit * se;
}

// The predictive margins of a least-squares fit without absorbed effects:
// `marginVar` empty = Stata's bare `margins` (one `_cons` row, the mean
// prediction over e(sample)), else one row per level of that factor. The engine's
// linear branch refits the fit's own y/x/vce (the caller has gated the family),
// and `--atmeans` holds the other design columns at their float-stored means.
ojson runLinearPredictiveMarginsM4(const LastModel& lm, SessionState& session,
                                   std::vector<std::string> args, const std::string& marginVar,
                                   bool atmeans, bool postOpt, const MarginsFitInfoM4& fitInfo) {
  if (marginVar.empty()) {
    args.push_back("--at");
    args.push_back("");
  } else {
    args.push_back("--marginlist");
    args.push_back(marginVar);
  }
  if (atmeans) args.push_back("--atmeans");
  if (!lm.vcov.empty() && lm.vcov != "iid") {
    args.push_back("--vcov");
    args.push_back(lm.vcov);
  }
  if (!lm.cluster.empty()) {
    args.push_back("--cluster");
    args.push_back(joinCommaM4(lm.cluster));
  }
  // FU-385, as the at() path: a two-way regress refits under the fit's own
  // multiway convention, not reghdfe's Gmin factor.
  for (const auto& f : fitConventionFlagsM4(lm))
    if (f == "--multiway-subset-factors" || f == "--keep-singletons" ||
        f == "--collinear-forward")
      args.push_back(f);
  args.push_back("--tidy");
  ojson raw;
  try {
    raw = runCobraJsonM4(args);
  } catch (const CobraError& ce) {
    // The engine's own refusals of these forms (a level spelling it cannot map,
    // a factor the design does not carry) are named, with an explicit rc.
    if (ce.code == "bad_input" || ce.code == "unsupported") {
      return marginsRefusalM4(cobraErrorStrM4(ce), 198);
    }
    return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
  }
  ojson ct = ojson::array();
  if (raw.contains("margins") && raw["margins"].is_array()) {
    for (const auto& m : raw["margins"]) {
      ojson row;
      row["name"] = marginVar.empty() ? std::string("_cons") : m.value("at", std::string());
      for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
               {"margin", "b"}, {"se", "se"}, {"t", "t"}, {"p", "p"}, {"ci_lo", "ci_lo"},
               {"ci_hi", "ci_hi"}}) {
        row[to] = m.contains(from) ? m[from] : ojson(nullptr);
      }
      if (marginsNormalRefM4(lm)) marginsNormalRowM4(row);  // BUG-1549: after reghdfe
      ct.push_back(std::move(row));
    }
  }
  ojson shaped;
  shaped["estimator"] = "margins";
  shaped["n_obs"] = raw.value("n_obs", 0);
  if (raw.contains("df")) shaped["df"] = raw["df"];
  shaped["coeftable"] = ct;
  ojson est = cobraToEstimationM4(shaped, "margins_cmd", &session);
  session.lastMargins = raw;
  session.lastMarginsDydx.clear();
  MarginsRInfoM4 info;
  info.leastSquares = !marginsNormalRefM4(lm);
  info.kMargins = marginVar.empty() ? 0 : 1;
  info.atPoints = atmeans ? 1 : 0;  // StataBE `margins, atmeans`: r(k_at) 1, r(atstats1)
  info.cluster = !lm.cluster.empty();
  info.fit = fitInfo;
  info.kAt = atmeans ? 1 : 0;
  info.title = marginsTitleM4(false, atmeans, std::string(), marginVar, fitInfo.ebNames);
  // Under atmeans StataBE names r(_N)'s column `c1` (s2.do B_am_N), and the
  // Jacobian is taken at the float means: neither is posted there.
  if (!atmeans && raw.contains("margins") && raw["margins"].is_array()) {
    info.nKnown = true;
    std::vector<double> levels, mg;
    bool ok = true;
    for (const auto& m : raw["margins"]) {
      mg.push_back(marginsNumM4(m, "margin"));
      if (marginVar.empty()) {
        levels.push_back(0.0);
        continue;
      }
      const std::string at = m.value("at", std::string());  // `<level>.<var>`
      const std::string lvText = at.substr(0, at.find('.'));
      char* end = nullptr;
      const double lv = std::strtod(lvText.c_str(), &end);
      ok = ok && !lvText.empty() && at.find('.') != std::string::npos && end != nullptr && *end == '\0';
      levels.push_back(lv);
    }
    if (ok && !marginsLinearJacobianM4(session, fitInfo, marginVar, levels, mg, info.jacobian))
      info.jacobian.clear();
  }
  postMarginsFromBlockM4(session, est, raw, info, !marginVar.empty());
  if (postOpt) postEstimationBlockM4(session, est, "margins", lm.depvar, lm.dataPath);
  return est;
}

// ---------------------------------------------------------------------------
// margins over at() points, a marginlist factor and atmeans, from the fit's own
// e(b)/e(V) (cecon::margins_grid, src/estimators/posthest.h). Oracle:
// tests/eval/golden/aer_w2/w2v_margins_corpus.log. The engine's margins refit
// sets the at() variable's own column and keeps every derived column (`z#x`,
// `x#x`) at its observed values, cannot carry the fit's weight, and has no
// response-scale path after a nonlinear fit; the grid rebuilds every column
// from the covariates at each point and takes V = G e(V) G' over the fit's own
// covariance.

// Stata's full column name -> the coefficient name `_b[]` reads: the `bn` and
// `o` flags dropped (`x:1bn._at` -> `x:1._at`, `1.tt:0bn.dcat` -> `1.tt:0.dcat`,
// `0b.tt:2o._at` -> `0b.tt:2._at`); a base flag `b` stays (`0b.treatment`,
// F5_dydx_post_bname2).
std::string marginsCoefNameM4(const std::string& full) {
  std::string out;
  size_t i = 0;
  while (i < full.size()) {
    size_t j = i;
    while (j < full.size() && std::isdigit(static_cast<unsigned char>(full[j]))) ++j;
    const bool levelStart = j > i && (i == 0 || full[i - 1] == ':' || full[i - 1] == '#');
    if (!levelStart) {
      out += full[i++];
      continue;
    }
    out.append(full, i, j - i);
    size_t k = j;
    std::string flags;
    while (k < full.size() && std::isalpha(static_cast<unsigned char>(full[k]))) flags += full[k++];
    if (k < full.size() && full[k] == '.') {
      const size_t bn = flags.find("bn");
      if (bn != std::string::npos) flags.erase(bn, 2);
      flags.erase(std::remove(flags.begin(), flags.end(), 'o'), flags.end());
      out += flags;
    } else {
      out.append(full, j, k - j);
    }
    i = k;
  }
  return out;
}

std::optional<ojson> pushGlmAtVceM4(std::vector<std::string>& args, const LastModel& lm,
                                    const std::string& form);  // defined below

// What the grid reads off a least-squares, logit/probit or poisson fit
// (MarginsGridFit). e(b)/e(V): the fit replayed once with `--emit-v`
// (lastModelBaseArgv, the refit test/lincom/nlcom already use; after poisson,
// whose engine fit has no --emit-v, `margins --model poisson --emit-v` under the
// fit's --vce/--cluster, the fit cmd_margins' GLM at() branches evaluate), whose
// coefficients and standard errors must reproduce the fitted table or nothing is
// returned. e(sample): the mask the fit installed on the working frame; a
// logit/probit/poisson fit carries none, so its rows are the fit frame's complete
// cases, which must count to e(N).
bool marginsGridFitM4(SessionState& session, const LastModel& lm, MarginsGridFit& fit,
                      std::string& why) {
  static const std::set<std::string> kEmitV = {"feols", "logit", "probit", "poisson"};
  if (!kEmitV.count(lm.cobraCmd) || !lm.result.has_value() || !lm.result->is_object()) {
    why = "the fit's covariance matrix is not available to margins after `" + lm.cobraCmd + "`";
    return false;
  }
  const ojson& res = *lm.result;
  std::map<std::string, std::pair<double, double>> fitted;  // name -> (b, se)
  if (res.contains("coeftable") && res["coeftable"].is_array())
    for (const auto& row : res["coeftable"])
      fitted[row.value("name", std::string())] = {marginsNumM4(row, "b"), marginsNumM4(row, "se")};
  const bool poisson = lm.cobraCmd == "poisson";
  if (poisson && (!lm.fitWeight.empty() || !lm.fitKeepif.empty() || !lm.fitDataPath.empty())) {
    why = std::string("the estimation sample of a ") +
          (lm.fitWeight.empty() ? "restricted" : "weighted") + " `poisson` fit is not recorded";
    return false;
  }
  nlohmann::json raw;
  try {
    std::vector<std::string> argv = lastModelBaseArgv(lm.cobraCmd, lm);
    if (poisson) {
      argv = {"margins", "--data", lm.dataPath, "--y", lm.y, "--x",
              lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x), "--model", "poisson"};
      if (pushGlmAtVceM4(argv, lm, "atmeans or a marginlist").has_value()) {
        why = "the fit's VCE cannot be carried to margins after `poisson`";
        return false;
      }
    } else if (lm.cobraCmd != "feols") {
      // logit/probit spell the VCE `--vce` (buildEstimationArgvM4); lm.vcov holds
      // the linear spelling (cacheLastModelM4).
      for (size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == "--vcov") {
          argv[i] = "--vce";
          if (argv[i + 1] == "hc1") argv[i + 1] = "robust";
        }
    }
    argv.push_back("--emit-v");
    argv.push_back("--tidy");
    raw = runCobraJsonM4(argv);
  } catch (const CobraError& ce) {
    why = "the fit could not be replayed for its covariance matrix (" + cobraErrorStrM4(ce) + ")";
    return false;
  }
  if (!raw.is_object() || !raw.contains("e_V") || !raw["e_V"].is_object() ||
      !raw.contains("coeftable") || !raw["coeftable"].is_array()) {
    why = "the engine's replay of the fit returned no covariance matrix";
    return false;
  }
  const auto& eV = raw["e_V"];
  std::vector<std::string> names;
  for (const auto& nm : eV.value("names", nlohmann::json::array())) names.push_back(nm.get<std::string>());
  const size_t k = names.size();
  std::map<std::string, double> bOf, seOf;
  for (const auto& row : raw["coeftable"]) {
    const std::string nm = row.value("name", std::string());
    bOf[nm] = row.contains("b") && row["b"].is_number() ? row["b"].get<double>() : 0.0;
    seOf[nm] = row.contains("se") && row["se"].is_number() ? row["se"].get<double>() : 0.0;
  }
  fit.names = names;
  fit.b.assign(k, 0.0);
  fit.V.assign(k * k, 0.0);
  const auto& mat = eV.value("matrix", nlohmann::json::array());
  if (mat.size() != k) {
    why = "the engine's covariance matrix does not match its coefficient names";
    return false;
  }
  for (size_t i = 0; i < k; ++i) {
    if (!mat[i].is_array() || mat[i].size() != k) {
      why = "the engine's covariance matrix does not match its coefficient names";
      return false;
    }
    for (size_t j = 0; j < k; ++j)
      fit.V[i * k + j] = mat[i][j].is_number() ? mat[i][j].get<double>() : 0.0;
    auto bit = bOf.find(names[i]);
    if (bit != bOf.end()) fit.b[i] = bit->second;
    // The replay must be the fit: every coefficient the fitted table shows, b and se.
    auto fi = fitted.find(names[i]);
    if (fi == fitted.end()) continue;
    const double b0 = fi->second.first, se0 = fi->second.second, se1 = std::sqrt(fit.V[i * k + i]);
    if (std::fabs(fit.b[i] - b0) > 1e-9 * std::max(1.0, std::fabs(b0)) ||
        (std::isfinite(se0) && se0 > 0 && std::fabs(se1 - se0) > 1e-8 * se0)) {
      why = "the engine's replay of the fit did not reproduce the coefficient of `" + names[i] + "`";
      return false;
    }
  }
  if (lm.cobraCmd == "logit") fit.link = static_cast<int>(cecon::GlmLinkFn::Logit);
  else if (lm.cobraCmd == "probit") fit.link = static_cast<int>(cecon::GlmLinkFn::Probit);
  else if (poisson) fit.link = static_cast<int>(cecon::GlmLinkFn::Log);
  else fit.link = static_cast<int>(cecon::GlmLinkFn::Identity);

  // The variables the columns are built from.
  std::set<std::string> vars;
  for (const auto& nm : names) {
    bool ok = true;
    for (const auto& a : cecon::margins_parse_column(nm, &ok)) vars.insert(a.var);
    if (!ok) {
      why = "the term `" + nm + "` cannot be rebuilt at an at() point";
      return false;
    }
  }
  const double eN = res.contains("n_obs") && res["n_obs"].is_number() ? res["n_obs"].get<double>() : -1;
  std::unique_ptr<Datastore> owned;
  Datastore* ds = nullptr;
  std::vector<int64_t> rows;
  bool sampleFromColumn = false;
  try {
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind == SessionState::EsampleReading::Kind::kColumn) {
      sampleFromColumn = true;
      ds = session.datastore();
      if (ds == nullptr || !ds->has(kEsampleColumn)) {
        why = "the estimation sample is not on the data in memory";
        return false;
      }
      const double* m = ds->col(kEsampleColumn).f64();
      const double want = static_cast<double>(er.serial);
      for (int64_t i = 0; i < ds->nRows(); ++i)
        if (m != nullptr && m[i] == want) rows.push_back(i);
    } else if (lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || poisson) {
      if (!lm.fitKeepif.empty() || !lm.fitDataPath.empty()) {
        why = "the estimation sample of a restricted `" + lm.cobraCmd + "` fit is not recorded";
        return false;
      }
      owned = std::make_unique<Datastore>(lm.dataPath);
      ds = owned.get();
      std::vector<std::string> need(vars.begin(), vars.end());
      need.push_back(lm.y);
      if (!lm.fitWeight.empty()) need.push_back(lm.fitWeight);
      for (const auto& c : lm.cluster) need.push_back(c);
      std::vector<const double*> cols;
      for (const auto& v : need) {
        if (!ds->has(v) || ds->col(v).dtype != cecon::DType::F64 || ds->col(v).f64() == nullptr) {
          why = "variable " + v + " is not a numeric column of the fit's data";
          return false;
        }
        cols.push_back(ds->col(v).f64());
      }
      for (int64_t i = 0; i < ds->nRows(); ++i) {
        bool ok = true;
        for (const double* c : cols) ok = ok && !std::isnan(c[i]);
        if (ok) rows.push_back(i);
      }
    } else {
      why = "the estimation sample of this fit is not recorded (" +
            (er.refusal.empty() ? std::string("no e(sample) marker") : er.refusal) + ")";
      return false;
    }
  } catch (const std::exception& e) {
    why = std::string("the estimation sample cannot be read: ") + e.what();
    return false;
  }
  fit.n = static_cast<int64_t>(rows.size());
  auto column = [&](const std::string& v, std::vector<double>& out) {
    if (!ds->has(v) || ds->col(v).dtype != cecon::DType::F64 || ds->col(v).f64() == nullptr) return false;
    const double* c = ds->col(v).f64();
    out.resize(rows.size());
    for (size_t r = 0; r < rows.size(); ++r) out[r] = c[rows[r]];
    return true;
  };
  for (const auto& v : vars) {
    if (!column(v, fit.data[v])) {
      why = "variable " + v + " is not a numeric column of the data in memory";
      return false;
    }
  }
  if (!lm.fitWeight.empty() && !column(lm.fitWeight, fit.weights)) {
    why = "the fit's weight " + lm.fitWeight + " is not a numeric column of the data in memory";
    return false;
  }
  fit.fweight = lm.fitWtype == "fw" || lm.fitWtype == "fweight";
  double count = static_cast<double>(fit.n);
  if (fit.fweight) {
    count = 0;
    for (double w : fit.weights) count += w;
  }
  // The live e(sample) column follows the rows: rows dropped after the fit are
  // gone from it, and Stata's margins averages over what is left (w2v_margins_
  // siblings.log:3360 F_dropped_at `N=200` after `drop if t==1` on a 240-row
  // fit; r(table)'s df stays the fit's e(df_r) 236). A reconstructed sample
  // must still be the fit's exactly.
  if (eN >= 0 && (sampleFromColumn ? count > eN : count != eN)) {
    why = "the estimation sample margins would average over (" + std::to_string(count) +
          ") is not the fit's e(N)";
    return false;
  }
  // r(table)'s reference (w2v_margins_corpus r(table) df rows): t(e(df_r)) after
  // regress and areg (F1r 196, F3a 39, F4 236, F5 231), the normal after xtreg,
  // reghdfe and logit (F1 p 1.369e-36 is the normal tail of 12.634).
  fit.tRef = lm.stataCmd == "regress" || lm.stataCmd == "reg" || lm.stataCmd == "areg";
  if (fit.tRef) {
    fit.dfR = res.contains("df_r") && res["df_r"].is_number() ? res["df_r"].get<double>() : 0.0;
    if (!(fit.dfR > 0)) {
      why = "the fit's residual degrees of freedom are not recorded";
      return false;
    }
  }
  fit.cluster = !lm.cluster.empty();
  fit.depvar = lm.depvar;
  fit.dataPath = lm.dataPath;
  return true;
}

// An at() Stata itself rejects (margins_parse_at rc 121, `at(d=(min(.2)max))`;
// rc 198 for the forms the parser names): StataBE stops before computing and
// leaves r() empty -- w2v_margins_corpus F7_dydx_minmax reads r(N) as `.` and
// colsof(r(b)) as a type mismatch -- so r() is replaced by the known-empty set
// rather than refused.
ojson marginsAtRefusalM4(SessionState& session, const cecon::MarginsAtParse& p) {
  if (p.rc == 121) {
    eval::replaceRClass(session.eval);
    eval::declareRPosted(session.eval, "margins", {});
    return marginsRefusalM4(p.error, 121, "engine_error");
  }
  return marginsRefusalM4(p.error, p.rc);
}

// BUG-1497: the name errors Stata's margins raises too leave r() empty as well:
// r(N) `.` and no r() scalar after `margins, dydx(nosuchvar)` and `margins,
// at(nosuchvar=1)` r(322), `margins nosuchvar` and `margins, dydx(x1)
// over(nosuchvar)` r(111) (StataBE 19.5, rlife/side.do; file-new-1/i7.do).
ojson marginsStataErrorM4(SessionState& session, ojson refusal) {
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "margins", {});
  return refusal;
}

// A fit whose covariance is not the model's OIM one: vce(robust), vce(cluster)
// or a cluster variable (lm.vcov carries the linear spelling, `hc1`).
bool marginsFitNonOimM4(const LastModel& lm) {
  return (!lm.vcov.empty() && lm.vcov != "iid" && lm.vcov != "oim") || !lm.cluster.empty();
}

// The fit's VCE for cmd_margins' GLM at() branches (cmds_postest.cpp
// ml_margins_fit, the fit the AME branch already uses): `--vce robust|cluster`
// and `--cluster C`. Those branches used to fit OIM whatever the fit's vce, so
// after `poisson yc x c1, vce(robust)` `margins, dydx(x) at(c1=(1 3))` answered
// the OIM se at rc 0 (Stata: the robust one, w2v_margins_siblings.log:2871-2921
// F_robust_dydx_at_po). After xtlogit/xtpois/fepois they refit the pooled model,
// whose sandwich is not the fit's, and probit takes no cluster sandwich there:
// such a fit is refused by name.
std::optional<ojson> pushGlmAtVceM4(std::vector<std::string>& args, const LastModel& lm,
                                    const std::string& form) {
  if (!marginsFitNonOimM4(lm)) return std::nullopt;
  std::string vce;
  if (lm.vcov == "hc1" || lm.vcov == "robust") vce = "robust";
  if (lm.vcov == "cluster" || !lm.cluster.empty()) vce = "cluster";
  const bool served = (lm.cobraCmd == "logit" || lm.cobraCmd == "poisson" ||
                       lm.cobraCmd == "cloglog" || (lm.cobraCmd == "probit" && vce == "robust")) &&
                      !vce.empty() && (vce == "robust" || lm.cluster.size() == 1);
  if (!served)
    return marginsRefusalM4("margins, " + form + " after `" + lm.cobraCmd + "` with vce(" +
                                (lm.vcov.empty() ? std::string("cluster") : lm.vcov) +
                                ") is not implemented in this version of PIET (the engine's "
                                "at() refit cannot carry that VCE); nothing was computed.",
                            198);
  args.push_back("--vce");
  args.push_back(vce);
  if (vce == "cluster") {
    args.push_back("--cluster");
    args.push_back(lm.cluster[0]);
  }
  return std::nullopt;
}

// Which margins forms runMargins answers through the grid: the ones the engine
// refit cannot answer as the fit (or answers with derived columns at observed
// values or means). Every other form keeps its landed path byte-for-byte
// (w2b_estim margins_atmeans_post/margins_overall_atmeans/margins_dydx_atmeans,
// w2p_estim_ev G_c_margins_at/G_pc_margins_dydx).
bool marginsGridWantedM4(const LastModel& lm, const std::vector<std::string>& marginlist,
                         const std::optional<std::string>& atVal,
                         const std::optional<std::string>& dydxVal, bool atmeans) {
  const bool linear = postestOlsCmdsM4().count(lm.cobraCmd) > 0;
  const bool binary = lm.cobraCmd == "logit" || lm.cobraCmd == "probit";
  const bool hasAt = atVal.has_value() && !trimM4(*atVal).empty();
  const bool hasDydx = dydxVal.has_value() && !trimM4(*dydxVal).empty();
  // After poisson (no absorbed effects: cobraCmd "fepois" is the absorb() fit),
  // the two forms no engine path computes: `margins, atmeans` and a one-factor
  // marginlist `margins g`, on the response scale exp(xb) with the fit's own
  // OIM, robust or cluster e(V) (w2v_margins_vce.log:1854-1958 V5_po_atmeans /
  // V5_po_g after vce(robust), :2205-2309 V6 after vce(cluster region)). Every
  // other poisson form keeps its landed path: dydx() (the engine AME), at() and
  // dydx()+at() (cmd_margins' GLM at() branches, V5_po_at / V5_po_dydx_at), and
  // the combinations no record pins (atmeans with at()/dydx() or a marginlist)
  // stay refused by name there.
  // A marginlist term that is no plain factor of the model keeps the landed
  // checks (r(111) / r(452) / r(322), FU-94) and their refusals.
  if (lm.cobraCmd == "poisson") {
    if (hasAt || hasDydx) return false;
    if (marginlist.empty()) return atmeans;
    const std::string& term = marginlist[0];
    return !atmeans && marginlist.size() == 1 &&
           marginsTermRoleM4(lm, term.rfind("i.", 0) == 0 ? term.substr(2) : term) ==
               MarginsTermM4::Factor;
  }
  if (!linear && !binary) return false;
  // A time-series operator column (`regress y L.x d`) is not a design column the
  // grid can rebuild from an at() point, and the grid's --emit-v replay carries
  // no tsset context. Such fits keep the landed path, which injects it (BUG-369)
  // and refuses an interacted at() variable by name (FU-342), as on main.
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    tsTokens.push_back(lm.y);
    if (translate::hasTsToken(tsTokens)) return false;
  }
  bool interacted = false;
  for (const std::string& term : lm.x) interacted = interacted || term.find('#') != std::string::npos;
  std::vector<std::string> atVars;
  size_t atPoints = 0;
  if (hasAt) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
    if (p.rc != 0) return true;  // the grid names the refusal with Stata's rc
    atVars = p.vars;
    atPoints = p.points.size();
  }
  bool atInteracted = false;
  for (const auto& v : atVars) atInteracted = atInteracted || marginsVarInInteractionM4(lm, v);
  bool dydxInteracted = false;
  if (hasDydx)
    for (std::string v : splitTokensM4(*dydxVal)) {
      if (v.rfind("i.", 0) == 0 || v.rfind("c.", 0) == 0) v = v.substr(2);
      dydxInteracted = dydxInteracted || marginsVarInInteractionM4(lm, v);
    }
  // A marginlist after logit/probit (F6 `margins year[, atmeans post]`); after a
  // least-squares fit the landed per-level path answers it.
  if (!marginlist.empty()) return binary && !hasAt;
  // After an fweighted logit/probit (F6's `[fweight=w]`) the landed engine path
  // cannot carry the weight (cmd_margins takes --wtype pw only, which surfaced
  // as rc 1), so every dydx()/at()/atmeans form goes to the grid, whose fw rows
  // equal the expanded-rows fit; the forms it cannot serve are refused by name.
  const bool fwBinary = binary && (lm.fitWtype == "fw" || lm.fitWtype == "fweight");
  if (fwBinary && (hasAt || hasDydx || atmeans)) return true;
  // at(): after a least-squares fit everything but one point on an unweighted,
  // unabsorbed fit whose at() variables enter no interaction (F1-F4, X1).
  // After a robust or cluster logit/probit, at() goes to the grid, whose V is the
  // fit's own (the --emit-v replay under the fit's --vce): the landed dydx()+at()
  // branch answered the OIM se at rc 0 (w2v_margins_siblings.log:2787-2837
  // F_robust_dydx_at_lg, `Model VCE: Robust`, se .0098585997756714). A weighted
  // fit keeps the landed path's named refusal (postestRefitMatchesFitM4).
  // A restricted robust/cluster fit (`logit ... if t>2, vce(robust)`) keeps the
  // landed path too: the grid's sample reader cannot read its estimation sample,
  // and that path refits on the fit's `if` under the fit's VCE (pushGlmAtVceM4;
  // w2v_margins_vce.log V9_lg_at / V9_lg_dydx_at).
  if (hasAt) {
    const bool restrictedFit = !lm.fitKeepif.empty() || !lm.fitDataPath.empty();
    if (binary)
      return atmeans || atInteracted || dydxInteracted ||
             (marginsFitNonOimM4(lm) && lm.fitWeight.empty() && !restrictedFit);
    return !(lm.fe.empty() && lm.fitWeight.empty() && atPoints == 1 && !atInteracted &&
             !(atmeans && interacted) && !hasDydx);
  }
  // dydx() of a variable inside an interaction (F5 `dydx(treatment)` after
  // treatment##i.blockcohort); atmeans after an interaction (X1_atmeans). After
  // logit/probit, dydx() without at() keeps the landed engine AME: it carries the
  // fit's `if` (BUG-163) and its perfect-prediction drops, and it is the path
  // W:P5 pins against Stata (p5_target_stata.log:177/:180, `margins, dydx(x1
  // treated)` after a restricted pweighted logit), which the grid's sample reader
  // would refuse.
  // After a cluster probit the landed AME cannot carry the VCE (cmd_margins has
  // no probit cluster sandwich and exited 2, which surfaced as rc 1); the grid
  // reads the fit's own clustered e(V), or refuses a fit it cannot read by name.
  if (hasDydx)
    return (!binary && dydxInteracted) ||
           (lm.cobraCmd == "probit" && (lm.vcov == "cluster" || !lm.cluster.empty()));
  return atmeans && interacted;
}

// margins' at() record ([R] margins, Stored results): r(at)/e(at) has one row
// per at() point (`1._at` ...) and one column per covariate of the model, the
// fixed value where the point sets the covariate and missing where it is left
// as observed; r(atstats#)/e(atstats#) names each column's statistic in that
// row ("values" or "asobserved"); e(k_at) counts the at() options. coefplot's at()
// reads exactly these (coefplot.ado collect_coefs: the `values`
// columns of e(atstats#), row # of e(at)); w2an_coefplot_family.log cf_at_mg
// (`margins, dydx(x1) at(x2=(0 2 4)) post` -> at 0 2 4). Posted only for a
// model whose covariates are all continuous: a factor covariate's at() column
// is one per level (`1.g 2.g ...`), which is not built here, so then nothing is
// posted and every reader keeps its refusal.
void postMarginsAtM4(SessionState& session, const std::vector<std::string>& modelNames,
                     const std::vector<cecon::MarginsGridPoint>& points, bool post) {
  if (points.empty()) return;
  std::vector<std::string> cov;
  for (const auto& nm : modelNames) {
    bool ok = true;
    const std::vector<cecon::MarginsAtom> atoms = cecon::margins_parse_column(nm, &ok);
    if (!ok) return;
    for (const auto& a : atoms) {
      if (a.factor) return;
      if (std::find(cov.begin(), cov.end(), a.var) == cov.end()) cov.push_back(a.var);
    }
  }
  if (cov.empty()) return;
  for (const auto& p : points)
    for (const auto& [v, val] : p.fix) {
      (void)val;
      if (std::find(cov.begin(), cov.end(), v) == cov.end()) return;
    }
  eval::EvalMatrix at;
  at.colnames = cov;
  std::vector<std::string> stats;
  for (size_t i = 0; i < points.size(); ++i) {
    at.rownames.push_back(std::to_string(i + 1) + "._at");
    std::vector<double> row;
    std::string st;
    for (const auto& v : cov) {
      double val = std::numeric_limits<double>::quiet_NaN();
      bool fixed = false;
      for (const auto& [fv, fval] : points[i].fix)
        if (fv == v) {
          val = fval;
          fixed = true;
        }
      row.push_back(val);
      st += (st.empty() ? "" : " ") + std::string(fixed ? "values" : "asobserved");
    }
    at.data.push_back(std::move(row));
    stats.push_back(st);
  }
  auto& ev = session.eval;
  // k_at counts at() OPTIONS, not points (margins.sthlp; StataBE 19.5 s1.do
  // `margins, at(x1=(2 4))`: r(k_at) 1 with r(at) 2 x 6), and this handler
  // takes one at() (BUG-1449).
  ev.rValues.set("k_at", 1.0);
  ev.rMatrices.emplace_back("at", at);
  for (size_t i = 0; i < stats.size(); ++i) ev.rStrings.set("atstats" + std::to_string(i + 1), stats[i]);
  if (!post) return;
  ev.setEScalar("k_at", 1.0);
  bool replaced = false;
  for (auto& p : ev.eMatrices)
    if (p.first == "at") {
      p.second = at;
      replaced = true;
    }
  if (!replaced) ev.eMatrices.emplace_back("at", at);
  for (size_t i = 0; i < stats.size(); ++i) ev.setEString("atstats" + std::to_string(i + 1), stats[i]);
}

}  // namespace

std::optional<ojson> runMarginsGrid(SessionState& session, const MarginsGridFit& fit,
                                    const std::vector<std::string>& marginlist,
                                    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::string atText, dydxText;
  bool hasAt = false, atmeans = false, post = false;
  for (const auto& [k, v] : opts) {
    if (k == "at" && !v.isBool) {
      atText = v.strValue;
      hasAt = !trimM4(atText).empty();
    }
    if (k == "dydx" && !v.isBool) dydxText = v.strValue;
    if (k == "atmeans") {
      if (!v.isBool) return marginsRefusalM4("option atmeans() not allowed", 198, "syntax_error");
      atmeans = v.boolValue;
    }
    if (k == "post" && (!v.isBool || v.boolValue)) post = true;
  }
  if (marginlist.size() > 1) return std::nullopt;
  std::string over;
  if (!marginlist.empty()) {
    over = marginlist[0].rfind("i.", 0) == 0 ? marginlist[0].substr(2) : marginlist[0];
    if (over.find_first_of("#()") != std::string::npos || hasAt) return std::nullopt;
  }
  std::vector<std::string> dydx;
  bool dydxAll = false;
  for (std::string v : splitTokensM4(dydxText)) {
    if (v == "*" || v == "_all") {
      dydxAll = true;
      continue;
    }
    if (v.rfind("i.", 0) == 0 || v.rfind("c.", 0) == 0) v = v.substr(2);
    if (std::find(dydx.begin(), dydx.end(), v) == dydx.end()) dydx.push_back(v);
  }
  // dydx(*) / dydx(_all): every covariate of the model, each once, in e(b)
  // order (w2v_margins_siblings.log:1907 F_fw_all_cols `x c1 1b.g 2.g 3.g`
  // after `logit remote x c1 i.g`); a factor contributes its level columns,
  // the base included, exactly as dydx(g) does.
  if (dydxAll) {
    std::vector<std::string> all;
    for (const auto& nm : fit.names) {
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(nm, &ok))
        if (!a.var.empty() && std::find(all.begin(), all.end(), a.var) == all.end())
          all.push_back(a.var);
      if (!ok) return std::nullopt;
    }
    for (const auto& v : dydx)
      if (std::find(all.begin(), all.end(), v) == all.end()) all.push_back(v);
    dydx = std::move(all);
  }

  cecon::MarginsGridInput in;
  in.names = fit.names;
  in.beta = fit.b;
  in.V = fit.V;
  in.link = static_cast<cecon::GlmLinkFn>(fit.link);
  in.n = fit.n;
  in.data = fit.data;
  in.weights = fit.weights;
  in.atmeans = atmeans;
  in.dydx = dydx;
  std::vector<double> overLevels;
  size_t atPoints = 0;
  if (!over.empty()) {
    // The marginlist term is a factor of the model (FU-94's r(322) otherwise);
    // its levels are its e(sample) values (F6: 2010bn.year .. 2013.year).
    bool factor = false;
    for (const auto& nm : fit.names) {
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(nm, &ok)) factor = factor || (a.factor && a.var == over);
    }
    auto it = fit.data.find(over);
    if (!factor || it == fit.data.end())
      return marginsRefusalM4("factor " + over + " not found in list of covariates", 322, "engine_error");
    std::set<double> lv(it->second.begin(), it->second.end());
    for (double l : lv) {
      overLevels.push_back(l);
      in.points.push_back(cecon::MarginsGridPoint{{{over, l}}});
    }
  } else if (hasAt) {
    const cecon::MarginsAtParse p = cecon::margins_parse_at(atText);
    if (p.rc != 0) return marginsAtRefusalM4(session, p);
    in.points = p.points;
    atPoints = p.points.size();
  } else {
    in.points = {cecon::MarginsGridPoint{}};
  }
  const cecon::MarginsGridResult r = cecon::margins_grid(in);
  if (!r.ok) {
    if (!r.notFound.empty())
      return marginsRefusalM4("variable " + r.notFound + " not found in list of covariates", 322,
                              "engine_error");
    return marginsRefusalM4(r.error + "; nothing was computed.", 198);
  }
  const bool single = over.empty() && in.points.size() == 1;
  const std::vector<std::string> names = cecon::margins_grid_colnames(r, single, over, overLevels);
  const size_t m = r.b.size();

  double nObs = static_cast<double>(fit.n);
  if (fit.fweight) {
    nObs = 0;
    for (double w : fit.weights) nObs += w;
  }
  const double crit = fit.tRef ? cecon::t_critical(0.05, fit.dfR) : cecon::z_critical(0.05);
  ojson tableRows = ojson::array(), ct = ojson::array(), coefs = ojson::array();
  std::vector<std::vector<double>> V(m, std::vector<double>(m, 0.0));
  for (size_t a = 0; a < m; ++a)
    for (size_t c = 0; c < m; ++c) V[a][c] = r.V[a * m + c];
  for (size_t j = 0; j < m; ++j) {
    ojson row;
    row["name"] = names[j];
    row["b"] = r.b[j];
    const double se = std::sqrt(std::max(0.0, r.V[j * m + j]));
    if (r.cols[j].base || !(se > 0)) {
      // A base level's column (F5_dydx 0b.treatment): 0 with variance 0, and
      // r(table) prints `.` for its se, statistic, p and interval.
      row["se"] = nullptr;
      row[fit.tRef ? "t" : "z"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = nullptr;
      row["ci_hi"] = nullptr;
    } else {
      const double t = r.b[j] / se;
      row["se"] = se;
      row[fit.tRef ? "t" : "z"] = t;
      row["p"] = fit.tRef ? 2.0 * cecon::stata_ttail(fit.dfR, std::fabs(t))
                          : std::erfc(std::fabs(t) / std::sqrt(2.0));
      row["ci_lo"] = r.b[j] - crit * se;
      row["ci_hi"] = r.b[j] + crit * se;
    }
    tableRows.push_back(row);
    ojson shown = row;
    shown["name"] = marginsCoefNameM4(names[j]);
    if (!fit.tRef) shown["t"] = row["z"];
    // _se[] of a base column is 0 (F5_dydx_post_bname2 `0b.treatment|0|0`); only
    // r(table) prints `.` for it.
    if (row["se"].is_null()) shown["se"] = 0.0;
    ct.push_back(shown);
    coefs.push_back(ojson{{"name", shown["name"]}, {"margin", r.b[j]}, {"se", row["se"]}});
  }
  ojson est;
  est["type"] = "estimation";
  est["estimator"] = "margins";
  est["depvar"] = fit.depvar;
  est["n_obs"] = nObs;
  est["coeftable"] = ct;
  // postMarginsRClassM4 reads e(df_r) off the live model, which is this block
  // once `post` has replaced e().
  if (fit.tRef) est["df_r"] = fit.dfR;
  session.lastMargins = ojson{{"model", "margins"}, {"n_obs", nObs}, {"coefs", coefs}};
  session.lastMarginsDydx = dydx;

  MarginsRInfoM4 info;
  info.leastSquares = fit.tRef;
  info.dydx = !dydx.empty();
  for (const auto& v : dydx) info.xvars += (info.xvars.empty() ? "" : " ") + v;
  info.kMargins = over.empty() ? 0 : 1;
  info.atPoints = hasAt ? static_cast<int>(atPoints) : (atmeans ? 1 : 0);
  info.cluster = fit.cluster;
  // BUG-1449: e() is still the fit's here (`post` is below).
  info.fit = marginsFitInfoM4(session);
  info.kAt = hasAt || atmeans ? 1 : 0;
  info.title = marginsTitleM4(info.dydx, atmeans, atText, over, info.fit.ebNames);
  info.nKnown = !atmeans;
  if (info.dydx) {
    bool factor = false;
    for (const auto& c : r.cols) factor = factor || c.factor;
    info.continuous = factor ? 0 : 1;
    // r(xvars) is the effects' columns in e(b) order, a factor's base level
    // included (StataBE 19.5 s1.do `dydx(x1 c)`: "x1 0b.c 1.c 2.c 3.c"; s4.do
    // `dydx(x2 x1)`: "x1 x2"), each once over the at() points.
    info.xvars.clear();
    for (const auto& nm : names) {
      const std::string head = nm.substr(0, nm.find(':'));
      if ((" " + info.xvars + " ").find(" " + head + " ") == std::string::npos)
        info.xvars += (info.xvars.empty() ? "" : " ") + head;
    }
    if (factor) info.bRow = "y1";  // r(b)'s row after a factor dydx() (s2.do B_dxc_br)
  }
  if (post) {
    // `post` replaces e() with the margins (w2v_margins_corpus *_post_e: e(cmd)
    // margins, e(N) = r(N)); e(b)/e(V) carry Stata's column names.
    postEstimationBlockM4(session, est, "margins", fit.depvar, fit.dataPath);
    eval::EvalMatrix eb, eV;
    eb.rownames = {"y1"};
    eb.colnames = names;
    eb.data.push_back(r.b);
    eV.rownames = names;
    eV.colnames = names;
    eV.data = V;
    auto setE = [&session](const std::string& nm, eval::EvalMatrix mat) {
      for (auto& p : session.eval.eMatrices) {
        if (p.first == nm) {
          p.second = std::move(mat);
          return;
        }
      }
      session.eval.eMatrices.emplace_back(nm, std::move(mat));
    };
    setE("b", std::move(eb));
    setE("V", std::move(eV));
  }
  // r() after e(): `post` installs a new estimation, and the r(table) posted
  // under the fit would be retired with it (F1_dydx_at_post_rt reads r(table)
  // after `post`, StataBE keeps it).
  postMarginsRClassM4(session, est, info, names, r.b, V, tableRows);
  if (hasAt && !atmeans) postMarginsAtM4(session, fit.names, in.points, post);
  return est;
}

void postGlmDydxRClass(SessionState& session, const ojson& block, const std::vector<std::string>& names,
                       const std::vector<double>& b, const std::vector<std::vector<double>>& V,
                       const ojson& tableRows, const std::string& xvars, bool cluster) {
  MarginsRInfoM4 info;
  info.dydx = true;
  info.xvars = xvars;
  info.cluster = cluster;
  postMarginsRClassM4(session, block, info, names, b, V, tableRows);
}

// ME06: the grid fit of an `xtpoisson, fe` model -- e(b)/e(V) from the fit's
// reply (--emit-v), the linear index with no constant (identity link), and the
// estimation sample rebuilt as the fit drew it: rows with y, the panel id and
// every covariate present, less the panels xtpoisson drops (all-zero outcome,
// or a single row). The count must be the fit's n_obs; otherwise nothing.
// BUG-1235: `xtlogit, fe` the same way, its default prediction pu0 = Pr(y = 1 |
// fixed effect 0) = invlogit(xb) (logit link, no constant), its sample less the
// panels clogit drops (an outcome that does not vary); StataBE 19.5
// (scratch-laneA/agents/N2/e14.do) `margins, dydx(x)` .090159189631237813.
bool xtpoisMarginsFitM4(const LastModel& lm, MarginsGridFit& fit, std::string& why) {
  const bool xtlogit = lm.cobraCmd == "xtlogit";
  if (!lm.result.has_value() || !lm.result->is_object() || !lm.result->contains("e_V") ||
      !lm.ivar.has_value()) {
    why = "the fit's covariance matrix is not recorded";
    return false;
  }
  if (!lm.fitKeepif.empty() || !lm.fitDataPath.empty() || !lm.fitWeight.empty()) {
    why = "the estimation sample of a restricted fit is not recorded";
    return false;
  }
  const ojson& raw = *lm.result;
  const ojson& ev = raw["e_V"];
  if (!ev.contains("names") || !ev.contains("matrix") || !raw.contains("coefs") ||
      !raw["coefs"].is_array() || !raw.contains("n_obs")) {
    why = "the fit's covariance matrix is not recorded";
    return false;
  }
  for (const auto& nm : ev["names"]) {
    if (!nm.is_string()) return false;
    const std::string n = nm.get<std::string>();
    if (n.find_first_of(".#") != std::string::npos) {
      why = "a factor or operator term after xtpoisson, fe";
      return false;
    }
    fit.names.push_back(n);
  }
  const size_t k = fit.names.size();
  for (const auto& nm : fit.names) {
    double b = std::numeric_limits<double>::quiet_NaN();
    for (const auto& c : raw["coefs"])
      if (c.value("name", std::string()) == nm && c.contains("beta") && c["beta"].is_number())
        b = c["beta"].get<double>();
    if (std::isnan(b)) return false;
    fit.b.push_back(b);
  }
  if (ev["matrix"].size() != k) return false;
  for (const auto& row : ev["matrix"]) {
    if (!row.is_array() || row.size() != k) return false;
    for (const auto& v : row) fit.V.push_back(v.is_number() ? v.get<double>() : 0.0);
  }
  try {
    Datastore ds(lm.dataPath);
    std::vector<std::string> need = fit.names;
    need.push_back(lm.y);
    need.push_back(*lm.ivar);
    for (const auto& v : need)
      if (!ds.has(v) || ds.col(v).dtype != cecon::DType::F64 || ds.col(v).f64() == nullptr) {
        why = "variable " + v + " is not a numeric column of the fit's data";
        return false;
      }
    const double* yv = ds.col(lm.y).f64();
    const double* iv = ds.col(*lm.ivar).f64();
    std::vector<int64_t> rows;
    for (int64_t i = 0; i < ds.nRows(); ++i) {
      bool ok = !std::isnan(yv[i]) && !std::isnan(iv[i]);
      for (const auto& v : fit.names) ok = ok && !std::isnan(ds.col(v).f64()[i]);
      if (ok) rows.push_back(i);
    }
    std::map<double, std::pair<int64_t, double>> grp;  // panel -> (rows, sum y; xtlogit: rows with y != 0)
    for (int64_t i : rows) {
      auto& g = grp[iv[i]];
      ++g.first;
      g.second += xtlogit ? (yv[i] != 0.0 ? 1.0 : 0.0) : yv[i];
    }
    std::vector<int64_t> used;
    for (int64_t i : rows) {
      const auto& g = grp[iv[i]];
      if (xtlogit ? (g.second > 0 && g.second < static_cast<double>(g.first)) : (g.first > 1 && g.second > 0))
        used.push_back(i);
    }
    if (static_cast<double>(used.size()) != raw["n_obs"].get<double>()) {
      why = "the estimation sample margins would average over is not the fit's e(N)";
      return false;
    }
    fit.n = static_cast<int64_t>(used.size());
    for (const auto& v : fit.names) {
      const double* c = ds.col(v).f64();
      auto& out = fit.data[v];
      for (int64_t i : used) out.push_back(c[i]);
    }
  } catch (const std::exception& e) {
    why = std::string("the estimation sample cannot be read: ") + e.what();
    return false;
  }
  fit.link = static_cast<int>(xtlogit ? cecon::GlmLinkFn::Logit : cecon::GlmLinkFn::Identity);
  fit.tRef = false;
  fit.cluster = lm.vcov == "hc1";
  fit.depvar = lm.depvar;
  fit.dataPath = lm.dataPath;
  return true;
}

namespace postest_detail {

// BUG-1237/BUG-1241: r() of a dydx() a least-squares fit answers from its own
// coefficients (buildLinearAmeRawM4). Stata's r(b) holds every e(b) column of
// each dydx() variable in e(b) order -- a factor's base level included, `0b.c`
// with b 0 -- r(V) is e(V) over those columns, off-diagonals included, and
// r(Jacobian) the unit row of each column (zero for the base): StataBE 19.5
// (scratch-laneA/agents/E3/rx2.do, p-margins/s1.do) after `regress y x1 x2 i.c`,
// dydx(x1 c) r(b) `x1 0b.c 1.c 2.c 3.c` with V[1.c,x1] -.00102863 =
// e(V)[1.c,x1]; dydx(x1 x2) el(r(V),1,2) -.0007857769903082221 = e(V)[x2,x1].
// false (nothing built) for a column this does not read: an omitted or
// interacted term of a dydx() variable, an equation-prefixed stripe.
struct LinearAmeRM4 {
  std::vector<std::string> names;
  std::vector<double> b;
  std::vector<std::vector<double>> V, J;
  ojson tableRows = ojson::array();
  bool factor = false;
};
bool linearAmeRM4(SessionState& session, const MarginsFitInfoM4& fit,
                  const std::vector<std::string>& dydxVars, const ojson& est, LinearAmeRM4& out) {
  const std::vector<std::string>& eb = fit.ebNames;
  if (eb.empty() || fit.ebValues.size() != eb.size() || !est.contains("coeftable") ||
      !est["coeftable"].is_array())
    return false;
  const eval::EvalSession::EMatrixAnswer va =
      session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
  if (!va.matrix.has_value() || va.matrix->colnames != eb || va.matrix->data.size() != eb.size())
    return false;
  const auto& eV = va.matrix->data;
  std::vector<size_t> sel;
  std::vector<bool> base;
  std::set<std::string> matched;
  for (size_t j = 0; j < eb.size(); ++j) {
    const std::string& nm = eb[j];
    if (nm.find(':') != std::string::npos) return false;
    if (nm == "_cons") continue;
    if (nm.find('#') != std::string::npos) {
      bool ok = true;
      for (const auto& a : cecon::margins_parse_column(nm, &ok))
        if (dydxVars.empty() ||
            std::find(dydxVars.begin(), dydxVars.end(), a.var) != dydxVars.end())
          return false;
      continue;
    }
    const size_t dot = nm.find('.');
    const std::string op = dot == std::string::npos ? std::string() : nm.substr(0, dot);
    const std::string var = dot == std::string::npos ? nm : nm.substr(dot + 1);
    if (!dydxVars.empty() && std::find(dydxVars.begin(), dydxVars.end(), var) == dydxVars.end()) continue;
    size_t d = 0;
    while (d < op.size() && std::isdigit(static_cast<unsigned char>(op[d]))) ++d;
    const std::string flags = op.substr(d);
    if (op.find('o') != std::string::npos || (d == 0 && !(op.empty() || op == "c"))) return false;
    sel.push_back(j);
    base.push_back(d > 0 && flags.find('b') != std::string::npos && flags.find("bn") == std::string::npos);
    out.factor = out.factor || d > 0;
    matched.insert(var);
  }
  if (sel.empty()) return false;
  for (const auto& v : dydxVars)
    if (!matched.count(v)) return false;
  std::map<std::string, const ojson*> shown;
  for (const auto& row : est["coeftable"]) shown[row.value("name", std::string())] = &row;
  for (size_t a = 0; a < sel.size(); ++a) {
    const std::string& nm = eb[sel[a]];
    out.names.push_back(nm);
    out.b.push_back(base[a] ? 0.0 : fit.ebValues[sel[a]]);
    std::vector<double> vr, jr(eb.size(), 0.0);
    for (size_t c = 0; c < sel.size(); ++c) vr.push_back(eV[sel[a]][sel[c]]);
    out.V.push_back(std::move(vr));
    if (!base[a]) jr[sel[a]] = 1.0;
    out.J.push_back(std::move(jr));
    ojson tr;
    tr["name"] = nm;
    if (base[a]) {
      // r(table) prints `.` for a base level's se, statistic, p and interval.
      tr["b"] = 0.0;
      for (const char* key : {"se", "t", "p", "ci_lo", "ci_hi"}) tr[key] = nullptr;
    } else {
      const auto it = shown.find(nm);
      if (it == shown.end()) return false;
      for (const char* key : {"b", "se", "t", "z", "p", "ci_lo", "ci_hi"})
        if (it->second->contains(key)) tr[key] = (*it->second)[key];
    }
    out.tableRows.push_back(std::move(tr));
  }
  return true;
}

// BUG-1241: the engine's AME (cmd_margins after logit/probit/poisson) sends each
// effect's standard error, not their covariances. The grid computes the same
// effects from the fit's own e(b)/e(V) with V = G e(V) G'; when it reads the fit
// and reproduces every effect and standard error, its off-diagonals complete
// r(V) (StataBE 19.5 rx2.do: `logit d x1 x2` / `margins, dydx(x1 x2)`
// el(r(V),1,2) -.0000380840715475375). Otherwise nothing is returned and the
// off-diagonals stay missing.
bool marginsAmeCrossVM4(SessionState& session, const LastModel& lm, const ojson& est,
                        std::vector<std::vector<double>>& V) {
  if (!est.contains("coeftable") || !est["coeftable"].is_array()) return false;
  const ojson& ct = est["coeftable"];
  if (ct.size() < 2) return false;
  MarginsGridFit fit;
  std::string why;
  if (!marginsGridFitM4(session, lm, fit, why)) return false;
  cecon::MarginsGridInput in;
  in.names = fit.names;
  in.beta = fit.b;
  in.V = fit.V;
  in.link = static_cast<cecon::GlmLinkFn>(fit.link);
  in.n = fit.n;
  in.data = fit.data;
  in.weights = fit.weights;
  in.points = {cecon::MarginsGridPoint{}};
  for (const auto& row : ct) in.dydx.push_back(row.value("name", std::string()));
  const cecon::MarginsGridResult r = cecon::margins_grid(in);
  if (!r.ok) return false;
  const std::vector<std::string> cols = cecon::margins_grid_colnames(r, true, std::string(), {});
  const size_t m = r.b.size();
  std::vector<size_t> at;
  for (const auto& row : ct) {
    const std::string nm = row.value("name", std::string());
    const auto it = std::find(cols.begin(), cols.end(), nm);
    if (it == cols.end()) return false;
    const size_t j = static_cast<size_t>(it - cols.begin());
    const double b = marginsNumM4(row, "b"), se = marginsNumM4(row, "se");
    const double seGrid = std::sqrt(std::max(0.0, r.V[j * m + j]));
    if (!std::isfinite(b) || !(se > 0) || std::fabs(r.b[j] - b) > 1e-9 * std::max(1.0, std::fabs(b)) ||
        std::fabs(seGrid - se) > 1e-8 * se)
      return false;
    at.push_back(j);
  }
  V.assign(ct.size(), std::vector<double>(ct.size(), 0.0));
  for (size_t a = 0; a < ct.size(); ++a)
    for (size_t c = 0; c < ct.size(); ++c) {
      const double sa = marginsNumM4(ct[a], "se");
      V[a][c] = a == c ? sa * sa : r.V[at[a] * m + at[c]];
    }
  return true;
}

}  // namespace

ojson runMargins(const std::vector<std::string>& marginlist,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  // BUG-1210: results read by `estimates use` carry no e(sample) (it is 0
  // everywhere), and margins computes over e(sample): StataBE r(322).
  if (lm.esample == LastModel::EsampleState::kNone)
    return eval::errorBlock("engine_error", "e(sample) does not identify the estimation sample",
                            std::nullopt, std::nullopt, std::nullopt, "r(322)");
  // BUG-1449: the fit's half of r(), read before any path's `post` replaces e().
  const MarginsFitInfoM4 fitInfo = marginsFitInfoM4(session);
  // FU-475: after a least-squares fit the engine's margins refit applies a TS
  // fit's --keepif after the operators (cmds_postest.cpp cmd_margins,
  // plan_keepif), and dydx() answers from the coefficient table. The ML refits
  // have no row-mask input, so they keep the named refusal.
  if (!postestOlsCmdsM4().count(lm.cobraCmd)) {
    if (auto refused = postestTsKeepifRefusalM4("margins", lm)) return *refused;
  }

  // BUG-163: margins computes over e(sample) (oracle: N=4,282 after a fit
  // with `if t>=3`), so replay the fitted sample exactly as
  // lastModelBaseArgv does. cmd_margins consumes --keepif (cmds_postest).
  std::vector<std::string> args = {"margins", "--data",
                                    lm.fitDataPath.empty() ? lm.dataPath : lm.fitDataPath,
                                    "--y", lm.y, "--x",
                                    lm.x.empty() ? "(intercept)" : joinCommaM4(lm.x)};
  if (!lm.fitKeepif.empty()) {
    args.push_back("--keepif");
    args.push_back(lm.fitKeepif);
  }
  if (!lm.fe.empty()) {
    args.push_back("--fe");
    args.push_back(joinCommaM4(lm.fe));
  }
  // BUG-369's shape for margins: an `L.x` regressor needs the tsset context the
  // fit had, or the refit cannot build it (`margins` after `regress y L.x1 x2`
  // was ts_no_tsset, r(111), on main 2026-10-13).
  {
    std::vector<std::string> tsTokens = lm.x;
    tsTokens.insert(tsTokens.end(), lm.fe.begin(), lm.fe.end());
    tsTokens.push_back(lm.y);  // an operated depvar too (BUG-1277)
    if (translate::hasTsToken(tsTokens)) args = translate::injectTsContext(args, session);
  }

  std::optional<std::string> atVal;
  std::optional<std::string> dydxVal;
  bool postOpt = false;
  bool atmeansOpt = false;
  for (const auto& [k, v] : opts) {
    if (k == "at" && !v.isBool) atVal = v.strValue;
    if (k == "dydx" && !v.isBool) dydxVal = v.strValue;
    // w2b_estim.do:111/:121/:126 (`margins t, atmeans post`, `margins, atmeans`,
    // `margins, dydx(x1) atmeans`). A valued `atmeans(...)` is not Stata syntax.
    if (k == "atmeans") {
      if (!v.isBool) return marginsRefusalM4("option atmeans() not allowed", 198, "syntax_error");
      atmeansOpt = v.boolValue;
    }
    // Item 4 (User ruling 2026-09-09): `post` REPLACES e() with the margin
    // (Cells `B4REC|margins|dydx_post|0|`,
    // `B4VAL|ecmd|margins|b_mpg|-1.8c19072612ae4X+005`). Read here, applied
    // at both success return points below via postEstimationBlockM4.
    if (k == "post" && (!v.isBool || v.boolValue)) postOpt = true;
  }

  // BS-097: `at`/`dydx`/`post`/`atmeans` are the ONLY margins options this
  // handler reads (B6 audit: margins had ZERO unknown-option net; `post` joined
  // 2026-09-09, `atmeans` with FU-94). --fe/--vcov/--cluster (BS-086, BETA D5 batch) are
  // forwarded from session.lastModel() itself, not from a new opts-derived
  // key here, so they don't collide with this net. Any other leftover key
  // (e.g. `level()`, or a typo) reads as an honest reject instead of a
  // silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kMarginsKnownOpts = kVocabMargins.known;
  // BUG-1502: an over() name that is no variable is r(111) "variable nosuchvar
  // not found" (StataBE 19.5, i11.do `margins, dydx(x1) over(nosuchvar)`), ahead
  // of the net below, which refuses over() itself (not served, BUG-1238). A name
  // that abbreviates a variable is left to the net.
  for (const auto& [k, v] : opts) {
    if (k != "over" || v.isBool) continue;
    std::vector<std::string> names;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const auto& vi : *card) names.push_back(vi.name);
    } else {
      Datastore* live = nullptr;
      try {
        live = session.datastore();
      } catch (...) {
        live = nullptr;
      }
      if (live != nullptr) names = live->names();
    }
    for (std::string tok : splitTokensM4(v.strValue)) {
      if (tok.rfind("i.", 0) == 0) tok = tok.substr(2);
      if (!strutil::isValidStataIdentifier(tok) || names.empty()) continue;
      if (std::none_of(names.begin(), names.end(), [&tok](const std::string& n) {
            return n.compare(0, tok.size(), tok) == 0;
          }))
        return marginsStataErrorM4(
            session, marginsRefusalM4("variable " + tok + " not found", 111, "engine_error"));
    }
  }
  if (auto err = eval::rejectUnknownOptions(opts, kMarginsKnownOpts)) {
    return *err;
  }
  if (auto refused = marginsCovariateRefusalM4(lm, dydxVal, atVal))
    return marginsStataErrorM4(session, std::move(*refused));

  // A no-constant fit (FU-131): every engine margins form below refits, so each
  // argv carries --no-intercept (pushed here, ahead of the branch flags; cmd_margins
  // reads it on every path) and the engine drops the constant the fit never had
  // (w2p_estim_ev.log:1372 G_nc_margins_at, :1628 G_pnc_margins_dydx). The linear
  // dydx() answer reads the coefficient table and never calls the engine. Only the
  // poisson refit cannot drop it, and refuses by name.
  if (postestNoConstantUnrefittableM4(lm)) return postestNoConstantRejectM4("margins", lm);
  if (postestFitLacksConstantM4(lm)) args.push_back("--no-intercept");

  // A fixed-effects ML fit (xtpoisson/xtlogit, ppmlhdfe with absorb()): every
  // engine margins path refits the POOLED model, which is not the fit. After
  // `xtpoisson yc x c1, fe` `margins, dydx(x)` answered .4846500614524408 at rc 0
  // (Stata: .1088590169532439, the fit's own linear prediction,
  // w2v_margins_vce.log V7_xtp_dydx_x_b / V7_xtp_at_b / V7r_xtp_dydx_x_b), and
  // after ppmlhdfe with absorb() the refit was rc 1 (cmd_margins takes no --fe
  // in its GLM modes). Refused by name until margins answers from e(b)/e(V).
  // ME06: after `xtpoisson, fe` margins' default prediction is the linear index
  // xb with no constant and no fixed effect, so every form is answered from the
  // fit's own e(b)/e(V) over e(sample) by the grid (identity link, z reference):
  // w2v_margins_vce.log:2380 V7_xtp_dydx_x .1088590169532439 = _b[x], :2433
  // V7_xtp_at `.5601492709718607|.5319851840546966`.
  if (lm.cobraCmd == "xtpois" || lm.cobraCmd == "xtlogit") {  // BUG-1235: xtlogit's pu0
    MarginsGridFit fit;
    std::string why;
    if (xtpoisMarginsFitM4(lm, fit, why)) {
      if (atVal.has_value() && !trimM4(*atVal).empty()) {
        const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
        if (p.rc != 0) return marginsAtRefusalM4(session, p);
      }
      if (auto served = runMarginsGrid(session, fit, marginlist, opts)) return *served;
    }
  }
  if (lm.cobraCmd == "xtpois" || lm.cobraCmd == "xtlogit" ||
      ((lm.cobraCmd == "fepois" || lm.cobraCmd == "ppmlhdfe") && !lm.fe.empty())) {
    return marginsRefusalM4("margins after a fixed-effects `" + lm.cobraCmd +
                                "` fit is not implemented in this version of PIET (the engine "
                                "refits the pooled model, which is not the fit); nothing was "
                                "computed.",
                            198);
  }

  // margins-corpus-shapes: the forms the grid answers from the fit's own e(b)/e(V)
  // (marginsGridWantedM4); a fit it cannot read is refused by name rather than
  // handed to a refit that answers a different model.
  if (marginsGridWantedM4(lm, marginlist, atVal, dydxVal, atmeansOpt)) {
    if (atVal.has_value() && !trimM4(*atVal).empty()) {
      const cecon::MarginsAtParse p = cecon::margins_parse_at(*atVal);
      if (p.rc != 0) return marginsAtRefusalM4(session, p);
    }
    MarginsGridFit fit;
    std::string why;
    if (!marginsGridFitM4(session, lm, fit, why))
      return marginsRefusalM4("margins: " + why + "; nothing was computed.", 198);
    // The FE estimator recovers _cons and its full covariance after compacting
    // omitted columns (FU-633). Older/incomplete fit payloads still cannot serve
    // a predictive margin without that row; dydx() can read their slopes alone.
    if (!lm.fe.empty() && (!dydxVal.has_value() || trimM4(*dydxVal).empty()) &&
        std::none_of(fit.names.begin(), fit.names.end(),
                     [](const std::string& n) { return n == "_cons" || n == "(intercept)"; }))
      return marginsRefusalM4("margins: the fit's coefficient vector has no intercept (a factor "
                              "interaction with the absorbed variable); predictive margins after it "
                              "are not implemented in this version of PIET; nothing was computed.",
                              198);
    if (auto served = runMarginsGrid(session, fit, marginlist, opts)) return *served;
    return marginsRefusalM4(
        "margins: this combination of a marginlist, at(), dydx(*) or several marginlist terms is "
        "not implemented in this version of PIET; nothing was computed.",
        198);
  }

  const bool hasAt = atVal.has_value() && !atVal->empty();
  const bool hasDydx = dydxVal.has_value() && !dydxVal->empty();
  // The least-squares family whose engine refit IS the fitted model (the at()
  // path's kAtSafeCmds below names the same five spellings).
  const bool leastSquares = postestOlsCmdsM4().count(lm.cobraCmd) > 0;

  // `atmeans` holds the COVARIATES at their means and rebuilds every term made
  // from them; the engine's --atmeans holds each expanded DESIGN COLUMN at its
  // mean. The two agree only while every column is a covariate or a factor
  // indicator. An interaction or a polynomial (`c.x1##c.x1`, `i.id#c.x2`) has a
  // column whose mean is not the product of its parts' means: after `regress y
  // c.x1##c.x1 x2 i.t`, `margins, atmeans` answered 12.673333 at rc 0 where the
  // fitted point at the means gives 12.950175. Refused by name for every form
  // the held point reaches (marginlist, bare, at()); dydx() after a linear fit
  // is the coefficient at any point, and buildLinearAmeRawM4 already refuses an
  // interacted dydx() variable.
  if (atmeansOpt && !(dydxVal.has_value() && !dydxVal->empty() &&
                      !(atVal.has_value() && !atVal->empty()))) {
    bool interacted = false;
    for (const std::string& term : lm.x) interacted = interacted || term.find('#') != std::string::npos;
    if (lm.result.has_value() && lm.result->contains("coeftable") &&
        (*lm.result)["coeftable"].is_array()) {
      for (const auto& row : (*lm.result)["coeftable"]) {
        interacted = interacted || row.value("name", std::string()).find('#') != std::string::npos;
      }
    }
    if (interacted) {
      return marginsRefusalM4(
          "margins, atmeans after a model with an interaction or polynomial term is not "
          "implemented in this version of PIET (the covariates would be held at their means "
          "and the derived terms rebuilt from them); nothing was computed.",
          198);
    }
  }

  // FU-94: the marginlist (the positional terms) used to be dropped at the
  // dispatcher, so `margins t` answered what bare `margins` answers, at rc 0.
  // Each term is checked the way StataBE 19.5 checks it (live, 2026-10-12, not a
  // golden): a name that is no variable is r(111), a variable with non-integer
  // values r(452) ("x1:  factor variables may not contain noninteger values"),
  // an integer variable that is no factor of the model r(322) ("factor id not
  // found in list of covariates"). `i.VAR` is the same term as `VAR`.
  if (!marginlist.empty()) {
    std::vector<std::string> vars;
    for (const std::string& term : marginlist) {
      if (term.find('#') != std::string::npos || term.find('(') != std::string::npos) {
        return marginsRefusalM4("margins: the marginlist term `" + term +
                                    "` (an interaction or a level list) is not implemented in "
                                    "this version of PIET; nothing was computed.",
                                198);
      }
      const std::string var = term.rfind("i.", 0) == 0 ? term.substr(2) : term;
      const MarginsTermM4 role = marginsTermRoleM4(lm, var);
      if (role == MarginsTermM4::Other) {
        return marginsRefusalM4("margins: " + var + " enters the model in an interaction or "
                                    "under an operator; per-level margins of it are not "
                                    "implemented in this version of PIET.",
                                198);
      }
      if (role != MarginsTermM4::Factor) {
        Datastore* live = nullptr;
        try {
          live = session.datastore();
        } catch (...) {
          live = nullptr;
        }
        if (live != nullptr && !live->has(var)) {
          return marginsStataErrorM4(
              session, marginsRefusalM4("variable " + var + " not found", 111, "engine_error"));
        }
        bool nonInteger = false;
        if (live != nullptr) {
          const cecon::Column& c = live->col(var);
          if (c.dtype == cecon::DType::F64 && c.f64() != nullptr) {
            const double* pv = c.f64();
            for (int64_t r = 0; r < live->nRows() && !nonInteger; ++r) {
              nonInteger = !std::isnan(pv[r]) && pv[r] != std::floor(pv[r]);
            }
          }
        }
        if (nonInteger) {
          return marginsRefusalM4(var + ":  factor variables may not contain noninteger values",
                                  452, "engine_error");
        }
        return marginsRefusalM4("factor " + var + " not found in list of covariates", 322,
                                "engine_error");
      }
      vars.push_back(var);
    }
    if (vars.size() > 1) {
      return marginsRefusalM4("margins: a marginlist of several terms is not implemented in this "
                              "version of PIET; ask for one factor at a time.",
                              198);
    }
    if (hasAt || hasDydx) {
      return marginsRefusalM4("margins " + vars[0] + " combined with " +
                                  std::string(hasDydx ? "dydx()" : "at()") +
                                  " is not implemented in this version of PIET; nothing was "
                                  "computed.",
                              198);
    }
    if (!leastSquares || !lm.fe.empty()) {
      return marginsRefusalM4("margins " + vars[0] + " after `" + lm.cobraCmd + "`" +
                                  std::string(lm.fe.empty() ? "" : " with absorbed effects") +
                                  " is not implemented in this version of PIET (per-level "
                                  "predictive margins cover regress without absorbed effects).",
                              198);
    }
    {
      ojson reject;
      if (!postestRefitMatchesFitM4("margins " + vars[0], lm, reject)) return reject;
    }
    return runLinearPredictiveMarginsM4(lm, session, args, vars[0], atmeansOpt, postOpt, fitInfo);
  }

  // FU-95: bare `margins` is the predictive margin of the response -- one row,
  // the mean prediction over e(sample) (live StataBE 19.5: el(r(b),1,1)
  // 12.67333335032065733 after `regress y x1 x2 i.t`, the mean of y). The
  // engine's no---dydx path is the logit/probit/poisson AME of every covariate,
  // which is what bare `margins` used to return after a nonlinear fit, at rc 0.
  // After a nonlinear fit the predictive margin waits for its oracle arm (the
  // w2p capture), so it refuses by name; it never answers with effects.
  if (!hasAt && !hasDydx) {
    if (!leastSquares) {
      return marginsRefusalM4(
          "margins without a marginlist, dydx() or at() after `" + lm.cobraCmd +
              "` (the average predicted response) is not implemented in this version of PIET; "
              "nothing was computed. Marginal effects are `margins, dydx(varlist)`.",
          198);
    }
    if (!lm.fe.empty()) {
      return marginsRefusalM4(
          "margins without a marginlist, dydx() or at() after a fit with absorbed effects is "
          "not implemented in this version of PIET; nothing was computed.",
          198);
    }
    {
      ojson reject;
      if (!postestRefitMatchesFitM4("margins", lm, reject)) return reject;
    }
    return runLinearPredictiveMarginsM4(lm, session, args, std::string(), atmeansOpt, postOpt, fitInfo);
  }

  // `atmeans` with at()/dydx(): after a least-squares fit dy/dx is the
  // coefficient (at any covariate values) and at() holds the other covariates
  // at their float-stored means (engine --atmeans). After a nonlinear fit both
  // become effects/predictions AT the means, which no engine path computes.
  if (atmeansOpt && (!leastSquares || !lm.fe.empty())) {
    return marginsRefusalM4("margins, atmeans after `" + lm.cobraCmd + "`" +
                                std::string(lm.fe.empty() ? "" : " with absorbed effects") +
                                " is not implemented in this version of PIET; nothing was "
                                "computed.",
                            198);
  }

  std::vector<std::string> dydxVars;
  // AER-B4 margins-dydx-OLS: set by the linear-AME branch below, which answers from the fitted
  // model's own coefficient table instead of calling the engine. When it holds
  // a value the shared tail skips runCobraJsonM4 and runs on this payload.
  std::optional<ojson> linearAmeRaw;
  if (atVal.has_value() && !atVal->empty()) {
    // An at() list Stata rejects (`d=(min(.2)max)` r(121), `(stat) varlist`
    // r(198)) is refused with the kernel parser's rc before anything runs; every
    // list below is expanded by that same parser (parseAtNumlistM4).
    {
      const cecon::MarginsAtParse atParse = cecon::margins_parse_at(*atVal);
      if (atParse.rc != 0) return marginsAtRefusalM4(session, atParse);
    }
    // BUG-571 sibling: every at() form below is answered by an engine refit
    // (the linear --at branch refits feols from y/x/fe; the GLM at() and
    // dydx()+at() branches refit from load_glm), and none of them carries the
    // fit's weight or areg's clustered dof. Measured on main 2026-09-29
    // (BUG-571 probe frame): after `regress y x1 x2 [aweight=w]`,
    // `margins, at(x1=1)` answered 3.3492323 at rc 0 -- the unweighted fit's
    // _b[_cons]+_b[x1]+_b[x2]*mean(x2) -- where the weighted fit's is
    // 3.5853964; after `areg ..., absorb(id) vce(cluster id)` its SE is the
    // reghdfe fit's, byte for byte. The same refusal as test/lincom/nlcom.
    {
      ojson reject;
      if (!postestRefitMatchesFitM4("margins, at()", lm, reject)) return reject;
    }
    // FU-342: an at() variable that also enters an interaction or polynomial
    // term (`c.x1##c.x1`, `i.t#c.x1`) owns a derived design column the engine's
    // at() paths cannot rebuild -- they override the column named x1 and keep
    // x1#x1 at its observed values (39fd4ccb cmds_postest.cpp:1030-1040), so
    // `margins, at(x1=5)` after `regress y c.x1##c.x1 x2` answered with x1#x1
    // at its sample mean instead of 25, at rc 0. Refused by name here for every
    // at() form; the engine's dydx()+at() branch already refuses the same shape.
    for (const auto& spec : parseAtNumlistM4(*atVal)) {
      size_t pos = 0;
      while (pos <= spec.size()) {
        const size_t comma = spec.find(',', pos);
        const std::string tok =
            spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const std::string var = trimM4(tok.substr(0, tok.find('=')));
        if (!var.empty() && marginsVarInInteractionM4(lm, var)) {
          return marginsRefusalM4(
              "margins, at(" + var + "=...) after a model where " + var +
                  " enters an interaction or polynomial term is not implemented in this version "
                  "of PIET (the derived terms would have to be rebuilt from the at() value); "
                  "nothing was computed.",
              198);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
    // BS-086 (BETA D5 batch) ENGINE FINDING: cobra's `margins --at`
    // (predictive margins) ALWAYS internally RE-ESTIMATES a plain OLS
    // regression from the model's own --y/--x/--fe, regardless of any
    // --model flag — live-verified DIRECTLY against the compiled engine:
    // `--model logit`, `--model probit`, `--model poisson`, and even a
    // BOGUS --model value all produce the IDENTICAL byte-for-byte output
    // in --at mode as no --model flag at all, and that shared output
    // matches a from-scratch plain-OLS refit of y~x to 10+ significant
    // digits. This is NOT a bug this batch introduced — the pre-existing
    // `--at` branch never forwarded --model at all either — but it means
    // `margins, at(...)` after ANY non-OLS estimator silently returns the
    // WRONG number: confirmed against the D5c/D5d oracle itself,
    // `margins, at(income=13000)` after `logit high income unemp` returns
    // this engine's 0.41098... vs Stata's real logistic-link 0.4074234 (a
    // ~1% relative gap, not rounding noise) — a "silently wrong" shape
    // this codebase's honest-reject doctrine forbids shipping (see e.g.
    // BS-023/BS-052). The SAME mechanism (raw --y/--x OLS refit) means
    // this is equally wrong after tobit/heckman/qreg/mediate (non-OLS
    // point estimates), newey/prais (HAC/AR1 transforms), iv/liml/reg3
    // (2SLS/GMM/SUR), and xtreg's random-effects GLS path (lm.cobraCmd
    // =="xtreg" — NOT the ,fe path, which routes through cobraCmd=="feols"
    // and is genuinely safe, see below) — none of these estimators'
    // actual fitted coefficients equal a plain OLS refit of y on x.
    // kAtSafeCmds is exactly the set of lm.cobraCmd values for which the
    // engine's OLS-refit IS the model's own true coefficients (plain
    // regress/feols, including reghdfe and xtreg's ,fe path, both of
    // which already share cobraCmd=="feols"/"ols"/"regress"/"reg"/"r" per
    // runXtreg (estimation/panel_estimation.cpp)/runRegress (estimation/linear_estimation.cpp)) — everything else honest-
    // rejects rather than silently answer a different question than the
    // one the user's `at()` call asked.
    static const std::set<std::string> kAtSafeCmds = {"feols", "ols", "regress", "reg", "r"};
    // Hoisted out of the dydx+at() branch below so the plain at() branch
    // (BUG-188-nonOLS mirror, further down) can test the same set — both
    // branches route the identical GLM family to cobra's real fitted-model
    // margins machinery instead of the OLS-refit path.
    static const std::set<std::string> kDydxAtModels = {"logit", "probit", "poisson", "fepois",
                                                        "ppmlhdfe", "xtpois", "xtlogit",
                                                        "cloglog"};  // BUG-1547
    // P7: `margins, dydx(...) at(...)` is a different request from the
    // predictive margin BS-086 describes above, and the engine gained a real
    // branch for it — the model is fitted once, then the AME is taken over a
    // design copy with the at() variables fixed counterfactually. That branch
    // reuses the ACTUAL fitted model, so the OLS-refit hazard the note above
    // documents does not apply to it, and the GLM estimators are supported.
    if (dydxVal.has_value() && !dydxVal->empty()) {
      if (!kDydxAtModels.count(lm.cobraCmd)) {
        return engineErrorBlockM4(
            "margins, dydx(...) at(...) is not supported after `" + lm.cobraCmd + "` in this "
            "version of PIET (the combined marginal-effects-at-a-point path covers the "
            "logit/probit/poisson family).",
            "unsupported_option");
      }
      for (const auto& spec : parseAtNumlistM4(*atVal)) {
        args.push_back("--at");
        args.push_back(floatAtSpecM4(spec));
      }
      args.push_back("--dydx");
      args.push_back(joinCommaM4(splitTokensM4(*dydxVal)));
      args.push_back("--model");
      args.push_back(lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "cloglog"
                         ? lm.cobraCmd
                         : std::string("poisson"));
      pushFitAsisM4(args, lm);
      if (auto refused = pushGlmAtVceM4(args, lm, "dydx(...) at(...)")) return *refused;
      // No --data here: this handler's shared tail runs against the RESIDENT
      // frame, which is the data source, and the serve path rejects an argv
      // that names --data or --out.
      std::vector<std::string> finalArgs = args;
      finalArgs.push_back("--tidy");
      ojson raw;
      try {
        raw = runCobraJsonM4(finalArgs);
      } catch (const CobraError& ce) {
        return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
      }
      ojson block = cobraToEstimationM4(raw, "margins", &session);
      // session.lastMargins/lastMarginsDydx feed marginsplot — publish the
      // `post` e() AFTER them, so marginsplot keeps working post-`post`.
      session.lastMargins = raw;
      // FU-179: r() is replaced. r(b)/r(V)/r(table) carry one column per effect
      // and point, the effects in the order their variables enter e(b), each
      // over the points: `x:1bn._at x:2._at` (w2v_margins_siblings.log:2894-2921
      // F_robust_dydx_at_po_cols/_b/_v/_rt), V across points from the engine's
      // Jacobians. A factor effect's matrix also carries the base level's column
      // (b 0, V 0), which no engine row represents; such a form posts r(N) only,
      // as the AME branch below does.
      {
        MarginsRInfoM4 info;
        info.dydx = true;
        if (dydxVal->find('*') == std::string::npos)  // dydx(*): expansion not reproduced
          for (const auto& v : splitTokensM4(*dydxVal))
            info.xvars += (info.xvars.empty() ? "" : " ") + v;
        info.atPoints = static_cast<int>(parseAtNumlistM4(*atVal).size());
        info.cluster = !lm.cluster.empty();
        info.fit = fitInfo;  // BUG-1449
        info.kAt = 1;
        info.title = marginsTitleM4(true, false, *atVal, std::string(), fitInfo.ebNames);
        info.nKnown = true;
        // Flat engine order: point by point, the effects in --dydx order within each.
        struct Cell {
          std::string var;
          size_t point = 0;
          size_t flat = 0;
          ojson row;
        };
        std::vector<Cell> cells;
        bool factorEffect = false, shapeOk = raw.contains("equations") && raw["equations"].is_array();
        size_t flat = 0;
        if (shapeOk) {
          const ojson& eqs = raw["equations"];
          for (size_t pnt = 0; pnt < eqs.size(); ++pnt) {
            const ojson& rows = eqs[pnt].contains("coeftable") ? eqs[pnt]["coeftable"] : ojson::array();
            for (const auto& row : rows) {
              const std::string name = row.value("name", std::string());
              const size_t dot = name.find('.');
              if (dot != std::string::npos && dot > 0 &&
                  std::isdigit(static_cast<unsigned char>(name[0])))
                factorEffect = true;
              cells.push_back(Cell{name, pnt, flat++, row});
            }
          }
          shapeOk = raw.contains("V") && raw["V"].is_array() && raw["V"].size() == flat && flat > 0;
        }
        if (shapeOk) info.continuous = factorEffect ? 0 : 1;
        if (shapeOk && !factorEffect) {
          auto fitIndex = [&](const std::string& var) {
            if (!lm.result.has_value() || !lm.result->contains("coeftable") ||
                !(*lm.result)["coeftable"].is_array())
              return static_cast<size_t>(0);
            const ojson& fitCt = (*lm.result)["coeftable"];
            for (size_t j = 0; j < fitCt.size(); ++j)
              if (coefBelongsToVarM4(fitCt[j].value("name", std::string()), var)) return j;
            return fitCt.size();
          };
          std::stable_sort(cells.begin(), cells.end(), [&](const Cell& x, const Cell& y) {
            const size_t fx = fitIndex(x.var), fy = fitIndex(y.var);
            if (fx != fy) return fx < fy;
            if (x.var != y.var) return x.var < y.var;
            return x.point < y.point;
          });
          const bool single = raw["equations"].size() == 1;
          std::vector<std::string> names;
          std::vector<double> bv;
          ojson tableRows = ojson::array();
          for (const auto& c : cells) {
            names.push_back(single ? c.var
                                   : c.var + ":" + std::to_string(c.point + 1) +
                                         (c.point == 0 ? "bn" : "") + "._at");
            bv.push_back(marginsNumM4(c.row, "b"));
            ojson tr;
            tr["name"] = names.back();
            for (const char* key : {"b", "se", "t", "z", "p", "ci_lo", "ci_hi"})
              if (c.row.contains(key)) tr[key] = c.row[key];
            tableRows.push_back(tr);
          }
          std::vector<std::vector<double>> V(cells.size(), std::vector<double>(cells.size(), 0.0));
          bool vOk = true;
          for (size_t i = 0; i < cells.size() && vOk; ++i) {
            const ojson& vr = raw["V"][cells[i].flat];
            vOk = vr.is_array() && vr.size() == flat;
            for (size_t j = 0; j < cells.size() && vOk; ++j)
              V[i][j] = vr[cells[j].flat].is_number() ? vr[cells[j].flat].get<double>()
                                                      : std::numeric_limits<double>::quiet_NaN();
          }
          if (vOk) {
            postMarginsRClassM4(session, block, info, names, bv, V, tableRows);
          } else {
            postMarginsRClassM4(session, block, info, {}, {}, {});
          }
        } else {
          postMarginsRClassM4(session, block, info, {}, {}, {});
        }
      }
      if (postOpt) postEstimationBlockM4(session, block, "margins", lm.depvar, lm.dataPath);
      return block;
    }
    // BUG-188-nonOLS: mirror of the dydx+at() branch above. `margins, at(...)`
    // with no dydx() is a predictive margin, and cobra's GLM predictive-margins
    // branch (apps/cmds/cmds_postest.cpp, `--at .. --model ..` with no --dydx)
    // already fits logit/probit/poisson for real and computes the average
    // adjusted prediction correctly (BUG-194's own oracle table exercises this
    // exact path) — the OLS-refit hazard the reject below exists for does not
    // apply to this family, so forward --model instead of rejecting.
    if (kDydxAtModels.count(lm.cobraCmd)) {
      args.push_back("--model");
      args.push_back(lm.cobraCmd == "logit" || lm.cobraCmd == "probit" || lm.cobraCmd == "cloglog"
                         ? lm.cobraCmd
                         : std::string("poisson"));
      pushFitAsisM4(args, lm);
      if (auto refused = pushGlmAtVceM4(args, lm, "at(...)")) return *refused;
    } else if (!kAtSafeCmds.count(lm.cobraCmd)) {
      return engineErrorBlockM4(
          "margins, at(...) is not supported after `" + lm.cobraCmd + "` in this version of PIET "
          "— the engine's predictive-margins machinery always re-estimates a plain OLS regression "
          "from the model's y/x (and fe, if any) rather than reusing the model actually fit, and "
          "completely ignores any --model flag in this mode (live-verified) — silently wrong for "
          "any non-OLS estimator. Only `regress`/`feols`/`reghdfe`/`xtreg ,fe` are supported for "
          "margins, at(...) in this version of PIET.",
          "unsupported_option");
    }
    // BUG-188 (P2-PERF ARC-P). This split at() on whitespace and commas, which
    // is exactly the bug P7 already fixed for the dydx+at branch: it turns
    // `at(x1=(0 1))` into the two nonsense fragments `x1=(0` and `1)` and the
    // engine answers "'x1=(0' has a non-numeric value". The parser that
    // understands Stata's real at() grammar — including the parenthesised
    // numlist and the cartesian product across several variables — is
    // parseAtNumlistM4, sitting a few hundred lines up and used by the other
    // branch. Use it here too rather than keep a second, worse grammar.
    //
    // A bare single-value at() (`at(x1=3)`, `at(x1=3 x2=4)`) produces exactly
    // the same argv it did before, so the previously-working spellings are
    // untouched.
    for (const auto& spec : parseAtNumlistM4(*atVal)) {
      args.push_back("--at");
      args.push_back(floatAtSpecM4(spec));
    }
    // Reached only by the least-squares family (atmeans after any other fit was
    // refused above): the covariates at() does not set are held at their
    // float-stored means rather than their exact ones.
    if (atmeansOpt) args.push_back("--atmeans");
    // BS-086: forward the SAME vcov/cluster context the original fit used
    // (cobra margins' own usage banner: `predictive margins (linear): ...
    // [--vcov ..] [--cluster C]`) — safe here specifically because
    // kAtSafeCmds (above) is exactly the set of estimators for which the
    // engine's internal OLS refit reproduces the ORIGINAL fit's own
    // coefficients; the reported SE should likewise reflect whatever
    // vce() the original model specified rather than silently reverting
    // to iid.
    // Only for the OLS-refit family: the GLM branches (kDydxAtModels, just
    // above) spell the fit's VCE `--vce`/`--cluster` (pushGlmAtVceM4).
    if (!kDydxAtModels.count(lm.cobraCmd)) {
      if (!lm.vcov.empty() && lm.vcov != "iid") {
        args.push_back("--vcov");
        args.push_back(lm.vcov);
      }
      if (!lm.cluster.empty()) {
        args.push_back("--cluster");
        args.push_back(joinCommaM4(lm.cluster));
      }
      // FU-385: after `regress ..., vce(cluster a b)` the refit runs the fit's
      // multiway convention, not reghdfe's Gmin factor (main served the Gmin
      // margin SE at rc 0). --keep-nested-fe-dof never reaches here: areg with a
      // cluster refuses above (postestRefitMatchesFitM4).
      for (const auto& f : fitConventionFlagsM4(lm))
        if (f == "--multiway-subset-factors" || f == "--keep-singletons" ||
            f == "--collinear-forward")
          args.push_back(f);
    }
  } else {
    // BS-063/BS-064 (BETA D1): "iv"/"reg3" were already anticipated here
    // (pre-dating this batch); "liml" is this batch's new lm.cobraCmd value
    // and needs the SAME linear-model dydx reject — without it, `ivregress
    // liml ...` followed by `margins, dydx(*)` would fall into the ELSE
    // branch below (kModelMap lookup misses "liml" too, so no --model flag
    // either) and silently run cobra's AME machinery over what is actually
    // a linear IV fit. "heckman"/"mediate" are this batch's (BETA D2, BS-070/
    // 072) additions: heckman's outcome equation is itself a plain linear
    // fit (dydx of a linear coefficient is a meaningless constant, same
    // rationale as tobit/qreg above), and margins has no coherent meaning
    // at all after `mediate` (lm.x is a best-effort {mediator, treatment}
    // placeholder, not a real regression spec) — both need the SAME reject
    // rather than falling to the ELSE branch's silent AME computation.
    // AER-B4 margins-dydx-OLS (2026-09-09) closes the half of this reject the oracle
    // contradicts. tests/eval/golden/aer_b4/pins_oracle_r.do:75-79 fits
    // `quietly regress price mpg weight` then `margins, dydx(mpg) post` and
    // Stata answers rc 0 — PIET refused the combination outright. The fix is
    // NOT to drop feols/regress from kLinearCmds and let this branch's engine
    // call handle them: cmd_margins (apps/cmds/cmds_postest.cpp:929) reads
    // `opt(a, "--model", "logit")` and its AME path computes the
    // logit/probit/poisson family only, so a continuous outcome like `price`
    // sent with no --model would be answered through a LOGISTIC AME — the
    // silent wrong number Doctrine gate ① forbids, strictly worse than the
    // honest reject. buildLinearAmeRawM4 (above) instead reads the answer off
    // the fitted model's own coefficient table, where a linear model's dy/dx
    // already sits, and never calls the engine.
    //
    // kLinearCmds therefore keeps all 15 names, and the OLS family is
    // diverted BEFORE it — only for a `dydx()` request. A `margins` with NO
    // dydx() after a linear model is a different question (the unconditional
    // predictive margin over the sample, which is not any coefficient), and
    // this engine still has no path to it, so it keeps refusing. Narrowing
    // kLinearCmds instead would have dropped that case through to the
    // kModelMap miss below and answered it as a logit.
    static const std::set<std::string> kLinearAmeCmds = {"feols", "ols", "regress", "reg", "r"};
    // Hoisted above the rejects (it used to sit after the kModelMap lookup) so
    // the linear-AME branch can see which variables were asked for. Parsing is
    // byte-for-byte the same; nothing between here and its old position reads
    // dydxVars.
    if (dydxVal.has_value() && !dydxVal->empty()) {
      std::string cur;
      for (char c : *dydxVal) {
        if (c == ' ' || c == ',' || c == '\t') {
          if (!cur.empty() && cur != "*") dydxVars.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (!cur.empty() && cur != "*") dydxVars.push_back(cur);
    }
    if (kLinearAmeCmds.count(lm.cobraCmd) && dydxVal.has_value() && !dydxVal->empty()) {
      ojson built;
      ojson reject;
      if (!buildLinearAmeRawM4(lm, dydxVars, built, reject)) return reject;
      // BUG-1549: the fit's rows are t(e(df_r))-referenced; after reghdfe and
      // xtreg, fe margins' are normal-referenced.
      if (marginsNormalRefM4(lm))
        for (auto& row : built["coeftable"]) marginsNormalRowM4(row);
      linearAmeRaw = std::move(built);
    }
    // BUG-473-sibling (item 4, 2026-09-09): the remaining ten names have real
    // structural reasons — iv/liml/reg3's endogenous system, xtreg's
    // random-effects GLS path, and tobit/qreg/heckman/mediate's non-OLS or
    // non-coefficient marginal effects — none of which the coefficient table
    // above answers, and none of which the engine's GLM AME answers either.
    static const std::set<std::string> kLinearCmds = {
        "feols",  "ols",     "regress", "reg",    "r",      "iv",       "liml",
        "reg3",   "xtreg",   "newey",   "prais",  "tobit",  "qreg",     "heckman",
        "mediate",
    };
    // FU-326: an option this build will not take is r(198); the default
    // engine_error code made `display _rc` 1 here (after tobit/qreg/...).
    if (!linearAmeRaw.has_value() && kLinearCmds.count(lm.cobraCmd)) {
      return engineErrorBlockM4(
          "margins: marginal effects (dydx) after a linear model are not supported. "
          "Use predictive margins, e.g. `margins, at(mpg=20)`.",
          "unsupported_option");
    }
    static const std::map<std::string, std::string> kModelMap = {
        {"logit", "logit"}, {"probit", "probit"}, {"fepois", "poisson"},
        {"poisson", "poisson"}, {"xtpois", "poisson"}, {"xtlogit", "logit"},
        {"cloglog", "cloglog"},  // BUG-1547
    };
    // BUG-702: a cobraCmd missing from kModelMap used to be sent with no
    // --model at all, and cmd_margins answers that through its `--model logit`
    // default. After `nbreg cnt x1 x2` that was rc 1 with a "not binary"
    // engine message; after `oprobit` on a 0/1 outcome it was a logit AME
    // (-.0012381763, where `probit` on the same data gives -.0012386098) at
    // rc 0. The same honest refusal the at() branches above give, by name.
    // The linear-AME answer (linearAmeRaw) never reaches the engine, so it
    // needs no --model and is not refused here.
    auto mit = kModelMap.find(lm.cobraCmd);
    if (mit != kModelMap.end()) {
      args.push_back("--model");
      args.push_back(mit->second);
      pushFitAsisM4(args, lm);
    } else if (!linearAmeRaw.has_value()) {
      return engineErrorBlockM4(
          "margins is not supported after `" + lm.cobraCmd + "` in this version of PIET "
          "(the engine's marginal-effects path covers the logit/probit/poisson family "
          "and the linear dydx() above; any other model would be answered as a logit).",
          "unsupported_option");
    }
    // W:P5: `--dydx` now takes a VALUE on the engine's CLI (it selects which
    // variables get an effect, and drives the interaction chain rule and the
    // factor discrete-change contrast). It used to be pushed as a bare flag
    // with the selection applied only as a post-hoc row filter here — which
    // meant the next argv element (`--tidy`) was swallowed as its value.
    // Stata's `dydx(*)` means "every covariate", which is the engine's own
    // behaviour when no `--dydx` is given — so an empty selection sends no
    // flag rather than forwarding a literal `*` the engine would reject as
    // an unknown variable name.
    if (!dydxVars.empty()) {
      args.push_back("--dydx");
      args.push_back(joinCommaM4(dydxVars));
    }

    // W:P5: the AME must be computed under the SAME weights and VCE the
    // model was fitted with — Stata's margins reports `Model VCE: Robust`
    // after a clustered pweighted logit, and the delta-method SE is built
    // from that VCE. Forwarding these was the difference between
    // dy/dx x1 = .293147 (wrong) and .2717222 (Stata) on the P5 answerbook.
    // BUG-1547: the engine's AME takes a pweight only (an fweighted logit/probit
    // goes to the grid, which has no cloglog link), so a cloglog fit under another
    // weight is refused by name, not with the engine's rc 1.
    if (lm.cobraCmd == "cloglog" && !lm.fitWeight.empty() && lm.fitWtype != "pw" &&
        lm.fitWtype != "pweight")
      return marginsRefusalM4("margins after a [" + lm.fitWtype + "=" + lm.fitWeight +
                                  "]-weighted `cloglog` fit is not implemented in this version "
                                  "of PIET; nothing was computed.",
                              198);
    if (!lm.fitWeight.empty()) {
      args.push_back("--weight");
      args.push_back(lm.fitWeight);
      if (!lm.fitWtype.empty()) {
        args.push_back("--wtype");
        args.push_back(lm.fitWtype);
      }
    }
    if (!lm.vcov.empty() && lm.vcov != "iid") {
      args.push_back("--vce");
      // lm.vcov carries the engine's linear spelling ("hc1"); the GLM
      // margins path names the same shape "robust", as cobra's own
      // logit/probit CLI does.
      args.push_back(lm.vcov == "hc1" ? std::string("robust") : lm.vcov);
    }
    if (!lm.cluster.empty()) {
      args.push_back("--cluster");
      args.push_back(joinCommaM4(lm.cluster));
    }
  }

  ojson raw;
  const bool linearAme = linearAmeRaw.has_value();
  if (linearAme) {
    // AER-B4 margins-dydx-OLS: the linear-AME branch already holds the whole answer, read off
    // the fitted model's own coefficient table — there is no engine call to
    // make, and `args` (built for one) is discarded. Everything below runs on
    // this payload unchanged: marginsplot caching, the dydx row filter, `post`.
    raw = std::move(*linearAmeRaw);
  } else {
    args.push_back("--tidy");
    try {
      raw = runCobraJsonM4(args);
    } catch (const CobraError& ce) {
      return engineErrorBlockM4(cobraErrorStrM4(ce));
    }
  }
  // 1.5.0 marginsplot (dispatcher.py's session.set_last_margins): cache the
  // RAW cobra JSON (not the formatted estimation block) + the requested
  // dydx var(s) so `marginsplot` can plot it without re-running cobra.
  session.lastMargins = raw;
  session.lastMarginsDydx = dydxVars;

  ojson est = cobraToEstimationM4(raw, "margins_cmd", &session);
  if (!dydxVars.empty() && est.contains("coeftable") && est["coeftable"].is_array()) {
    std::set<std::string> dydxSet(dydxVars.begin(), dydxVars.end());
    ojson filtered = ojson::array();
    for (const auto& row : est["coeftable"]) {
      if (!row.contains("name")) continue;
      const std::string name = row["name"].get<std::string>();
      // W:P5: a FACTOR variable's effect row is named `<level>.<var>`
      // (BUG-101), but the user asked for the bare `<var>` — `margins,
      // dydx(x1 treated)` must keep the `1.treated` row. Matching only the
      // literal name silently dropped every factor effect from the table
      // while the engine had computed it correctly.
      bool wanted = dydxSet.count(name) > 0;
      if (!wanted) {
        const size_t dot = name.find('.');
        if (dot != std::string::npos && dot + 1 < name.size()) {
          wanted = dydxSet.count(name.substr(dot + 1)) > 0;
        }
      }
      if (wanted) filtered.push_back(row);
    }
    // Stata posts the effects in the order the variables enter e(b), not the
    // dydx() order (w2v_margins_siblings C2_dydx_tr_x_cols `x 0b.treatment
    // 1.treatment` for dydx(treatment x), C7_dydx_c1x_cols `x c1` for
    // dydx(c1 x)); the engine's AME answers in --dydx order. The grid
    // (posthest.h margins_grid) and buildLinearAmeRawM4 already use the fit's.
    if (lm.result.has_value() && lm.result->contains("coeftable") && (*lm.result)["coeftable"].is_array()) {
      const ojson& fitCt = (*lm.result)["coeftable"];
      auto fitIndex = [&](const ojson& row) {
        const std::string var = coefSourceVarM4(row.value("name", std::string()));
        for (size_t j = 0; j < fitCt.size(); ++j)
          if (coefBelongsToVarM4(fitCt[j].value("name", std::string()), var)) return j;
        return fitCt.size();
      };
      std::vector<std::pair<size_t, ojson>> keyed;
      for (const auto& row : filtered) keyed.emplace_back(fitIndex(row), row);
      std::stable_sort(keyed.begin(), keyed.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      filtered = ojson::array();
      for (auto& kr : keyed) filtered.push_back(std::move(kr.second));
    }
    est["coeftable"] = filtered;
  }
  // FU-179: r(b)/r(V)/r(N) from the rows the user sees -- after the dydx filter
  // above, so r(b) holds exactly the effects asked for. A FACTOR effect row
  // (`2.t`) is the exception: Stata's matrix also carries the base level as a
  // column of its own (b 0, V 0; w2_glm_lifecycle.log:1663 gam_cl_margins
  // column 1 after `dydx(g)`), which no row here represents, so el(r(b),1,1)
  // would read the 2.t effect where Stata reads 0. Until that layout (and its
  // order beside continuous effects) is pinned for r(), such a form replaces
  // r() with r(N) only and r(b) stays honestly not set.
  bool factorEffect = false;
  if (est.contains("coeftable") && est["coeftable"].is_array()) {
    for (const auto& row : est["coeftable"]) {
      const std::string name = row.value("name", std::string());
      const size_t dot = name.find('.');
      if (dot == std::string::npos || dot == 0) continue;
      bool digits = true;
      for (size_t c = (name[0] == '-' ? 1 : 0); c < dot; ++c)
        if (!std::isdigit(static_cast<unsigned char>(name[c]))) digits = false;
      if (digits && !(name[0] == '-' && dot == 1)) factorEffect = true;
    }
  }
  MarginsRInfoM4 info;
  info.leastSquares = leastSquares && !marginsNormalRefM4(lm);  // BUG-1549
  info.dydx = hasDydx;
  if (hasDydx && !factorEffect && dydxVal->find('*') == std::string::npos) {
    // In e(b) order, as the effects (StataBE 19.5 s4.do `dydx(x2 x1)` after
    // regress and after logit: r(xvars) "x1 x2"); the rows above are sorted so.
    std::string xv;
    if (est.contains("coeftable") && est["coeftable"].is_array())
      for (const auto& row : est["coeftable"])
        xv += (xv.empty() ? "" : " ") + row.value("name", std::string());
    info.xvars = xv;  // not for dydx(*): Stata's expansion is not reproduced
  }
  info.atPoints = hasAt ? static_cast<int>(parseAtNumlistM4(*atVal).size()) : (atmeansOpt ? 1 : 0);
  info.cluster = !lm.cluster.empty();
  // BUG-1449
  info.fit = fitInfo;
  info.kAt = hasAt || atmeansOpt ? 1 : 0;
  info.title = marginsTitleM4(hasDydx, atmeansOpt, hasAt ? *atVal : std::string(), std::string(),
                              fitInfo.ebNames);
  info.nKnown = !atmeansOpt;
  if (hasDydx) info.continuous = factorEffect ? 0 : 1;
  LinearAmeRM4 lin;
  const bool linR = linearAme && linearAmeRM4(session, fitInfo, dydxVars, est, lin);
  if (linR) {
    // r(xvars) is the columns (s1.do dydx(c) "0b.c 1.c 2.c 3.c", dydx(*) "x1 x2
    // 0b.c 1.c 2.c 3.c") and r(b)'s row is `y1` after a factor dydx().
    info.xvars.clear();
    for (const auto& nm : lin.names) info.xvars += (info.xvars.empty() ? "" : " ") + nm;
    if (lin.factor) info.bRow = "y1";
    info.jacobian = lin.J;
    postMarginsRClassM4(session, est, info, lin.names, lin.b, lin.V, lin.tableRows);
  } else if (factorEffect) {
    postMarginsRClassM4(session, est, info, {}, {}, {});
  } else {
    std::vector<std::vector<double>> crossV;
    if (!linearAme && hasDydx && !hasAt && marginsAmeCrossVM4(session, lm, est, crossV)) {
      ojson withV = raw;
      withV["V"] = crossV;
      postMarginsFromBlockM4(session, est, withV, info);
    } else {
      postMarginsFromBlockM4(session, est, raw, info);
    }
  }
  // Item 4, `post`: published AFTER the dydxVars coeftable filter above, so
  // e(b)/_b[x] resolve from the FILTERED table (the margin rows the user
  // asked for), not from every row cobra returned. Also after
  // lastMargins/lastMarginsDydx (just above), so marginsplot keeps working.
  if (postOpt) {
    postEstimationBlockM4(session, est, "margins", lm.depvar, lm.dataPath);
    if (linR) {
      // e(b)/e(V) are r(b)/r(V) (BUG-1237, StataBE `margins, dydx(c) post`:
      // e(b) `0b.c 1.c 2.c 3.c`, el(e(b),1,1) 0, row `y1`).
      eval::EvalMatrix eb, eV;
      eb.rownames = {"y1"};
      eb.colnames = lin.names;
      eb.data.push_back(lin.b);
      eV.rownames = lin.names;
      eV.colnames = lin.names;
      eV.data = lin.V;
      for (auto* m : {&eb, &eV}) {
        const std::string nm = m == &eb ? "b" : "V";
        bool replaced = false;
        for (auto& p : session.eval.eMatrices)
          if (p.first == nm) {
            p.second = *m;
            replaced = true;
          }
        if (!replaced) session.eval.eMatrices.emplace_back(nm, *m);
      }
    }
  }
  return est;
}

// ===========================================================================
// predict


}  // namespace piet::handlers
