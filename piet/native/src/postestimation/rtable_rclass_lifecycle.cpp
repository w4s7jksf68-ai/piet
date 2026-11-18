// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// r(table) build/post/retire and the r-class lifecycle after every command.

#include "piet/handlers/results.h"

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
using namespace rtable_detail;

// ===========================================================================
// r(table) after estimation — THE posting site (w2c_postest family 1)
//
// Stata posts r(table) from every e-class estimation's coefficient table: a
// 9 x k matrix with rows `b se t pvalue ll ul df crit eform` (`z` instead of `t`
// for a normal-referenced fit) and one column per e(b) column, in e(b) order —
// INCLUDING the base level of a factor, which is a column of its own
// (`x1 x2 0b.c 1.c 2.c 3.c _cons`, arm rtable_regress_colnames) holding b 0 (1 in
// an eform table) and missing se/t/p/ll/ul. ~25 papers read it straight after the
// estimation command.
//
// Design (one site, not per-command copies): dispatcher.cpp's dispatch() captures
// the identity of the live e() block on entry (liveEstimationIdentity) and calls
// rTableAfterCommand() on exit. "e() changed" is decided by that identity, never by
// comparing the returned block with session.lastEstBlock: a dispatcher arm may
// transform the returned block after the handler stored e() (`logistic` / `logit,
// or` exponentiate it), and a value comparison then misses a real new estimation and
// serves the PREVIOUS model's table.
//   * LEAF command (no other dispatch() ran inside it) that changed e(): the old
//     table is retired, and a new one is built from the estimation block the command
//     returned — the transformed one, so an odds-ratio table is posted as Stata posts
//     it (eform 1). No usable block = no table (an honest "not found").
//   * CONTAINER (quietly, capture, a do-file, a bootstrap-style prefix) that changed
//     e(): the table survives only if it was posted for the e() now live — so a
//     prefix that refits and replaces e() after its inner leaves ran never leaves a
//     replicate's table behind.
//   * `margins` is r-class and posts its own table in Stata; PIET's margins posts
//     none, so the estimation's table is retired rather than served as margins'.
//     Likewise the replicating prefixes (bootstrap, jackknife, permute, rolling):
//     their last replicate's leaf posted a table, and Stata's is the resampling one.
//
// It posts ONLY what it can reproduce exactly; otherwise nothing, and a later read
// is the honest "r(table) not found" instead of a matrix whose cell is not Stata's:
//   * every design row must map onto a varlist term, in term order;
//   * a factor term's base column comes from the estimation sample's levels: the
//     one sample level with no design row IS the base (the base is always such a
//     level, whatever ib#./ib(first) rule chose it). Zero missing levels is `ibn.`;
//     two or more (a collinear-omitted level, or a level outside the fitted sample)
//     cannot be placed with certainty, so nothing is posted;
//   * interactions (`#`), equation-striped tables and omitted continuous regressors
//     are not reproduced (named limitation: no oracle arm pins their column names);
//   * the confidence level is DECLARED — the command's own level(#), else `set
//     level` — never inferred from the block; the reference distribution is t(e(df_r))
//     when the block carries df_r and the normal otherwise; and every row's p-value
//     and interval must agree with that level and reference. A block that disagrees
//     (tobit prints t intervals without publishing df_r; `set level` does not reach
//     regress's engine interval) posts nothing. The two exceptions are the ones
//     whose Stata definition is fixed and whose block is known to use another
//     reference: nlcom (always normal in Stata) and poisson/ppmlhdfe (normal in
//     Stata; the engine prints t(dof_resid) intervals) — their p/ll/ul are rebuilt
//     from b and se under the normal.
//
// Which commands replace it: in Stata EVERY r-class command replaces all of r(),
// r(table) included, and an e-class fit replaces r() with its own table
// (w2n_followups.log:968, E07). PIET's r-class commands write r() from dozens of
// handlers, most of which MERGE their keys into what the previous command left, so
// the replacement is done generically, at one site (DIAGNOSIS.md
// postest-rtable F, design W2): dispatch() takes an r() mark at entry
// (eval::RStoreMark); when the command returns, if it wrote r() at all (a store
// generation moved, see eval/session.h nextStoreGeneration), replaceStaleRClass
// erases every r() scalar, user macro and matrix last written BEFORE the mark --
// the stale r(table) among them (arms rtable_survives_summarize,
// rtable_replaced_by_test). A container (a program, a prefix) is judged against
// the mark taken after its last inner command, which is where an rclass program's
// `return` set lands. A command that writes nothing leaves r() alone (a failing
// `summarize nosuchvar`: w2n_followups.log:988/:1000). The bookkeeping that says
// which r(table) is the estimation machinery's (EvalSession::rTableGen/Eid/Model)
// and the refusal note (rTableRefusal) live OUTSIDE every r() store, so no
// wholesale wipe of r() can delete them while the table lives on (the 10-11
// review: `label language` deleted a stamp kept in rStrings and the table was
// never retired again).
namespace {

double critFor(double level, bool zRef, double df) {
  const double alpha = (level == 95.0) ? 0.05 : 1.0 - level / 100.0;
  return zRef ? cecon::z_critical(alpha) : cecon::t_critical(alpha, df);
}

// What the live e() estimated: estimator, depvar, N and the coefficient table. An e()
// replaced by a copy that estimates the same thing (`estadd` re-assigns e() with a
// scalar added) keeps its table; any other replacement retires it. Computed only
// when a table is posted or e() was replaced — never on the per-command path.
std::string modelPrint(const SessionState& session) {
  if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object()) return std::string();
  const ojson& b = *session.lastEstBlock;
  std::string s = b.value("estimator", std::string()) + "\x04" + b.value("depvar", std::string()) + "\x04";
  if (b.contains("n_obs")) s += b["n_obs"].dump();
  s += "\x04";
  if (b.contains("coeftable")) s += b["coeftable"].dump();
  return std::to_string(std::hash<std::string>{}(s));
}

// Post `tbl` as r(table) and record it as the estimation machinery's own, for the
// e() that is live now (EvalSession::rTableGen/Eid/Model).
void setRTable(SessionState& session, eval::EvalMatrix tbl) {
  auto& ev = session.eval;
  tbl.gen = eval::nextStoreGeneration();  // posted now, whenever it was built
  ev.rTableGen = tbl.gen;
  ev.rTableEid = liveEstimationIdentity(session);
  ev.rTableModel = modelPrint(session);
  for (auto& kv : ev.rMatrices) {
    if (kv.first == "table") {
      kv.second = std::move(tbl);
      return;
    }
  }
  ev.rMatrices.emplace_back("table", std::move(tbl));
}

void forgetRTableRefusal(eval::EvalSession& ev) {
  ev.rTableRefusal.clear();
  ev.rTableRefusalEid = 0;
  ev.rTableRefusalGen = 0;
}

void retireRTable(SessionState& session) {
  auto& ev = session.eval;
  for (auto it = ev.rMatrices.begin(); it != ev.rMatrices.end();) {
    if (it->first == "table") it = ev.rMatrices.erase(it);
    else ++it;
  }
  ev.rTableGen = 0;
  ev.rTableEid = 0;
  ev.rTableModel.clear();
  forgetRTableRefusal(ev);
}

// Is the r(table) now in r() the one the estimation machinery posted? A table
// posted by anyone else (sktest's) has another gen.
bool ownsRTable(const eval::EvalSession& ev) {
  const eval::EvalMatrix* t = ev.findClassMatrix('r', "table");
  return t != nullptr && ev.rTableGen != 0 && t->gen == ev.rTableGen;
}

