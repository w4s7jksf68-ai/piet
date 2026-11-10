// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Hypothesis tests.
#include "piet/handlers.h"
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
#include "estimation_internal.h"
#include "estimators/postest_names.h"  // parse_level_ref (the engine's one resolver)
#include "estimators/posthest.h"  // margins_grid / margins_parse_at (header-inline)

#include "postestimation_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace postest_detail {}
using namespace postest_detail;
using namespace postestimation_vocab;

namespace postest_detail {

// Stata-style glob for a coefficient NAME: `*` any run, `?` one CHARACTER (a
// UTF-8 sequence, not a byte: Unicode names). The shared iterative matcher.
bool globMatchM4(const std::string& pat, const std::string& str) {
  return strutil::globMatch(pat, str);
}

// Does coefficient `name` belong to variable `var`? Two shapes, because that is
// how our coeftable actually names things (measured, not assumed): a continuous
// regressor is its own name (`beertax`), and a factor level is `<level>.<var>`
// (`1983.dyear`) — the same spelling Stata uses.
bool coefBelongsToVarM4(const std::string& name, const std::string& var) {
  if (name == var) return true;
  if (name.size() <= var.size() + 1) return false;
  if (name.compare(name.size() - var.size(), var.size(), var) != 0) return false;
  if (name[name.size() - var.size() - 1] != '.') return false;
  // Everything before the dot must be the level value (digits, or a negative /
  // decimal level), never another variable's name.
  for (size_t k = 0; k + var.size() + 1 < name.size(); ++k) {
    char c = name[k];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.') return false;
  }
  return true;
}

// The VARIABLE a coefficient came from: `beertax` -> "beertax",
// `1983.dyear` -> "dyear". Stata's `testparm x*` globs VARIABLE names and then
// tests every coefficient those variables produced, so the glob has to be
// matched here and not against the printed coefficient label.
std::string coefSourceVarM4(const std::string& name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return name;
  for (size_t k = 0; k < dot; ++k) {
    char c = name[k];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '.') return name;
  }
  return name.substr(dot + 1);
}

bool isInterceptNameM4(const std::string& n) {
  return n == "(intercept)" || n == "_cons" || n == "cons";
}

}  // namespace

ojson runTestparm(const std::vector<std::string>& varlist, SessionState& session) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  {  // BUG-203 — the same estimator gate `test` carries; testparm IS `test`.
    ojson reject;
    if (!postestEstimatorOkM4("testparm", lm, reject)) return reject;
  }
  if (varlist.empty())
    return engineErrorBlockM4("testparm: varlist required", "syntax_error");

  // Coefficient names come from the stored fit, in coeftable order, so the
  // constraint list reads in the order the user saw the model printed.
  std::vector<std::string> coefs;
  if (lm.result.has_value() && lm.result->contains("coeftable") &&
      (*lm.result)["coeftable"].is_array()) {
    for (const auto& row : (*lm.result)["coeftable"]) {
      std::string nm = row.value("name", std::string());
      if (!nm.empty()) coefs.push_back(nm);
    }
  }
  if (coefs.empty())
    return engineErrorBlockM4(
        "testparm: the last estimation exposes no coefficient names to expand against",
        "unimplemented");

  std::vector<std::string> picked;
  auto addOnce = [&picked](const std::string& n) {
    for (const auto& q : picked)
      if (q == n) return;
    picked.push_back(n);
  };

  for (const std::string& rawTok : varlist) {
    std::string tok = trimM4(rawTok);
    if (tok.empty()) continue;
    // `i(numlist).var` — the level-restricted factor spelling (paper 077's
    // `testparm i(2/4).topicn`; w2c_postest arms testparm_i23_c / testparm_i24_g):
    // exactly the coefficients of the listed levels of `var`. The numlist takes
    // `a/b` ranges and space/comma separated levels. Levels the model has no
    // coefficient for (the base, an absent level) are simply not tested; a list
    // that selects nothing is refused below.
    {
      static const std::regex reLevelRange(R"(^i\(([^()]*)\)\.([A-Za-z_]\w*)$)");
      std::smatch lm2;
      if (std::regex_match(tok, lm2, reLevelRange)) {
        std::vector<double> lv;
        std::string nl = lm2.str(1);
        for (char& ch : nl)
          if (ch == ',') ch = ' ';
        std::istringstream in(nl);
        std::string item;
        bool bad = false;
        while (in >> item) {
          const size_t slash = item.find('/');
          try {
            if (slash == std::string::npos) {
              lv.push_back(std::stod(item));
            } else {
              const double lo = std::stod(item.substr(0, slash));
              const double hi = std::stod(item.substr(slash + 1));
              if (hi < lo || hi - lo > 1e6) { bad = true; break; }
              for (double v = lo; v <= hi; v += 1.0) lv.push_back(v);
            }
          } catch (...) {
            bad = true;
            break;
          }
        }
        if (bad || lv.empty())
          return engineErrorBlockM4("testparm: invalid numlist in `" + tok + "'",
                                    "syntax_error");
        const size_t before = picked.size();
        for (const std::string& c : coefs) {
          double lvl = 0;
          std::string var;
          if (!cecon::postest::parse_level_ref(c, lvl, var) || var != lm2.str(2)) continue;
          if (std::find(lv.begin(), lv.end(), lvl) != lv.end()) addOnce(c);
        }
        if (picked.size() == before)
          return engineErrorBlockM4(
              "testparm: `" + tok + "' matches no coefficient in the last estimation",
              "bad_input");
        continue;
      }
    }
    // `i.`/`c.`/`ib#.`/`o.` prefixes name the SOURCE variable; the coefficients
    // they produce are what gets tested, so strip the prefix and match the rest.
    std::string bare = tok;
    for (const char* pfx : {"i.", "c.", "o.", "ibn."}) {
      size_t plen = std::strlen(pfx);
      if (bare.size() > plen && bare.compare(0, plen, pfx) == 0) {
        bare = bare.substr(plen);
        break;
      }
    }
    const bool isGlob = bare.find('*') != std::string::npos || bare.find('?') != std::string::npos;
    size_t before = picked.size();
    for (const std::string& c : coefs) {
      // The intercept is never swept in by a pattern — Stata's testparm tests
      // the named parameters, and `_cons` has to be asked for by name.
      if (isGlob) {
        // Stata's testparm globs VARIABLE names: `dyear*` -> every `NNNN.dyear`,
        // and `*1*` -> `x1` but NOT the level `1.c` (w2c_postest arm
        // testparm_wildcard_substr: df 1, F 1046.48 — matching the printed level
        // names too swept in 1.c and 1.g, df 3). The printed name is matched only
        // when the pattern itself spells a level (`198*.dyear`).
        const bool patternNamesLevel = bare.find('.') != std::string::npos;
        if (!isInterceptNameM4(c) &&
            (globMatchM4(bare, coefSourceVarM4(c)) ||
             (patternNamesLevel && globMatchM4(bare, c))))
          addOnce(c);
      } else if (coefBelongsToVarM4(c, bare)) {
        addOnce(c);
      }
    }
    if (picked.size() == before)
      return engineErrorBlockM4(
          "testparm: `" + tok + "' matches no coefficient in the last estimation",
          "bad_input");
  }

  // Delegate. Everything above is name resolution; the arithmetic, the vcov, the
  // r-class fill and the estimator gate all stay in the one place that owns them.
  std::string joined;
  for (const std::string& c : picked) {
    if (!joined.empty()) joined += ' ';
    joined += c;
  }
  return runTestCmd(joined, session);
}

