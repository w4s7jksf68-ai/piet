// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Categorical estimation.
#include "piet/handlers/estimation.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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
#include "postestimation/rtable_internal.h"  // stataColumns: a factor fit's e(b) columns

#include "estimation_families_internal.h"
#include "estimation_options.h"
#include "estimator_command_support.h"  // e3::setEScalarOnFit / setEStringOnFit
#include "extended_estimation_internal.h"  // stataRefusalM4b

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace est_detail;
namespace fit_detail {}
using namespace fit_detail;
using namespace estimation_vocab;

namespace {
// iterate(#): ml stops after # Newton iterations from Stata's start, converged or not
// (bank 9375308e `ologit rep78 mpg weight, iterate(2)`: e(converged) 0, e(ic) 2,
// e(ll) -86.51390680743226 at the second iterate); the engine runs that same path.
// nullopt when forwarded (or absent), else the refusal.
std::optional<ojson> pushIterateM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv) {
  for (const auto& [k, v] : parsed.opts) {
    if (k != "iterate") continue;
    const std::string val = trimM4(v.isBool ? std::string() : v.strValue);
    char* end = nullptr;
    const double d = std::strtod(val.c_str(), &end);
    if (val.empty() || end == nullptr || *end != '\0' || !(d >= 0 && d <= 16000) ||
        d != std::floor(d) || val.find_first_not_of("0123456789.eE+-") != std::string::npos)
      return engineErrorBlockM4("option iterate() must be an integer from 0 to 16000 in this build "
                                "(got `" + (v.isBool ? std::string() : v.strValue) + "`)",
                                "unsupported_option");
    argv.push_back("--iterate");
    argv.push_back(std::to_string(static_cast<int>(d)));
    break;
  }
  return std::nullopt;
}
// fweights, iweights and pweights (ologit.ado / oprobit.ado / mlogit.ado `[fw iw
// pw]`; aweight is Stata's own r(101), answered before the arm): forwarded to the
// engine, a pweight with the sandwich it implies (vs-stata bank aa1bb3ea /
// 1198712b / e6cc710e `ologit rep78 mpg weight [fw|iw|pw=w]`). An fweight or
// iweight under vce(robust)/vce(cluster) is valid Stata the engine does not fit.
std::optional<ojson> pushMlWeightM4(const eval::ParsedCommand& parsed, const std::string& cmd,
                                    std::vector<std::string>& argv) {
  if (!parsed.weightType.has_value()) return std::nullopt;
  const std::string wt = lowerM4(*parsed.weightType);
  std::string code;
  if (wt == "fw" || wt == "fweight") code = "fw";
  else if (wt == "iw" || wt == "iweight") code = "iw";
  else if (wt == "pw" || wt == "pweight") code = "pw";
  else return ojson(eval::errorBlock("engine_error", wt + "s not allowed", std::nullopt,
                                     std::nullopt, std::nullopt, std::string("r(101)")));
  const std::string var = trimM4(parsed.weightVar.value_or(""));
  if (var.empty() || var.find_first_of(" ()*+-/") != std::string::npos)
    return engineErrorBlockM4("`" + cmd + "` weights must be a variable name in this version of PIET",
                              "unsupported_option");
  bool sandwich = false;
  for (size_t i = 0; i + 1 < argv.size(); ++i)
    if (argv[i] == "--vce" && argv[i + 1] != "iid") sandwich = true;
  if (code != "pw" && sandwich)
    return engineErrorBlockM4("`" + cmd + "` with [" + *parsed.weightType + "=] and a robust or "
                              "cluster VCE is not implemented in this version of PIET",
                              "unsupported_option");
  argv.push_back("--weight");
  argv.push_back(var);
  argv.push_back("--wtype");
  argv.push_back(code);
  if (code == "pw" && !sandwich) {
    argv.push_back("--vce");
    argv.push_back("robust");
  }
  return std::nullopt;
}
// A boolean display option (`or`, `rrr`) as typed.
bool flagOptM4(const eval::ParsedCommand& parsed, const char* key) {
  for (const auto& [k, v] : parsed.opts)
    if (k == key && v.isBool && v.boolValue) return true;
  return false;
}