// e() was replaced but by a copy of the same model (see modelPrint): re-bind the
// table to the new identity and keep it. False = the table must go.
bool rebindIfSameModel(SessionState& session, std::uintptr_t eIdNow) {
  auto& ev = session.eval;
  if (!ownsRTable(ev)) return false;
  if (ev.rTableEid == eIdNow) return true;
  if (ev.rTableModel.empty() || ev.rTableModel != modelPrint(session)) return false;
  ev.rTableEid = eIdNow;
  return true;
}

// The confidence level a command declared: its own level(#), else `set level`.
// NaN when a level() is present but not a number this code can read (an unexpanded
// macro, say) — the caller then posts nothing rather than guess.
double declaredLevel(const std::string& rawCmd, const SessionState& session) {
  const long long comma = eval::findUnquotedComma(rawCmd);
  if (comma >= 0) {
    std::vector<std::pair<std::string, eval::OptValue>> opts;
    try {
      opts = eval::parseOpts(rawCmd.substr(static_cast<size_t>(comma) + 1));
    } catch (const std::exception&) {
      return rtNan();
    }
    // level() as typed or abbreviated down to `l`: Stata's estimation commands declare
    // `Level(cilevel)` (bank ea81be70 `reghdfe ..., absorb(rep78) l(90)`, 84c174ba
    // `regress ..., l(90)`, 6fbbd6d7 `ivregress 2sls ..., l(90)`, 753d3a4a `ivreg ...,
    // l(90)`, 213ec4a0 `sureg ..., l(90)`, a41ed48a `newey ..., lag(3) l(90)`, e893f695
    // `logistic foreign mpg weight, l(90)`, 1007f386 `suest A B, l(90)`: r(table) at 90).
    for (const auto& [k, v] : opts) {
      const std::string lk = strutil::lower(k);
      if (lk.empty() || std::string("level").compare(0, lk.size(), lk) != 0) continue;
      if (v.isBool) return rtNan();
      const std::string s = strutil::trim(v.strValue);
      char* end = nullptr;
      const double lv = std::strtod(s.c_str(), &end);
      if (s.empty() || end == nullptr || *end != '\0' || !(lv >= 10.0 && lv <= 99.99)) return rtNan();
      return lv;
    }
  }
  return static_cast<double>(session.level);
}

// eval/macros.cpp turns a later read of r(table) into a NAMED r(111) while this
// note lives: Stata posts a table there, this build does not.
void refuseRTable(SessionState& session, const std::string& why) {
  auto& ev = session.eval;
  ev.rTableRefusal = why;
  ev.rTableRefusalEid = liveEstimationIdentity(session);
  ev.rTableRefusalGen = eval::nextStoreGeneration();
}

// True when the refusal note was left for the e() that is live now.
bool refusedFor(const SessionState& session, std::uintptr_t eIdNow) {
  return !session.eval.rTableRefusal.empty() && session.eval.rTableRefusalEid == eIdNow;
}

std::string estimationRefusal(const SessionState& session) {
  const auto& lm = session.lastModel();
  if (lm.has_value())
    for (const auto& term : lm->x)
      if (term.find('#') != std::string::npos)
        return "r(table) is not posted after a model with interaction (#) terms in this "
               "version of PIET: Stata's column names for interaction cells are not reproduced. "
               "Read the coefficients with _b[] or copy e(b) instead.";
  return "r(table) is not posted after this estimation in this version of PIET: its "
         "coefficient table (omitted or equation-striped coefficients, or a confidence level "
         "or reference distribution this build cannot reproduce exactly) is not Stata's. "
         "Read the coefficients with _b[]/_se[] instead.";
}

}  // namespace

namespace rtable_detail {

// First word of a command line, lower-cased.
std::string commandWord(const std::string& rawCmd) {
  size_t i = 0;
  while (i < rawCmd.size() && std::isspace(static_cast<unsigned char>(rawCmd[i]))) ++i;
  size_t j = i;
  while (j < rawCmd.size() && !std::isspace(static_cast<unsigned char>(rawCmd[j])) &&
         rawCmd[j] != ',')
    ++j;
  return strutil::lower(rawCmd.substr(i, j - i));
}

}  // namespace rtable_detail

std::uintptr_t liveEstimationIdentity(const SessionState& session) {
  if (!session.lastEstBlock.has_value()) return 0;
  // An e() block is replaced by assignment (`session.lastEstBlock = block`), and
  // nlohmann's assignment builds the new value before it releases the old one, so
  // the address of the object's storage is a new one for every new estimation. It
  // is read in O(1) — dispatch() calls this for every command, loops included.
  if (const auto* obj = session.lastEstBlock->get_ptr<const ojson::object_t*>())
    return reinterpret_cast<std::uintptr_t>(obj);
  return static_cast<std::uintptr_t>(std::hash<std::string>{}(session.lastEstBlock->dump())) | 1u;
}

namespace {
// var's and csdid's table is the posted e(b) / e(V) (vector_time_series.cpp
// postVarEResults; did_estimation.cpp postCsdidPostingM4):
// normal-referenced, one column per e(b) column with its equation (vs-stata bank
// 47c5c432 `var close volume, nobigf`: r(table) 9 x 10, `close:L.close` ...,
// row z, df missing). Built from the live matrices, so a block without them
// posts nothing.
// `var ..., small` reports t(T - m) (bank fc4ce2a4: row t, df 241); the block
// carries that df as var_small_df_r.
bool buildRTableFromEbV(SessionState& session, double level, double dfR, eval::EvalMatrix& out) {
  const bool zRef = !(dfR > 0);
  const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kNames);
  const auto ev = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kNames);
  if (!eb.matrix.has_value() || !ev.matrix.has_value()) return false;
  const eval::EvalMatrix& B = *eb.matrix;
  const eval::EvalMatrix& V = *ev.matrix;
  const size_t k = B.colnames.size();
  if (k == 0 || B.data.size() != 1 || B.data[0].size() != k || V.data.size() != k) return false;
  const double crit = critFor(level, zRef, dfR);
  if (!(crit > 0)) return false;
  eval::EvalMatrix m;
  m.rownames = {"b", "se", zRef ? "z" : "t", "pvalue", "ll", "ul", "df", "crit", "eform"};
  m.colnames = B.colnames;
  m.data.assign(9, std::vector<double>());
  for (size_t j = 0; j < k; ++j) {
    if (V.data[j].size() != k) return false;
    const double b = B.data[0][j], v = V.data[j][j];
    double se = rtNan(), z = rtNan(), p = rtNan(), ll = rtNan(), ul = rtNan();
    if (v > 0) {
      se = std::sqrt(v);
      z = b / se;
      p = zRef ? std::erfc(std::fabs(z) / std::sqrt(2.0)) : cecon::t_twosided_pvalue(z, dfR);
      ll = b - crit * se;
      ul = b + crit * se;
    }
    const double col[9] = {b, se, z, p, ll, ul, zRef ? rtNan() : dfR, crit, 0.0};
    for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
  }
  out = std::move(m);
  return true;
}
}  // namespace

