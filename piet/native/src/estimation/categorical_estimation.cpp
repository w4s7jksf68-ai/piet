// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Categorical estimation.
#include "piet/handlers/estimation.h"
#include <algorithm>
#include <functional>
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
// The constraints a fit drops (their notes, keyed by position in the
// constraints() list) and the positions of those it keeps.
struct CnsDropsM4 {
  std::vector<std::pair<int, std::string>> notes;
  std::vector<int> keptPos;
};
// constraints(numlist) of ologit/oprobit/mlogit: the defined constraints as the
// engine's `--cns` spec (cmds/common.h). `eqOf` maps a typed equation ("" when
// none) to the engine's EQ field, or nullopt when the model has no such
// equation. A constraint that cannot be laid on the model is dropped, as makecns
// drops a constraint that causes an error, and the fit goes on with the rest:
// an unknown equation here (r(303)), an unknown coefficient by the engine
// (dropCnsAfterFitM4). `dropped` gets each such constraint's note, keyed by its
// position in the constraints() list, and the kept ones' positions. Returns an error block, or nullopt with
// `spec` empty when the option is absent.
std::optional<ojson> constraintsSpecM4(
    const eval::ParsedCommand& parsed, SessionState& session,
    const std::function<std::optional<std::string>(const std::string&, std::string&)>& eqOf,
    std::string& spec, std::vector<est_detail::CnsDefM4>& defs, CnsDropsM4& dropped) {
  spec.clear();
  dropped = CnsDropsM4();
  ojson err;
  if (!est_detail::readConstraintsM4(parsed, session, defs, err)) return err;
  std::vector<est_detail::CnsDefM4> kept;
  for (size_t di = 0; di < defs.size(); ++di) {
    est_detail::CnsDefM4& d = defs[di];
    std::string row;
    bool badEq = false;
    for (auto& t : d.terms) {
      std::string nm = t.name;
      const std::optional<std::string> eq = eqOf(t.eq, nm);
      if (!eq.has_value()) {
        badEq = true;
        break;
      }
      t.eq = *eq;
      t.name = nm;
      char c[40];
      std::snprintf(c, sizeof(c), "%.17g", t.coef);
      row += (row.empty() ? "" : ",") + std::string(c) + "|" + t.eq + "|" + t.name;
    }
    if (badEq) {
      dropped.notes.emplace_back(static_cast<int>(di), "constraint number " + std::to_string(d.num) +
                                                           " caused error r(303)");
      continue;
    }
    kept.push_back(d);
    dropped.keptPos.push_back(static_cast<int>(di));
    if (row.empty()) continue;  // a constraint on numbers alone
    char r[40];
    std::snprintf(r, sizeof(r), "%.17g", d.rhs);
    spec += (spec.empty() ? "" : ";") + row + "=" + r;
  }
  defs.swap(kept);
  return std::nullopt;
}

// After the fit: the engine's `cns_dropped` rows (a coefficient the model does
// not have, r(111)) leave `defs`, each with its note; the notes go above the
// table in constraints() order (`note: constraint number 6 caused error r(111)`).
void dropCnsAfterFitM4(const ojson& raw, std::vector<est_detail::CnsDefM4>& defs,
                       CnsDropsM4& dropped, ojson& block) {
  if (raw.is_object() && raw.contains("cns_dropped") && raw["cns_dropped"].is_array()) {
    std::map<size_t, int> rcOfRow;  // engine spec row -> rc
    for (const auto& d : raw["cns_dropped"])
      if (d.is_object() && d.contains("row") && d["row"].is_number_integer())
        rcOfRow[d["row"].get<size_t>()] = d.value("rc", 111);
    std::vector<est_detail::CnsDefM4> kept;
    std::vector<int> keptPos;
    size_t row = 0;
    for (size_t i = 0; i < defs.size(); ++i) {
      const est_detail::CnsDefM4& d = defs[i];
      const int pos = i < dropped.keptPos.size() ? dropped.keptPos[i] : static_cast<int>(i);
      const auto it = d.terms.empty() ? rcOfRow.end() : rcOfRow.find(row++);
      if (it == rcOfRow.end()) {
        kept.push_back(d);
        keptPos.push_back(pos);
        continue;
      }
      dropped.notes.emplace_back(pos, "constraint number " + std::to_string(d.num) + " caused error r(" +
                                          std::to_string(it->second) + ")");
    }
    defs.swap(kept);
    dropped.keptPos.swap(keptPos);
  }
  if (dropped.notes.empty() || !block.is_object() || block.value("type", std::string()) == "error") return;
  std::stable_sort(dropped.notes.begin(), dropped.notes.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  ojson notes = ojson::array();
  for (const auto& [pos, text] : dropped.notes) notes.push_back(text);
  if (block.contains("warnings") && block["warnings"].is_array())
    for (const auto& w : block["warnings"]) notes.push_back(w);
  block["warnings"] = std::move(notes);
}

// `( 1)  [rep78]mpg = -.1`: the constraint lines Stata prints above the table.
std::vector<std::string> constraintLinesM4(
    const std::vector<est_detail::CnsDefM4>& defs,
    const std::function<std::string(const est_detail::CnsTermM4&)>& shown) {
  std::vector<std::string> out;
  int i = 0;
  for (const auto& d : defs) {
    if (d.terms.empty()) continue;
    std::vector<std::pair<double, std::string>> terms;
    for (const auto& t : d.terms) terms.emplace_back(t.coef, shown(t));
    char head[16];
    std::snprintf(head, sizeof(head), "(%2d)  ", ++i);
    out.push_back(head + est_detail::cnsDisplayLineM4(terms, d.rhs));
  }
  return out;
}

// A coefficient the constraints fix (variance exactly 0): Stata shows its value
// and `(constrained)`, with no standard error, test or interval.
void markConstrainedRowM4(ojson& row) {
  if (!row.is_object() || !row.value("constrained", false)) return;
  for (const char* k : {"se", "z", "t", "p", "ci_lo", "ci_hi"})
    if (row.contains(k)) row[k] = nullptr;
}
// The same coefficient's r(table) column: its value, the statistics missing.
void markConstrainedRtColM4(ojson& col) {
  col["constrained"] = true;
  for (const char* k : {"se", "z", "p", "ci_lo", "ci_hi"}) col[k] = nullptr;
}
}  // namespace