// `or` / `rrr` report exp(b), exp(b)*se and the exponentiated interval for a
// coefficient row, z and p unchanged; the row carries eform, so r(table) keeps
// e(b)'s metric with eform 1 (the rule `zip ..., irr` uses). e(b) is untouched:
// the option is a display metric. An omitted coefficient (b = se = 0) stays as it
// is, Stata's `(omitted)` row.
void eformRowM4(ojson& row) {
  if (!row.is_object() || !row.contains("b") || !row["b"].is_number()) return;
  const double b = row["b"].get<double>();
  const bool omitted = b == 0.0 && row.contains("se") && row["se"].is_number() &&
                       row["se"].get<double>() == 0.0;
  if (omitted) return;
  const double e = std::exp(b);
  row["b"] = e;
  if (row.contains("se") && row["se"].is_number()) row["se"] = e * row["se"].get<double>();
  if (row.contains("ci_lo") && row["ci_lo"].is_number())
    row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
  if (row.contains("ci_hi") && row["ci_hi"].is_number())
    row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
  row["eform"] = true;
}
}  // namespace

// `ologit depvar indepvars [if] [, vce(robust|cluster V)]` and its probit twin.
ojson runOrdered(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                 SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  if (auto err = pushMlWeightM4(parsed, cobraCmd, b.argv)) return *err;
  // offset(varname): a term in the linear predictor with coefficient 1 (vs-stata
  // bank 13941585 `ologit rep78 mpg weight, offset(off)`). An expression is not a
  // variable name: refused by name.
  for (const auto& [k, v] : parsed.opts) {
    if (k != "offset") continue;
    const std::string ov = v.isBool ? std::string() : trimM4(v.strValue);
    static const std::regex rePlainOff(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
    if (!std::regex_match(ov, rePlainOff))
      return engineErrorBlockM4("offset(" + ov + ") must name one variable in this version of PIET",
                                "unsupported_option");
    b.argv.push_back("--offset");
    b.argv.push_back(ov);
  }
  // level(#) sets the reported intervals (bank 352ed8e7 `oprobit ..., level(90)`);
  // it was read by nobody.
  pushLevelIfGivenM4(parsed, b.argv);
  if (auto err = pushIterateM4(parsed, b.argv)) return *err;
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle(cobraCmd, b.argv, session, &raw);
  // BUG-1196: Stata's e(b) is the depvar's equation, then the cutpoints
  // (StataBE 19.5 `yo:x1 yo:x2 /cut1 /cut2`); e(V) is the fit's, in that order.
  if (raw.is_object() && raw.contains("e_V") && raw.contains("coefs") && raw.contains("cuts") &&
      raw["cuts"].is_array()) {
    std::vector<AncillaryEbColM4> cols;
    bool ok = depvarEqColsM4(parsed.varlist[0], raw["coefs"], "b", cols);
    // A factor-variable fit: Stata's columns are those r(table) lays out (the
    // base level a structural 0, `rep78:0b.foreign`; bank d37aa219 `ologit rep78
    // mpg i.foreign`: e(b) `rep78:mpg rep78:0b.foreign rep78:1.foreign /cut1 ..`).
    // An engine-omitted column is not named here: nothing is posted then.
    if (!ok && session.lastModel().has_value() && block.contains("coeftable") &&
        block["coeftable"].is_array()) {
      std::vector<rtable_detail::RtCol> rc;
      bool placed = false;
      try {
        placed = rtable_detail::stataColumns(session, *session.lastModel(), block["coeftable"], rc);
      } catch (const std::exception&) {
        placed = false;
      }
      if (placed) {
        ok = true;
        cols.clear();
        for (const auto& c : rc) {
          if (c.row == nullptr) {
            cols.push_back({parsed.varlist[0] + ":" + c.name, std::string(), 0.0});
            continue;
          }
          const std::string en = c.row->value("name", std::string());
          const ojson& bv = c.row->contains("b") ? (*c.row)["b"] : ojson();
          const ojson& sv = c.row->contains("se") ? (*c.row)["se"] : ojson();
          if (en.empty() || en == "(intercept)" || en == "_cons" || !bv.is_number() ||
              (bv.get<double>() == 0.0 && sv.is_number() && sv.get<double>() == 0.0)) {
            ok = false;
            break;
          }
          cols.push_back({parsed.varlist[0] + ":" + c.name, en, bv.get<double>()});
        }
      }
    }
    for (const auto& c : raw["cuts"]) {
      if (!ok || !c.is_object() || !c.contains("name") || !c["name"].is_string() ||
          !c.contains("b") || !c["b"].is_number()) {
        ok = false;
        break;
      }
      const std::string nm = c["name"].get<std::string>();
      cols.push_back({nm, nm, c["b"].get<double>()});
    }
    if (ok) (void)postAncillaryEbM4(session, raw, cols);
  }
  // ologit's `or`: odds ratios for the slopes; the cutpoints stay in their own
  // metric (bank 57b46533 `ologit rep78 mpg weight, or`: mpg 1.124197, /cut1
  // -2.300255). It was accepted and ignored, so the table showed the coefficients.
  if (cobraCmd == "ologit" && flagOptM4(parsed, "or") && block.contains("coeftable") &&
      block["coeftable"].is_array())
    for (auto& row : block["coeftable"]) eformRowM4(row);
  // Stata prints the cutpoints in the SAME table as the coefficients, under a
  // /cut1, /cut2 … stanza. The engine emits them under their own `cuts` key
  // (they are not slopes and are excluded from the model Wald test), and
  // cobraToEstimationM4 carries only its known key set through — so without
  // this the rendered table had no cutpoints in it at all: an ordered model
  // missing exactly the parameters that make it ordered. Read from the RAW
  // payload for that reason. z and p stay null because Stata leaves those
  // columns blank for cuts.
  if (raw.contains("cuts") && raw["cuts"].is_array() && !raw["cuts"].empty()) {
    if (!block.contains("coeftable") || !block["coeftable"].is_array())
      block["coeftable"] = ojson::array();
    for (const auto& c : raw["cuts"]) {
      ojson row;
      row["name"] = c.value("name", "");
      row["b"] = c.contains("b") ? c["b"] : ojson(nullptr);
      row["se"] = c.contains("se") ? c["se"] : ojson(nullptr);
      row["t"] = nullptr;
      row["p"] = nullptr;
      row["ci_lo"] = c.contains("ci_lo") ? c["ci_lo"] : ojson(nullptr);
      row["ci_hi"] = c.contains("ci_hi") ? c["ci_hi"] : ojson(nullptr);
      block["coeftable"].push_back(row);
    }
  }
  return block;
}

// `mlogit depvar indepvars [if] [, baseoutcome(#) vce(...)]`.
ojson runMlogit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv("mlogit", parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  if (auto err = pushMlWeightM4(parsed, "mlogit", b.argv)) return *err;
  for (const auto& [k, v] : parsed.opts) {
    if ((k == "baseoutcome" || k == "base") && !v.isBool) {
      b.argv.push_back("--base");
      b.argv.push_back(trimM4(v.strValue));
    }
  }
  pushLevelIfGivenM4(parsed, b.argv);  // level(#): the reported intervals
  // iterate(#) (bank d678752f `mlogit race age grade wage, iterate(2)`: e(ll)
  // -1383.0289533345524, e(converged) 0)
  if (auto err = pushIterateM4(parsed, b.argv)) return *err;
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle("mlogit", b.argv, session, &raw);
  // baseoutcome(#) naming no outcome: Stata's r(111) "basecategory not found"
  // (vs-stata bank be442720 `mlogit race age grade wage, baseoutcome(9)`).
  if (block.is_object() && block.value("type", std::string()) == "error" &&
      block.value("message", std::string()).find("--base value") != std::string::npos &&
      block.value("message", std::string()).find("does not occur in outcome") != std::string::npos)
    return extended_fit_detail::stataRefusalM4b("basecategory not found", 111);
  // BUG-1196: Stata's e(b) has one equation per outcome, ascending, named by the
  // outcome's value label or, unlabeled, its value; the base outcome's columns are
  // `o.` zeros, as are its rows and columns of e(V) (StataBE 19.5 `1:o.x1 1:o.x2
  // 1:o._cons 2:x1 2:x2 2:_cons 3:x1 3:x2 3:_cons`; vs-stata bank e9b3681e `mlogit
  // race age grade wage`: `White:o.age ... Black:age ...`). A label that is not a
  // plain name, or an outcome that is not a nonnegative integer, names its
  // equation otherwise; nothing is posted then.
  const std::string& y = parsed.varlist[0];
  std::map<int, std::string> outcomeLabels;
  {
    const auto vl = session.valueLabels.find(y);
    if (vl != session.valueLabels.end()) outcomeLabels = vl->second;
    const auto lbl = session.varLblname.find(y);
    if (outcomeLabels.empty() && lbl != session.varLblname.end() && !lbl->second.empty()) {
      const auto defs = session.labelDefs.find(lbl->second);
      if (defs != session.labelDefs.end())
        for (const auto& [code, text] : defs->second) {
          char* e = nullptr;
          const long v = std::strtol(code.c_str(), &e, 10);
          if (e != nullptr && *e == '\0' && !code.empty()) outcomeLabels[static_cast<int>(v)] = text;
        }
    }
  }
  static const std::regex rePlainEq(R"(^[A-Za-z_][A-Za-z0-9_]{0,31}$)");
  std::vector<std::string> eqNames;  // Stata's equation name of each outcome, ascending
  bool eqOk = raw.is_object() && raw.contains("equations") && raw["equations"].is_array();
  if (eqOk) {
    for (const auto& e : raw["equations"]) {
      const double v = e.is_object() && e.contains("outcome") && e["outcome"].is_number()
                           ? e["outcome"].get<double>()
                           : -1.0;
      if (!(v >= 0.0 && v < 1e9 && v == std::floor(v))) {
        eqOk = false;
        break;
      }
      const auto lab = outcomeLabels.find(static_cast<int>(v));
      if (lab != outcomeLabels.end()) {
        if (!std::regex_match(lab->second, rePlainEq)) {
          eqOk = false;
          break;
        }
        eqNames.push_back(lab->second);
      } else {
        char eq[64];
        std::snprintf(eq, sizeof(eq), "%.17g", v);
        eqNames.push_back(eq);
      }
    }
  }
  bool ebPosted = false;
  if (eqOk && raw.contains("e_V")) {
    std::vector<std::string> coefNames;  // a non-base equation's names, engine spelling
    for (const auto& e : raw["equations"])
      if (e.is_object() && !e.value("base", false) && e.contains("coefs") && e["coefs"].is_array()) {
        for (const auto& c : e["coefs"])
          if (c.is_object() && c.contains("name") && c["name"].is_string())
            coefNames.push_back(c["name"].get<std::string>());
        break;
      }
    std::vector<AncillaryEbColM4> cols;
    bool ok = !coefNames.empty();
    size_t ei = 0;
    for (const auto& e : raw["equations"]) {
      if (!ok) break;
      const double v = e["outcome"].get<double>();
      char eng[64];
      std::snprintf(eng, sizeof(eng), "%.17g", v);  // the engine's `e_V` spelling of the outcome
      const std::string& eq = eqNames[ei++];
      const bool isBase = e.value("base", false);
      ojson coefs = isBase ? ojson::array() : e.value("coefs", ojson::array());
      if (isBase)
        for (const auto& n : coefNames) coefs.push_back({{"name", n}, {"b", 0.0}});
      std::vector<AncillaryEbColM4> ec;  // `y:name`, re-keyed to the outcome's equation
      if (!depvarEqColsM4(y, coefs, "b", ec)) {
        ok = false;
        break;
      }
      for (auto& c : ec) {
        c.stata = eq + (isBase ? ":o." : ":") + c.stata.substr(y.size() + 1);
        c.engine = isBase ? std::string() : std::string(eng) + ":" + c.engine;
        cols.push_back(std::move(c));
      }
    }
    if (ok) ebPosted = postAncillaryEbM4(session, raw, cols);
  }
  // r(table) laid out as e(b) (the base equation's columns `o.`: b 0, the rest
  // missing; bank e9b3681e r(label1..4) `(omitted)`), in the coefficient metric,
  // each column marked eform under `rrr` (bank a127b83e: the base column's b 1).
  if (ebPosted && block.is_object()) {
    const bool rrr = flagOptM4(parsed, "rrr");
    ojson rt = ojson::array();
    size_t ei = 0;
    std::vector<std::string> coefNames;
    for (const auto& e : raw["equations"])
      if (!e.value("base", false)) {
        for (const auto& c : e["coefs"]) coefNames.push_back(c.value("name", std::string()));
        break;
      }
    // Stata's order within an equation: the regressors, then _cons
    std::vector<size_t> order;
    for (size_t i = 0; i < coefNames.size(); ++i)
      if (coefNames[i] != "(intercept)" && coefNames[i] != "_cons") order.push_back(i);
    for (size_t i = 0; i < coefNames.size(); ++i)
      if (coefNames[i] == "(intercept)" || coefNames[i] == "_cons") order.push_back(i);
    for (const auto& e : raw["equations"]) {
      const std::string& eq = eqNames[ei++];
      const bool isBase = e.value("base", false);
      for (size_t i : order) {
        const std::string nm = coefNames[i] == "(intercept)" ? std::string("_cons") : coefNames[i];
        ojson col;
        col["name"] = eq + (isBase ? ":o." : ":") + nm;
        col["eform"] = rrr;
        if (isBase) {
          col["omitted"] = true;
        } else {
          const ojson& c = e["coefs"][i];
          for (const char* key : {"b", "se", "z", "p", "ci_lo", "ci_hi"})
            col[key] = c.contains(key) ? c[key] : ojson(nullptr);
        }
        rt.push_back(std::move(col));
      }
    }
    if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
      (*session.lastEstBlock)["rtable_cols"] = rt;  // a replay posts the same table
    block["rtable_cols"] = std::move(rt);
  }
  if (ebPosted) {
    // e(eqnames) / e(baselab): the outcome equations' names and the base's
    std::string all, base;
    size_t ei = 0;
    for (const auto& e : raw["equations"]) {
      all += (all.empty() ? "" : " ") + eqNames[ei];
      if (e.value("base", false)) base = eqNames[ei];
      ++ei;
    }
    putEStringNL(session, "eqnames", all);
    if (!base.empty()) putEStringNL(session, "baselab", base);
  }
  // `rrr`: relative-risk ratios in every non-base equation, the constant included
  // (bank a127b83e `mlogit race age grade wage, rrr`: Black age .945433, _cons
  // 19.87725). It was accepted and ignored, so the table showed the coefficients.
  if (flagOptM4(parsed, "rrr") && block.contains("outcomes") && block["outcomes"].is_array())
    for (auto& o : block["outcomes"])
      if (o.is_object() && !o.value("base", false) && o.contains("coeftable") &&
          o["coeftable"].is_array())
        for (auto& row : o["coeftable"]) eformRowM4(row);
  return block;
}

// `zip`/`zinb depvar indepvars [if], inflate(...) [vce(robust)]`.
// inflate() is REQUIRED by Stata and only the constant-only form is implemented;
// a covariate list there would be a different model, so it rejects rather than
// quietly fitting the constant-only one.
ojson runZeroInflated(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  std::optional<std::string> inflate;
  for (const auto& [k, v] : parsed.opts)
    if (k == "inflate" && !v.isBool) inflate = trimM4(v.strValue);
  if (!inflate.has_value()) {
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires inflate()", "syntax_error");
  }
  if (*inflate != "_cons") {
    return engineErrorBlockM4(
        "Option `inflate(" + *inflate + ")` is a valid Stata option that this version of PIET does "
        "not implement for `" + cobraCmd + "` — only the constant-only inflation equation "
        "`inflate(_cons)` is available.",
        "unsupported_option");
  }
  b.argv.push_back("--inflate");
  b.argv.push_back("_cons");
  bool irr = false;
  for (const auto& [k, v] : parsed.opts)
    if (k == "irr" && v.isBool && v.boolValue) irr = true;
  ojson block = runP7Mle(cobraCmd, b.argv, session);
  // `irr` reports incidence-rate ratios for the COUNT equation only (zip/zinb:
  // "Estimates are transformed only in the first equation"): its rows show
  // exp(b), exp(b)*se and the exponentiated interval, z and p unchanged, and
  // r(table) carries them with eform 1 while inflate: stays in its own metric
  // (eform 0). e(b) is untouched -- `irr` is a display metric. It was accepted
  // and ignored, so r(table) posted the log-scale count coefficients.
  if (irr && block.value("type", std::string()) == "estimation" && block.contains("coeftable") &&
      block["coeftable"].is_array()) {
    for (auto& row : block["coeftable"]) {
      if (!row.is_object() || !row.contains("b") || !row["b"].is_number()) continue;
      const double e = std::exp(row["b"].get<double>());
      row["b"] = e;
      if (row.contains("se") && row["se"].is_number()) row["se"] = e * row["se"].get<double>();
      if (row.contains("ci_lo") && row["ci_lo"].is_number())
        row["ci_lo"] = std::exp(row["ci_lo"].get<double>());
      if (row.contains("ci_hi") && row["ci_hi"].is_number())
        row["ci_hi"] = std::exp(row["ci_hi"].get<double>());
      row["eform"] = true;
    }
  }
  return block;
}

// fracreg's e() contract (StataBE 19.5 bank 2bef1615 `fracreg logit fr weight
// length`, 952f78c4 `... weight i.foreign`): the ml record of the quasi-likelihood
// fit (e(ic), e(ilog) 1 x 20, e(gradient) named c1..ck, e(V_modelbased) the
// inverse of -H, e(converged)), the constant-only e(ll_0) and e(r2_p), the Wald
// test of the slopes on the robust e(V) (e(chi2), e(df_m), e(p), e(chi2type)),
// e(mns) and the macros fracreg posts. e(mns) is what fracreg records there: the
// mean of each regressor over the whole dataset, listed in reverse order under the
// e(b) names, 0 for a base level, and 1 for _cons (bank 952f78c4: weight's column
// holds mean(1.foreign) .2972973, 1.foreign's holds mean(weight) 3019.46).
// Run from postStataEContract, once the e(b) stripe carries its base levels.
void est_detail::postFracregContractM4(SessionState& session, const ojson& raw) {
  if (!raw.is_object() || !raw.contains("ilog") || !raw["ilog"].is_array() || !raw.contains("e_gradient") ||
      !raw["e_gradient"].is_object())
    return;
  // kNames: e(b)/e(V) on Stata's stripe, base levels included
  const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kNames);
  const auto eV = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kNames);
  if (!eb.matrix.has_value() || !eV.matrix.has_value() || eb.matrix->data.size() != 1) return;
  const std::vector<std::string> cols = eb.matrix->colnames;
  const size_t K = cols.size();
  if (K == 0 || eV.matrix->data.size() != K) return;
  const auto bareOf = [](const std::string& c) {
    const size_t colon = c.find(':');
    return colon == std::string::npos ? c : c.substr(colon + 1);
  };
  const auto engineName = [&](const std::string& c) {
    const std::string b = bareOf(c);
    return b == "_cons" ? std::string("(intercept)") : b;
  };
  // engine index of each e(b) column (-1: a base or omitted level)
  const ojson& gn = raw["e_gradient"]["names"];
  const ojson& gv = raw["e_gradient"]["values"];
  if (!gn.is_array() || !gv.is_array() || gn.size() != gv.size()) return;
  std::vector<long> at(K, -1);
  size_t found = 0;
  for (size_t j = 0; j < K; ++j) {
    const std::string nm = engineName(cols[j]);
    for (size_t e = 0; e < gn.size(); ++e)
      if (gn[e].is_string() && gn[e].get<std::string>() == nm) at[j] = static_cast<long>(e);
    if (at[j] >= 0) {
      ++found;
    } else {
      const std::string b = bareOf(cols[j]);
      const size_t dot = b.find('.');
      const std::string op = dot == std::string::npos ? std::string() : b.substr(0, dot);
      if (op.find('b') == std::string::npos && op.find('o') == std::string::npos) return;
    }
  }
  if (found != gn.size()) return;
  const auto putE = [&session](const std::string& name, eval::EvalMatrix m) {
    auto& store = session.eval.eMatrices;
    store.erase(std::remove_if(store.begin(), store.end(), [&name](const auto& q) { return q.first == name; }),
                store.end());
    store.emplace_back(name, std::move(m));
    session.noteEOnFit(name);
  };
  {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(K, 0.0));
    for (size_t j = 0; j < K; ++j) {
      m.colnames.push_back("c" + std::to_string(j + 1));
      if (at[j] >= 0 && gv[static_cast<size_t>(at[j])].is_number())
        m.data[0][j] = gv[static_cast<size_t>(at[j])].get<double>();
    }
    putE("gradient", std::move(m));
  }
  {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    size_t i = 0;
    for (const auto& v : raw["ilog"]) {
      if (i >= 20) break;
      if (v.is_number()) m.data[0][i] = v.get<double>();
      ++i;
    }
    putE("ilog", std::move(m));
  }
  if (raw.contains("e_Vmb") && raw["e_Vmb"].is_object() && raw["e_Vmb"].contains("matrix") &&
      raw["e_Vmb"]["matrix"].is_array() && raw["e_Vmb"]["matrix"].size() == gn.size()) {
    const ojson& M = raw["e_Vmb"]["matrix"];
    eval::EvalMatrix m;
    m.rownames = cols;
    m.colnames = cols;
    m.data.assign(K, std::vector<double>(K, 0.0));
    for (size_t a = 0; a < K; ++a)
      for (size_t c = 0; c < K; ++c)
        if (at[a] >= 0 && at[c] >= 0 && M[static_cast<size_t>(at[a])].is_array() &&
            M[static_cast<size_t>(at[a])][static_cast<size_t>(at[c])].is_number())
          m.data[a][c] = M[static_cast<size_t>(at[a])][static_cast<size_t>(at[c])].get<double>();
    putE("V_modelbased", std::move(m));
  }
  // e(mns): whole-dataset means of each regressor, in reverse order (see header)
  bool hasCons = false;
  std::vector<std::string> xcols;
  for (const auto& c : cols) {
    if (bareOf(c) == "_cons") hasCons = true;
    else xcols.push_back(bareOf(c));
  }
  {
    std::vector<double> means;
    bool ok = true;
    for (const auto& x : xcols) {
      double mean = 0.0;
      const size_t dot = x.find('.');
      if (x.find('#') != std::string::npos) { ok = false; break; }
      if (dot == std::string::npos) {
        const double* col = f64ColumnM4(session, x);
        const Datastore* ds = session.datastore();
        if (col == nullptr || ds == nullptr) { ok = false; break; }
        double s = 0.0;
        int64_t n = 0;
        for (int64_t i = 0; i < ds->nRows(); ++i)
          if (!(std::isnan(col[i]) || std::fabs(col[i]) >= eval::kStataMissingSentinel)) { s += col[i]; ++n; }
        mean = n > 0 ? s / static_cast<double>(n) : 0.0;
      } else {
        const std::string op = x.substr(0, dot), var = x.substr(dot + 1);
        if (op.find('b') != std::string::npos || op.find('o') != std::string::npos) {
          mean = 0.0;
        } else {
          char* end = nullptr;
          const double lv = std::strtod(op.c_str(), &end);
          const double* col = f64ColumnM4(session, var);
          const Datastore* ds = session.datastore();
          if (end == nullptr || *end != '\0' || col == nullptr || ds == nullptr) { ok = false; break; }
          double s = 0.0;
          int64_t n = 0;
          for (int64_t i = 0; i < ds->nRows(); ++i)
            if (!(std::isnan(col[i]) || std::fabs(col[i]) >= eval::kStataMissingSentinel)) { s += col[i] == lv ? 1.0 : 0.0; ++n; }
          mean = n > 0 ? s / static_cast<double>(n) : 0.0;
        }
      }
      means.push_back(mean);
    }
    if (ok) {
      eval::EvalMatrix m;
      m.rownames = {"r1"};
      m.colnames = xcols;
      m.data.assign(1, std::vector<double>(means.rbegin(), means.rend()));
      if (hasCons) {
        m.colnames.push_back("_cons");
        m.data[0].push_back(1.0);
      }
      putE("mns", std::move(m));
    }
  }
  const std::string vce = raw.value("vce", std::string()) == "cluster" ? "cluster" : "robust";
  const std::string clusterVar = vce == "cluster" ? raw.value("cluster_var", std::string()) : std::string();
  const auto num = [&raw](const char* k) -> std::optional<double> {
    if (!raw.contains(k) || !raw[k].is_number()) return std::nullopt;
    return raw[k].get<double>();
  };
  const auto setS = [&session](const char* n, double v) { e3::setEScalarOnFit(session, n, v); };
  const auto setM = [&session](const char* n, const std::string& v) { e3::setEStringOnFit(session, n, v); };
  double rank = 0.0, dfm = 0.0;
  for (size_t j = 0; j < K; ++j)
    if (at[j] >= 0 && eV.matrix->data[j][j] > 0) {
      rank += 1.0;
      if (bareOf(cols[j]) != "_cons") dfm += 1.0;
    }
  const auto ll = num("ll");
  const auto ll0 = num("ll_0");
  setS("rank", rank);
  setS("k", static_cast<double>(K));
  setS("k_eq", 1.0);
  setS("k_dv", 1.0);
  setS("rc", 0.0);
  setS("cons", hasCons ? 0.0 : 1.0);
  setS("noconstant", hasCons ? 0.0 : 1.0);
  setS("consonly", xcols.empty() ? 1.0 : 0.0);
  if (const auto ic = num("ic")) setS("ic", *ic);
  if (raw.contains("converged") && raw["converged"].is_boolean())
    setS("converged", raw["converged"].get<bool>() ? 1.0 : 0.0);
  if (ll) setS("ll", *ll);
  if (ll0 && *ll0 != 0.0) {
    setS("ll_0", *ll0);
    if (ll) setS("r2_p", 1.0 - *ll / *ll0);
  }
  if (!xcols.empty()) {
    setS("df_m", dfm);
    setS("k_eq_model", 1.0);
    if (const auto c2 = num("chi2")) {
      setS("chi2", *c2);
      setS("p", est_detail::chi2UpperTailM4(dfm, *c2));
    }
    setM("chi2type", "Wald");
  }
  if (vce == "cluster")
    if (const auto g = num("n_clusters")) setS("N_clust", *g);
  setM("crittype", "log pseudolikelihood");
  setM("depvar", raw.value("depvar", std::string()));
  setM("estat_cmd", "fracreg_estat");
  setM("estimator", "logit");
  setM("frac", "fractional");
  setM("ml_method", "lf2");
  setM("opt", "moptimize");
  setM("predict", "fracreg_p");
  setM("technique", "nr");
  setM("title", "Fractional logistic regression");
  setM("vcetype", "Robust");
  setM("which", "max");
  {
    std::string xv;
    for (const auto& x : xcols) xv += (xv.empty() ? "" : " ") + x;
    setM("xvars", xv);
  }
  if (!clusterVar.empty()) setM("clustvar", clusterVar);
  session.eval.setEString("vce", vce);
}