bool buildRTableFromBlock(SessionState& session, const ojson& block, double level,
                          eval::EvalMatrix& out) {
  // reg3 / sureg: one table over the equations' coefficients, columns `depvar:name`
  // (`_cons` for the constant) in e(b)'s order, on the normal reference (their
  // large-sample default; bank 77bbda65 `sureg (price mpg weight) (trunk length
  // weight)`: r(table) 9 x 6, `price:mpg` ... `trunk:_cons`, row z, df missing, crit
  // 1.959964). The interval is b -/+ crit*se at the declared level.
  if (block.is_object() && block.value("estimator", std::string()) == "reg3" &&
      block.contains("outcomes") && block["outcomes"].is_array() && !block["outcomes"].empty() &&
      !block.contains("coeftable")) {
    if (!(level >= 10.0 && level <= 99.99)) return false;
    const double crit = critFor(level, true, rtNan());
    eval::EvalMatrix m;
    m.rownames = {"b", "se", "z", "pvalue", "ll", "ul", "df", "crit", "eform"};
    m.data.assign(9, std::vector<double>());
    for (const auto& eq : block["outcomes"]) {
      if (!eq.is_object() || !eq.contains("coeftable") || !eq["coeftable"].is_array()) return false;
      const std::string dep = eq.value("depvar", std::string());
      if (dep.empty()) return false;
      for (const auto& row : eq["coeftable"]) {
        if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) return false;
        std::string nm = row["name"].get<std::string>();
        if (nm == "(intercept)") nm = "_cons";
        const double b = rowNum(row, "b"), se = rowNum(row, "se");
        if (!std::isfinite(b) || !(se > 0)) return false;
        const double z = b / se;
        const double col[9] = {b, se, z, std::erfc(std::fabs(z) / std::sqrt(2.0)), b - crit * se,
                               b + crit * se, rtNan(), crit, 0.0};
        m.colnames.push_back(dep + ":" + nm);
        for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
      }
    }
    out = std::move(m);
    return true;
  }
  // csdid prints its posting with `_coef_table` likewise (bank 56c8eb59: r(table)
  // 9 x 24, `g1984:t_1982_1983` ... `wgt:w1986_1988`, row z).
  if (block.is_object() &&
      (block.value("estimator", std::string()) == "var" ||
       block.value("estimator", std::string()) == "csdid") &&
      level >= 10.0 && level <= 99.99)
    return buildRTableFromEbV(session, level,
                              block.contains("var_small_df_r") && block["var_small_df_r"].is_number()
                                  ? block["var_small_df_r"].get<double>()
                                  : rtNan(),
                              out);
  if (!block.is_object()) return false;
  const bool prelaid = block.contains("rtable_cols") && block["rtable_cols"].is_array() &&
                       !block["rtable_cols"].empty();
  if (!prelaid && (!block.contains("coeftable") || !block["coeftable"].is_array() ||
                   block["coeftable"].empty()))
    return false;
  // mean / proportion carry the level their intervals were built at (applied again,
  // with its reasons, where their table is built below).
  if ((block.value("estimator", std::string()) == "mean" ||
       block.value("estimator", std::string()) == "proportion") &&
      block.contains("level") && block["level"].is_number())
    level = block["level"].get<double>();
  if (!(level >= 10.0 && level <= 99.99)) return false;
  // A multi-equation fit whose handler laid r(table) out as e(b) (mlogit:
  // `rtable_cols`, every column `eq:name` in the coefficient metric, `omitted` for
  // the base equation's `o.` columns, `eform` under rrr). Normal reference. Posted
  // only when the columns are e(b)'s and each interval and p-value is the one
  // the declared level and the normal give.
  if (block.contains("rtable_cols") && block["rtable_cols"].is_array() &&
      !block["rtable_cols"].empty()) {
    const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kNames);
    const ojson& rc = block["rtable_cols"];
    if (!eb.matrix.has_value() || eb.matrix->colnames.size() != rc.size()) return false;
    const double crit = critFor(level, true, rtNan());
    if (!(crit > 0)) return false;
    eval::EvalMatrix m;
    m.rownames = {"b", "se", "z", "pvalue", "ll", "ul", "df", "crit", "eform"};
    m.data.assign(9, std::vector<double>());
    for (size_t j = 0; j < rc.size(); ++j) {
      const ojson& c = rc[j];
      if (!c.is_object() || c.value("name", std::string()) != eb.matrix->colnames[j]) return false;
      const bool eform = c.value("eform", false);
      double col[9] = {eform ? 1.0 : 0.0, rtNan(), rtNan(), rtNan(), rtNan(), rtNan(), rtNan(), crit,
                       eform ? 1.0 : 0.0};
      if (!c.value("omitted", false)) {
        const double b = rowNum(c, "b"), se = rowNum(c, "se"), z = rowNum(c, "z"), p = rowNum(c, "p");
        const double ll = rowNum(c, "ci_lo"), ul = rowNum(c, "ci_hi");
        if (!std::isfinite(b) || !(se > 0) || !std::isfinite(z) || !std::isfinite(p) ||
            !std::isfinite(ll) || !std::isfinite(ul))
          return false;
        const double half = (ul - ll) / (2.0 * se);
        if (std::fabs(half - crit) > 1e-6 * crit) return false;
        const double pRef = std::erfc(std::fabs(z) / std::sqrt(2.0));
        if (!(std::fabs(p - pRef) <= 1e-8 + 1e-6 * pRef)) return false;
        col[0] = b;
        col[1] = se;
        col[2] = z;
        col[3] = p;
        col[4] = ll;
        col[5] = ul;
        if (eform) {
          const double eb0 = std::exp(b);
          col[0] = eb0;
          col[1] = eb0 * se;
          col[4] = std::exp(ll);
          col[5] = std::exp(ul);
        }
      }
      m.colnames.push_back(eb.matrix->colnames[j]);
      for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
    }
    // Transformed ancillary parameters Stata adds after e(b)'s columns (heckman's
    // `_diparm1:rho _diparm1:sigma _diparm1:lambda`, bank fdeed49b): b, se and the
    // interval as given, no test statistic (.b), df missing.
    if (block.contains("rtable_diparm") && block["rtable_diparm"].is_array()) {
      for (const auto& c : block["rtable_diparm"]) {
        const double b = rowNum(c, "b"), se = rowNum(c, "se"), ll = rowNum(c, "ci_lo"),
                     ul = rowNum(c, "ci_hi");
        const std::string nm = c.value("name", std::string());
        if (nm.empty() || !std::isfinite(b) || !(se > 0) || !std::isfinite(ll) || !std::isfinite(ul))
          return false;
        m.colnames.push_back(nm);
        const double col[9] = {b, se, eval::stataMissingWithKind(2), eval::stataMissingWithKind(2),
                               ll, ul, rtNan(), crit, 0.0};
        for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
      }
    }
    out = std::move(m);
    return true;
  }
  const ojson& ct = block["coeftable"];
  const std::string estimator = block.value("estimator", std::string());
  // mean / proportion: a table of estimates, not of a design's coefficients. Its
  // columns are e(b)'s, in the block's order; its reference is t(e(df_r)) at the
  // level the fit's intervals were built at (the handler records it in the block:
  // an abbreviated `l(90)` is not read back from the command line); a proportion's
  // interval is the logit one, taken as the block has it. A column whose estimate
  // and standard error are both 0 has, for a proportion, a missing standard error;
  // every other standard error of 0 is kept; t, p and the interval are then missing,
  // and no column is renamed `o.` (vs-stata bank 0b672af1 `proportion foreign,
  // over(rep78)`; e5beada5 `mean v, over(foreign)` with v all 0: se 0).
  const bool isMeanProp = estimator == "mean" || estimator == "proportion";
  if (isMeanProp && block.contains("level") && block["level"].is_number())
    level = block["level"].get<double>();
  if (!(level >= 10.0 && level <= 99.99)) return false;
  // bootstrap's table (estimation/bootstrap_prefix.cpp runBootstrap) is normal-based too,
  // named by its own e(b) stripe: the same shape as nlcom's.
  // leebounds.ado posts its own equation-striped e(b) (`collgrad:lower
  // collgrad:upper`) and prints it with `ereturn display`: a normal-referenced
  // table named by that stripe (vs-stata bank 17e80ef6 `leebounds w2 collgrad`:
  // r(table) 9 x 2, columns collgrad:lower collgrad:upper, row z).
  // suest's stacked table (postestimation/suest_estimation.cpp) likewise: normal-based, its
  // columns e(b)'s own `A_mean:mpg ... B_lnvar:_cons` (bank e9d66afe).
  const bool isNlcom = estimator == "nlcom" || estimator == "bootstrap" || estimator == "leebounds" ||
                       estimator == "suest";
  // runMargins' own table (margins.cpp postMarginsRClassM4): its columns
  // are margins' r(b) names (`_cons`, `1.t`, `x1`), not the model's e(b); the
  // reference is t(df_r) when the block carries one, the normal otherwise.
  const bool isMargins = estimator == "margins_rtable";

  std::vector<RtCol> cols;
  // r(table) is e(b)-shaped: a fit whose e(b) carries the depvar as its equation
  // (logit, probit, poisson: `rep78:mpg rep78:weight rep78:_cons`) names r(table)'s
  // columns the same way (bank 7e0e14c8 `poisson rep78 mpg weight`: XCN r(table)
  // `rep78:mpg`; e70606b9 `probit foreign mpg weight`: `foreign:mpg`).
  std::string eqPrefix;
  // An ancillary parameter row (`/cut1` of ologit/oprobit): no z and no p, an
  // interval from its own se. Stata's r(table) carries it after the coefficients,
  // as e(b) does (bank d8de89e3 `ologit rep78 mpg weight`: columns `rep78:mpg
  // rep78:weight /cut1 ... /cut4`, z and pvalue missing on the cutpoints).
  const auto isAncillary = [](const ojson& row) {
    const std::string nm = row.value("name", std::string());
    return !nm.empty() && nm[0] == '/' && row.contains("t") && row["t"].is_null() &&
           row.contains("p") && row["p"].is_null();
  };
  // A transformed ancillary parameter Stata adds after e(b)'s columns (a diparm:
  // xtpoisson's `_diparm1:alpha`, bank f8d5160a): b, se and the interval as given,
  // no test statistic.
  const auto isDiparm = [](const ojson& row) {
    const std::string nm = row.value("name", std::string());
    return nm.rfind("_diparm", 0) == 0 && row.contains("t") && row["t"].is_null() &&
           row.contains("p") && row["p"].is_null();
  };
  ojson mainCt = ojson::array();
  std::vector<const ojson*> ancillary, diparm;
  for (const auto& row : ct) {
    if (!isNlcom && !isMargins && row.is_object() && isDiparm(row)) diparm.push_back(&row);
    else if (!isNlcom && !isMargins && row.is_object() && isAncillary(row)) ancillary.push_back(&row);
    else mainCt.push_back(row);
  }
  if (isNlcom || isMargins || isMeanProp) {
    for (const auto& row : ct) cols.push_back({row.value("name", std::string()), &row});
  } else {
    const auto& lmOpt = session.lastModel();
    if (!lmOpt.has_value() || mainCt.empty() ||
        !stataColumns(session, *lmOpt, ancillary.empty() && diparm.empty() ? ct : mainCt, cols))
      return false;
    for (const ojson* row : ancillary) cols.push_back({row->value("name", std::string()), row});
    eqPrefix = eval::depvarEquationOf(session.eval);
    if (!eqPrefix.empty()) eqPrefix += ":";
  }

  // Reference distribution: t(e(df_r)) when the fit publishes a residual df
  // (G-1 under clustering), the normal otherwise (logit, ivregress's default, ...:
  // `z` row, df missing). nlcom is ALWAYS normal in Stata, whatever the model
  // (w2c_postest.log prints `z` after `regress`).
  const char* dfKey = isMeanProp ? "df" : "df_r";
  const double dfR = (!isNlcom && block.contains(dfKey) && block[dfKey].is_number())
                         ? block[dfKey].get<double>() : rtNan();
  const bool zRef = isNlcom || !(dfR > 0);
  const double crit = critFor(level, zRef, dfR);
  if (!(crit > 0)) return false;
  // Blocks whose Stata definition is normal-referenced but whose printed p/CI use
  // another reference: rebuilt from b and se (see the header).
  // stcox likewise: normal-referenced in Stata at the declared level, while the
  // engine's printed interval does not follow level() (vs-stata bank 441354d5
  // `stcox age i.drug, breslow nohr level(90)`: r(table) ll .0516731 = b -
  // 1.644854 se).
  const bool rebuild = isNlcom || estimator == "poisson" || estimator == "ppmlhdfe" ||
                       estimator == "fepois" || estimator == "stcox";
  // An eform block (`logistic`, `logit, or`, stcox's hazard ratios) carries exp(b),
  // exp(b)*se and exp() of the interval; the checks run on the coefficient metric.
  const bool blockEform = block.contains("coef_metric") && block["coef_metric"].is_string();
  // An eform equation beside an ancillary parameter (`nbreg ..., irr`): its rows carry
  // the mark, and a column with no row of its own (a factor's base level, laid out from
  // e(b)) is that equation's too -- exp(0) = 1 with eform 1 (bank 3589963928edcb2f
  // `nbreg ... i.year, vce(cluster sid) irr`: the base column's b and eform are 1).
  const bool rowlessEform = block.value("eq_eform", false);

  eval::EvalMatrix m;
  m.rownames = {"b", "se", zRef ? "z" : "t", "pvalue", "ll", "ul", "df", "crit", "eform"};
  m.data.assign(9, std::vector<double>());
  size_t colIdx = 0;
  for (const auto& c : cols) {
    const size_t ci = colIdx++;
    // A row may carry its own `eform` mark: an equation-wise transform, as
    // `zip ..., irr` exponentiates only the count equation (Stata's r(table) row
    // eform is 1 there and 0 on inflate:).
    const bool eform = blockEform || (c.row != nullptr ? c.row->is_object() &&
                                                           c.row->value("eform", false)
                                                       : rowlessEform);
    // An engine-omitted column (b 0 and se 0, kept in the table) is Stata's
    // `o.` column: b 0, se/t/p/ll/ul missing -- the same zero rule
    // annotateEbStripe applies to e(b), so r(table) and e(b) name it alike
    // (StataBE r1.do: an omitted column is `o.z`, se `.`).
    // In an eform table the same column is exp(0) = 1 with se 0 (bank 506b9692
    // `logistic foreign mpg i.rep78`: `2o.rep78` and `5o.rep78`, b 1, eform 1).
    if (isMeanProp && c.row != nullptr && rowNum(*c.row, "se") == 0.0) {
      const double b0 = rowNum(*c.row, "b");
      m.colnames.push_back(c.name);
      // A proportion's interval stays the one its citype() builds, se 0 or not: the
      // logit one is missing at 0 and 1 (logit(p) is), wilson, agresti, exact and
      // jeffreys are numbers (`proportion foreign, over(rep78) citype(wilson)`, a
      // level that is all of its group: [z^2/(n + z^2), 1]). mean's is missing.
      double ll0 = rtNan(), ul0 = rtNan();
      if (estimator == "proportion" && b0 != 0.0) {
        const std::string ct0 = block.value("citype", std::string("logit"));
        if (ct0 != "logit" || (b0 > 0.0 && b0 < 1.0)) {
          ll0 = rowNum(*c.row, "ci_lo");
          ul0 = rowNum(*c.row, "ci_hi");
        }
      }
      const double col[9] = {b0, b0 == 0.0 && estimator == "proportion" ? rtNan() : 0.0, rtNan(),
                             rtNan(), ll0, ul0,
                             zRef ? rtNan() : dfR, crit, 0.0};
      for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
      continue;
    }
    if (c.row != nullptr && rowNum(*c.row, "b") == (eform ? 1.0 : 0.0) &&
        rowNum(*c.row, "se") == 0.0) {
      m.colnames.push_back(eqPrefix + omittedColumnName(c.name));
      const double col[9] = {eform ? 1.0 : 0.0, rtNan(), rtNan(), rtNan(), rtNan(), rtNan(),
                             zRef ? rtNan() : dfR, crit, eform ? 1.0 : 0.0};
      for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
      continue;
    }
    m.colnames.push_back(eqPrefix + c.name);
    double b = 0, se = rtNan(), t = rtNan(), p = rtNan(), ll = rtNan(), ul = rtNan();
    bool colRebuild = rebuild;
    if (c.row != nullptr) {
      b = rowNum(*c.row, "b");
      se = rowNum(*c.row, "se");
      if (eform && !(b > 0) && !blockEform) return false;
      if (eform && !(b > 0)) {
        // exp(b) underflowed to 0 (bank 016b8ffe `logit foreign mpg tw, or` with
        // tw = weight*1e-6: odds ratio 0, se 0, z -3.86, interval 0 0): the
        // coefficient-metric b and se are e(b)'s and sqrt(e(V))'s, and the test
        // and interval are rebuilt from them.
        const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
        const auto eV = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
        if (!eb.matrix.has_value() || !eV.matrix.has_value() || eb.matrix->data.size() != 1 ||
            eb.matrix->data[0].size() != cols.size() || eV.matrix->data.size() != cols.size() ||
            eV.matrix->data[ci].size() != cols.size())
          return false;
        b = eb.matrix->data[0][ci];
        const double v = eV.matrix->data[ci][ci];
        if (!(v > 0) || !std::isfinite(b)) return false;
        se = std::sqrt(v);
        colRebuild = true;
      } else if (eform) {
        se = se / b;
        b = std::log(b);
      }
      if (se > 0 && isAncillary(*c.row) && !eform) {
        // Stata posts the absent test of an ancillary parameter as `.b`, not `.`
        // (bank d8de89e3: r(table)[3,3] +1.0020000000000X+3ff).
        t = eval::stataMissingWithKind(2);
        p = eval::stataMissingWithKind(2);
        ll = rowNum(*c.row, "ci_lo");
        ul = rowNum(*c.row, "ci_hi");
        if (!std::isfinite(ll) || !std::isfinite(ul)) return false;
        const double ulpB = std::nextafter(std::fabs(b), HUGE_VAL) - std::fabs(b);
        const double half = (ul - ll) / (2.0 * se);
        // A variance's interval is on the log scale (tobit's `/var(e.y)`: b
        // exp(-/+ crit se / b)), checked in that metric.
        if (c.row->value("ci_log", false)) {
          if (!(b > 0) || !(ll > 0) ||
              std::fabs(std::log(ul / ll) / (2.0 * se / b) - crit) > 1e-6 * crit)
            return false;
        } else if (crit * se > 1e3 * ulpB && std::fabs(half - crit) > 1e-6 * crit) {
          return false;
        }
      } else if (se > 0) {
        if (colRebuild) {
          t = b / se;
          p = std::erfc(std::fabs(t) / std::sqrt(2.0));
          ll = b - crit * se;
          ul = b + crit * se;
        } else {
          t = rowNum(*c.row, "t");
          if (std::isnan(t)) t = rowNum(*c.row, "z");
          p = rowNum(*c.row, "p");
          ll = rowNum(*c.row, "ci_lo");
          ul = rowNum(*c.row, "ci_hi");
          if (eform) {
            ll = std::log(ll);
            ul = std::log(ul);
          }
          // The block must agree with the declared level and reference; a block
          // that does not is not Stata's table (header), so nothing is posted.
          if (!std::isfinite(t) || !std::isfinite(p) || !std::isfinite(ll) || !std::isfinite(ul))
            return false;
          // A half-width within ~1000 ulps of b cannot be checked: ul - ll is
          // quantized by b's spacing (BUG-1056: b 2, se 1.6e-16 gives a measured
          // half of 2.7 against crit 2.0106). The p check below still pins the
          // reference distribution.
          const double ulpB = std::nextafter(std::fabs(b), HUGE_VAL) - std::fabs(b);
          const double half = (ul - ll) / (2.0 * se);
          if (estimator != "proportion" && crit * se > 1e3 * ulpB &&
              std::fabs(half - crit) > 1e-6 * crit)
            return false;
          const double pRef = zRef ? std::erfc(std::fabs(t) / std::sqrt(2.0))
                                   : cecon::t_twosided_pvalue(t, dfR);
          if (!(std::fabs(p - pRef) <= 1e-8 + 1e-6 * pRef)) return false;
        }
      } else if (!std::isnan(se) && se != 0) {
        return false;  // a negative/invalid se: not a table this code reproduces
      } else if (se == 0 && c.row->contains("t") && c.row->at("t").is_null() &&
                 c.row->contains("p") && c.row->at("p").is_null()) {
        // The engine's exact `regress` fit (V = 0, t and p posted missing):
        // Stata's r(table) prints the se as `.` too (StataBE `regress yx x` on
        // an exact line: se, t, pvalue, ll, ul all missing), while _se[] is 0.
        se = rtNan();
      }
    }
    if (eform) {
      // exp() back to the reported metric; the base column's 0 becomes 1.
      const double eb = std::exp(b);
      b = eb;
      if (!std::isnan(se)) se = eb * se;
      if (!std::isnan(ll)) ll = std::exp(ll);
      if (!std::isnan(ul)) ul = std::exp(ul);
    }
    const double col[9] = {b, se, t, p, ll, ul, zRef ? rtNan() : dfR, crit, eform ? 1.0 : 0.0};
    for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
  }
  // r(table)'s columns are e(b)'s full names: after a fit whose e(b) carries an
  // equation (logit's and glm's depvar equation, BUG-1328) they are
  // `foreign:mpg`, not `mpg` (vs-stata truth bank: `logit foreign mpg weight`
  // then `: colfullnames r(table)`). Taken from the e(b) the session answers, and
  // only when it lists the same columns in the same order (names compared after
  // their last `:`), so a table this code laid out differently keeps its names.
  if (isMeanProp) {
    const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kNames);
    if (!eb.matrix.has_value() || eb.matrix->colnames.size() != m.colnames.size()) return false;
    m.colnames = eb.matrix->colnames;
  } else if (!isNlcom && !isMargins) {
    const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kNames);
    // A table with ancillary columns is Stata's only when e(b) names the same
    // columns (the cutpoints are e(b)'s own): otherwise nothing is posted.
    if (!ancillary.empty() &&
        (!eb.matrix.has_value() || eb.matrix->colnames.size() != m.colnames.size()))
      return false;
    if (eb.matrix.has_value() && eb.matrix->colnames.size() == m.colnames.size()) {
      const auto bare = [](const std::string& n) {
        const size_t c = n.rfind(':');
        return c == std::string::npos ? n : n.substr(c + 1);
      };
      bool same = true;
      for (size_t i = 0; same && i < m.colnames.size(); ++i)
        same = bare(eb.matrix->colnames[i]) == bare(m.colnames[i]);
      if (same) m.colnames = eb.matrix->colnames;
      else if (!ancillary.empty()) return false;
    }
    for (const ojson* row : diparm) {
      const double b = rowNum(*row, "b"), se = rowNum(*row, "se");
      const double ll = rowNum(*row, "ci_lo"), ul = rowNum(*row, "ci_hi");
      // A bound the transform cannot represent (exp() of a huge limit: alpha's upper
      // bound when xtpoisson's fit is at the alpha -> 0 boundary) is posted by the
      // handler as an explicit null and is missing in r(table), as Stata's diparm
      // gives it; any other non-finite bound is not a table this code reproduces.
      const auto boundOk = [row](const char* key, double v) {
        return std::isfinite(v) || (row->contains(key) && row->at(key).is_null());
      };
      if (!std::isfinite(b) || !(se > 0) || !boundOk("ci_lo", ll) || !boundOk("ci_hi", ul))
        return false;
      m.colnames.push_back(row->value("name", std::string()));
      const double col[9] = {b, se, eval::stataMissingWithKind(2), eval::stataMissingWithKind(2), ll, ul,
                             zRef ? rtNan() : dfR, crit, 0.0};
      for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
    }
  }
  // nbreg's ancillary parameter: e(b)'s `/lnalpha` and its transform alpha
  // (`_diparm1:alpha`), which Stata's table lists after the equation. Both have
  // z and p `.b` and df `.`; lnalpha's interval is b -/+ crit*se from e(V), alpha's
  // is exp() of it with se exp(lnalpha)*se (bank dba6710a `nbreg fatal beertax
  // unemp`: r(table) 9 x 5, `/lnalpha` -.3321361 se .0699984, alpha .7173897 se
  // .0502162). When this build has no readable e(b) ending in `/lnalpha` (a
  // factor-variable fit) or no usable se for it (the alpha -> 0 boundary), the
  // equation's columns are still posted, as they are Stata's; the two ancillary
  // columns are then absent rather than invented.
  if (estimator == "nbreg") {
    const auto eb = session.eval.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
    const auto eV = session.eval.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
    bool usable = eb.matrix.has_value() && eV.matrix.has_value() && eb.matrix->data.size() == 1 &&
                  !eb.matrix->colnames.empty() && eb.matrix->colnames.back() == "/lnalpha" &&
                  eb.matrix->colnames.size() == m.colnames.size() + 1;
    const size_t j = usable ? eb.matrix->colnames.size() - 1 : 0;
    if (usable) usable = eV.matrix->data.size() == j + 1 && eV.matrix->data[j].size() == j + 1;
    // the equation's columns take e(b)'s full names (`fatal:beertax`)
    std::vector<std::string> fullNames;
    for (size_t i = 0; usable && i < j; ++i) {
      const std::string& full = eb.matrix->colnames[i];
      const size_t c = full.rfind(':');
      const std::string bare = c == std::string::npos ? full : full.substr(c + 1);
      const size_t c2 = m.colnames[i].rfind(':');
      if (bare != (c2 == std::string::npos ? m.colnames[i] : m.colnames[i].substr(c2 + 1))) usable = false;
      else fullNames.push_back(full);
    }
    const double b = usable ? eb.matrix->data[0][j] : rtNan();
    const double var = usable ? eV.matrix->data[j][j] : rtNan();
    if (!(usable && std::isfinite(b) && var > 0 && std::isfinite(var))) {
      // The equation is the depvar's (bank d93bc657 `nbreg fatal beertax i.year`:
      // `fatal:beertax ... fatal:_cons`; 3dec0bd8 at the boundary: `rep78:mpg`).
      const std::string dv = block.value("depvar", std::string());
      if (!dv.empty())
        for (auto& c : m.colnames)
          if (c.find(':') == std::string::npos) c = dv + ":" + c;
    } else {
      for (size_t i = 0; i < j; ++i) m.colnames[i] = fullNames[i];
      const double se = std::sqrt(var);
      const double dotB = eval::stataMissingWithKind(2);  // `.b`
      const double lo = b - crit * se, hi = b + crit * se;
      const double lnCol[9] = {b, se, dotB, dotB, lo, hi, rtNan(), crit, 0.0};
      const double alCol[9] = {std::exp(b), std::exp(b) * se, dotB, dotB, std::exp(lo), std::exp(hi),
                               rtNan(), crit, 0.0};
      m.colnames.push_back("/lnalpha");
      m.colnames.push_back("_diparm1:alpha");
      for (int r = 0; r < 9; ++r) {
        m.data[static_cast<size_t>(r)].push_back(lnCol[r]);
        m.data[static_cast<size_t>(r)].push_back(alCol[r]);
      }
    }
  }
  out = std::move(m);
  return true;
}