namespace postest_detail {

// A post-estimation name the model does not have. The engine raises it as
// EngineError("coef_not_found", "<name> not found") — one code for test, testparm
// and lincom, from the one resolver (src/estimators/postest_names.h) — and Stata
// answers the same thing with r(111): `test 9.c` -> "9.c not found", r(111)
// (w2c_postest.log arm test_level_not_in_model). Any other engine failure keeps the
// historical engine_error block.
ojson postestErrorBlockM4(const CobraError& ce) {
  if (ce.code == "coef_not_found")
    return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                  std::nullopt, std::string("r(111)")));
  return engineErrorBlockM4(cobraErrorStrM4(ce), ce.code);
}

// Split Stata's three `test` spellings into one restriction per entry:
//   test coeflist               -> each name is `name = 0`
//   test exp = exp              -> ONE equation (spaces allowed inside it)
//   test (spec) [(spec) ...]    -> each parenthesised spec is either of the above
// Parentheses INSIDE an equation are arithmetic and are kept: the old flattening
// (strip every paren, then split on spaces) turned `2.c = 0.5*(1.c+3.c)` into
// `2.c=0.5*1.c+3.c` — a different restriction, answered silently.
// Returns false (with `err`) on unbalanced parentheses or text between groups.
struct TestItem {
  bool joint;        // true: `name`; false: an equation for --constraint
  std::string text;
};
std::string collapseOpsM4(const std::string& in) {
  // BUG-015: whitespace around = + - * / is not significant.
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '=' || c == '+' || c == '-' || c == '*' || c == '/') {
      while (!out.empty() && out.back() == ' ') out.pop_back();
      out += c;
      while (i + 1 < in.size() && (in[i + 1] == ' ' || in[i + 1] == '\t')) ++i;
    } else {
      out += (c == '\t') ? ' ' : c;
    }
  }
  return trimM4(out);
}
void addTestSpecM4(const std::string& specIn, std::vector<TestItem>& out) {
  const std::string spec = collapseOpsM4(specIn);
  if (spec.empty()) return;
  const size_t eq = spec.find('=');
  if (eq == std::string::npos) {
    std::string cur;
    for (char c : spec) {
      if (c == ' ' || c == ',') {
        if (!cur.empty()) out.push_back({true, cur});
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) out.push_back({true, cur});
    return;
  }
  // `name = 0` stays a joint name, as it always has (the engine answers both the
  // same way; the joint spelling keeps the printed restriction `name = 0`).
  const std::string lhs = trimM4(spec.substr(0, eq));
  const std::string rhs = trimM4(spec.substr(eq + 1));
  bool rhsZero = false;
  try {
    size_t pos = 0;
    const double v = std::stod(rhs, &pos);
    rhsZero = pos == rhs.size() && v == 0.0;
  } catch (...) {
  }
  const bool bareName = !lhs.empty() && lhs.find_first_of(" +-*/()") == std::string::npos;
  if (rhsZero && bareName) out.push_back({true, lhs});
  else out.push_back({false, spec});
}
bool splitTestSpecsM4(const std::string& expr, std::vector<TestItem>& out, std::string& err) {
  const std::string e = trimM4(expr);
  if (e.empty() || e.front() != '(') {
    addTestSpecM4(e, out);
    return true;
  }
  size_t i = 0;
  while (i < e.size()) {
    while (i < e.size() && std::isspace(static_cast<unsigned char>(e[i]))) ++i;
    if (i >= e.size()) break;
    if (e[i] != '(') {
      err = "invalid syntax: text between parenthesised test specifications";
      return false;
    }
    int depth = 0;
    size_t j = i;
    for (; j < e.size(); ++j) {
      if (e[j] == '(') ++depth;
      else if (e[j] == ')' && --depth == 0) break;
    }
    if (j >= e.size()) {
      err = "unmatched '(' in test specification";
      return false;
    }
    addTestSpecM4(e.substr(i + 1, j - i - 1), out);
    i = j + 1;
  }
  return true;
}

// `test ..., accumulate` — SESSION STATE, by design (dispatcher.cpp's `test` arm).
// Stata tests the new restrictions jointly with the ones the previous `test`
// tested (w2c_postest arm test_2c_accumulate: `test 3.c` then `test 2.c, accumulate`
// equals `test 2.c 3.c`, F 6.3024 on (2, 54)). The previous test's restriction set
// is kept here after every successful `test`, REPLACED by the next plain `test` and
// extended by the next `test ..., accumulate`.
//
// Where it lives: a reserved r()-macro key (lead byte \x01, the convention
// sreturn's "\x01s:" established; `return list`/`return clear`/`return add` all skip
// such keys), so it dies with the session and with `clear all`/`clear results`, and
// never shows up as a user r() value. It is BOUND to the estimation it was built
// against by a fingerprint of the live e() block: a set recorded against an earlier
// model is never accumulated onto a new one (Stata's constraints belong to the
// estimation they were tested on).
constexpr const char* kTestAccumKey = "\x01test:accum";
std::string estFingerprintM4(const SessionState& session) {
  if (!session.lastEstBlock.has_value()) return std::string();
  return std::to_string(std::hash<std::string>{}(session.lastEstBlock->dump()));
}
std::vector<TestItem> loadAccumM4(const SessionState& session) {
  std::vector<TestItem> items;
  const std::string* raw = session.eval.rStrings.find(kTestAccumKey);
  if (raw == nullptr) return items;
  try {
    const ojson j = ojson::parse(*raw);
    if (j.value("fp", std::string()) != estFingerprintM4(session)) return items;
    for (const auto& it : j.value("items", ojson::array()))
      if (it.is_array() && it.size() == 2)
        items.push_back({it[0].get<std::string>() == "j", it[1].get<std::string>()});
  } catch (const nlohmann::json::exception&) {
  }
  return items;
}
void storeAccumM4(SessionState& session, const std::vector<TestItem>& items) {
  ojson j;
  j["fp"] = estFingerprintM4(session);
  j["items"] = ojson::array();
  for (const auto& it : items) j["items"].push_back(ojson::array({it.joint ? "j" : "c", it.text}));
  session.eval.rStrings.set(kTestAccumKey, j.dump());
}

// posted-matrix-wald: the live e() is a results set whose e(b)/e(V) PIET posted
// itself and the engine cannot re-fit -- csdid/drdid (cobraCmd "csdid"), or a
// posting with no fitted model behind it (`estat simple, post`, `ereturn post b
// V`). test/lincom/nlcom then read e(b)/e(V) as Stata's test.ado/lincom.ado do
// (w2ag log:1085 ag_asimple_test `test ATT` chi2 102.9610082305245).
bool postedWaldServesM4(const SessionState& session) {
  if (session.eval.findClassMatrix('e', "b") == nullptr ||
      session.eval.findClassMatrix('e', "V") == nullptr)
    return false;
  const auto& lm = session.lastModel();
  if (!lm.has_value()) return session.ePostedLive;
  // Fits whose e(b)/e(V) are PIET's own posting (no engine refit exists):
  // csdid/drdid, acreg (handlers_e3_acreg.cpp; w2j_e3_acreg.log:710 D01 `test x1`),
  // qreg2 and its posted margins (handlers_e3_qreg2.cpp; w2j_e3_qreg2.log T01, M02).
  // bootstrap (handlers_resample.cpp runBootstrap): e(V) is the replicate
  // covariance, which no engine refit reproduces.
  return lm->cobraCmd == "csdid" || lm->cobraCmd == "acreg" || lm->cobraCmd == "suest" ||
         lm->cobraCmd == "qreg2" || lm->cobraCmd == "qreg2_margins" || lm->cobraCmd == "bootstrap";
}

// The engine's `--posted FILE` mode (apps/cmds/cmds_postest.cpp load_posted_model):
// writes {names, b, V, df_r, n} from the live e() and runs `cobra VERB --posted FILE
// <combo>`. e(df_r) present -> the F / t reference, absent -> chi2 / normal.
ojson runPostedPostestM4(const std::string& verb, const std::vector<std::string>& combo,
                         SessionState& session) {
  const eval::EvalMatrix* b = session.eval.findClassMatrix('e', "b");
  const eval::EvalMatrix* V = session.eval.findClassMatrix('e', "V");
  ojson j;
  j["names"] = b->colnames;
  ojson bv = ojson::array();
  for (double x : b->data.empty() ? std::vector<double>() : b->data[0])
    bv.push_back(std::isnan(x) ? ojson(nullptr) : ojson(x));
  j["b"] = bv;
  ojson Vm = ojson::array();
  for (const auto& row : V->data) {
    ojson r = ojson::array();
    for (double x : row) r.push_back(std::isnan(x) ? ojson(nullptr) : ojson(x));
    Vm.push_back(r);
  }
  j["V"] = Vm;
  const double* dfr = session.eval.eScalars.find("df_r");
  j["df_r"] = (dfr != nullptr && std::isfinite(*dfr)) ? ojson(*dfr) : ojson(nullptr);
  // lincom.ado is `version 6.0`, and under it `test` reads the posting's own dof
  // rather than e(df_r): qreg2 records it (w2j_e3_qreg2.log L05 r(df) 201 where
  // e(df_r) is 196; handlers_e3_qreg2.cpp postQregE).
  if (verb == "lincom") {
    const auto& lmNow = session.lastModel();
    if (lmNow.has_value() && lmNow->cobraCmd == "qreg2" && lmNow->result.has_value() &&
        lmNow->result->contains("qreg2_lincom_dof") &&
        (*lmNow->result)["qreg2_lincom_dof"].is_number())
      j["df_r"] = (*lmNow->result)["qreg2_lincom_dof"];
  }
  const double* nObs = session.eval.eScalars.find("N");
  if (nObs != nullptr && std::isfinite(*nObs)) {
    j["n"] = *nObs;
  } else if (session.lastEstBlock.has_value() && session.lastEstBlock->contains("n_obs") &&
             (*session.lastEstBlock)["n_obs"].is_number()) {
    j["n"] = (*session.lastEstBlock)["n_obs"];
  } else {
    j["n"] = 0;
  }
  const std::string path = nextPathM4(session.workdir(), "posted_eb");
  {
    std::ofstream out(path);
    out << j.dump();
  }
  std::vector<std::string> argv = {verb, "--posted", path};
  argv.insert(argv.end(), combo.begin(), combo.end());
  try {
    nlohmann::json raw = runCobraJsonM4(argv);
    cecon::native_unlink(path);
    return ojson::parse(raw.dump());
  } catch (...) {
    cecon::native_unlink(path);
    throw;
  }
}

}  // namespace

