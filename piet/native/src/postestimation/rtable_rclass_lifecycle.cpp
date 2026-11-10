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
    for (const auto& [k, v] : opts) {
      if (strutil::lower(k) != "level") continue;
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

bool buildRTableFromBlock(SessionState& session, const ojson& block, double level,
                          eval::EvalMatrix& out) {
  if (!block.is_object() || !block.contains("coeftable") || !block["coeftable"].is_array() ||
      block["coeftable"].empty())
    return false;
  if (!(level >= 10.0 && level <= 99.99)) return false;
  const ojson& ct = block["coeftable"];
  const std::string estimator = block.value("estimator", std::string());
  // bootstrap's table (estimation/bootstrap_prefix.cpp runBootstrap) is normal-based too,
  // named by its own e(b) stripe: the same shape as nlcom's.
  const bool isNlcom = estimator == "nlcom" || estimator == "bootstrap";
  // runMargins' own table (margins.cpp postMarginsRClassM4): its columns
  // are margins' r(b) names (`_cons`, `1.t`, `x1`), not the model's e(b); the
  // reference is t(df_r) when the block carries one, the normal otherwise.
  const bool isMargins = estimator == "margins_rtable";

  std::vector<RtCol> cols;
  if (isNlcom || isMargins) {
    for (const auto& row : ct) cols.push_back({row.value("name", std::string()), &row});
  } else {
    const auto& lmOpt = session.lastModel();
    if (!lmOpt.has_value() || !stataColumns(session, *lmOpt, ct, cols)) return false;
  }

  // Reference distribution: t(e(df_r)) when the fit publishes a residual df
  // (G-1 under clustering), the normal otherwise (logit, ivregress's default, ...:
  // `z` row, df missing). nlcom is ALWAYS normal in Stata, whatever the model
  // (w2c_postest.log prints `z` after `regress`).
  const double dfR = (!isNlcom && block.contains("df_r") && block["df_r"].is_number())
                         ? block["df_r"].get<double>() : rtNan();
  const bool zRef = isNlcom || !(dfR > 0);
  const double crit = critFor(level, zRef, dfR);
  if (!(crit > 0)) return false;
  // Blocks whose Stata definition is normal-referenced but whose printed p/CI use
  // another reference: rebuilt from b and se (see the header).
  const bool rebuild = isNlcom || estimator == "poisson" || estimator == "ppmlhdfe" ||
                       estimator == "fepois";
  // An eform block (`logistic`, `logit, or`, stcox's hazard ratios) carries exp(b),
  // exp(b)*se and exp() of the interval; the checks run on the coefficient metric.
  const bool eform = block.contains("coef_metric") && block["coef_metric"].is_string();

  eval::EvalMatrix m;
  m.rownames = {"b", "se", zRef ? "z" : "t", "pvalue", "ll", "ul", "df", "crit", "eform"};
  m.data.assign(9, std::vector<double>());
  for (const auto& c : cols) {
    // An engine-omitted column (b 0 and se 0, kept in the table) is Stata's
    // `o.` column: b 0, se/t/p/ll/ul missing -- the same zero rule
    // annotateEbStripe applies to e(b), so r(table) and e(b) name it alike
    // (StataBE r1.do: an omitted column is `o.z`, se `.`).
    if (c.row != nullptr && !eform && rowNum(*c.row, "b") == 0.0 && rowNum(*c.row, "se") == 0.0) {
      m.colnames.push_back(omittedColumnName(c.name));
      const double col[9] = {0.0, rtNan(), rtNan(), rtNan(), rtNan(), rtNan(), zRef ? rtNan() : dfR, crit, 0.0};
      for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
      continue;
    }
    m.colnames.push_back(c.name);
    double b = 0, se = rtNan(), t = rtNan(), p = rtNan(), ll = rtNan(), ul = rtNan();
    if (c.row != nullptr) {
      b = rowNum(*c.row, "b");
      se = rowNum(*c.row, "se");
      if (eform) {
        if (!(b > 0)) return false;
        se = se / b;
        b = std::log(b);
      }
      if (se > 0) {
        if (rebuild) {
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
          if (crit * se > 1e3 * ulpB && std::fabs(half - crit) > 1e-6 * crit) return false;
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
  out = std::move(m);
  return true;
}

bool postRTableFromBlock(SessionState& session, const ojson& block, double level) {
  eval::EvalMatrix tbl;
  if (!buildRTableFromBlock(session, block, level, tbl)) return false;
  setRTable(session, std::move(tbl));
  // r(level) beside r(table) (w2o_rnames.log:400 N24 `regress` r(level) 95,
  // :410 N25 level(90) -> 90).
  session.eval.rValues.set("level", level);
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
  // A new estimation REPLACES r() (w2n_followups.log:968, E07: r(N)/r(mean) of an
  // earlier summarize read `.` after `regress`) and posts its own table.
  if (eChanged && est != nullptr) {
    eval::replaceRClass(session.eval);
    if (session.lastEstBlock.has_value() &&
        !postRTableFromBlock(session, *est, declaredLevel(rawCmd, session)))
      refuseRTable(session, estimationRefusal(session));
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
  if (!leaf) {
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