bool postRTableFromBlock(SessionState& session, const ojson& block, double level) {
  // csdid's Level(int 95) defaults to a literal 95, not to c(level): after
  // `set level 90` a plain csdid still prints, and posts, its 95% interval. The
  // fit records the level it printed at (estimation/did_estimation.cpp).
  if (block.is_object() && block.value("estimator", std::string()) == "csdid" &&
      block.contains("level") && block["level"].is_number())
    level = block["level"].get<double>();
  eval::EvalMatrix tbl;
  if (!buildRTableFromBlock(session, block, level, tbl)) return false;
  // proportion, percent: the table's estimates, standard errors and limits are in
  // percent; t, p, df and crit are unchanged (vs-stata bank cdcb1db2 `proportion rep78,
  // percent`: r(table) b 2.898550724637681, se 2.019662319890042, ll .7079410586736167)
  const bool percent = block.is_object() && block.value("estimator", std::string()) == "proportion" &&
                       block.value("percent", false);
  if (percent && tbl.data.size() >= 6)
    for (size_t r : {0u, 1u, 4u, 5u})
      for (double& x : tbl.data[r]) x *= 100.0;
  // The table's column notes (r(label#)). nlcom posts none here; margins only
  // the base level of a factor effect (vs-stata bank ad03c6f5 `margins,
  // dydx(foreign)`: r(label1) `(base)` for `0b.foreign`) -- its other notes come
  // from its error codes, which this table does not carry.
  const std::string est = block.is_object() ? block.value("estimator", std::string()) : std::string();
  if (est == "mean" || est == "proportion") {
    // A column with no estimate is noted: a mean of 0 with variance 0 "(omitted)", a
    // proportion of 0 "(no observations)" (vs-stata bank e5beada5 `mean v, over(foreign)`,
    // v all 0: r(label1) r(label2) "(omitted)"; 0b672af1 `proportion foreign,
    // over(rep78)`: r(label6) "(no observations)").
    for (size_t i = 0; tbl.data.size() >= 2 && i < tbl.colnames.size(); ++i) {
      if (!(tbl.data[0][i] == 0.0 && (est == "mean" ? tbl.data[1][i] == 0.0 : std::isnan(tbl.data[1][i]))))
        continue;
      const std::string key = "label" + std::to_string(i + 1);
      session.eval.rStrings.set(key, est == "mean" ? "(omitted)" : "(no observations)");
      session.eval.markRMacroHidden(key);
    }
  } else if (est != "nlcom") {
    postCoefficientLabels(session, tbl, est == "margins_rtable");
  }
  const size_t nCols = tbl.colnames.size();
  setRTable(session, std::move(tbl));
  // r(level) beside r(table) (w2o_rnames.log:400 N24 `regress` r(level) 95,
  // :410 N25 level(90) -> 90).
  session.eval.rValues.set("level", level);
  // mean / proportion's table also returns its title, the multiple-comparison method,
  // the interval type and k_eform (vs-stata bank 85536cc0 `mean price mpg`: r(title)
  // "Estimated means", r(mcmethod) "noadjust", r(citype) "normal", r(k_eform) 1;
  // 4525943d `proportion rep78`: "Estimated proportions", "logit", 0; one column
  // f84271c3 `mean big`: "Estimated mean").
  if (est == "mean" || est == "proportion") {
    const bool one = nCols == 1;
    const std::string what = est == "mean" ? (one ? "mean" : "means")
                                           : (one ? "proportion" : "proportions");
    session.eval.rStrings.set("title", percent ? std::string(one ? "Estimated percent"
                                                                : "Estimated percentages")
                                               : "Estimated " + what);
    session.eval.rStrings.set("mcmethod", "noadjust");
    session.eval.rStrings.set("citype", est == "mean" ? std::string("normal")
                                                      : block.value("citype", std::string("logit")));
    session.eval.rValues.set("k_eform", est == "mean" ? 1.0 : 0.0);
  }
  return true;
}