ojson runTestCmd(const std::string& exprIn, SessionState& session) {
  return runTestCmd(exprIn, session, /*accumulate=*/false);
}

ojson runTestCmd(const std::string& exprIn, SessionState& session, bool accumulate) {
  const bool posted = postedWaldServesM4(session);
  const auto& lmOpt = session.lastModel();
  if (!posted && !lmOpt.has_value()) return noPreviousEstimationFor(session, true);
  // BUG-203: refuse before any parsing — the OLS-refit answer is not this
  // model's answer for any estimator outside kPostestOlsCmds.
  if (!posted) {
    ojson reject;
    if (!postestEstimatorOkM4("test", *lmOpt, reject)) return reject;
  }

  std::vector<TestItem> items;
  if (accumulate) items = loadAccumM4(session);
  {
    std::vector<TestItem> fresh;
    std::string err;
    if (!splitTestSpecsM4(exprIn, fresh, err)) return engineErrorBlockM4(err, "syntax_error");
    if (fresh.empty())
      return engineErrorBlockM4("test: a coefficient list or an equation is required",
                                "syntax_error");
    items.insert(items.end(), fresh.begin(), fresh.end());
  }

  // Combination-only flags. On §31's handle path the stored fit IS the model,
  // so nothing accumulated here may name it; runPostEstimationM4 prepends
  // lastModelBaseArgv only when it falls back to the full specification.
  // The items go in the order they were typed (each run of names as one --joint):
  // the engine numbers the restrictions that way, as Stata's "( 1)" lines and
  // r(dropped_#) do (BUG-1276: `test (2*x1=0) (x1=0)` drops constraint 2).
  std::vector<std::string> combo;
  for (size_t i = 0; i < items.size();) {
    if (!items[i].joint) {
      combo.push_back("--constraint");
      combo.push_back(items[i++].text);
      continue;
    }
    std::vector<std::string> run;
    for (; i < items.size() && items[i].joint; ++i) run.push_back(items[i].text);
    combo.push_back("--joint");
    combo.push_back(joinCommaM4(run));
  }

  combo.push_back("--tidy");
  ojson raw;
  try {
    raw = posted ? runPostedPostestM4("test", combo, session)
                 : runPostEstimationM4("test", combo, session, *lmOpt);
  } catch (const CobraError& ce) {
    return postestErrorBlockM4(ce);
  }
  ojson blk = cobraToEstimationM4(raw, "test", &session);
  // Restrictions that named only omitted coefficients were dropped by the engine,
  // and every one when R V R' is zero (BUG-1276); the dispatcher publishes r(drop)
  // from the count and captureRFromBlock r(dropped_#) from their numbers.
  if (raw.contains("dropped")) blk["dropped"] = raw["dropped"];
  if (raw.contains("dropped_rows")) blk["dropped_rows"] = raw["dropped_rows"];
  storeAccumM4(session, items);
  return blk;
}

