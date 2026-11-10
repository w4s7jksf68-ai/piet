// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// macro matrices: see README.md for ownership and call flow.
#include "piet/eval/macros.h"
#include "estimators/postest_names.h"  // coefFullName: the engine's coefficient-name rule

#include "dispatch/dispatch_profile.h"  // WS-D1: per-line regex counter (opt-in, stderr)

#ifndef _WIN32
#include <fnmatch.h>  // WP-W5: POSIX-only; the Windows branch uses globMatchPeriod below
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <unordered_set>
#include <vector>

#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/pyvalue.h"
#include "piet/strutil.h"
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "core/expr_blanks.h"  // FU-875: the blanks Stata drops inside ( ) and [ ]
#include "data/stata_dates.h"  // the literal pseudo-function scan (liftDateLiterals)
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include "macro_expressions_internal.h"
#include "macro_matrices_internal.h"
#include "macro_results_internal.h"
#include "macro_text_internal.h"
#include "macros_internal.h"

namespace piet::eval { namespace macro_detail {} }

namespace piet::eval {
using namespace macro_detail;


// eb-order T-P1 (SPEC B.4 "One builder", rules 1-3 and 5; declared in
// eval/session.h). Stata's rule, from the oracle records only (SPEC A):
// `_cons` is the last column of its equation and is never spelled
// `(intercept)` (w2g_ebmat.log:230 `x1 x2 _cons` after `regress y x1 x2`, where
// the engine's feols coeftable puts `(intercept)` first); the other columns
// keep the order the engine emitted (IV: endogenous, exogenous, then `_cons`,
// w2g_ebmat.log:536); e(V) is the fit's full covariance in that same order with
// its real off-diagonals (:365 V[1,2] = -1.f7d0ccaeed4b1X-00a). Equation-
// prefixed names (`eq:name`) are grouped by equation in the order the
// equations first appear. Base/omitted factor columns are not in the engine's
// coeftable yet (SPEC Q1, task T-E2) and are not invented here.
std::optional<EvalMatrix> canonicalEMatrix(const nlohmann::ordered_json& block, char which,
                                           std::string* why) {
  const auto fail = [why](std::string w) -> std::optional<EvalMatrix> {
    if (why != nullptr) *why = std::move(w);
    return std::nullopt;
  };
  const std::string label = std::string("e(") + which + ")";
  // A reg3/sureg system: no single table, one `outcomes` entry per equation
  // ({depvar, coeftable}); each column is `depvar:coef` (Stata's reg3/sureg
  // e(b) stripe, `y:x1 y:x2 y:_cons x3:x2 x3:_cons`).
  nlohmann::ordered_json systemTable;
  const bool system = block.is_object() && block.value("estimator", std::string()) == "reg3" &&
                      !block.contains("coeftable") && block.contains("outcomes") &&
                      block.at("outcomes").is_array();
  if (system) {
    systemTable = nlohmann::ordered_json::array();
    for (const auto& o : block.at("outcomes")) {
      if (!o.is_object() || !o.contains("coeftable") || !o.at("coeftable").is_array()) continue;
      const std::string eq = o.value("depvar", std::string());
      for (const auto& row : o.at("coeftable")) {
        if (!row.is_object() || !row.contains("name") || !row.at("name").is_string()) continue;
        nlohmann::ordered_json r = row;
        r["name"] = eq + ":" + row.at("name").get<std::string>();
        systemTable.push_back(std::move(r));
      }
    }
  }
  const nlohmann::ordered_json* table =
      system ? &systemTable
             : (block.is_object() && block.contains("coeftable") ? &block.at("coeftable") : nullptr);
  if (table == nullptr || !table->is_array() || table->empty())
    return fail("matrix " + label + " not found");
  struct Col {
    std::string engine;  // the coeftable's own spelling
    std::string eq;      // "" for a single-equation name
    std::string bare;    // the name after the equation, `(intercept)` spelled `_cons`
    double b = std::numeric_limits<double>::quiet_NaN();
  };
  std::vector<Col> cols;
  for (const auto& row : *table) {
    if (!row.is_object() || !row.contains("name") || !row.at("name").is_string()) continue;
    Col c;
    c.engine = row.at("name").get<std::string>();
    const size_t colon = c.engine.find(':');
    c.eq = colon == std::string::npos ? std::string() : c.engine.substr(0, colon);
    c.bare = colon == std::string::npos ? c.engine : c.engine.substr(colon + 1);
    if (c.bare == "(intercept)") c.bare = "_cons";
    if (row.contains("b") && !row.at("b").is_null()) {
      if (auto f = jsonToFloat(row.at("b"))) c.b = *f;
    }
    cols.push_back(std::move(c));
  }
  if (cols.empty()) return fail("matrix " + label + " not found");
  // Equation order of first appearance; within each, `_cons` after the rest.
  std::vector<std::string> eqs;
  for (const auto& c : cols)
    if (std::find(eqs.begin(), eqs.end(), c.eq) == eqs.end()) eqs.push_back(c.eq);
  std::vector<size_t> order;
  for (const auto& eq : eqs)
    for (int consPass = 0; consPass < 2; ++consPass)
      for (size_t i = 0; i < cols.size(); ++i)
        if (cols[i].eq == eq && (cols[i].bare == "_cons") == (consPass == 1)) order.push_back(i);
  EvalMatrix m;
  for (size_t i : order)
    m.colnames.push_back(cols[i].eq.empty() ? cols[i].bare : cols[i].eq + ":" + cols[i].bare);
  const size_t k = order.size();
  if (which == 'b') {
    m.rownames = {"y1"};
    std::vector<double> row;
    for (size_t i : order) row.push_back(cols[i].b);
    m.data.push_back(std::move(row));
    return m;
  }
  // e(V): the engine's full covariance (`e_V` = {names, matrix}, the shape of
  // apps/cmds/common.cpp add_e_v), permuted by name. Without it there is no
  // e(V) to give: the coeftable's standard errors are only its diagonal.
  const auto eVit = block.find("e_V");
  if (eVit == block.end() || !eVit->is_object() || !eVit->contains("names") ||
      !eVit->contains("matrix") || !eVit->at("names").is_array() || !eVit->at("matrix").is_array())
    return fail("no-e_V");
  const auto& vnames = eVit->at("names");
  const auto& vmat = eVit->at("matrix");
  std::vector<size_t> at(k, std::string::npos);
  for (size_t a = 0; a < k; ++a) {
    const Col& c = cols[order[a]];
    for (size_t j = 0; j < vnames.size(); ++j) {
      if (!vnames[j].is_string()) continue;
      const std::string vn = vnames[j].get<std::string>();
      if (vn == c.engine || (c.bare == "_cons" && (vn == (c.eq.empty() ? "_cons" : c.eq + ":_cons") ||
                                                   vn == (c.eq.empty() ? "(intercept)"
                                                                       : c.eq + ":(intercept)")))) {
        at[a] = j;
        break;
      }
    }
    if (at[a] == std::string::npos || at[a] >= vmat.size())
      return fail("the fit's covariance matrix does not name the coefficient `" + c.engine + "`");
  }
  const auto cell = [&vmat](size_t r, size_t c) -> double {
    if (r >= vmat.size() || !vmat[r].is_array() || c >= vmat[r].size()) return std::nan("");
    const auto& v = vmat[r][c];
    if (v.is_null()) return std::numeric_limits<double>::quiet_NaN();
    auto f = jsonToFloat(v);
    return f ? *f : std::numeric_limits<double>::quiet_NaN();
  };
  m.rownames = m.colnames;
  m.data.assign(k, std::vector<double>(k, 0.0));
  for (size_t a = 0; a < k; ++a)
    for (size_t c = 0; c < k; ++c)
      // Stata's e(V) is exactly symmetric; an engine sandwich can differ from
      // its transpose in the last bit, and the mean of the two is the same
      // value whenever the engine's matrix is symmetric already.
      m.data[a][c] = 0.5 * (cell(at[a], at[c]) + cell(at[c], at[a]));
  return m;
}


std::optional<std::vector<EbStripeEntry>> ebStripeOf(const nlohmann::ordered_json& block,
                                                     const std::vector<std::string>& canonNames) {
  if (!block.is_object()) return std::nullopt;
  const auto it = block.find("eb_stripe");
  if (it == block.end() || !it->is_array() || it->empty()) return std::nullopt;
  std::vector<EbStripeEntry> out;
  std::vector<int> used(canonNames.size(), 0);
  for (const auto& e : *it) {
    if (!e.is_object() || !e.contains("name") || !e.at("name").is_string()) return std::nullopt;
    EbStripeEntry en;
    en.name = e.at("name").get<std::string>();
    if (e.contains("src") && e.at("src").is_string()) en.src = e.at("src").get<std::string>();
    if (!en.src.empty()) {
      const auto f = std::find(canonNames.begin(), canonNames.end(), en.src);
      if (f == canonNames.end()) return std::nullopt;
      if (++used[static_cast<size_t>(f - canonNames.begin())] > 1) return std::nullopt;
    }
    out.push_back(std::move(en));
  }
  for (int u : used)
    if (u != 1) return std::nullopt;
  return out;
}


bool applyEbStripe(const nlohmann::ordered_json& block, char which, EvalMatrix& m) {
  const std::optional<std::vector<EbStripeEntry>> st = ebStripeOf(block, m.colnames);
  if (!st.has_value()) return false;
  const size_t k = st->size();
  std::vector<long> at(k, -1);
  for (size_t a = 0; a < k; ++a) {
    if ((*st)[a].src.empty()) continue;
    at[a] = static_cast<long>(std::find(m.colnames.begin(), m.colnames.end(), (*st)[a].src) -
                              m.colnames.begin());
  }
  EvalMatrix out;
  for (const auto& e : *st) out.colnames.push_back(e.name);
  if (which == 'b') {
    out.rownames = m.rownames;
    out.data.assign(m.data.size(), std::vector<double>(k, 0.0));
    for (size_t r = 0; r < m.data.size(); ++r)
      for (size_t a = 0; a < k; ++a)
        if (at[a] >= 0) out.data[r][a] = m.data[r][static_cast<size_t>(at[a])];
  } else {
    out.rownames = out.colnames;
    out.data.assign(k, std::vector<double>(k, 0.0));
    for (size_t a = 0; a < k; ++a)
      for (size_t c = 0; c < k; ++c)
        if (at[a] >= 0 && at[c] >= 0)
          out.data[a][c] = m.data[static_cast<size_t>(at[a])][static_cast<size_t>(at[c])];
  }
  m = std::move(out);
  return true;
}


std::vector<EbColumnRow> stataEbColumns(const nlohmann::ordered_json& block) {
  std::vector<EbColumnRow> out;
  if (!block.is_object() || !block.contains("coeftable") || !block.at("coeftable").is_array())
    return out;
  // canonicalEMatrix's order, by the same rule: equations in order of first
  // appearance, `_cons` last within each.
  struct C {
    std::string full;
    std::string eq;
    bool cons = false;
    const nlohmann::ordered_json* row = nullptr;
  };
  std::vector<C> cols;
  for (const auto& r : block.at("coeftable")) {
    if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
    const std::string n = r.at("name").get<std::string>();
    const size_t colon = n.find(':');
    C c;
    c.eq = colon == std::string::npos ? std::string() : n.substr(0, colon);
    std::string bare = colon == std::string::npos ? n : n.substr(colon + 1);
    if (bare == "(intercept)") bare = "_cons";
    c.cons = bare == "_cons";
    c.full = c.eq.empty() ? bare : c.eq + ":" + bare;
    c.row = &r;
    cols.push_back(std::move(c));
  }
  std::vector<std::string> eqs;
  for (const auto& c : cols)
    if (std::find(eqs.begin(), eqs.end(), c.eq) == eqs.end()) eqs.push_back(c.eq);
  std::vector<std::string> names;
  std::vector<const nlohmann::ordered_json*> rows;
  for (const auto& eq : eqs)
    for (int pass = 0; pass < 2; ++pass)
      for (const auto& c : cols)
        if (c.eq == eq && c.cons == (pass == 1)) {
          names.push_back(c.full);
          rows.push_back(c.row);
        }
  if (const auto st = ebStripeOf(block, names)) {
    for (const auto& e : *st) {
      EbColumnRow cr;
      cr.name = e.name;
      if (!e.src.empty())
        cr.row = rows[static_cast<size_t>(std::find(names.begin(), names.end(), e.src) -
                                          names.begin())];
      out.push_back(std::move(cr));
    }
    return out;
  }
  for (size_t i = 0; i < names.size(); ++i) out.push_back({names[i], rows[i]});
  return out;
}


std::string coefFullName(const nlohmann::ordered_json& block, const std::string& name) {
  if (!block.is_object()) return std::string();
  std::vector<std::string> names;
  for (const char* key : {"coeftable", "eb_stripe"}) {
    const auto it = block.find(key);
    if (it == block.end() || !it->is_array()) continue;
    for (const auto& r : *it)
      if (r.is_object() && r.contains("name") && r.at("name").is_string()) names.push_back(r.at("name").get<std::string>());
  }
  const cecon::postest::CoefRef r = cecon::postest::resolve_coef(names, name, nullptr);
  if (r.kind != cecon::postest::CoefKind::Column) return std::string();
  const std::string& full = names[static_cast<size_t>(r.idx)];
  return full == name ? std::string() : full;
}


std::optional<double> ebStripeCoef(const nlohmann::ordered_json& block, const std::string& name,
                                   const std::string& kind) {
  if (!block.is_object()) return std::nullopt;
  const auto it = block.find("eb_stripe");
  if (it == block.end() || !it->is_array()) return std::nullopt;
  const size_t colon = name.find(':');
  const std::string bare = colon == std::string::npos ? name : name.substr(colon + 1);
  // `0b.g` is also read as `0.g`: the level number, then the variable.
  const auto unmarked = [](const std::string& n) {
    size_t i = 0;
    while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i])) != 0) ++i;
    if (i > 0 && i + 1 < n.size() && n[i] == 'b' && n[i + 1] == '.') return n.substr(0, i) + n.substr(i + 1);
    return n;
  };
  // Every atom's base/omitted marker dropped: `1o.t#0b.e` is read as `1.t#0.e`,
  // `0b.a#1o.b` as `0.a#1.b` (StataBE bc3.do: `_b[1.t#0.e]` is 0 after reghdfe
  // omits that cell; the engine sends no row for it since BUG-1476).
  const auto plain = [](const std::string& n) {
    static const std::regex reLevel(R"(^(\d+)(?:bn|b|o|bo|ob)\.)");
    std::string out;
    size_t start = 0;
    for (;;) {
      const size_t hash = n.find('#', start);
      std::string atom = n.substr(start, hash == std::string::npos ? std::string::npos : hash - start);
      atom = std::regex_replace(atom, reLevel, "$1.");
      if (atom.rfind("co.", 0) == 0) atom = "c." + atom.substr(3);
      else if (atom.rfind("o.", 0) == 0) atom = atom.substr(2);
      out += atom;
      if (hash == std::string::npos) break;
      out += '#';
      start = hash + 1;
    }
    return out;
  };
  const std::string barePlain = plain(bare);
  for (const auto& e : *it) {
    if (!e.is_object() || !e.contains("name") || !e.at("name").is_string()) continue;
    const std::string nm = e.at("name").get<std::string>();
    const std::string src = e.contains("src") && e.at("src").is_string()
                                ? e.at("src").get<std::string>()
                                : std::string();
    if (src.empty()) {
      if (bare == nm || bare == unmarked(nm) || barePlain == plain(nm)) return 0.0;
      continue;
    }
    if (bare != nm || nm == src) continue;
    // An `o.` spelling: the engine's row under its bare name.
    if (!block.contains("coeftable") || !block.at("coeftable").is_array()) return std::nullopt;
    for (const auto& r : block.at("coeftable")) {
      if (!r.is_object() || r.value("name", std::string()) != src) continue;
      const char* field = kind == "b" ? "b" : "se";
      if (!r.contains(field) || r.at(field).is_null()) return std::nullopt;
      return jsonToFloat(r.at(field));
    }
  }
  return std::nullopt;
}