bool replaceStaleRClass(eval::EvalSession& ev, const eval::RStoreMark& mark) {
  size_t leftOver = 0;
  bool postedMatrix = false;
  for (const auto& kv : ev.rMatrices) {
    if (kv.second.gen <= mark.seq) ++leftOver;
    else postedMatrix = true;
  }
  const bool wroteR = ev.rValues.generation() > mark.seq || ev.rStrings.generation() > mark.seq ||
                      postedMatrix || leftOver < mark.rMatrixCount;
  if (!wroteR) return false;
  ev.rValues.eraseWrittenAtOrBefore(mark.seq);
  ev.rStrings.eraseWrittenAtOrBefore(mark.seq);
  for (auto it = ev.rMatrices.begin(); it != ev.rMatrices.end();) {
    if (it->second.gen <= mark.seq) it = ev.rMatrices.erase(it);
    else ++it;
  }
  if (!ownsRTable(ev)) {
    ev.rTableGen = 0;
    ev.rTableEid = 0;
    ev.rTableModel.clear();
  }
  if (!ev.rTableRefusal.empty() && ev.rTableRefusalGen <= mark.seq) forgetRTableRefusal(ev);
  return true;
}

namespace {

// What a command did to r(), for the identity of the live r-class command
// (EvalSession::rClassCmd; RULING.md C.3): nothing, wrote r() (replaced it),
// a fit (r() is the fit's: {table, level}), or a write whose Stata set this
// build does not know (margins, a resampling prefix).
enum class RAfter { kNone, kWrote, kFit, kUnknown };

// A fit whose structured block stays out of the reply (it prints its own table
// text): leebounds. Its if/in rows are marked by inner `generate` lines, so it is
// a container to dispatch(), yet it is the leaf fit that posts r().
bool fitKeepsBlockOutOfReply(const SessionState& session, const std::string& rawCmd) {
  return commandWord(rawCmd) == "leebounds" && session.lastEstBlock.has_value() &&
         session.lastEstBlock->is_object() &&
         session.lastEstBlock->value("estimator", std::string()) == "leebounds";
}

// The leaf half of rTableAfterCommand: an estimation replaces r() and posts its
// table, a replay re-posts, a restore of another model retires, margins replaces
// r() and refuses its table.
RAfter leafPostsOrRetires(SessionState& session, const ojson& result, const std::string& rawCmd,
                          bool eChanged, std::uintptr_t eIdNow) {
  // tabstat.ado is an r-class program: however it ends, r() is what it
  // returned, and a refused tabstat returned nothing (BUG-1151, StataBE 19.5:
  // after `capture tabstat x, s(nosuch) save`, `capture tabstat x [pw=w], save`
  // or `capture tabstat x in 10/12, save`, `matrix list r(StatTotal)` is r(111),
  // and an earlier summarize's r(N) reads `.` after any failed tabstat). Judged
  // here, after the line, so the refusals raised before the tabstat arm runs
  // (the tokenizer's weight and `in` checks, a weight variable that does not
  // exist) empty r() too, and an `if` read the caller's r() while it ran.
  if (commandWord(rawCmd) == "tabstat" && result.is_array()) {
    for (const auto& b : result) {
      if (!b.is_object() || b.value("type", std::string()) != "error") continue;
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "tabstat", {});
      return RAfter::kWrote;
    }
  }
  // FU-276: a margins form runMargins served posted its own r() (r(b), r(V),
  // r(table), r(N), ...) and declared Stata's set for the invocation
  // (declareRPosted): r() is margins', `post` included -- live StataBE 19.5
  // (2026-10-13) still lists r(b)/r(V)/r(table)/r(N) after `margins t, post` --
  // so neither the fit branch below nor the refusal branch may replace it.
  if (commandWord(rawCmd) == "margins" && session.eval.rDeclaredNames.has_value() &&
      session.eval.rDeclaredCmd == "margins")
    return RAfter::kWrote;
  // csdid-estat-estore-post: `estat simple|event` after csdid returns its own
  // r(b)/r(V)/r(table)/r(agg) (csdid_estat.ado, rclass) and, under `post`, installs
  // the posting as a new e(); its r() is the estat's, never a fit's table
  // (w2ag log:1564 ag_simple_nopost_r, :1777 ag_evwin_rtable).
  if (commandWord(rawCmd) == "estat" && session.eval.rDeclaredNames.has_value() &&
      session.eval.rDeclaredCmd == "estat")
    return RAfter::kWrote;
  // nlcom posts its own r(table) (dispatcher's nlcom arm), `post` or not.
  const ojson* est = nullptr;
  if (result.is_array()) {
    for (const auto& b : result) {
      if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
      if (b.value("estimator", std::string()) == "nlcom") return RAfter::kNone;
      est = &b;
    }
  }
  // A fit that prints its own table text and keeps its structured block out of
  // the reply (leebounds_estimation.cpp: the ado's `ereturn display` header would
  // be printed twice otherwise) is still a new estimation: its block is the live
  // one, installed by this very command (never by `estimates restore`, whose
  // command word is not the estimator's).
  if (eChanged && est == nullptr && fitKeepsBlockOutOfReply(session, rawCmd))
    est = &*session.lastEstBlock;
  // A new estimation REPLACES r() (w2n_followups.log:968, E07: r(N)/r(mean) of an
  // earlier summarize read `.` after `regress`) and posts its own table.
  if (eChanged && est != nullptr) {
    eval::replaceRClass(session.eval);
    if (session.lastEstBlock.has_value() &&
        !postRTableFromBlock(session, *est, declaredLevel(rawCmd, session)))
      refuseRTable(session, estimationRefusal(session));
    // A fit whose ado leaves its own r-class results behind (ivreg2's last
    // `_ms_parse_parts`) carries them as the block's `r_after_fit`: posted on top.
    if (est->contains("r_after_fit") && (*est)["r_after_fit"].is_object()) {
      const ojson& ra = (*est)["r_after_fit"];
      if (ra.contains("scalars") && ra["scalars"].is_object())
        for (const auto& [k, v] : ra["scalars"].items())
          if (v.is_number()) session.eval.rValues.set(k, v.get<double>());
      if (ra.contains("macros") && ra["macros"].is_object())
        for (const auto& [k, v] : ra["macros"].items())
          if (v.is_string()) session.eval.rStrings.set(k, v.get<std::string>());
    }
    annotateEbStripe(session);
    postDefaultCmdline(session);
    postDefaultEMacros(session);
    return RAfter::kFit;
  }
  // e() replaced by a copy of the same model (`estadd`) keeps the table it has; any
  // other replacement with no estimation output (`estimates restore` of another
  // model) leaves no table behind.
  if (eChanged) {
    if (!rebindIfSameModel(session, eIdNow)) retireRTable(session);
    return RAfter::kNone;
  }
  // A replay (`regress` with no arguments) returns the live block unchanged: it
  // re-displays the fit, and r() is the fit's again.
  if (est != nullptr && session.lastEstBlock.has_value() && *est == *session.lastEstBlock) {
    eval::replaceRClass(session.eval);
    if (!postRTableFromBlock(session, *est, declaredLevel(rawCmd, session)))
      refuseRTable(session, estimationRefusal(session));
    return RAfter::kFit;
  }
  // margins: Stata's r() is margins' own (margins's help). Every refused
  // margins (and runGlmMargins, which declares nothing) replaces r()
  // (replaceRClass also retires the table) and every later read of it refuses
  // (RULING.md D.2.3): when PIET refuses a margins that Stata runs, Stata's r() is
  // still margins'. Unconditional for those -- the served forms returned above.
  if (commandWord(rawCmd) == "margins") {
    eval::replaceRClass(session.eval);
    refuseRTable(session, "r(table) after margins is margins' own table, which this version "
                          "of PIET does not build.");
    return RAfter::kUnknown;
  }
  return RAfter::kNone;
}