// BUG-333: lincom/nlcom now take the parsed option vector. `level(#)` forwards
// to cobra's new --level (engine half landed this wave); lincom's eform family
// (eform/or/hr/irr/rrr/shr) is a DISPLAY metric applied to the returned block,
// exactly as `logit, or` is — Stata computes the linear combination and its CI
// in the linear metric and exponentiates for display, which is why the SE uses
// the delta factor exp(b) and t/p are left alone.
namespace postest_detail {
const char* eformLabelM4(const std::string& k) {
  if (k == "or") return "odds_ratio";
  if (k == "hr") return "hazard_ratio";
  if (k == "irr") return "incidence_rate_ratio";
  if (k == "rrr") return "relative_risk_ratio";
  if (k == "shr") return "subhazard_ratio";
  return "exp_b";
}
void applyEformM4(ojson& block, const char* metric) {
  if (block.value("type", "") == "error") return;
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) return;
  for (auto& row : block["coeftable"]) {
    if (!row.contains("b") || row["b"].is_null()) continue;
    const double e = std::exp(row["b"].get<double>());
    row["b"] = e;
    if (row.contains("se") && row["se"].is_number()) row["se"] = e * row["se"].get<double>();
    if (row.contains("ci_lo") && row["ci_lo"].is_number())
      row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
    if (row.contains("ci_hi") && row["ci_hi"].is_number())
      row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
  }
  block["coef_metric"] = metric;
}