std::optional<double> ancillaryEbCoef(const EvalSession& session, const std::string& name,
                                      const std::string& kind) {
  // Also a factor level, `#.var` or `#bn.var`, which proportion's posted columns name
  // without its engine row (`0bn.d 1.d`): StataBE 19.5 answers `_b[0.d]`, `_b[0bn.d]`,
  // `_b[1bn.d]` and `_se[1.d]` alike after `proportion d` (scratch-laneA2/agents/epost/
  // s11.log), and `_b[d:0]` is r(111).
  static const std::regex reLevel(R"(^(\d+)(?:bn)?\.([A-Za-z_][A-Za-z0-9_]*)$)");
  const auto plainLevel = [](const std::string& n) {
    std::smatch m;
    return std::regex_match(n, m, reLevel) ? m.str(1) + "." + m.str(2) : std::string();
  };
  const std::string level = plainLevel(name);
  if (name.size() < 2 || (name.front() != '/' && level.empty()) ||
      session.coefOverride != nullptr)
    return std::nullopt;
  const EvalMatrix* m = session.findClassMatrix('e', kind == "b" ? "b" : "V");
  if (m == nullptr) return std::nullopt;
  for (size_t j = 0; j < m->colnames.size(); ++j) {
    if (m->colnames[j] != name && (level.empty() || plainLevel(m->colnames[j]) != level)) continue;
    if (kind == "b") {
      if (m->data.size() != 1 || m->data[0].size() <= j) return std::nullopt;
      return m->data[0][j];
    }
    if (m->data.size() <= j || m->data[j].size() <= j || !(m->data[j][j] >= 0.0))
      return std::nullopt;
    return std::sqrt(m->data[j][j]);
  }
  return std::nullopt;
}

}