RAfter afterCommand(SessionState& session, const ojson& result, const std::string& rawCmd,
                    std::uintptr_t eIdAtEntry, bool leaf, const eval::RStoreMark& markAtEntry) {
  const std::uintptr_t eIdNow = liveEstimationIdentity(session);
  const bool eChanged = eIdNow != eIdAtEntry;
  // The replicating prefixes (dispatcher.cpp's reResamplePrefix) run their inner
  // command once per replicate through dispatch(), so the LAST replicate's leaf
  // posted a table. Stata's table there is the resampling table, which PIET does
  // not build: none is served.
  {
    const std::string word = commandWord(rawCmd);
    // bootstrap posts its own normal-based table for the e() it installs
    // (runBootstrap); that one stays, and the container path below keeps it.
    // Only bootstrap's own posting qualifies: on a failure path the live e()
    // and table are the full-sample fit's or a replicate's.
    const bool bootstrapOwn = word == "bootstrap" && ownsRTable(session.eval) &&
                              session.eval.rTableEid == eIdNow && eIdNow != 0 &&
                              session.lastModel().has_value() &&
                              session.lastModel()->cobraCmd == "bootstrap";
    if (!bootstrapOwn &&
        (word == "bootstrap" || word == "jackknife" || word == "permute" || word == "rolling")) {
      retireRTable(session);
      refuseRTable(session, "r(table) after " + word +
                                " is the resampling table, which this version of PIET does not "
                                "build.");
      return RAfter::kUnknown;
    }
  }
  // Whatever this command wrote into r() replaced all of it. A leaf is judged
  // against r() at its entry; a container against r() after its last inner
  // command, so only its OWN writes count (an rclass program's `return` set is
  // committed to r() when the program ends, after every inner leaf).
  //
  // The one measured exception: `timer list` ADDS r(tN)/r(ntN) to r() and keeps
  // every other r() result (StataNow BE 19.5 probe of BUG-151: `count`'s r(N) 74
  // was still present after `timer list 1`; selftest "C4/BS-124 BUG-151"). It is
  // keyed on the command word because the timer handler maintains its own keys.
  bool wrote = false;
  if (!(leaf && commandWord(rawCmd) == "timer"))
    wrote = replaceStaleRClass(session.eval, leaf ? markAtEntry : session.eval.rMarkAfterLastCommand);
  if (!leaf && !(eChanged && fitKeepsBlockOutOfReply(session, rawCmd))) {
    // A container that replaced e(): keep the table only if a leaf inside posted it
    // for the e() that is live now.
    if (eChanged && !rebindIfSameModel(session, eIdNow) && !refusedFor(session, eIdNow))
      retireRTable(session);
    return wrote ? RAfter::kWrote : RAfter::kNone;
  }
  const RAfter leafWhat = leafPostsOrRetires(session, result, rawCmd, eChanged, eIdNow);
  if (leafWhat != RAfter::kNone) return leafWhat;
  return wrote ? RAfter::kWrote : RAfter::kNone;
}