// Items 4/5 (User ruling 2026-09-09): `margins ..., post` and
// `nlcom ..., post` REPLACE the posted estimation results wholesale --
// e(cmd) becomes "margins"/"nlcom" and _b[x] becomes the margin/combination
// afterwards. Same shape as eststo_esttab.cpp's runEstimatesUse (the
// canonical "replace e() wholesale" precedent) and the SAME order its own
// comment insists on: setLastModel() (republishes e(cmd)/e(depvar)/e(vce)
// and re-points eval.lastModelResult) BEFORE lastEstBlock, then
// estAddedClear() (the estadd bag belongs to the estimate being replaced).
// y/x/fe are left EMPTY on the new LastModel — postestEstimatorOkM4 (below)
// refuses every later lincom/test/predict verb BY NAME for a cobraCmd it
// does not recognize (neither postestOlsCmdsM4() nor postestGlmCmdsM4()
// list "margins"/"nlcom"), which turns a post-post replay into an honest
// named refusal instead of a silent wrong number computed from stale
// y/x/fe — leaving them stale would be exactly the silent-wrong shape this
// codebase's Doctrine gate ① forbids.
//
// KNOWN OPEN GAP (BUG-467, filed 2026-09-09, OPEN): setLastModel does NOT
// clear session.eval.eScalars/eStrings, so an e-scalar/e-string posted by
// an EARLIER model (oaxaca/pca/teffects/estadd) still answers after this
// publish. This helper does not fix that — it is a NEW CALLER of
// setLastModel, one more surface the open bug reaches, not a new instance
// of the bug itself.
void postEstimationBlockM4(SessionState& session, const ojson& block, const std::string& cmdName,
                           const std::string& depvar, const std::string& dataPath) {
  LastModel lm2;
  lm2.cobraCmd = cmdName;
  lm2.stataCmd = cmdName;
  lm2.depvar = depvar;
  lm2.dataPath = dataPath;
  lm2.result = block;
  session.setLastModel(lm2);
  session.lastEstBlock = block;
  session.estAddedClear();
}