// `ologit depvar indepvars [if] [, vce(robust|cluster V)]` and its probit twin.
ojson runOrdered(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                 SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  if (auto err = pushInRangeKeepifM4(session, parsed, b.argv, cobraCmd)) return *err;
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
  // constraints(numlist): `mpg`, `[rep78]mpg`, `_b[mpg]`, `/cut1` or
  // `[cut1]_cons` (bank eae58c67 `constraint 1 mpg = -0.1`, `ologit rep78 mpg
  // weight, const(1)`).
  const std::string& depvar = parsed.varlist[0];
  std::vector<est_detail::CnsDefM4> cnsDefs;
  CnsDropsM4 cnsDrops;
  {
    std::string spec;
    const auto eqOf = [&depvar](const std::string& eq, std::string& name) -> std::optional<std::string> {
      if (eq.empty() || eq == depvar || eq == "#1") return std::string();
      static const std::regex reCut(R"(^/?cut([0-9]+)$)");
      std::smatch m;
      if (std::regex_match(eq, m, reCut) && name == "_cons") {
        name = "/cut" + m[1].str();
        return std::string();
      }
      if (eq == "/" && name.rfind("cut", 0) == 0) {
        name = "/" + name;
        return std::string();
      }
      return std::nullopt;
    };
    if (auto err = constraintsSpecM4(parsed, session, eqOf, spec, cnsDefs, cnsDrops)) return *err;
    if (!spec.empty()) {
      b.argv.push_back("--cns");
      b.argv.push_back(spec);
    }
  }
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle(cobraCmd, b.argv, session, &raw);
  dropCnsAfterFitM4(raw, cnsDefs, cnsDrops, block);
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
      if (c.value("constrained", false)) row["constrained"] = true;
      block["coeftable"].push_back(row);
    }
  }
  if (!cnsDefs.empty() && raw.contains("cns") && block.contains("coeftable") &&
      block["coeftable"].is_array()) {
    std::set<std::string> fixedSlopes;  // the engine's slopes the constraints fix
    if (raw.contains("coefs") && raw["coefs"].is_array())
      for (const auto& c : raw["coefs"])
        if (c.is_object() && c.value("constrained", false)) fixedSlopes.insert(c.value("name", std::string()));
    for (auto& row : block["coeftable"]) {
      if (row.is_object() && fixedSlopes.count(row.value("name", std::string())) != 0)
        row["constrained"] = true;
      markConstrainedRowM4(row);
    }
    block["chi2type"] = "Wald";
    block["constraint_lines"] = constraintLinesM4(cnsDefs, [&depvar](const est_detail::CnsTermM4& t) {
      return t.name.rfind("/", 0) == 0 ? t.name : "[" + depvar + "]" + t.name;
    });
  }
  return block;
}