// Record the identity of the r-class command whose r() is now live (RULING.md
// C.3). A command that did not replace r() records nothing: the previous
// identity stays, exactly as r() does. "present" = what r() holds now.
void setRClassIdentity(eval::EvalSession& ev, std::string cmd,
                       const std::vector<std::string>& stataNames) {
  ev.rClassCmd = std::move(cmd);
  ev.rClassSetUnknown = false;
  ev.rUnposted.clear();
  for (const auto& n : stataNames) {
    const bool present = ev.rValues.find(n) != nullptr ||
                         (!eval::isReservedStoreKey(n) && ev.rStrings.find(n) != nullptr) ||
                         ev.findClassMatrix('r', n) != nullptr;
    if (!present) ev.rUnposted.push_back(n);
  }
}

void recordRClassIdentity(eval::EvalSession& ev, RAfter what, bool leaf, const std::string& rawCmd) {
  switch (what) {
    case RAfter::kNone:
      break;
    case RAfter::kFit:
      // r(level) after a fit is UNPINNED (RULING.md D.2.4, FU-o): kept in the set,
      // so it refuses until a capture settles it.
      setRClassIdentity(ev, commandWord(rawCmd), {"table", "level"});
      break;
    case RAfter::kUnknown:
      ev.rClassCmd = commandWord(rawCmd);
      ev.rUnposted.clear();
      ev.rClassSetUnknown = true;
      break;
    case RAfter::kWrote:
      if (ev.rDeclaredNames.has_value()) {
        setRClassIdentity(ev, ev.rDeclaredCmd.empty() ? commandWord(rawCmd) : ev.rDeclaredCmd,
                          *ev.rDeclaredNames);
      } else if (!leaf) {
        // A container's own write after its last inner command: an rclass
        // program's return set IS r() (class X).
        ev.rClassCmd = commandWord(rawCmd);
        ev.rUnposted.clear();
        ev.rClassSetUnknown = false;
      } else {
        ev.rClassCmd = commandWord(rawCmd);
        ev.rUnposted.clear();
        ev.rClassSetUnknown = true;
      }
      break;
  }
  ev.rDeclaredNames.reset();
  ev.rDeclaredCmd.clear();
}

}  // namespace

void rTableAfterCommand(SessionState& session, const ojson& result, const std::string& rawCmd,
                        std::uintptr_t eIdAtEntry, bool leaf, const eval::RStoreMark& markAtEntry) {
  const RAfter what = afterCommand(session, result, rawCmd, eIdAtEntry, leaf, markAtEntry);
  recordRClassIdentity(session.eval, what, leaf, rawCmd);
  session.eval.rMarkAfterLastCommand = session.eval.rMark();
}

}  // namespace piet::handlers