// AER-B4 margins-dydx-OLS (2026-09-09) — `margins, dydx(x)` AFTER A PLAIN OLS FIT.
//
// Oracle: tests/eval/golden/aer_b4/pins_oracle_r.do:75-79 (log lines
// 199-224) fits `quietly regress price mpg weight` and asks for
// `margins, dydx(mpg) post`. Stata answers rc 0 with
//   dy/dx -49.51222  std. err. 86.15604  t -0.57  P>|t| 0.567
//   [95% conf. interval] -221.3025 .. 122.278   Number of obs = 74
// and every one of those columns is IDENTICAL to the `mpg` row of that
// regression's own coefficient table (measured on this build:
// b -49.51222066635501, se 86.1560388671846, t -0.5746807921692114,
// p 0.5673237265003643, ci -221.302482965428 .. 122.27804163271796). That is
// not a coincidence: a linear model's dy/dx IS its coefficient — no sample
// average enters the derivative, so the AME does not depend on the data at
// all, and the delta-method variance of a single coefficient is that
// coefficient's own variance. Stata's header says so too ("Model VCE: OLS",
// "Expression: Linear prediction, predict()"), and the t/CI it prints are the
// fit's own t-distribution ones, NOT the normal-approximation pair
// cobraToEstimationM4's BUG-006 AME remap builds for the GLM family.
//
// Why it is NOT enough to drop feols/regress from runMargins' kLinearCmds:
// cmd_margins (apps/cmds/cmds_postest.cpp:929) reads
// `std::string model = opt(a, "--model", "logit");` and its AME path computes
// the logit/probit/poisson family only — with no --model flag a continuous
// outcome like `price` would be pushed through a LOGISTIC AME, a silent wrong
// number strictly worse than the honest reject it replaced (Doctrine gate ①).
// So the answer is built HERE, from the fitted model's own already-published
// coefficient table (LastModel::result — the same payload macros.cpp's
// _b[]/_se[] resolver reads), and the engine is not called at all.
//
// Two shapes stay REFUSED, because for them the coefficient is NOT the
// marginal effect:
//   * an INTERACTED regressor. Measured on this build, `regress price
//     c.mpg##c.mpg weight` names its rows "(intercept)", "mpg", "mpg#mpg",
//     "weight", and `regress price c.mpg#c.weight mpg weight` names one
//     "mpg#weight". Wherever the requested variable appears inside a `#` term
//     the chain rule puts a sample average into dy/dx (AME = b_x +
//     b_{x#z}*mean(z)), so the bare coefficient is wrong. `dydx(*)` refuses
//     when ANY row is interacted, since it asks for every covariate.
//   * a requested name no coefficient belongs to — an empty margins table is
//     a silent no-op, so say which name found nothing.
// A FACTOR regressor IS answered: a `<level>.<var>` row carries the predicted
// difference between that level and the base level, which for a linear model
// is exactly that level's coefficient and its own SE. `<level>.<var>` is the
// spelling coefBelongsToVarM4 (above) and runMargins' own dydx row filter
// already key on, so one predicate covers both.
//
// Fills `out` with a payload shaped like cobra's own `margins --tidy` wire
// (estimator/n_obs/coeftable/coefs, no depvar — measured against the engine's
// GLM AME output) so the caller's shared tail — marginsplot caching, the dydx
// row filter, `post` — runs over it unchanged. Returns false and fills
// `reject` with the honest-refusal block instead when it cannot.
bool buildLinearAmeRawM4(const LastModel& lm, const std::vector<std::string>& dydxVars,
                         ojson& out, ojson& reject) {
  if (!lm.result.has_value() || !lm.result->contains("coeftable") ||
      !(*lm.result)["coeftable"].is_array() || (*lm.result)["coeftable"].empty()) {
    reject = engineErrorBlockM4(
        "margins, dydx(...): the fitted model's coefficient table is not in memory, so its "
        "marginal effects cannot be read from it.",
        "unsupported_option");
    return false;
  }
  const ojson& ct = (*lm.result)["coeftable"];

  auto rowName = [](const ojson& row) {
    return (row.contains("name") && row["name"].is_string()) ? row["name"].get<std::string>()
                                                             : std::string();
  };
  // Does `var` appear as one component of some `#` interaction row?
  auto isInteracted = [&ct, &rowName](const std::string& var) {
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.find('#') == std::string::npos) continue;
      size_t start = 0;
      for (;;) {
        const size_t hash = name.find('#', start);
        const std::string part =
            (hash == std::string::npos) ? name.substr(start) : name.substr(start, hash - start);
        if (coefBelongsToVarM4(part, var)) return true;
        if (hash == std::string::npos) break;
        start = hash + 1;
      }
    }
    return false;
  };
  // `what` names the request (a variable, or "*" for every covariate); `who`
  // names what is actually interacted, so the `*` form reads as a sentence.
  auto interactionReject = [](const std::string& what, const std::string& who) {
    return engineErrorBlockM4(
        "margins, dydx(" + what + ") after a linear model is not supported while " + who +
            " enters an interaction: the marginal effect is then the coefficient plus the "
            "interacted terms weighted by the sample, not the coefficient itself. Fit without "
            "the interaction, or use `margins, at(...)` at the point of interest.",
        "unsupported_option");
  };

  ojson coeftable = ojson::array();
  ojson coefs = ojson::array();
  auto take = [&coeftable, &coefs, &rowName](const ojson& row) {
    coeftable.push_back(row);
    ojson c;
    c["name"] = rowName(row);
    // Mirrors cobra's own AME wire shape, which marginsplot (graph_m4.cpp)
    // reads by `dydx`/`se` — the coeftable copy above is what the block and
    // `post` read.
    c["dydx"] = row.contains("b") ? row["b"] : ojson(nullptr);
    c["se"] = row.contains("se") ? row["se"] : ojson(nullptr);
    c["t"] = row.contains("t") ? row["t"] : ojson(nullptr);
    coefs.push_back(c);
  };

  if (dydxVars.empty()) {
    // `dydx(*)` (and `dydx()` with only a `*` token) — every covariate, which
    // is also what Stata prints, and never the intercept.
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.empty() || isInterceptNameM4(name)) continue;
      if (name.find('#') != std::string::npos) {
        reject = interactionReject("*", "`" + name + "`");
        return false;
      }
      take(row);
    }
  } else {
    for (const auto& var : dydxVars) {
      if (isInteracted(var)) {
        reject = interactionReject(var, var);
        return false;
      }
    }
    // Iterated over the MODEL's row order (not the request's), matching the
    // order the engine's GLM AME path returns its rows in.
    std::vector<bool> matched(dydxVars.size(), false);
    for (const auto& row : ct) {
      const std::string name = rowName(row);
      if (name.empty() || isInterceptNameM4(name)) continue;
      for (size_t i = 0; i < dydxVars.size(); ++i) {
        if (!coefBelongsToVarM4(name, dydxVars[i])) continue;
        matched[i] = true;
        take(row);
        break;
      }
    }
    for (size_t i = 0; i < dydxVars.size(); ++i) {
      if (!matched[i]) {
        reject = engineErrorBlockM4(
            "margins, dydx(" + dydxVars[i] + "): `" + dydxVars[i] +
                "` is not a covariate of the last estimation.",
            "unsupported_option");
        return false;
      }
    }
  }
  if (coeftable.empty()) {
    reject = engineErrorBlockM4(
        "margins, dydx(...): the last estimation has no covariate to take a marginal effect of.",
        "unsupported_option");
    return false;
  }

  out = ojson::object();
  out["estimator"] = "margins";
  if (lm.result->contains("n_obs")) out["n_obs"] = (*lm.result)["n_obs"];
  out["coeftable"] = coeftable;
  out["coefs"] = coefs;
  return true;
}
}  // namespace