// `fracreg logit depvar indepvars [if] [, vce(robust)]` — the link is a leading
// positional token, not an option.
ojson runFracreg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (parsed.varlist.empty())
    return engineErrorBlockM4("syntax_error: fracreg requires a link and a dependent variable",
                              "syntax_error");
  const std::string link = lowerM4(trimM4(parsed.varlist[0]));
  if (link != "logit" && link != "probit")
    return engineErrorBlockM4("syntax_error: fracreg requires a link (logit or probit)",
                              "syntax_error");
  if (link == "probit")
    return engineErrorBlockM4(
        "`fracreg probit` is a valid Stata command that this version of PIET does not implement "
        "(only the logit link is available).",
        "unsupported_command");
  eval::ParsedCommand shifted = parsed;
  shifted.varlist.erase(shifted.varlist.begin());
  P7MleArgv b = buildP7MleArgv("fracreg", shifted, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  b.argv.push_back("--link");
  b.argv.push_back("logit");
  pushLevelIfGivenM4(parsed, b.argv);
  // iterate(#) (bank ad764cde `fracreg logit fr weight length, iterate(2)`)
  if (auto err = pushIterateM4(parsed, b.argv)) return *err;
  b.argv.push_back("--emit-v");
  // e() beyond e(b)/e(V): est_detail::postFracregContractM4, from postStataEContract
  ojson block = runP7Mle("fracreg", b.argv, session);
  // `or`: odds ratios, the logistic metric (bank 4cdb3a92 `fracreg logit fr weight
  // length, or`: weight .9993743, _cons 21.42164); e(b) is untouched and r(table)
  // carries eform 1.
  if (flagOptM4(parsed, "or") && block.value("type", std::string()) == "estimation" &&
      block.contains("coeftable") && block["coeftable"].is_array()) {
    for (auto& row : block["coeftable"]) eformRowM4(row);
  }
  return block;
}

// ===========================================================================
// P7 — vector autoregression: `var`, `vargranger`, `irf create`
//
// ONE engine verb serves all three: it returns the per-equation results, the
// Granger block and the IRF cells in a single payload. `vargranger` is therefore
// a pure re-render of the stored fit — zero new mathematics on this side, which
// is the point — and `irf create` materializes the cells as a `.irf` file (which
// is a Stata `.dta`).


}  // namespace piet::handlers