namespace piet::eval::macro_detail {




// eb-order ROUND 3: the CLOSED stripe table (internal capture
// DIAGNOSIS.md section 3). canonicalEMatrix keeps the
// engine's names, so what Stata's e(b)/e(V) is called -- and whether the
// engine's coefficient table holds all of its columns at all -- depends on the
// command. A command is BUILT only when a row below names it with the record
// that resolves its stripe; every other e(cmd) refuses every e(b)/e(V) reader
// by name (the last line of eStripeRowFor). Rounds 1 and 2 of this goal were
// one class: a deny list whose default answered as emitted (csdid's one `ATT`
// column where Stata's e(b) has one column per ATT(g,t) cell).
//   kAsEmitted          -- the coeftable's names (equation-less in Stata).
//   kDepvarEq           -- every column in one equation named after the depvar.
//   kDepvarEqUnderscore -- the same, the depvar's `.` spelled `_`.
//   kAbsent             -- Stata's e() has no e(b)/e(V) (the 0x0 path:
//                          EvalSession::eMatrixAbsentFromLiveE).
//   kRefuse             -- a named r(198) with `reason`.
// A matrix POSTED for the live e() (glm, oaxaca, teffects, `ereturn post`,
// erepost, estadd matrix, `estat ..., post`) is answered before this table.
enum class EStripe { kAsEmitted, kDepvarEq, kDepvarEqUnderscore, kAbsent, kRefuse };


// What the coefficient table must say about `_cons` for the matrix to be whole:
//   kNeedCons   -- Stata keeps `_cons` unless the fit was noconstant; a table
//                  without one is built only when the fit's block records
//                  `noconstant` (runRegress), since areg/xtreg fe/reghdfe drop
//                  it from the table when a regressor is collinear with the
//                  absorbed effects while Stata's e(b) keeps it
//                  (w2d_uncovered.log:3195-3221; FU-986);
//   kNoCons     -- Stata's e(b) has no `_cons` (the conditional fe fits);
//   kAnyCons    -- the names are Stata's own (the margins/nlcom postings).
enum class ConsRule { kNeedCons, kNoCons, kAnyCons };


struct EStripeRow {
  const char* key;
  EStripe rule;
  ConsRule cons;
  const char* reason;  // kRefuse only
};


constexpr const char* kNoStripeRecord = "";
  // reason filled from the key

// Section 3 rows. Each answering row cites the record that resolves it.
const EStripeRow kEStripeTable[] = {
    // w2g_ebmat.log:402 `e(b)[1,3]` x1 x2 _cons; :230 e1_colnames_eb `x1 x2 _cons`
    {"regress", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:798 `symmetric e(V)[3,3]` (x1 x2 _cons)
    {"areg", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:860 `symmetric e(V)[3,3]`
    {"reghdfe", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:594 `symmetric e(V)[3,3]` after `ivregress 2sls y x2 (x1 = id)`
    {"ivregress", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // aer_b4/ivreg2-partial.log:121 `e(b)[1,4]`
    {"ivreg2", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // ivreghdfe.ado (corpus 065) absorb() sets noconstant,
    // `_cons` only `if `cons''; the absorb rule is in resolveEMatrixFor
    {"ivreghdfe", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2p_estim_ev.log:755 B_xtreg_fe `|3|3|`; xtreg,re est post, no coleq
    {"xtreg", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // qreg posts `b' and `V' without coleq; w2j_e3_qreg2.log:229-232
    {"qreg", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // newey: bare coefficient names (the
    // census fit esc_newey failed r(198), so this stays the row's evidence)
    {"newey", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // prais: bare coefficient names
    {"prais", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // ppmlhdfe (PIET's e(cmd) spells it `fepois`, FU-967): ppmlhdfe.ado (corpus
    // 090) :298-302 `matrix colnames `b' = `indepvars'` (no coleq). Element
    // (1,1) is pinned by w2l_vce_shape_det.log ppmlhdfe_*_se, which bin/main
    // (FU-118) answers; w2v_estripe_census.log:1112 esc_ppmlhdfe `x1 x2 _cons`.
    {"fepois", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    {"ppmlhdfe", EStripe::kAsEmitted, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:653-656 `yb:x1 yb:x2 yb:_cons`; logistic fits `logit`
    {"logit", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    // e(cmd) `logistic` (BUG-1192): logit's fit, logit's stripe
    {"logistic", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    // w2g_ebmat.log:723
    {"probit", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    {"cloglog", EStripe::kDepvarEq, ConsRule::kNeedCons, nullptr},
    // poisson: a depvar's `.` becomes `_` in the equation name
    {"poisson", EStripe::kDepvarEqUnderscore, ConsRule::kNeedCons, nullptr},
    // xtlogit ,fe fits clogit and reposts nothing; the equation is
    // named `depvar:`.
    // The fe fit only (block estimator xtlogit_fe): re has /lnsig2u.
    {"xtlogit_fe", EStripe::kDepvarEq, ConsRule::kNoCons, nullptr},
    // xtpoisson ,fe: a depvar's `.` becomes `_` in the equation name.
    // The fe fit only.
    {"xtpoisson_fe", EStripe::kDepvarEqUnderscore, ConsRule::kNoCons, nullptr},
    // margins/nlcom `..., post`: the block's names are Stata's (w2v_margins_vce /
    // w2v_margins_corpus readers, 37/37 on bin/main)
    // reg3/sureg: one equation per depvar, each `depvar:coef` with `_cons`
    // last (reg3.ado's `y:x1 y:x2 y:_cons x3:x2 x3:_cons`); canonicalEMatrix
    // builds the columns from the block's per-equation tables and e(V) is the
    // engine's stacked system covariance. Neither accepts noconstant or factor
    // terms here (runReg3 refuses them), so the table's names are the stripe.
    {"reg3", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    {"sureg", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    {"margins", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    {"nlcom", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
    // No e(b): w2g_ebmat.log:2087 e15c_matrix_b `0|`, :2096 (pca);
    // w2v_estpost_ematrix ep13_b_shape `0|0|0||`; w2g_ebmat.log:2486
    // e15d_list_eb2 `111` (rddensity)
    {"pca", EStripe::kAbsent, ConsRule::kAnyCons, nullptr},
    {"estpost", EStripe::kAbsent, ConsRule::kAnyCons, nullptr},
    {"rddensity", EStripe::kAbsent, ConsRule::kAnyCons, nullptr},
    // REFUSE rows with a reason of their own (owners: DIAGNOSIS section 3).
    {"csdid", EStripe::kRefuse, ConsRule::kAnyCons,
     "Stata's e(b) after csdid has one column per ATT(g,t) cell (`g####:t_####_####`, "
     "csdid_kitchensink_oracle.log:61-87, :107), which the engine does not send"},
    {"drdid", EStripe::kRefuse, ConsRule::kAnyCons,
     "Stata posts e(b) as `ATET:r1vs0.<treatment>` (drdid), which this version "
     "does not build"},
    {"rdrobust", EStripe::kRefuse, ConsRule::kAnyCons,
     // runRdrobust posts e(b)/e(V) as e() matrices, which answer before this
     // table; the row answers only an rdrobust e() that carries neither.
     "Stata's e(b) after rdrobust is 1x3 (Conventional, Bias-corrected, Robust; "
     "w2g_ebmat.log:2305), and these results carry none"},
    {"mean", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"proportion", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"did_imputation", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"eventstudyinteract", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"sdid", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"stcox", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"fracreg", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"xtabond", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    {"mediate", EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord},
    // nl's free parameters are the engine's own row names (`/b0 /b1`), which is
    // e(b)'s stripe in StataBE 19.5 (BUG-1246: colfullnames `/b0 /b1`, el(e(b),1,2)
    // = _b[/b1]); no `_cons` column.
    {"nl", EStripe::kAsEmitted, ConsRule::kAnyCons, nullptr},
};


// Ancillary parameters or extra equations the engine's table does not carry
// (tobit/ivtobit var(e.y), nbreg lnalpha, ologit/oprobit cuts, mixed variance
// components, streg ancillaries, the selection/inflation equations of
// heckman/zip/zinb/ivprobit, the outcome equations of mlogit/var, the
// parameter equations of gmm; w2u_binary_ml_edges.log heckman `yh:... s:...
// /mills`; owner FU-968).
const char* const kAncillaryCmds[] = {"nbreg", "tobit", "ivtobit", "ologit", "oprobit", "mlogit",
                                      "zip",   "zinb",  "heckman", "ivprobit", "var",
                                      "gmm",   "mixed",   "streg"};


// The block spellings of a key, for the scoped coefOverride (no e(cmd)) and for
// the fe fits whose e(cmd) does not say fe.
std::string eStripeKeyOfEstimator(const std::string& est) {
  if (est == "feols") return "regress";
  if (est == "logistic") return "logit";
  const std::string head = est.substr(0, est.find(' '));  // "ivregress 2sls|gmm|liml"
  return head;
}


EStripeRow eStripeRowFor(const std::string& eCmd, const std::string& blockEstimator) {
  std::string key = eCmd.empty() ? eStripeKeyOfEstimator(blockEstimator) : eCmd;
  // e(cmd) `xtlogit`/`xtpoisson` covers re and fe; only the fe fit has a row.
  if (key == "xtlogit" || key == "xtpoisson")
    key = blockEstimator == key + "_fe" ? blockEstimator : key + " (not fe)";
  // xtlogit, fe with an empty cluster() posts the underlying clogit e(cmd).
  // The fitted block still identifies the conditional FE estimator and now
  // carries its full covariance, so it uses that same no-constant stripe.
  if (key == "clogit" && blockEstimator == "xtlogit_fe") key = "xtlogit_fe";
  for (const EStripeRow& r : kEStripeTable)
    if (key == r.key) return r;
  for (const char* c : kAncillaryCmds)
    if (key == c)
      return {c, EStripe::kRefuse, ConsRule::kAnyCons,
              "Stata's e(b) has equations or ancillary parameters the engine does not send"};
  return {nullptr, EStripe::kRefuse, ConsRule::kAnyCons, kNoStripeRecord};
}


// A factor-variable column (`2.g`, `1b.g`, `x1#2.g`, `2.g#c.x`): any `#`, or a
// part that starts with a level number followed by `.` (FU-118's rule).
bool isFactorColumn(const std::string& name) {
  if (name.find('#') != std::string::npos) return true;
  static const std::regex reLevel(R"((^|[:#])\d+[A-Za-z]*\.)");
  return std::regex_search(name, reLevel);
}


// Whether a column name already carries Stata's base (`1b.`, `1bn.`) or
// omitted (`o.`, `2o.`, `co.`) marker in any `:`/`#`-separated part.
bool hasBaseOrOmittedMarker(const std::string& name) {
  static const std::regex reMark(R"((^|[:#])(\d*(b|bn|o)|co)\.)");
  return std::regex_search(name, reMark);
}

}

namespace piet::eval {
using namespace macro_detail;


std::string depvarEquationOf(const EvalSession& session) {
  const nlohmann::ordered_json* source = session.coefSource();
  if (source == nullptr || !source->is_object()) return std::string();
  const std::string* eCmd = session.eStrings.find("cmd");
  const EStripeRow row = eStripeRowFor(eCmd != nullptr ? *eCmd : std::string(),
                                       source->value("estimator", std::string()));
  if (row.rule != EStripe::kDepvarEq && row.rule != EStripe::kDepvarEqUnderscore)
    return std::string();
  std::string eq = source->value("depvar", std::string());
  if (eq.empty())
    if (const std::string* dv = session.eStrings.find("depvar")) eq = *dv;
  if (row.rule == EStripe::kDepvarEqUnderscore) std::replace(eq.begin(), eq.end(), '.', '_');
  return eq;
}


std::string omittedColumnSpelling(const std::string& name) {
  const size_t colon = name.find(':');
  const std::string eq = colon == std::string::npos ? std::string() : name.substr(0, colon + 1);
  const std::string bare = colon == std::string::npos ? name : name.substr(colon + 1);
  if (bare.empty() || std::isdigit(static_cast<unsigned char>(bare[0])) != 0) return std::string();
  for (char c : bare)
    if (!(std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_')) return std::string();
  if (bare == "_cons") return std::string();
  return eq + "o." + bare;
}


bool ebHasUnsentColumns(const EvalSession& session) {
  const nlohmann::ordered_json* source = session.coefSource();
  const std::string* eCmd = session.eStrings.find("cmd");
  const std::string key =
      eCmd != nullptr && !eCmd->empty()
          ? *eCmd
          : (source != nullptr && source->is_object()
                 ? eStripeKeyOfEstimator(source->value("estimator", std::string()))
                 : std::string());
  for (const char* c : kAncillaryCmds)
    if (key == c) return true;
  return false;
}


EvalSession::EMatrixAnswer EvalSession::resolveEMatrixFor(const std::string& name,
                                                          EReader reader) const {
  EMatrixAnswer a;
  const auto fail = [&a](int code, std::string w, bool absent) {
    a.rc = code;
    a.why = std::move(w);
    a.absent = absent;
    return a;
  };
  if (const EvalMatrix* posted = findClassMatrix('e', name)) {
    a.matrix = *posted;
    return a;
  }
  // e(beta) and e(V_modelbased) of a fit whose block records them (regress's
  // `beta_std`, a built-in multiway fit's `e_Vmb`; estimation/estimation_results.cpp
  // postENameListsRegressM4): read at the moment of need from a local copy of
  // the live block with its coefficients (covariance) replaced, through the very
  // e(b) (e(V)) reader below, so they carry e(b)'s (e(V)'s) stripe -- the base
  // and `o.` columns annotateEbStripe records after the fit included (StataBE
  // 19.5 `regress y x1 i.g`: e(beta) `x1 1b.g 2.g 3.g` with a 0 base column;
  // the multiway-cluster e(V_modelbased) on e(V)'s stripe). e(beta)
  // drops `_cons` ([R] regress, Stored results) and its row is `r1` (StataBE
  // `matrix list e(beta)`).
  if ((name == "beta" || name == "V_modelbased") && eLive()) {
    const nlohmann::ordered_json* live = coefSource();
    const char* key = name == "beta" ? "beta_std" : "e_Vmb";
    if (live != nullptr && live->is_object() && live->contains(key) && live->at(key).is_object()) {
      nlohmann::ordered_json copy = *live;
      if (name == "V_modelbased") {
        copy["e_V"] = live->at(key);
        return resolveFitEMatrixFrom("V", reader, &copy);
      }
      const auto& beta = live->at(key);
      if (!copy.contains("coeftable") || !copy["coeftable"].is_array())
        return fail(111, "matrix e(beta) not found", true);
      for (auto& r : copy["coeftable"]) {
        if (!r.is_object() || !r.contains("name") || !r["name"].is_string())
          return fail(111, "matrix e(beta) not found", true);
        const std::string nm = r["name"].get<std::string>();
        if (nm == "(intercept)" || nm == "_cons") continue;
        const auto it = beta.find(nm);
        if (it != beta.end() && it->is_number()) {
          r["b"] = *it;
          continue;
        }
        // A column the fit omitted (b 0): Stata's e(beta) is 0 there too.
        const auto b = r.find("b");
        if (b == r.end() || !b->is_number() || b->get<double>() != 0.0)
          return fail(111, "matrix e(beta) not found", true);
      }
      EMatrixAnswer bm = resolveFitEMatrixFrom("b", reader, &copy);
      if (!bm.matrix.has_value()) {
        if (!bm.why.empty()) {
          const size_t at = bm.why.find("e(b)");
          if (at != std::string::npos) bm.why.replace(at, 4, "e(beta)");
        }
        return bm;
      }
      if (bm.matrix->data.size() != 1) return fail(111, "matrix e(beta) not found", true);
      EvalMatrix out;
      out.rownames = {"r1"};
      out.data.emplace_back();
      for (size_t j = 0; j < bm.matrix->colnames.size(); ++j) {
        const std::string& c = bm.matrix->colnames[j];
        const size_t colon = c.find(':');
        const std::string bare = colon == std::string::npos ? c : c.substr(colon + 1);
        if (bare == "_cons") continue;
        out.colnames.push_back(c);
        out.data[0].push_back(bm.matrix->data[0][j]);
      }
      if (out.colnames.empty()) return fail(111, "matrix e(beta) not found", true);
      a.matrix = std::move(out);
      return a;
    }
  }
  if (name != "b" && name != "V") return fail(111, "matrix e(" + name + ") not found", true);
  return resolveFitEMatrixFrom(name, reader, coefSource());
}


EvalSession::EMatrixAnswer EvalSession::resolveFitEMatrixFrom(
    const std::string& name, EReader reader, const nlohmann::ordered_json* source) const {
  EMatrixAnswer a;
  const auto fail = [&a](int code, std::string w, bool absent) {
    a.rc = code;
    a.why = std::move(w);
    a.absent = absent;
    return a;
  };
  if (name != "b" && name != "V") return fail(111, "matrix e(" + name + ") not found", true);
  // No e() at all: Stata's r(301) "last estimates not found" (w2g_ebmat.log:2229
  // e15c_fresh_matrix_b, :2238 e15c_fresh_matlist_eb).
  if (!eLive()) return fail(301, "last estimates not found", true);
  const std::string* eCmdP = coefOverride == nullptr ? eStrings.find("cmd") : nullptr;
  const std::string eCmd = eCmdP != nullptr ? *eCmdP : std::string();
  const std::string blockEst = source != nullptr && source->is_object()
                                   ? source->value("estimator", std::string())
                                   : std::string();
  const EStripeRow row = eStripeRowFor(eCmd, blockEst);
  const std::string cmdName = !eCmd.empty() ? eCmd : blockEst;
  const auto refuse = [&](const std::string& reason) {
    return fail(198,
                "e(" + name + ") is not available in this version of PIET after `" +
                    (cmdName.empty() ? std::string("this estimation") : cmdName) + "`: " + reason,
                false);
  };
  const auto rowReason = [&row, &cmdName]() -> std::string {
    if (row.reason != nullptr && *row.reason != '\0') return row.reason;
    return "Stata's stripe for " + (cmdName.empty() ? std::string("this estimation") : cmdName) +
           " is not recorded here";
  };
  if (source == nullptr) {
    // A live e() without this matrix (pca, estpost, rddensity, an `ereturn post`
    // without b: w2g_ebmat.log:2486 e15d_list_eb2 `matrix e(b) not found`,
    // r(111)). A named row Stata DOES post an e(b) for (rdrobust) refuses.
    if (row.key != nullptr && row.rule == EStripe::kRefuse) return refuse(rowReason());
    return fail(111, "matrix e(" + name + ") not found", true);
  }
  // `erepost b = M` (tables/table_erepost.cpp runErepost marks the block): on a
  // posting with no e(b) (estpost) the rows ARE M -- its names, its order, no
  // e(V) (erepost.ado `eret post `b''; w2v_estpost_ematrix.log
  // ep14e_colnames_eb `c1 c2`, ep14e2_colnames_eb `x1 x2`). On a fit the rows
  // keep the fit's names and only the values change, so the stripe row and
  // both guards still apply (G1 reads the row's `omitted` mark there).
  const auto repIt = source->find("reposted_b");
  const bool reposted = repIt != source->end() && repIt->is_boolean() && repIt->get<bool>();
  if (row.rule == EStripe::kAbsent && reposted) {
    if (name != "b") return fail(111, "matrix e(" + name + ") not found", true);
    EvalMatrix pm;
    pm.rownames = {"y1"};
    pm.data.emplace_back();
    if (source->contains("coeftable") && source->at("coeftable").is_array()) {
      for (const auto& r : source->at("coeftable")) {
        if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
        pm.colnames.push_back(r.at("name").get<std::string>());
        double v = std::numeric_limits<double>::quiet_NaN();
        if (r.contains("b") && !r.at("b").is_null())
          if (auto f = jsonToFloat(r.at("b"))) v = *f;
        pm.data[0].push_back(v);
      }
    }
    a.matrix = std::move(pm);
    return a;
  }
  if (row.rule == EStripe::kRefuse || row.rule == EStripe::kAbsent) return refuse(rowReason());
  // The coefficient table's terms, in the engine's spelling.
  struct Term {
    std::string name;
    bool zero = false;  // b == 0 exactly and se 0 or missing, or marked omitted
  };
  std::vector<Term> terms;
  if (source->is_object() && source->contains("coeftable") && source->at("coeftable").is_array()) {
    for (const auto& r : source->at("coeftable")) {
      if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
      Term t;
      t.name = r.at("name").get<std::string>();
      std::optional<double> b, se;
      if (r.contains("b") && !r.at("b").is_null()) b = jsonToFloat(r.at("b"));
      if (r.contains("se") && !r.at("se").is_null()) se = jsonToFloat(r.at("se"));
      // A reposted row's se is null and its b is M's value, so the fit's
      // omission survives only as the row's `omitted` mark (runErepost sets it
      // before it nulls se); a reposted 0 alone is not an omission.
      const auto omIt = r.find("omitted");
      const bool marked = omIt != r.end() && omIt->is_boolean() && omIt->get<bool>();
      t.zero = marked ||
               (!reposted && b.has_value() && *b == 0.0 && (!se.has_value() || *se == 0.0));
      terms.push_back(std::move(t));
    }
  }
  // (G1) an omitted (collinear) column: the engine keeps it at b 0 / se 0 under
  // its bare name, where Stata marks the omitted column `o.` (w2u_binary_ml_edges.log:215
  // bm_l_dz0_names `x1 o.dz0 _cons`). Stata may omit a different member of the
  // collinear set than the engine (w2v_estripe_census.log:274 esc_regress_omit
  // `o.x1 x2 x3c _cons` where the engine dropped x3c), so the refusal names
  // only the engine's column. Owner: T-E2.
  // eb-order T-E2: a stripe the fit recorded (ebStripeOf) names the base
  // columns and the `o.` spelling of a screen-omitted column itself.
  std::vector<std::string> stripeNames;  // the stripe's Stata names, when it has one
  std::vector<std::string> stripeSrcOmitted;  // canonical columns it spells `o.`
  if (const auto stIt = source->find("eb_stripe"); stIt != source->end() && stIt->is_array()) {
    for (const auto& e : *stIt) {
      if (!e.is_object() || !e.contains("name") || !e.at("name").is_string()) continue;
      const std::string nm = e.at("name").get<std::string>();
      stripeNames.push_back(nm);
      if (e.contains("src") && e.at("src").is_string() && hasBaseOrOmittedMarker(nm))
        stripeSrcOmitted.push_back(e.at("src").get<std::string>());
    }
  }
  for (const Term& t : terms)
    if (t.zero && !hasBaseOrOmittedMarker(t.name) &&
        std::find(stripeSrcOmitted.begin(), stripeSrcOmitted.end(), t.name) ==
            stripeSrcOmitted.end())
      return refuse("the fit omitted the collinear column `" + t.name +
                    "` (Stata marks an omitted column `o.`; the engine and Stata may omit "
                    "different columns of a collinear set)");
  // Factor-variable terms whose base levels the engine does not send (`2.g 3.g`
  // where Stata has `1b.g 2.g 3.g`, w2g_ebmat.log:452 e2_colnames_eb). Owner
  // T-E2; `matrix b = e(b)` and `matrix list e(b)` keep bin/main's rc-0 answer
  // (e2_list_eb, w2g_ebmat.log:495), the name and operand readers refuse.
  if (row.cons != ConsRule::kAnyCons && reader != EReader::kCopy) {
    const bool anyFactor = std::any_of(terms.begin(), terms.end(),
                                       [](const Term& t) { return isFactorColumn(t.name); });
    const bool anyBase = std::any_of(terms.begin(), terms.end(), [](const Term& t) {
      return hasBaseOrOmittedMarker(t.name);
    }) || std::any_of(stripeNames.begin(), stripeNames.end(), [](const std::string& n) {
      return hasBaseOrOmittedMarker(n);
    });
    // A recorded stripe placed every column under Stata's name (a `c.x#c.z`
    // cell has no base level to mark).
    if (anyFactor && !anyBase && stripeNames.empty())
      return refuse("the fit has factor-variable terms, and the engine does not send their "
                    "base-level columns (`1b.` ...) that Stata's e(" + name + ") carries");
  }
  const auto isCons = [](const std::string& n) {
    const size_t colon = n.find(':');
    const std::string bare = colon == std::string::npos ? n : n.substr(colon + 1);
    return bare == "_cons" || bare == "(intercept)";
  };
  const bool hasCons =
      std::any_of(terms.begin(), terms.end(), [&isCons](const Term& t) { return isCons(t.name); });
  if (row.cons == ConsRule::kNoCons && hasCons)
    return refuse("the fit's coefficient table has a `_cons` that Stata's e(" + name +
                  ") does not");
  if (row.cons == ConsRule::kNeedCons) {
    // ivreghdfe under absorb() partials the constant out (w2b_estim.log:1168
    // `Partialled-out: _cons`); the block names the absorbed effects.
    const auto absorbedIt = source->find("absorbed");
    const bool consPartialled = std::string(row.key) == "ivreghdfe" &&
                                absorbedIt != source->end() && absorbedIt->is_object() &&
                                !absorbedIt->empty();
    const auto ncIt = source->find("noconstant");
    const bool noConstant = ncIt != source->end() && ncIt->is_boolean() && ncIt->get<bool>();
    if (consPartialled) {
      if (hasCons)
        return refuse("the fit's coefficient table has a `_cons` that ivreghdfe partials out");
    } else if (!hasCons && !noConstant) {
      return refuse("the fit's coefficient table has no `_cons`, and this build cannot tell a "
                    "noconstant fit from a constant the engine dropped, so the columns of "
                    "Stata's e(" + name + ") are unknown");
    }
  }
  std::string w;
  std::optional<EvalMatrix> m = canonicalEMatrix(*source, name[0], &w);
  if (!m.has_value() && w == "no-e_V") {
    if (reader == EReader::kOperand) {
      // Only the coefficient table's standard errors exist: V's DIAGONAL. The
      // operand caller answers el(e(V),i,i) = se_i^2 and the dimensions, and
      // refuses an off-diagonal cell by name (FU-118), never the silent 0 of a
      // diagonal synthesis (FU-98).
      std::optional<EvalMatrix> bm = canonicalEMatrix(*source, 'b', &w);
      // (a reg3/sureg system has no top-level table to read the se's from)
      if (bm.has_value() && source->contains("coeftable") && source->at("coeftable").is_array()) {
        EvalMatrix d;
        d.colnames = bm->colnames;
        d.rownames = bm->colnames;
        const size_t k = d.colnames.size();
        d.data.assign(k, std::vector<double>(k, 0.0));
        // canonicalEMatrix's column order, read back by name from the table.
        for (size_t i = 0; i < k; ++i) {
          d.data[i][i] = std::numeric_limits<double>::quiet_NaN();
          for (const auto& r : source->at("coeftable")) {
            if (!r.is_object() || !r.contains("name") || !r.at("name").is_string()) continue;
            std::string n = r.at("name").get<std::string>();
            const size_t colon = n.find(':');
            std::string bare = colon == std::string::npos ? n : n.substr(colon + 1);
            if (bare == "(intercept)") bare = "_cons";
            const std::string full =
                colon == std::string::npos ? bare : n.substr(0, colon + 1) + bare;
            if (full != d.colnames[i]) continue;
            if (r.contains("se") && !r.at("se").is_null())
              if (auto se = jsonToFloat(r.at("se"))) d.data[i][i] = *se * *se;
            break;
          }
        }
        m = std::move(d);
        a.diagonalOnly = true;
      }
    } else {
      // SPEC Q2 interim, an open completion gap: the engine sent no covariance
      // for this fit, and a diagonal of se^2 would answer every off-diagonal
      // with a silent 0. A named refusal, never a diagonal.
      return refuse("the engine did not send the fit's covariance matrix");
    }
  }
  if (!m.has_value()) return fail(111, w, false);  // defensive: G1 runs first
  // The fit's own stripe (base columns, `o.` names): e(b) and e(V) alike. A
  // diagonal-only e(V) keeps zeros off the diagonal there too.
  if (!applyEbStripe(*source, name[0], *m) && !stripeNames.empty() &&
      row.cons != ConsRule::kAnyCons && reader != EReader::kCopy &&
      std::any_of(terms.begin(), terms.end(), [](const Term& t) { return isFactorColumn(t.name); }))
    return refuse("the fit's recorded e(" + name + ") stripe no longer matches its coefficient "
                  "table, so the base-level columns Stata's e(" + name + ") carries are unknown");
  if (row.rule == EStripe::kDepvarEq || row.rule == EStripe::kDepvarEqUnderscore) {
    std::string eq = source->value("depvar", std::string());
    if (eq.empty() && coefOverride == nullptr) {
      if (const std::string* dv = eStrings.find("depvar")) eq = *dv;
    }
    if (eq.empty()) {
      a.diagonalOnly = false;
      return refuse("the fit did not record its dependent variable");
    }
    if (row.rule == EStripe::kDepvarEqUnderscore) std::replace(eq.begin(), eq.end(), '.', '_');
    const auto stripeOf = [&eq](std::vector<std::string>& names) {
      for (auto& n : names)
        if (n.find(':') == std::string::npos) n = eq + ":" + n;
    };
    stripeOf(m->colnames);
    if (name == "V") stripeOf(m->rownames);
  }
  a.matrix = std::move(m);
  return a;
}

}

namespace piet::eval::macro_detail {


// ---------------------------------------------------------------------------
// matrix_op.substitute_matrix_refs + get_matrix_element



// Throws PyExc on out-of-range (ValueError) — caller treats like Python.
double readMatrixElement(const EvalMatrix* mat, long long i, long long j) {
  if (mat == nullptr) throw PyExc("KeyError: matrix not defined");
  long long nrows = static_cast<long long>(mat->data.size());
  long long ncols = nrows > 0 ? static_cast<long long>(mat->data[0].size()) : 0;
  if (!(1 <= i && i <= nrows)) throw PyExc("ValueError: row index out of range");
  if (!(1 <= j && j <= ncols)) throw PyExc("ValueError: column index out of range");
  return mat->data[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)];
}


double getMatrixElement(const EvalSession& session, const std::string& name, long long i,
                         long long j) {
  return readMatrixElement(session.findMatrix(name), i, j);
}


// FU-118: el(M,i,j) is MISSING for an index outside M ([FN] Matrix functions,
// el(): "missing if i or j are out of range"), unlike the M[i,j] subscript,
// which refuses. A 0x0 M (`matrix b = e(b)` after estpost) reads `.` at any
// index (w2v_estpost_ematrix.log ep13_b_11 +1.0000000000000X+3ff). An
// undefined M still throws, so the caller leaves the text and
// unresolvedMatrixOperandError names it. (The same page says el() of a matrix
// that does not exist is missing too, but StataBE refuses it: w2f_residuals.log
// :1261-1267 b560_unknown, `NoSuchMatrix not found`, rc 111.)
double readElFunction(const EvalMatrix* mat, long long i, long long j) {
  if (mat == nullptr) throw PyExc("KeyError: matrix not defined");
  const long long nrows = static_cast<long long>(mat->data.size());
  const long long ncols = nrows > 0 ? static_cast<long long>(mat->data[0].size()) : 0;
  if (!(1 <= i && i <= nrows) || !(1 <= j && j <= ncols))
    return std::numeric_limits<double>::quiet_NaN();
  return mat->data[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)];
}


// FU-118 / FU-178 / FU-190: e(b) and e(V) as an OPERAND of el(), colsof() and
// rowsof() in an expression (display, `local =`, `scalar =`). eb-order: the
// operand is EvalSession::resolveEMatrixFor's (reader kOperand), the same one
// source `matrix X = e(b)`, `matrix list` and `: colnames e(b)` read -- a posted
// matrix, else the live fit's block in Stata's order under the closed stripe
// table -- so the two can never disagree. Its two operand-only answers: e(V)
// known only from the standard errors (`diagonalOnly`: el(e(V),i,i) is se_i^2,
// an off-diagonal cell is refused by name, never the silent 0 of FU-98), and an
// absent operand, which the caller reads as Stata's r(109).
// mat() is the matrix, or nullptr with `why` set: rc "198" (a refusal) or ""
// (absent -- the caller applies Stata's absent-operand rc).
struct EOperand {
  std::optional<EvalMatrix> hold;
  bool diagonalOnly = false;
  const EvalMatrix* mat() const { return hold.has_value() ? &*hold : nullptr; }
  std::string rc;   // "198" refused; "" absent (when mat() is null)
  std::string why;
};


EOperand resolveEOperand(const EvalSession& session, const std::string& name) {
  EOperand out;
  EvalSession::EMatrixAnswer a = session.resolveEMatrixFor(name, EvalSession::EReader::kOperand);
  if (a.matrix.has_value()) {
    out.hold = std::move(a.matrix);
    out.diagonalOnly = a.diagonalOnly;
    return out;
  }
  out.why = std::move(a.why);
  if (!a.absent) out.rc = std::to_string(a.rc);
  return out;
}


// FU-118: the named error for a matrix operand substituteMatrixRefs left in
// `expr` (it answers every operand it can, so one still present cannot be
// read). Before this, the text reached the evaluator as a bare call and came
// back as "NameError: name 'el' is not defined" -- rc 1 on display, r(133)
// "unknown function el()" on `local =`, and the expression TEXT stored at rc 0
// by `scalar =` (FU-348). Returns "r(NNN): message" for the leftmost operand,
// or "" when none is left. The codes:
//   * an e()/r() operand that is absent -- r(109) "type mismatch" inside el(),
//     colsof(), rowsof(): Stata then reads e(V) as an (empty) e() macro, a
//     string where a matrix is required. Pinned: w2l_vce_shape.log:280-283
//     (`se11=type mismatch`, display_rc=109, after a failed fit under
//     `ereturn clear`), and the r() twin's w2c_postest.log:377-384;
//   * a subscript `e(NAME)[i,j]` / `r(NAME)[i,j]`: e() keeps the r(111) of
//     display's own whole-token arm (language/scalar_expression.cpp resolveToken 3b,
//     posted matrices only, W2D-T03), r() keeps absentRMatrixRc's r(198)
//     (w2c_postest.log:363-370); an index outside a present matrix is r(503);
//   * a refusal from resolveEOperand, and an off-diagonal cell of an e(V) that
//     is known only on its diagonal: r(198), named;
//   * el()/colsof()/rowsof() of a bare NAME that is not a matrix: r(111)
//     "NAME not found", StataBE's text and code for el() (w2f_residuals.log
//     :1261-1267 b560_unknown, `NoSuchMatrix not found` / rc 111); colsof()/
//     rowsof() share it (their own rc is unpinned). A NAME that is a scalar is
//     r(109) "type mismatch" for all three (StataBE 19.5, 2026-11-03): scalars
//     and matrices share one namespace, so it is the wrong kind, not absent.
// el() with an index that is not an integer literal (`el(M,1,`z'+1)`,
// `el(vecdiag(M),1,3)`) is not claimed: it keeps the unknown-function r(133)
// classifyFailedEqExpr gives it (w2c_functions.log:110 vecdiag_el_local 133).
std::string unresolvedMatrixOperandError(const std::string& expr,
                                                const EvalSession& session) {
  static const std::regex reClsEl(
      R"(\bel\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))",
      std::regex::icase);
  static const std::regex reClsDim(
      R"(\b(colsof|rowsof)\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\))", std::regex::icase);
  static const std::regex reClsElem(
      R"(\b([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reEl(
      R"(\bel\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))", std::regex::icase);
  static const std::regex reDim(R"(\b(colsof|rowsof)\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))",
                                std::regex::icase);
  const std::string lower = toLower(expr);
  if (lower.find("el") == std::string::npos && lower.find("sof") == std::string::npos &&
      expr.find('[') == std::string::npos)
    return std::string();
  struct Hit {
    size_t pos = std::string::npos;
    std::string err;
  };
  Hit best;
  const auto consider = [&best, &expr](const std::regex& re,
                                       const std::function<std::string(const std::smatch&)>& fn) {
    std::smatch m;
    if (std::regex_search(expr, m, re)) {
      const size_t pos = static_cast<size_t>(m.position(0));
      if (pos < best.pos) {
        std::string e = fn(m);
        if (!e.empty()) best = Hit{pos, std::move(e)};
      }
    }
  };
  const auto classAbsent = [&session](char cls, const std::string& name, bool subscript) {
    if (cls == 'r') {
      if (auto why = unpostedRRefusal(session, name)) return "r(111): " + *why;
      return subscript ? "r(198): invalid syntax: r(" + name + ") is not a matrix"
                       : std::string("r(109): type mismatch");
    }
    if (subscript)
      return session.lastModelResult.has_value()
                 ? "r(111): matrix e(" + name + ") has not been posted by the last estimation "
                   "in this version of PIET; read single coefficients with _b[name] / _se[name]"
                 : "r(111): matrix e(" + name + ") not found";
    return std::string("r(109): type mismatch");
  };
  consider(reClsEl, [&](const std::smatch& m) -> std::string {
    const char cls = static_cast<char>(std::tolower(static_cast<unsigned char>(m.str(1)[0])));
    const std::string name = m.str(2);
    if (cls == 'r') {
      if (session.findClassMatrix('r', name) != nullptr) return std::string();
      return classAbsent('r', name, false);
    }
    const EOperand op = resolveEOperand(session, name);
    if (op.mat() == nullptr)
      return op.rc.empty() ? classAbsent('e', name, false) : "r(" + op.rc + "): " + op.why;
    if (op.diagonalOnly && m.str(3) != m.str(4))
      return "r(198): e(V)[" + m.str(3) + "," + m.str(4) +
             "] is not available in this version of PIET after `" +
             *session.eStrings.find("cmd") +
             "`: the engine sent the fit's standard errors (the diagonal of e(V)), not its "
             "covariances";
    return std::string();
  });
  consider(reClsDim, [&](const std::smatch& m) -> std::string {
    const char cls = static_cast<char>(std::tolower(static_cast<unsigned char>(m.str(2)[0])));
    const std::string name = m.str(3);
    if (cls == 'r')
      return session.findClassMatrix('r', name) != nullptr ? std::string()
                                                           : classAbsent('r', name, false);
    const EOperand op = resolveEOperand(session, name);
    if (op.mat() == nullptr)
      return op.rc.empty() ? classAbsent('e', name, false) : "r(" + op.rc + "): " + op.why;
    return std::string();
  });
  consider(reClsElem, [&](const std::smatch& m) -> std::string {
    const char cls = m.str(1)[0];
    const std::string name = m.str(2);
    const EvalMatrix* mat = session.findClassMatrix(cls, name);
    if (mat == nullptr) return classAbsent(cls, name, true);
    return "r(503): conformability error";
  });
  consider(reEl, [&](const std::smatch& m) -> std::string {
    if (session.findMatrix(m.str(1)) != nullptr) return std::string();
    if (session.scalars.find(m.str(1)) != nullptr) return "r(109): type mismatch";
    return "r(111): " + m.str(1) + " not found";
  });
  consider(reDim, [&](const std::smatch& m) -> std::string {
    if (session.findMatrix(m.str(2)) != nullptr) return std::string();
    if (session.scalars.find(m.str(2)) != nullptr) return "r(109): type mismatch";
    return "r(111): " + m.str(2) + " not found";
  });
  return best.err;
}

}

namespace piet::eval {
using namespace macro_detail;


// A matrix cell can hold Stata's missing, and pythonFloatRepr spells that "nan",
// which is a NameError once it is substituted back into the expression. Emit the
// missing TOKEN instead (`.`, or `.a` for a `.a` cell, matrix/matrix_commands.cpp stores
// stataMissingWithKind) -- replaceStataMissingLiterals runs after this pass and
// turns it back into the exact double. A decimal spelling of a band value would
// be read by the next layer as a number. Reachable from B3(c) onward because
// `J(3,2,.)` is the corpus's own spelling (24 uses) and paper 049 reads those
// cells straight back. r() values go out through here too.
std::string matrixElementText(double v) {
  if (std::isnan(v) || std::fabs(v) >= kStataMissingSentinel) return stataMissingText(v);
  return pythonFloatRepr(v);
}


std::string substituteMatrixRefs(const std::string& expr, const EvalSession& session) {
  static const std::regex reElem(R"((?:\b|(?=[\x80-\xff]))([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reColsof(R"(\bcolsof\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))", std::regex::icase);
  static const std::regex reRowsof(R"(\browsof\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))", std::regex::icase);
  // AER scope 2 / B3(c): colnumb(M,"name") -> the 1-based column index, or
  // missing when the name is not a column. Paper 020 drives an entire loop off
  // it (`local idx_row = colnumb(RHO_D`dd'_res, "rho_Dy`cc'_Dr`cc'")`). It
  // joins the family here rather than in the function namespace because that is
  // where every other matrix reference in an expression is resolved -- element
  // access, colsof and rowsof are all text substitutions against the session
  // store, and a fourth spelling of the same idea would be a second mechanism.
  // Stata takes either the bare column name or the equation-qualified one
  // ("a" and "eq1:a" both answer 1), so both are compared.
  static const std::regex reColnumb(
      // Custom raw-string delimiter: the pattern contains `)"`, which would close
      // a plain R"( ... )" early.
      R"RX(\bcolnumb\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*"([^"]*)"\s*\))RX", std::regex::icase);
  // e(<name>)[i,j] / r(<name>)[i,j], and the same wrapper inside colsof/rowsof.
  // These run FIRST because the three bare-name patterns above cannot express
  // them: `\b([A-Za-z_]\w*)\s*\[` stops at the ')' in `e(Ev)[1,1]`, so before
  // AER bundle H there was no syntax at all for reading an element out of a
  // posted class matrix -- pca could publish e(Ev) and nothing could read it.
  static const std::regex reClsElem(
      R"(\b([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\[\s*(\d+)\s*,\s*(\d+)\s*\])");
  static const std::regex reClsColsof(
      R"(\bcolsof\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\))", std::regex::icase);
  static const std::regex reClsRowsof(
      R"(\browsof\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*\))", std::regex::icase);
  // el(NAME,i,j) / el(e(NAME),i,j) / el(r(NAME),i,j) -- Stata's function form
  // of NAME[i,j] (papers 058 and 044). Joins the same family for the reason
  // colsof/rowsof/colnumb do: every matrix reference in an expression is a
  // text substitution against the session store. The class-wrapped form runs
  // first for the same reason reClsElem does: `el(e(Ev),1,2)`'s inner
  // identifier is "e" up to the "(", so the bare pattern below never actually
  // matches it (the literal "," it requires next is a "(" instead), but
  // running the specific form first keeps that guarantee explicit rather than
  // incidental. Both corpus call sites (058's `` `z' ``, 044's r()-matrix
  // index) have already gone through macro expansion by the time evalArith
  // reaches this pass, so both indices are plain integer literals -- only
  // \d+ is accepted here, exactly like reElem. An arithmetic index such as
  // `el(M,1,`z'+1)` is a named omission, not a silent wrong value: it fails
  // to match, is left unsubstituted, and the parser refuses it the same way
  // an out-of-range NAME[i,j] does below.
  static const std::regex reClsEl(
      R"(\bel\s*\(\s*([er])\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))",
      std::regex::icase);
  static const std::regex reEl(
      R"(\bel\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))", std::regex::icase);

  // csdid-ematrix-post: an el() index written as integer arithmetic once the
  // macros are expanded (`el(e(V),`__w',`__w'+1)` -> `el(e(V),13,13+1)`,
  // w2ag_csdid_drdid.do:75/:112, StataBE rc 0 at log ag_defx_vw / ag_cl_vw) is
  // folded to its literal so the passes below read it. Only digits joined by
  // + - * (Stata's precedence: * first); anything else -- `el(vecdiag(M),1,3)`,
  // a name, a function -- is left as it was.
  static const std::regex reElArith(
      R"(\bel\s*\(\s*((?:[er]\(\s*[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*\))|[A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*([0-9][0-9 +*-]*)\s*,\s*([0-9][0-9 +*-]*)\s*\))",
      std::regex::icase);
  const auto foldIntArith = [](const std::string& s, long long& out) -> bool {
    // a sum of products of non-negative integer literals
    std::string t;
    for (char c : s)
      if (c != ' ') t += c;
    if (t.empty()) return false;
    long long total = 0, prod = 1;
    int sign = 1;
    size_t i = 0;
    bool needNum = true;
    while (i < t.size()) {
      if (needNum) {
        if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
        long long v = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
          v = v * 10 + (t[i] - '0');
          if (v > 1000000000LL) return false;
          ++i;
        }
        prod *= v;
        needNum = false;
      } else {
        const char op = t[i++];
        if (op == '*') {
          needNum = true;
        } else if (op == '+' || op == '-') {
          total += sign * prod;
          prod = 1;
          sign = op == '+' ? 1 : -1;
          needNum = true;
        } else {
          return false;
        }
      }
    }
    if (needNum) return false;
    out = total + sign * prod;
    return true;
  };
  const std::string folded =
      regexSubFn(reElArith, expr, [&foldIntArith](const std::smatch& m) -> std::string {
        long long i = 0, j = 0;
        if (!foldIntArith(m.str(2), i) || !foldIntArith(m.str(3), j) || i < 0 || j < 0)
          return m.str(0);
        return "el(" + m.str(1) + "," + std::to_string(i) + "," + std::to_string(j) + ")";
      });

  // eb-order T-P1: an e() matrix is EvalSession::resolveEMatrix's -- a posted
  // one, else e(b)/e(V) in Stata's canonical order from the live fit's own
  // block (w2g_ebmat.log:260 el(e(b),1,1) = _b[x1], :305 colsof(e(b)) = 3) --
  // and an r() matrix the posted store's. Unresolved stays text for the
  // parser to refuse, as before.
  std::string pre = regexSubFn(reClsElem, folded, [&session](const std::smatch& m) -> std::string {
    try {
      const long long i = std::stoll(m.str(3)), j = std::stoll(m.str(4));
      const EvalMatrix* mat = nullptr;
      EOperand op;
      if (m.str(1)[0] == 'e') {
        op = resolveEOperand(session, m.str(2));
        if (op.diagonalOnly && i != j) return m.str(0);  // a covariance the engine did not send
        mat = op.mat();
      } else {
        mat = session.findClassMatrix(m.str(1)[0], m.str(2));
      }
      double val = readMatrixElement(mat, i, j);
      return valueRefText(matrixElementText(val));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged, as below
    } catch (const std::exception&) {
      return m.str(0);
    }
  });
  // FU-118: an e() operand of el()/colsof()/rowsof() is resolveEOperand's (a
  // posted matrix, else e(b)/e(V) built from the live fit under Stata's rules);
  // an r() operand is the posted store's. An operand left unsubstituted is
  // named by unresolvedMatrixOperandError, never left for the evaluator's
  // NameError. The subscript form above stays posted-only (W2D-T03).
  pre = regexSubFn(reClsEl, pre, [&session](const std::smatch& m) -> std::string {
    try {
      const long long i = std::stoll(m.str(3)), j = std::stoll(m.str(4));
      if (std::tolower(static_cast<unsigned char>(m.str(1)[0])) == 'e') {
        const EOperand op = resolveEOperand(session, m.str(2));
        if (op.mat() == nullptr || (op.diagonalOnly && i != j)) return m.str(0);
        return valueRefText(matrixElementText(readElFunction(op.mat(), i, j)));  // FU-842
      }
      const EvalMatrix* mat = session.findClassMatrix(m.str(1)[0], m.str(2));
      return valueRefText(matrixElementText(readElFunction(mat, i, j)));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // undefined matrix — leave unchanged, as below
    } catch (const std::exception&) {
      return m.str(0);
    }
  });
  const auto classDims = [&session](const std::smatch& m, bool cols) -> std::string {
    EOperand op;
    const EvalMatrix* mat = nullptr;
    if (std::tolower(static_cast<unsigned char>(m.str(1)[0])) == 'e') {
      op = resolveEOperand(session, m.str(2));
      mat = op.mat();
    } else {
      mat = session.findClassMatrix(m.str(1)[0], m.str(2));
    }
    if (mat == nullptr) return m.str(0);
    if (!cols) return pythonFloatRepr(static_cast<double>(mat->data.size()));
    long long ncols = mat->data.empty() ? 0 : static_cast<long long>(mat->data[0].size());
    return pythonFloatRepr(static_cast<double>(ncols));
  };
  pre = regexSubFn(reClsColsof, pre,
                   [&classDims](const std::smatch& m) { return classDims(m, true); });
  pre = regexSubFn(reClsRowsof, pre,
                   [&classDims](const std::smatch& m) { return classDims(m, false); });

  pre = regexSubFn(reColnumb, pre, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = nullptr;
    for (const auto& p : session.matrices)
      if (p.first == m.str(1)) { mat = &p.second; break; }
    if (mat == nullptr) return m.str(0);  // unknown matrix: leave for the parser to reject
    const std::string want = m.str(2);
    // colnumb(M, "") is 1 in StataBE 19.5 (w2x_fn_string_math fx_colnumb_empty).
    if (want.empty() && !mat->data.empty() && !mat->data[0].empty()) return pythonFloatRepr(1.0);
    for (size_t i = 0; i < mat->colnames.size(); ++i) {
      const std::string& c = mat->colnames[i];
      if (c == want) return pythonFloatRepr(static_cast<double>(i + 1));
      const size_t colon = c.find(':');
      if (colon != std::string::npos && c.substr(colon + 1) == want)
        return pythonFloatRepr(static_cast<double>(i + 1));
    }
    return ".";  // a name that is not a column is MISSING, not an error
  });
  // rownumb(M,"name"): colnumb's row twin, on the row names (w2j_e3_qreg2.do's
  // `rownumb(_e3_T, "df")` on a copy of margins' r(table): StataBE 7, M01 log:2915).
  static const std::regex reRownumb(
      R"RX(\brownumb\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*"([^"]*)"\s*\))RX", std::regex::icase);
  pre = regexSubFn(reRownumb, pre, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = nullptr;
    for (const auto& p : session.matrices)
      if (p.first == m.str(1)) { mat = &p.second; break; }
    if (mat == nullptr) return m.str(0);  // unknown matrix: leave for the parser to reject
    const std::string want = m.str(2);
    if (want.empty() && !mat->data.empty()) return pythonFloatRepr(1.0);
    for (size_t i = 0; i < mat->rownames.size(); ++i) {
      const std::string& c = mat->rownames[i];
      if (c == want) return pythonFloatRepr(static_cast<double>(i + 1));
      const size_t colon = c.find(':');
      if (colon != std::string::npos && c.substr(colon + 1) == want)
        return pythonFloatRepr(static_cast<double>(i + 1));
    }
    return ".";
  });

  std::string out = regexSubFn(reElem, pre, [&session](const std::smatch& m) -> std::string {
    try {
      double val = getMatrixElement(session, m.str(1), std::stoll(m.str(2)), std::stoll(m.str(3)));
      return valueRefText(matrixElementText(val));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged
    } catch (const std::exception&) {
      return m.str(0);  // stoll overflow etc. — leave unchanged
    }
  });
  out = regexSubFn(reEl, out, [&session](const std::smatch& m) -> std::string {
    try {
      double val = readElFunction(session.findMatrix(m.str(1)), std::stoll(m.str(2)),
                                  std::stoll(m.str(3)));  // FU-118: out of range -> missing
      return valueRefText(matrixElementText(val));  // FU-842
    } catch (const PyExc&) {
      return m.str(0);  // KeyError/ValueError — leave unchanged
    } catch (const std::exception&) {
      return m.str(0);  // stoll overflow etc. — leave unchanged
    }
  });
  // mreldif(X,Y): max_ij |x_ij - y_ij| / (|y_ij| + 1) over two named matrices
  // (w2j_e3_rlasso.do:406 D1_seed_invariant, mreldif of two tempname copies of
  // e(b)/e(Psi): StataBE 0). Unknown names are left for the parser to refuse;
  // matrices of different shape are Stata's r(503), left the same way.
  static const std::regex reMreldif(
      R"(\bmreldif\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*\))", std::regex::icase);
  if (out.find("reldif") != std::string::npos) {
    out = regexSubFn(reMreldif, out, [&session](const std::smatch& m) -> std::string {
      const EvalMatrix* x = session.findMatrix(m.str(1));
      const EvalMatrix* y = session.findMatrix(m.str(2));
      if (x == nullptr || y == nullptr || x->data.size() != y->data.size()) return m.str(0);
      double mx = 0.0;
      for (size_t i = 0; i < x->data.size(); ++i) {
        if (x->data[i].size() != y->data[i].size()) return m.str(0);
        for (size_t j = 0; j < x->data[i].size(); ++j) {
          const double a = x->data[i][j];
          const double b = y->data[i][j];
          if (std::isnan(a) && std::isnan(b)) continue;
          const double d = std::fabs(a - b) / (std::fabs(b) + 1.0);
          if (std::isnan(d)) return std::string(".");
          if (d > mx) mx = d;
        }
      }
      return valueRefText(matrixElementText(mx));
    });
  }
  out = regexSubFn(reColsof, out, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    long long ncols = mat->data.empty() ? 0 : static_cast<long long>(mat->data[0].size());
    return pythonFloatRepr(static_cast<double>(ncols));  // repr(float(ncols))
  });
  out = regexSubFn(reRowsof, out, [&session](const std::smatch& m) -> std::string {
    const EvalMatrix* mat = session.findMatrix(m.str(1));
    if (mat == nullptr) return m.str(0);
    return pythonFloatRepr(static_cast<double>(mat->data.size()));
  });
  return out;
}

}