ojson runLincom(const std::string& expr, SessionState& session,
                const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const bool posted = postedWaldServesM4(session);  // posted-matrix-wald
  const auto& lmOpt = session.lastModel();
  if (!posted && !lmOpt.has_value()) return noPreviousEstimationFor(session, true);
  if (!posted) {  // BUG-203 — see postestEstimatorOkM4.
    ojson reject;
    if (!postestEstimatorOkM4("lincom", *lmOpt, reject)) return reject;
  }
  ojson raw;
  try {
    std::vector<std::string> argv = {"--exp", expr, "--tidy"};
    for (const auto& [k, v] : opts)
      if (k == "level" && !v.isBool) { argv.push_back("--level"); argv.push_back(v.strValue); break; }
    raw = posted ? runPostedPostestM4("lincom", argv, session)
                 : runPostEstimationM4("lincom", argv, session, *lmOpt);
  } catch (const CobraError& ce) {
    return postestErrorBlockM4(ce);
  }
  ojson blk = cobraToEstimationM4(raw, "lincom", &session);
  // A normal-reference combination (the engine's df 0: an ML fit, or the 2SLS
  // refit without `small`) has no degrees of freedom, and Stata's lincom posts no
  // r(df) then:
  // w2s_multiway_class.log:840 mw_iv_c2_lincom r(df) `.`, w2o_rnames.log:125 N04
  // after logit `.`. A t-reference fit always has a positive dof.
  if (blk.contains("df") && blk["df"].is_number() && blk["df"].get<double>() == 0.0)
    blk["df"] = nullptr;
  // Two eform-family options at once is r(198) in Stata (measured: `lincom unemp,
  // or hr` -> 198, while each alone is rc 0 — including after `regress`, where the
  // exponentiated OLS coefficient is meaningless but Stata still prints it, so we
  // do not second-guess the estimator either).
  std::string metricKey;
  int eformCount = 0;
  bool coef = false;
  for (const auto& [k, v] : opts) {
    if ((k == "eform" || k == "or" || k == "hr" || k == "irr" || k == "rrr" || k == "shr") &&
        v.isBool && v.boolValue) {
      ++eformCount;
      if (metricKey.empty()) metricKey = k;
    }
    if (k == "coef" && v.isBool && v.boolValue) coef = true;
  }
  if (eformCount > 1)
    return engineErrorBlockM4("option `" + metricKey + "` may not be combined with another "
                              "eform option", "unsupported_option");
  // `coef` excludes the eform family (StataBE 19.5 `lincom x1, or coef`: "only one
  // of options or or coef is allowed", r(198)).
  if (coef && eformCount == 1)
    return engineErrorBlockM4("only one of options " + metricKey + " or coef is allowed",
                              "unsupported_option");
  // BUG-1480: after `logistic` lincom reports odds ratios unless `coef` is given --
  // lincom.ado sets `or` when e(cmd) is logistic, whichever eform option was typed
  // (StataBE 19.5, p-est-b/lg2.do and scratch-laneA2/agents/post/l1.do: `lincom x1`
  // "Odds ratio" 4.403289 (1.051442), r(estimate) 4.4032885449567045 = exp(_b[x1]),
  // r(se) 1.0514416968201232, r(lb)/r(ub) the exponentiated interval; `lincom x1,
  // irr` the same "Odds ratio" table; `lincom x1, coef` 1.482352 (.2387856)).
  const std::string* ecmd = session.eval.eStrings.find("cmd");
  if (!coef && ecmd != nullptr && *ecmd == "logistic") {
    metricKey = "or";
    eformCount = 1;
  }
  if (eformCount == 1) applyEformM4(blk, eformLabelM4(metricKey));
  return blk;
}