// `mlogit depvar indepvars [if] [, baseoutcome(#) vce(...)]`.
ojson runMlogit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  P7MleArgv b = buildP7MleArgv("mlogit", parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  if (auto err = pushInRangeKeepifM4(session, parsed, b.argv, "mlogit")) return *err;
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
  // constraints(numlist): an equation is named by its outcome's value label, its
  // value or `#n` (bank 908e6dce `constraint 1 [2]age = 0`, `mlogit race age grade
  // wage, c(1)`: outcome 2, Black).
  const std::string& yv = parsed.varlist[0];
  std::map<int, std::string> cnsLabels;
  {
    const auto vl = session.valueLabels.find(yv);
    if (vl != session.valueLabels.end()) cnsLabels = vl->second;
    const auto lbl = session.varLblname.find(yv);
    if (cnsLabels.empty() && lbl != session.varLblname.end() && !lbl->second.empty()) {
      const auto defs = session.labelDefs.find(lbl->second);
      if (defs != session.labelDefs.end())
        for (const auto& [code, text] : defs->second) {
          char* e = nullptr;
          const long v = std::strtol(code.c_str(), &e, 10);
          if (e != nullptr && *e == '\0' && !code.empty()) cnsLabels[static_cast<int>(v)] = text;
        }
    }
  }
  std::vector<est_detail::CnsDefM4> cnsDefs;
  CnsDropsM4 cnsDrops;
  {
    std::string spec;
    const auto eqOf = [&cnsLabels](const std::string& eq, std::string&) -> std::optional<std::string> {
      if (eq.empty()) return std::string();
      static const std::regex reNum(R"(^#?[0-9]+$)");
      for (const auto& [v, text] : cnsLabels)
        if (text == eq) return "=" + std::to_string(v);
      if (std::regex_match(eq, reNum)) return eq[0] == '#' ? eq : "=" + eq;
      return std::nullopt;
    };
    if (auto err = constraintsSpecM4(parsed, session, eqOf, spec, cnsDefs, cnsDrops)) return *err;
    if (!spec.empty()) {
      b.argv.push_back("--cns");
      b.argv.push_back(spec);
    }
  }
  b.argv.push_back("--emit-v");  // BUG-1196: the fit's e(V), posted below
  ojson raw;
  ojson block = runP7Mle("mlogit", b.argv, session, &raw);
  dropCnsAfterFitM4(raw, cnsDefs, cnsDrops, block);
  // A coefficient the constraints hold at 0 is `o.`-marked, as ml's findomitted
  // marks it (bank 908e6dce `Black:o.age`, r(label5) `(omitted)`): the engine
  // outcome value -> the names of its coefficients so fixed.
  std::map<std::string, std::set<std::string>> cnsZero;
  if (!cnsDefs.empty() && raw.contains("cns") && raw.contains("equations") && raw["equations"].is_array())
    for (const auto& e : raw["equations"]) {
      if (!e.is_object() || e.value("base", false) || !e.contains("coefs") || !e.contains("outcome")) continue;
      char eng[64];
      std::snprintf(eng, sizeof(eng), "%.17g", e["outcome"].get<double>());
      for (const auto& c : e["coefs"])
        if (c.is_object() && c.value("constrained", false) && c.value("b", 1.0) == 0.0) {
          std::string nm = c.value("name", std::string());
          if (nm == "(intercept)") nm = "_cons";
          cnsZero[eng].insert(nm);
        }
    }
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
  // a factor-variable fit's per-equation layout (Stata name, engine name; "" = base)
  std::optional<std::vector<std::pair<std::string, std::string>>> mlogitFvLayout;
  std::vector<AncillaryEbColM4> mlogitEbCols;  // the posted columns of such a fit
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
        // Factor-variable terms: each equation's columns as r(table) lays them out
        // (a base level a structural 0; the base outcome's every column off its
        // base level `o.`-marked: bank 6b328e96 `mlogit race age i.collgrad`
        // White:o.age White:0b.collgrad White:1o.collgrad White:o._cons
        // Black:age Black:0b.collgrad Black:1.collgrad Black:_cons ..., 2ef46669
        // `i.collgrad##c.age` White:0b.collgrad#co.age Black:1.collgrad#c.age).
        if (!mlogitFvLayout.has_value()) {
          mlogitFvLayout.emplace();
          ojson ct = ojson::array();
          for (const auto& e2 : raw["equations"])
            if (e2.is_object() && !e2.value("base", false) && e2.contains("coefs")) {
              ct = e2["coefs"];
              break;
            }
          std::vector<rtable_detail::RtCol> rc;
          bool placedFv = false;
          try {
            placedFv = session.lastModel().has_value() && ct.is_array() && !ct.empty() &&
                       rtable_detail::stataColumns(session, *session.lastModel(), ct, rc);
          } catch (const std::exception&) {
            placedFv = false;
          }
          if (placedFv)
            for (const auto& c : rc) {
              std::string en = c.row != nullptr ? c.row->value("name", std::string()) : std::string();
              // (a coefficient the constraints hold at 0 is b 0, se 0 too, but it is
              // the user's, not an omitted regressor: placed, and `o.`-marked below)
              const bool zeroRow = c.row != nullptr && c.row->contains("b") && (*c.row)["b"].is_number() &&
                                   (*c.row)["b"].get<double>() == 0.0 && c.row->contains("se") &&
                                   (*c.row)["se"].is_number() && (*c.row)["se"].get<double>() == 0.0 &&
                                   !c.row->value("constrained", false);
              if (zeroRow) {  // an engine-omitted regressor: its Stata name is not known here
                mlogitFvLayout->clear();
                break;
              }
              mlogitFvLayout->push_back({c.name, en});
            }
        }
        if (mlogitFvLayout->empty()) {
          ok = false;
          break;
        }
        std::map<std::string, const ojson*> byName;
        if (!isBase)
          for (const auto& c : e["coefs"]) byName[c.value("name", std::string())] = &c;
        for (const auto& [stataName, engName] : *mlogitFvLayout) {
          AncillaryEbColM4 c;
          if (isBase || engName.empty()) {
            c.stata = eq + ":" + (isBase ? rtable_detail::omittedColumnName(stataName) : stataName);
            c.b = 0.0;
          } else {
            const auto it = byName.find(engName);
            if (it == byName.end() || !(*it->second).contains("b") || !(*it->second)["b"].is_number()) {
              ok = false;
              break;
            }
            c.b = (*it->second)["b"].get<double>();
            // held at 0 by the constraints: `o.`-marked as ml's findomitted marks it
            // (bank 908e6dce `Black:o.age`)
            const bool fixed0 = (*it->second).value("constrained", false) && c.b == 0.0;
            c.stata = eq + ":" + (fixed0 ? rtable_detail::omittedColumnName(stataName) : stataName);
            c.engine = std::string(eng) + ":" + engName;
          }
          cols.push_back(std::move(c));
        }
        if (!ok) break;
        continue;
      }
      for (auto& c : ec) {
        const std::string nm = c.stata.substr(y.size() + 1);
        const auto z = cnsZero.find(eng);
        const bool fixed0 = !isBase && z != cnsZero.end() && z->second.count(nm) != 0;
        c.stata = eq + (isBase || fixed0 ? ":o." : ":") + nm;
        c.engine = isBase ? std::string() : std::string(eng) + ":" + c.engine;
        cols.push_back(std::move(c));
      }
    }
    if (ok) ebPosted = postAncillaryEbM4(session, raw, cols);
    if (ok && mlogitFvLayout.has_value()) mlogitEbCols = cols;
  }
  // r(table) laid out as e(b) (the base equation's columns `o.`: b 0, the rest
  // missing; bank e9b3681e r(label1..4) `(omitted)`), in the coefficient metric,
  // each column marked eform under `rrr` (bank a127b83e: the base column's b 1).
  if (ebPosted && block.is_object() && !mlogitEbCols.empty()) {
    // the factor-variable layout: r(table)'s columns are the posted e(b)'s
    const bool rrr = flagOptM4(parsed, "rrr");
    std::map<std::string, const ojson*> rowOf;  // engine `outcome:name` -> its coef row
    for (const auto& e : raw["equations"]) {
      if (e.value("base", false) || !e.contains("coefs")) continue;
      char eng[64];
      std::snprintf(eng, sizeof(eng), "%.17g", e["outcome"].get<double>());
      for (const auto& c : e["coefs"]) rowOf[std::string(eng) + ":" + c.value("name", std::string())] = &c;
    }
    ojson rt = ojson::array();
    for (const auto& c : mlogitEbCols) {
      ojson col;
      col["name"] = c.stata;
      col["eform"] = rrr;
      const auto it = c.engine.empty() ? rowOf.end() : rowOf.find(c.engine);
      const bool cns = it != rowOf.end() && it->second->value("constrained", false);
      if (it == rowOf.end() || (cns && it->second->value("b", 1.0) == 0.0)) {
        col["omitted"] = true;
      } else {
        for (const char* key : {"b", "se", "z", "p", "ci_lo", "ci_hi"})
          col[key] = it->second->contains(key) ? (*it->second)[key] : ojson(nullptr);
        if (cns) markConstrainedRtColM4(col);
      }
      rt.push_back(std::move(col));
    }
    if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object())
      (*session.lastEstBlock)["rtable_cols"] = rt;
    block["rtable_cols"] = std::move(rt);
  } else if (ebPosted && block.is_object()) {
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
        char engv[64];
        std::snprintf(engv, sizeof(engv), "%.17g", e["outcome"].get<double>());
        const auto z = cnsZero.find(engv);
        const bool fixed0 = !isBase && z != cnsZero.end() && z->second.count(nm) != 0;
        col["name"] = eq + (isBase || fixed0 ? ":o." : ":") + nm;
        col["eform"] = rrr;
        if (isBase || fixed0) {
          col["omitted"] = true;
        } else {
          const ojson& c = e["coefs"][i];
          for (const char* key : {"b", "se", "z", "p", "ci_lo", "ci_hi"})
            col[key] = c.contains(key) ? c[key] : ojson(nullptr);
          if (c.value("constrained", false)) markConstrainedRtColM4(col);
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
  // constraints(): a coefficient held at 0 is drawn `(omitted)`, and the
  // constraint lines (`( 1)  [Black]o.age = 0`) go above the table.
  if (!cnsDefs.empty() && raw.contains("cns") && block.contains("outcomes") && block["outcomes"].is_array()) {
    for (auto& o : block["outcomes"]) {
      if (!o.is_object() || o.value("base", false) || !o.contains("outcome") || !o.contains("coeftable"))
        continue;
      char eng[64];
      std::snprintf(eng, sizeof(eng), "%.17g", o["outcome"].get<double>());
      // held at a nonzero value: its value and `(constrained)`, no statistics
      // (Stata's coefficient table, as ologit's `mpg | -.1 (constrained)`)
      std::set<std::string> fixedNonzero;
      for (const auto& e : raw["equations"]) {
        if (!e.is_object() || e.value("base", false) || !e.contains("coefs") || !e.contains("outcome")) continue;
        char ev[64];
        std::snprintf(ev, sizeof(ev), "%.17g", e["outcome"].get<double>());
        if (std::string(ev) != eng) continue;
        for (const auto& c : e["coefs"])
          if (c.is_object() && c.value("constrained", false) && c.value("b", 0.0) != 0.0)
            fixedNonzero.insert(c.value("name", std::string()));
      }
      const auto z = cnsZero.find(eng);
      for (auto& row : o["coeftable"]) {
        const std::string en = row.value("name", std::string());
        if (fixedNonzero.count(en) != 0) {
          row["constrained"] = true;
          markConstrainedRowM4(row);
          continue;
        }
        if (z == cnsZero.end()) continue;
        std::string nm = en;
        if (nm == "(intercept)") nm = "_cons";
        if (z->second.count(nm) == 0) continue;
        for (const char* k : {"b", "se", "t", "z", "p", "ci_lo", "ci_hi"})
          if (row.contains(k)) row[k] = nullptr;
      }
    }
    std::vector<std::string> engOf;  // outcome position -> engine spelling of its value
    if (raw.contains("equations"))
      for (const auto& e : raw["equations"]) {
        char eng[64];
        std::snprintf(eng, sizeof(eng), "%.17g", e.value("outcome", 0.0));
        engOf.push_back(eng);
      }
    block["chi2type"] = "Wald";
    block["constraint_lines"] = constraintLinesM4(cnsDefs, [&](const est_detail::CnsTermM4& t) {
      size_t pos = 0;
      if (!t.eq.empty() && t.eq[0] == '#') pos = static_cast<size_t>(std::max(1, std::atoi(t.eq.c_str() + 1)) - 1);
      std::string engv = pos < engOf.size() ? engOf[pos] : std::string();
      if (!t.eq.empty() && t.eq[0] == '=') {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", std::strtod(t.eq.c_str() + 1, nullptr));
        engv = buf;
        for (size_t i = 0; i < engOf.size(); ++i)
          if (engOf[i] == engv) pos = i;
      }
      const std::string eqName = pos < eqNames.size() ? eqNames[pos] : engv;
      const auto z = cnsZero.find(engv);
      const bool fixed0 = z != cnsZero.end() && z->second.count(t.name) != 0;
      return "[" + eqName + "]" + (fixed0 ? "o." : "") + t.name;
    });
  }
  return block;
}

// `zip`/`zinb depvar indepvars [if] [fw pw], inflate(varlist | _cons) [probit
// exposure() offset() iterate() irr level() vce()]`: the engine fits zip.ado's /
// zinb.ado's constant-only then full model (cmds_zicount.cpp run_zi_ml), and the
// e() contract is posted here from its reply (vs-stata bank ce0a6a9f `zip ten age
// grade, inflate(married south)`, d496c5be the zinb twin): e(b)/e(V) on the
// `depvar:` / `inflate:` (/lnalpha) stripe, e(gradient), e(ilog), e(ll_0), the
// LR (default VCE) or Wald model test of the count slopes, and zip's macros.
// Factor variables (base-level columns of their own on the stripe) and
// iweights are refused by name.
ojson runZeroInflated(const std::string& cobraCmd, const eval::ParsedCommand& parsed,
                      SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  const bool nb = cobraCmd == "zinb";
  // The options this fit reads; zip.ado's others are real but not implemented. A
  // word in neither is r(198) (bank 7d468089 `..., zznotanoption`; 2a764ccb `...,
  // nolog`: zip's own parser answers "invalid 'nolog'", r(198)).
  {
    // kVocabZip (estimation_options.cpp) is registered for both tokens, so the
    // dispatcher has already resolved every abbreviation to its full name here.
    if (auto e = eval::rejectUnknownOptions(parsed.opts, kVocabZip.known, kVocabZip.unimplemented))
      return ojson(*e);
    // A second comma at the top level of the option list is not an option
    // separator to zip.ado's parser: the word after it is "invalid" (bank 2a764ccb
    // `..., inflate(married south), nolog`: r(198) invalid 'nolog').
    int depth = 0, commas = 0;
    bool inQ = false;
    for (size_t i = 0; i < parsed.raw.size(); ++i) {
      const char c = parsed.raw[i];
      if (c == '"') inQ = !inQ;
      if (inQ) continue;
      if (c == '(') ++depth;
      else if (c == ')') --depth;
      else if (c == ',' && depth == 0 && ++commas == 2) {
        std::istringstream rest(parsed.raw.substr(i + 1));
        std::string w;
        rest >> w;
        const size_t paren = w.find('(');
        if (paren != std::string::npos) w = w.substr(0, paren);
        return extended_fit_detail::stataRefusalM4b("invalid '" + w + "'", 198);
      }
    }
  }
  P7MleArgv b = buildP7MleArgv(cobraCmd, parsed, {"iid", "robust", "cluster"});
  if (b.errored) return b.errorBlock;
  std::optional<std::string> inflate;
  bool irr = false, probit = false;
  std::string exposure, offset;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "inflate" && !v.isBool) inflate = trimM4(v.strValue);
    else if (k == "irr" && v.isBool && v.boolValue) irr = true;
    else if (k == "probit" && v.isBool && v.boolValue) probit = true;
    else if (k == "exposure" && !v.isBool) exposure = trimM4(v.strValue);
    else if (k == "offset" && !v.isBool) offset = trimM4(v.strValue);
  }
  if (!inflate.has_value()) {
    return engineErrorBlockM4("syntax_error: " + cobraCmd + " requires inflate()", "syntax_error");
  }
  const auto isFactorish = [](const std::string& t) { return t.find_first_of(".#") != std::string::npos; };
  std::vector<std::string> infl;
  {
    std::istringstream iss(*inflate);
    for (std::string t; iss >> t;) infl.push_back(t);
  }
  bool factors = false;
  for (size_t i = 1; i < parsed.varlist.size(); ++i) factors = factors || isFactorish(parsed.varlist[i]);
  for (const auto& t : infl) factors = factors || isFactorish(t) || t.find(',') != std::string::npos;
  if (infl.empty() || factors) {
    return engineErrorBlockM4(
        "Option `inflate(" + *inflate + ")` with factor variables or suboptions, or a factor-variable "
        "count equation, is a valid Stata form that this version of PIET does not implement for `" +
            cobraCmd + "`.",
        "unsupported_option");
  }
  if (!exposure.empty() && !offset.empty())
    return extended_fit_detail::stataRefusalM4b("cannot specify both exposure() and offset()", 198);
  std::string inflCsv;
  if (!(infl.size() == 1 && infl[0] == "_cons"))
    for (const auto& t : infl)
      if (t != "_cons") inflCsv += (inflCsv.empty() ? "" : ",") + t;
  b.argv.push_back("--inflate");
  b.argv.push_back(inflCsv.empty() ? std::string("_cons") : inflCsv);
  if (probit) b.argv.push_back("--probit");
  // cluster(varname), unlike vce(cluster varname), is marked out of the whole
  // sample by zip.ado / zinb.ado before the zero count and the constant-only fit
  for (const auto& [k, v] : parsed.opts)
    if (k == "cluster" && !v.isBool && !trimM4(v.strValue).empty()) {
      b.argv.push_back("--cluster-markout");
      break;
    }
  if (!exposure.empty()) { b.argv.push_back("--exposure"); b.argv.push_back(exposure); }
  if (!offset.empty()) { b.argv.push_back("--offset"); b.argv.push_back(offset); }
  if (parsed.weightType.has_value()) {
    const std::string wt = lowerM4(*parsed.weightType);
    const std::string var = trimM4(parsed.weightVar.value_or(""));
    // zinb: frequency and sampling weights stay refused -- StataBE does not converge
    // on the bank's weighted fits (75da680e, b1fec1b0: r(430) after 300 nonconcave
    // iterations) and the pweight sandwich's inflation block is not Stata's
    // (7ba9aa16: V[inflate:married] 4.5751 vs 4.6559)
    if (wt == "iw" || wt == "iweight" || nb)
      return engineErrorBlockM4("Option `[" + *parsed.weightType + "=" + var + "]` is not supported in this version "
                                "of PIET for `" + cobraCmd + "`" +
                                    (nb ? std::string(" (no weights are)") : std::string(" (frequency and sampling weights are)")) + ".",
                                "unsupported_option");
    if (wt != "fw" && wt != "fweight" && wt != "pw" && wt != "pweight")
      return ojson(eval::errorBlock("engine_error", wt + "s not allowed", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(101)")));
    const bool pw = wt == "pw" || wt == "pweight";
    b.argv.push_back("--weight");
    b.argv.push_back(var);
    b.argv.push_back("--wtype");
    b.argv.push_back(pw ? "pweight" : "fweight");
    bool sandwich = false;
    for (size_t i = 0; i + 1 < b.argv.size(); ++i)
      if (b.argv[i] == "--vce" && b.argv[i + 1] != "iid") sandwich = true;
    if (pw && !sandwich) { b.argv.push_back("--vce"); b.argv.push_back("robust"); }
  }
  pushLevelIfGivenM4(parsed, b.argv);
  if (auto err = pushIterateM4(parsed, b.argv)) return *err;
  ojson raw;
  ojson block = runP7Mle(cobraCmd, b.argv, session, &raw, /*recordSample=*/true);
  if (!block.is_object() || block.value("type", std::string()) != "estimation" || !raw.is_object()) return block;
  const std::string y = parsed.varlist[0];
  // ---- e(b) / e(V) on Stata's stripe ----
  std::vector<AncillaryEbColM4> cols;
  std::vector<std::string> stripe;
  const auto addEq = [&](const char* key, const std::string& eq, const char* eng) {
    std::optional<AncillaryEbColM4> cons;
    for (const auto& c : raw.value(key, ojson::array())) {
      const std::string nm = c.value("name", std::string());
      AncillaryEbColM4 col{eq + ":" + (nm == "(intercept)" ? std::string("_cons") : nm),
                           std::string(eng) + ":" + nm, c.value("b", 0.0)};
      if (nm == "(intercept)") cons = col;
      else cols.push_back(col);
    }
    if (cons) cols.push_back(*cons);
  };
  addEq("coefs", y, "count");
  addEq("inflate", "inflate", "inflate");
  if (nb && raw.contains("lnalpha")) cols.push_back({"/lnalpha", "lnalpha", raw["lnalpha"].value("b", 0.0)});
  if (!postAncillaryEbM4(session, raw, cols)) return block;
  for (const auto& c : cols) stripe.push_back(c.stata);
  const size_t K = cols.size();
  const auto putE = [&session](const std::string& name, eval::EvalMatrix m) {
    auto& store = session.eval.eMatrices;
    store.erase(std::remove_if(store.begin(), store.end(), [&name](const auto& q) { return q.first == name; }),
                store.end());
    store.emplace_back(name, std::move(m));
    session.noteEOnFit(name);
  };
  // engine order of the gradient: count (x..., intercept), inflate (z..., intercept), lnalpha
  {
    std::vector<std::string> engOrder;
    for (const auto& c : raw.value("coefs", ojson::array())) engOrder.push_back("count:" + c.value("name", std::string()));
    for (const auto& c : raw.value("inflate", ojson::array())) engOrder.push_back("inflate:" + c.value("name", std::string()));
    if (nb) engOrder.push_back("lnalpha");
    const ojson& gr = raw.value("gradient", ojson::array());
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.colnames = stripe;
    m.data.assign(1, std::vector<double>(K, 0.0));
    for (size_t j = 0; j < K; ++j)
      for (size_t e = 0; e < engOrder.size() && e < gr.size(); ++e)
        if (engOrder[e] == cols[j].engine && gr[e].is_number()) m.data[0][j] = gr[e].get<double>();
    putE("gradient", std::move(m));
  }
  {
    eval::EvalMatrix m;
    m.rownames = {"r1"};
    m.data.assign(1, std::vector<double>(20, 0.0));
    for (int c = 1; c <= 20; ++c) m.colnames.push_back("c" + std::to_string(c));
    size_t i = 0;
    for (const auto& v : raw.value("ilog", ojson::array())) {
      if (i >= 20) break;
      if (v.is_number()) m.data[0][i] = v.get<double>();
      ++i;
    }
    putE("ilog", std::move(m));
  }
  // ---- scalars and macros (zip.ado / zinb.ado, ml) ----
  const auto setS = [&session](const char* n, double v) { e3::setEScalarOnFit(session, n, v); };
  const auto setM = [&session](const char* n, const std::string& v) { e3::setEStringOnFit(session, n, v); };
  const std::string vce = raw.value("vce", std::string("iid"));
  const bool pseudo = vce != "iid";
  const double kz = static_cast<double>(raw.value("inflate", ojson::array()).size());
  setS("N", raw.value("n_obs", 0.0));
  setS("N_zero", raw.value("n_zero", 0.0));
  setS("ll", raw.value("ll", 0.0));
  if (raw.contains("ll_0") && raw["ll_0"].is_number()) setS("ll_0", raw["ll_0"].get<double>());
  if (raw.contains("chi2") && raw["chi2"].is_number()) {
    setS("chi2", raw["chi2"].get<double>());
    // The LR test is ml's against lf0(1 ll_0): its e(p) is taken on k - 1 degrees of
    // freedom, the constant-only model counted as one parameter, while zip.ado then
    // posts e(df_m) as the count slopes (bank ce0a6a9f: chi2 130.888, df_m 2, p
    // 1.5421e-26 = chi2tail(5, chi2); d496c5be zinb: p = chi2tail(6, 36.643)).
    if (raw.value("chi2type", std::string()) == "LR")
      setS("p", chi2UpperTailM4(static_cast<double>(cols.size()) - 1.0, raw["chi2"].get<double>()));
    else if (raw.contains("p_chi2") && raw["p_chi2"].is_number())
      setS("p", raw["p_chi2"].get<double>());
  }
  setS("df_m", raw.value("df_chi2", 0.0));
  setS("df_c", kz);
  setS("ic", raw.value("ic", 0.0));
  setS("converged", raw.value("converged", false) ? 1.0 : 0.0);
  setS("rc", 0.0);
  setS("k", static_cast<double>(K));
  setS("rank", static_cast<double>(K));
  setS("k_eq", nb ? 3.0 : 2.0);
  setS("k_eq_model", 1.0);
  setS("k_dv", 1.0);
  setS("k_autoCns", 0.0);
  setS("noconstant", 0.0);
  setS("consonly", 0.0);
  if (nb) setS("k_aux", 1.0);
  if (raw.contains("n_clusters")) setS("N_clust", raw.value("n_clusters", 0.0));
  setM("cmd", cobraCmd);
  setM("depvar", y);
  setM("title", nb ? "Zero-inflated negative binomial regression" : "Zero-inflated Poisson regression");
  setM("inflate", probit ? "probit" : "logit");
  setM("predict", "zip_p");
  setM("user", std::string(nb ? "zinb" : "zip") + (probit ? "_plf" : "_llf"));
  setM("chi2type", raw.value("chi2type", std::string("LR")));
  setM("crittype", pseudo ? "log pseudolikelihood" : "log likelihood");
  setM("ml_method", "e2");
  setM("opt", "moptimize");
  setM("technique", "nr");
  setM("singularHmethod", "m-marquardt");
  setM("which", "max");
  setM("deriv_useminbound", "off");
  setM("properties", "b V");
  setM("vce", vce == "iid" ? "oim" : vce);
  if (pseudo) setM("vcetype", "Robust");
  if (vce == "cluster") setM("clustvar", raw.value("cluster_var", std::string()));
  // the count equation's offset (bank 80e6a489 `zip ..., exposure(hours)`: e(offset1) ln(hours))
  if (!exposure.empty()) setM("offset1", "ln(" + exposure + ")");
  else if (!offset.empty()) setM("offset1", offset);
  if (parsed.weightType.has_value()) {
    const std::string wt = lowerM4(*parsed.weightType);
    setM("wtype", wt == "fw" || wt == "fweight" ? "fweight" : "pweight");
    setM("wexp", "= " + trimM4(parsed.weightVar.value_or("")));
  }
  // e(V_modelbased) under a sandwich: the inverse of -H (bank 227d2d83)
  if (raw.contains("e_Vmb") && raw["e_Vmb"].is_object()) {
    const ojson& vn = raw["e_Vmb"].value("names", ojson::array());
    const ojson& vm = raw["e_Vmb"].value("matrix", ojson::array());
    std::vector<long> at(K, -1);
    bool ok = vn.is_array() && vm.is_array() && vm.size() == vn.size();
    for (size_t a = 0; ok && a < K; ++a) {
      for (size_t j = 0; j < vn.size(); ++j)
        if (vn[j].is_string() && vn[j].get<std::string>() == cols[a].engine) at[a] = static_cast<long>(j);
      ok = at[a] >= 0;
    }
    if (ok) {
      eval::EvalMatrix m;
      m.rownames = stripe;
      m.colnames = stripe;
      m.data.assign(K, std::vector<double>(K, 0.0));
      for (size_t a = 0; a < K; ++a)
        for (size_t c = 0; c < K; ++c) {
          const ojson& r1 = vm[static_cast<size_t>(at[a])];
          const ojson& r2 = vm[static_cast<size_t>(at[c])];
          if (r1.is_array() && r2.is_array() && r1[static_cast<size_t>(at[c])].is_number() &&
              r2[static_cast<size_t>(at[a])].is_number())
            m.data[a][c] = 0.5 * (r1[static_cast<size_t>(at[c])].get<double>() + r2[static_cast<size_t>(at[a])].get<double>());
        }
      putE("V_modelbased", std::move(m));
    }
  }
  if (nb) setM("diparm1", "lnalpha, exp label(\"alpha\")");
  // ---- the reported table: each equation, then (zinb) /lnalpha and alpha ----
  // `irr` reports incidence-rate ratios in the COUNT equation only ("Estimates are
  // transformed only in the first equation"): r(table) carries them with eform 1
  // while inflate: stays in its own metric; e(b) is untouched.
  ojson rt = ojson::array();
  ojson outcomes = ojson::array();
  const auto rowsOf = [&](const char* key, const std::string& eq, bool eform) {
    ojson ct = ojson::array();
    std::optional<ojson> cons;
    for (const auto& c : raw.value(key, ojson::array())) {
      ojson r = c;
      const std::string nm = c.value("name", std::string());
      r["name"] = nm == "(intercept)" ? std::string("_cons") : nm;
      if (eform && r.contains("b") && r["b"].is_number()) {
        const double e = std::exp(r["b"].get<double>());
        r["b"] = e;
        if (r.contains("se") && r["se"].is_number()) r["se"] = e * r["se"].get<double>();
        if (r.contains("ci_lo") && r["ci_lo"].is_number()) r["ci_lo"] = std::exp(r["ci_lo"].get<double>());
        if (r.contains("ci_hi") && r["ci_hi"].is_number()) r["ci_hi"] = std::exp(r["ci_hi"].get<double>());
      }
      ojson col = c;
      col["name"] = eq + ":" + r["name"].get<std::string>();
      col["eform"] = eform;
      if (nm == "(intercept)") {
        cons = r;
        (*cons)["__col"] = col;
      } else {
        ct.push_back(r);
        rt.push_back(col);
      }
    }
    if (cons) {
      rt.push_back((*cons)["__col"]);
      cons->erase("__col");
      ct.push_back(*cons);
    }
    outcomes.push_back({{"depvar", eq}, {"coeftable", ct}});
  };
  rowsOf("coefs", y, irr);
  rowsOf("inflate", "inflate", false);
  if (nb && raw.contains("lnalpha")) {
    ojson c = raw["lnalpha"];
    c["name"] = "/lnalpha";
    c["eform"] = false;
    rt.push_back(c);
    ojson a = raw.value("alpha", ojson::object());
    ojson r1 = raw["lnalpha"];
    r1["name"] = "/lnalpha";
    ojson r2 = a;
    r2["name"] = "alpha";
    outcomes.push_back({{"depvar", "/"}, {"coeftable", ojson::array({r1})}});
    block["rtable_diparm"] = ojson::array({ojson{{"name", "_diparm1:alpha"}, {"b", a.value("b", 0.0)},
                                                 {"se", a.value("se", 0.0)}, {"ci_lo", a.value("ci_lo", 0.0)},
                                                 {"ci_hi", a.value("ci_hi", 0.0)}}});
  }
  block["rtable_cols"] = rt;
  block["outcomes"] = outcomes;
  block["depvar"] = y;
  block["n_obs"] = raw.value("n_obs", 0.0);
  block.erase("coeftable");
  if (session.lastEstBlock.has_value() && session.lastEstBlock->is_object()) {
    (*session.lastEstBlock)["rtable_cols"] = block["rtable_cols"];
    if (block.contains("rtable_diparm")) (*session.lastEstBlock)["rtable_diparm"] = block["rtable_diparm"];
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