ojson runNlcom(const std::string& exprIn, SessionState& session,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value()) return noPreviousEstimation();
  const LastModel& lm = *lmOpt;
  {  // BUG-203 — see postestEstimatorOkM4.
    ojson reject;
    if (!postestEstimatorOkM4("nlcom", lm, reject)) return reject;
  }

  std::string raw = trimM4(exprIn);
  // w2c_postest family 5: Stata's `nlcom (name1: exp1) (name2: exp2) ...` takes
  // SEVERAL combinations, each in its own balanced paren pair (paper 077:
  // `nlcom (Info: _b[_cons]) (Click: _b[_cons]+_b[1.clicktreat]) ...`, arm
  // nlcom_multi_nopost_rtable; paper 044's `..., post`, arm nlcom_multi_post).
  // The string splits into groups ONLY when it is nothing but top-level paren
  // groups; a leading paren that is expression grouping (`(_b[x1]+_b[x2])/2`)
  // leaves text after its group, so it stays one expression. One group is Item 5
  // defect (a)'s whole-string wrap (User ruling 2026-09-09): `(r: expr)` is the
  // single labelled combination; freeze/nlcom.json's bare `_b[x1]/_b[x2]` has no
  // leading paren and is untouched.
  std::vector<std::string> specs;
  if (!raw.empty() && raw.front() == '(') {
    size_t i = 0;
    bool allGroups = true;
    std::vector<std::string> groups;
    while (i < raw.size()) {
      while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
      if (i >= raw.size()) break;
      if (raw[i] != '(') { allGroups = false; break; }
      int depth = 0;
      size_t j = i;
      for (; j < raw.size(); ++j) {
        if (raw[j] == '(') ++depth;
        else if (raw[j] == ')' && --depth == 0) break;
      }
      if (j >= raw.size()) { allGroups = false; break; }
      groups.push_back(trimM4(raw.substr(i + 1, j - i - 1)));
      i = j + 1;
    }
    if (allGroups && !groups.empty()) specs = groups;
  }
  if (specs.empty()) specs.push_back(raw);
  std::vector<std::string> exprArgs;
  {
    std::regex lblRe(R"(^(\w+)\s*:\s*(.+)$)");
    for (size_t k = 0; k < specs.size(); ++k) {
      std::string label = "_nl_" + std::to_string(k + 1);
      std::string body = specs[k];
      std::smatch m;
      if (std::regex_match(specs[k], m, lblRe)) {
        label = m[1].str();
        body = trimM4(m[2].str());
      }
      // e10_nlcom (w2g_ebmat.do:648, log:1311-1338): an element of e(b) is a
      // parameter, not a constant. Stata differentiates the combination with
      // respect to it, so `el(e(b),1,k)` and `e(b)[1,k]` give the same estimate
      // and se as `_b[<column k>]` (measured). They are rewritten to that name
      // (the full `eq:name` stripe, as e(b) carries it). e(V) is not a
      // parameter, so `el(e(V),...)` stays a constant and is left.
      static const std::regex ebElement(
          R"(\bel\(\s*e\(\s*b\s*\)\s*,\s*1\s*,\s*([0-9]+)\s*\)|\be\(\s*b\s*\)\s*\[\s*1\s*,\s*([0-9]+)\s*\])");
      if (std::regex_search(body, ebElement)) {
        const std::optional<eval::EvalMatrix> eb =
            session.eval.resolveEMatrix("b", nullptr, nullptr, eval::EvalSession::EReader::kNames);
        std::string translated;
        size_t offset = 0;
        for (std::sregex_iterator it(body.begin(), body.end(), ebElement), end; it != end; ++it) {
          const std::smatch& el = *it;
          const size_t column = std::stoull(el[1].matched ? el.str(1) : el.str(2));
          if (!eb.has_value() || column == 0 || column > eb->colnames.size())
            return engineErrorBlockM4("nlcom: " + el.str(0) + " is not an element of e(b)",
                                      "syntax_error");
          translated += body.substr(offset, static_cast<size_t>(el.position()) - offset);
          translated += "_b[" + eb->colnames[column - 1] + "]";
          offset = static_cast<size_t>(el.position() + el.length());
        }
        body = translated + body.substr(offset);
      }
      exprArgs.push_back("--expr");
      exprArgs.push_back(label + "=" + body);
    }
  }

  // Item 5 defect (b), User ruling 2026-09-09: `post` moved from
  // `unimplemented` to `known` in kVocabNlcom (dispatcher.cpp) — read it
  // here the same way `level` is read just below.
  bool postOpt = false;
  for (const auto& [k, v] : opts) {
    if (k == "post" && (!v.isBool || v.boolValue)) postOpt = true;
  }

  // BS-098/BS-013 (BETA A4): shares lastModelBaseArgv with test/lincom
  // (above, now via runPostEstimationM4's fallback) — nlcom ALSO inherits
  // vcov/cluster, not just fe (previously this hand-rolled the same
  // {data,y,x,fe} skeleton WITHOUT vcov/cluster, duplicating and diverging
  // from lastModelBaseArgv). On the §31 handle path the same guarantee holds
  // structurally instead: the stored model carries its vcov as fitted, and a
  // postest passes no --vcov at all, so there is no default to fall back to.
  ojson rawJson;
  try {
    std::vector<std::string> nargv = exprArgs;
    nargv.push_back("--tidy");
    for (const auto& [k, v] : opts)
      if (k == "level" && !v.isBool) { nargv.push_back("--level"); nargv.push_back(v.strValue); break; }
    rawJson = runPostEstimationM4("nlcom", nargv, session, lm);
  } catch (const CobraError& ce) {
    return postestErrorBlockM4(ce);
  }
  ojson blk = cobraToEstimationM4(rawJson, "nlcom", &session);
  // dispatcher.cpp's captureRFromBlock (Branch 2) still runs on the RETURNED
  // block regardless of postOpt — Stata's nlcom is BOTH r-class (always)
  // and, under `post`, ALSO e-class. This only adds the e() publish.
  if (postOpt) {
    postEstimationBlockM4(session, blk, "nlcom", lm.depvar, lm.dataPath);
  }
  // FU-410: the engine's G V G' (cmd_nlcom `V`, in the order of its coefs) rides
  // to the dispatcher's nlcom arm, which posts it as r(V) and removes the key
  // before the block is shown; it is attached after the `post` publish, so e()
  // never carries it.
  if (rawJson.is_object() && rawJson.contains("V") && rawJson["V"].is_array())
    blk["nlcom_V"] = rawJson["V"];
  return blk;
}

// ===========================================================================
// margins


}  // namespace piet::handlers
