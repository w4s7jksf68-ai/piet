// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/observations.h"
#include "piet/handlers/utility.h"
#include "piet/dispatcher.h"  // carryforward.ado's steps run as commands

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "datamgmt/dataset_shape_support.h"
#include "piet/datastore.h"
#include "piet/eval/session.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"

#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// carryforward
// ===========================================================================
//
// Last-non-missing forward fill, reset at every by-group boundary.
//
// ROW ORDER IS THE ANSWER, not an implementation detail. The dispatcher's
// by-prefix branch already sorted the frame; this walks the frame in its
// CURRENT physical order, carrying each group's last non-missing value forward,
// and writes every result back at the row index it came from. A group whose
// rows are not contiguous (an explicit `by()` on unsorted data) still fills
// from the previous row OF THE SAME GROUP, which is the same answer sorting
// would have given and a defensible one when it was not.
namespace {

// carryforward.ado's own steps, for the forms with a sample or a carry rule:
// clone the variable (gen) or keep the original (replace with cfindic or
// carryalong), then one `[by g:] replace x = x[_n-1] if _n>1 & <sample> &
// <missing here, not missing before> [& sample before (strict)] [& rule]`,
// which Stata evaluates row by row so a carried value carries again; then
// the indicator (`x ~= original`), the note, and each carryalong variable
// carried where the indicator is on. `@` in dynamic_condition() is the value
// being carried, x[_n-1]; extmiss fills only `.` and may carry `.a`-`.z`.
ojson carryforwardStata(const eval::ParsedCommand& parsed, SessionState& session,
                        const std::vector<std::string>& newVars,
                        const std::vector<std::string>& byVars) {
  auto errOf = [](const ojson& blocks) -> std::optional<ojson> {
    if (blocks.is_object() && blocks.value("type", std::string()) == "error") return blocks;
    if (blocks.is_array())
      for (const auto& b : blocks)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return std::nullopt;
  };
  ojson out = ojson::array();
  auto keep = [&](const ojson& blocks) {
    if (!blocks.is_array()) return;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) != "echo") out.push_back(b);
  };
  const bool back = hasFlag(parsed.opts, "back");
  const bool strict = hasFlag(parsed.opts, "strict");
  const bool extmiss = hasFlag(parsed.opts, "extmiss");
  const bool nonotes = hasFlag(parsed.opts, "nonotes");
  const std::string fwdback = back ? " (backward)" : "";
  std::vector<std::string> cfindic, carryalong;
  if (auto c = optStr(parsed.opts, "cfindic")) cfindic = splitWs(*c);
  if (auto c = optStr(parsed.opts, "carryalong")) carryalong = splitWs(*c);
  const std::string dyn = optStr(parsed.opts, "dynamic_condition").value_or("");
  const std::vector<std::string>& vars = parsed.varlist;
  if (!carryalong.empty() && vars.size() > 1)
    return utilErrorRc("carryalong may not be specified if varlist has more than one variable", 198);
  if (!cfindic.empty()) {
    if (cfindic.size() != vars.size())
      return utilErrorRc("there must be as many variables in cfindic as in varlist", 198);
    for (const auto& c : cfindic) {
      ojson guard = newVarGuard(session, "carryforward", c);
      if (!guard.is_null()) return guard;
      if (std::find(newVars.begin(), newVars.end(), c) != newVars.end())
        return utilErrorRc("cfindic and gen may not have elements in common", 198);
    }
  }
  const std::string byPrefix =
      byVars.empty() ? std::string() : "by " + joinWith(byVars, " ") + ": ";

  // The if/in sample (marksample, novarlist) as a 1/. column, and the
  // temporaries this routine makes, all dropped on the way out.
  std::vector<std::string> temps;
  struct DropTemps {
    SessionState& s;
    std::vector<std::string>& names;
    ~DropTemps() {
      if (names.empty()) return;
      try {
        SessionState::TempDropScope quiet(s);
        (void)dispatch("quietly drop " + joinWith(names, " "), s);
      } catch (...) {
      }
    }
  } dropTemps{session, temps};
  int serial = 0;
  auto tempName = [&]() {
    Datastore* d = session.datastore();
    std::string nm;
    do {
      nm = "__cf" + std::to_string(++serial);
    } while (d != nullptr && d->has(nm));
    return nm;
  };
  std::string touseCond = "1", strictCond;
  const bool haveIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
  const bool haveIn = parsed.inRange.has_value() && !parsed.inRange->empty();
  if (haveIf || haveIn) {
    const std::string tu = tempName();
    std::string q;
    if (haveIf) q += " if " + *parsed.ifExpr;
    if (haveIn) q += " in " + *parsed.inRange;
    ojson r = dispatch("quietly generate byte " + tu + " = 1" + q, session);
    if (auto e = errOf(r)) return *e;
    temps.push_back(tu);
    touseCond = "!missing(" + tu + ")";
    if (strict) strictCond = " & !missing(" + tu + "[_n-1])";
  }
  for (size_t k = 0; k < vars.size(); ++k) {
    const std::string& v = vars[k];
    Datastore* ds = session.datastore();
    const bool numeric = ds != nullptr && ds->dtype(v) != "factor";
    std::string ggg = v, orig;
    if (!newVars.empty()) {
      ggg = newVars[k];
      ojson r = dispatch("quietly clonevar " + ggg + " = " + v, session);
      if (auto e = errOf(r)) return *e;
    } else if (!cfindic.empty() || !carryalong.empty()) {
      orig = tempName();
      ojson r = dispatch("quietly clonevar " + orig + " = " + v, session);
      if (auto e = errOf(r)) return *e;
      temps.push_back(orig);
    }
    out.push_back(ojson{{"type", "display"}, {"text", ggg + ": "}});
    std::string dynCond;
    if (!dyn.empty()) {
      std::string d = dyn;
      for (size_t at = d.find('@'); at != std::string::npos; at = d.find('@', at))
        d.replace(at, 1, ggg + "[_n-1]");
      dynCond = " & (" + d + ")";
    }
    const std::string misval = extmiss && numeric
                                   ? "(" + ggg + "==.) & (" + ggg + "[_n-1]~=.)"
                                   : "missing(" + ggg + ") & !missing(" + ggg + "[_n-1])";
    ojson rr = dispatch(byPrefix + "replace " + ggg + " = " + ggg + "[_n-1] if _n>1 & " +
                            touseCond + " & " + misval + strictCond + dynCond,
                        session);
    if (auto e = errOf(rr)) return *e;
    keep(rr);
    std::string ccc = k < cfindic.size() ? cfindic[k] : std::string();
    if (ccc.empty() && !carryalong.empty()) {
      ccc = tempName();
      temps.push_back(ccc);
    }
    if (!ccc.empty()) {
      ojson g = dispatch("quietly generate byte " + ccc + " = " + ggg + " ~= " +
                             (newVars.empty() ? orig : v),
                         session);
      if (auto e = errOf(g)) return *e;
      ojson l = dispatch("label variable " + ccc + " \"" + ggg + " has a carryforward" + fwdback +
                             " value\"",
                         session);
      if (auto e = errOf(l)) return *e;
    }
    if (!nonotes) {
      ojson nt = dispatch("notes " + ggg + ": subjected to a carryforward" + fwdback + " operation",
                          session);
      if (auto e = errOf(nt)) return *e;
    }
    for (const auto& var : carryalong) {
      const std::string t2 = tempName();
      ojson c1 = dispatch("quietly clonevar " + t2 + " = " + var, session);
      if (auto e = errOf(c1)) return *e;
      temps.push_back(t2);
      ojson c2 = dispatch("quietly " + byPrefix + "replace " + var + " = " + var +
                              "[_n-1] if _n>1 & " + ccc,
                          session);
      if (auto e = errOf(c2)) return *e;
      ojson c3 = dispatch("quietly count if " + var + " ~= " + t2, session);
      if (auto e = errOf(c3)) return *e;
      const double* nch = session.eval.rValues.find("N");
      out.push_back(ojson{{"type", "display"},
                          {"text", "(" + var + ": " + intText(nch ? static_cast<int64_t>(*nch) : 0) +
                                       " changes made)"}});
      if (!nonotes) {
        ojson nt = dispatch("notes " + var + ": subjected to a carryforward" + fwdback +
                                " operation as a carryalong variable, along with " + v,
                            session);
        if (auto e = errOf(nt)) return *e;
      }
    }
  }
  return out;
}

}  // namespace

ojson runCarryforward(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCarryforward.known,
                                            kVocabCarryforward.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return utilSyntax(
        "carryforward: syntax is `[bysort g:] carryforward varlist, gen(newvarlist)|replace`");

  const bool replace = hasFlag(parsed.opts, "replace");
  const std::optional<std::string> gen = optStrAny(parsed.opts, {"gen", "generate"});
  if (replace == gen.has_value())
    return utilSyntax("carryforward: give exactly one of gen(newvarlist) and replace.");

  std::vector<std::string> newVars;
  if (gen.has_value()) {
    newVars = splitWs(*gen);
    if (newVars.size() != parsed.varlist.size())
      return utilSyntax("carryforward: gen() names " + intText(static_cast<int64_t>(newVars.size())) +
                        " variable(s) for " + intText(static_cast<int64_t>(parsed.varlist.size())) +
                        " input variable(s); the two lists must be the same length.");
  }

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);

  // A sample, or any of carryforward.ado's carry rules, takes the ado's own
  // steps (carryforwardStata above).
  if ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
      (parsed.inRange.has_value() && !parsed.inRange->empty()) || hasFlag(parsed.opts, "back") ||
      hasFlag(parsed.opts, "strict") || hasFlag(parsed.opts, "extmiss") ||
      hasFlag(parsed.opts, "nonotes") || optStr(parsed.opts, "cfindic").has_value() ||
      optStr(parsed.opts, "carryalong").has_value() ||
      optStr(parsed.opts, "dynamic_condition").has_value()) {
    for (const auto& nm : newVars) {
      ojson guard = newVarGuard(session, "carryforward", nm);
      if (!guard.is_null()) return guard;
    }
    return carryforwardStata(parsed, session, newVars, byVars);
  }
  {
    ojson q = refuseQualifiers(
        parsed, "`carryforward` writes a column as long as the dataset, and this build cannot map a "
        "filtered sample back onto the original rows. Restrict first (`preserve` + `keep if "
        "...`), then run carryforward.");
    if (!q.is_null()) return q;
  }

  std::vector<std::string> readCols = parsed.varlist;
  for (const auto& b : byVars) readCols.push_back(b);
  ReadScope scope(session, parsed, readCols, "carryforward", false);
  if (scope.failed())
    return scope.error().is_null() ? utilError("carryforward: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }
  for (const auto& nm : newVars) {
    ojson guard = newVarGuard(session, "carryforward", nm);
    if (!guard.is_null()) return guard;
  }

  const int64_t n = ds.nRows();
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

  std::string cur = *session.currentHbin();
  std::vector<std::string> rewritten;
  for (size_t k = 0; k < parsed.varlist.size(); ++k) {
    const std::string& src = parsed.varlist[k];
    const std::string dtype = (ds.dtype(src) == "factor") ? "factor" : "f64";
    std::vector<std::string> cells = columnAsStrings(ds, src);
    // "" is missing in a setcol payload, which is exactly the state that gets
    // filled -- and a group's FIRST observation, having nothing before it,
    // keeps its missing rather than borrowing from another group.
    std::vector<std::string> last(static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
    std::vector<char> haveLast(static_cast<size_t>(std::max<int64_t>(nGroups, 1)), 0);
    for (int64_t i = 0; i < n; ++i) {
      const size_t g = static_cast<size_t>(gid[static_cast<size_t>(i)]);
      std::string& cell = cells[static_cast<size_t>(i)];
      if (!cell.empty()) {
        last[g] = cell;
        haveLast[g] = 1;
      } else if (haveLast[g]) {
        cell = last[g];
      }
    }
    const std::string target = gen.has_value() ? newVars[k] : src;
    std::string out;
    if (auto err = writeColumn(session, cur, target, cells, dtype, gen.has_value(), &out))
      return *err;
    cur = out;
    if (!gen.has_value()) rewritten.push_back(target);
  }

  session.load(cur);
  if (gen.has_value()) return mutationResult(session, "gen", newVars);
  return mutationResult(session, "gen", {}, {}, rewritten);
}

// ===========================================================================
// tsspell
// ===========================================================================
//
// Spells over a tsset series, the way tsspell.ado builds them, walking each panel in
// time order:
//
//   cond-form (cond(), or pcond() as `(x) > 0 & (x) < .`):
//     _seq   = running count of consecutive observations where cond (and the if/in
//              sample) is true, restarting from 0 on a false one
//     _spell = a counter that advances at each _seq == 1, and is 0 where _seq is 0
//   fcond-form (fcond(), or `tsspell varname`, whose fcond is `v != L.v | _n == 1`):
//     _spell = running count of the observations where fcond (and the sample) holds,
//              0 outside the sample
//     _seq   = the position inside its _spell, 0 where _spell is 0
//   _end   = 1 on the last observation of a spell: _seq >= the next row's _seq, with
//            _seq non-zero (the panel's last row: _seq > 0)
//
// A gap in TIME does NOT break a cond() spell: the ado only warns, and leaves the count of
// the gaps in r(N). It does break a `tsspell varname` run, because L.v is missing across a gap.
//
// LIKE rangestat, THE ANSWERS GO BACK IN ORIGINAL ROW ORDER: the panel/time
// ordering below is an index permutation and never a rewrite of the frame.
ojson runTsspell(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTsspell.known,
                                            kVocabTsspell.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (!session.timevar.has_value() || session.timevar->empty())
    // StataBE 19.5: r(111) "time variable not set, use tsset varname ..."
    // (vs-stata bank, `tsspell, cond(change>0)` without tsset).
    return utilErrorRc("time variable not set, use tsset varname ...", 111);

  std::optional<std::string> cond = optStr(parsed.opts, "cond");
  const std::optional<std::string> fcond = optStr(parsed.opts, "fcond");
  const std::optional<std::string> pcond = optStr(parsed.opts, "pcond");
  if (parsed.varlist.size() > 1)
    return utilSyntax("tsspell: syntax is `tsspell [varname] [if] [in] [, cond(expression) "
                      "fcond(expression) pcond(expression) spell(name) seq(name) end(name) "
                      "replace]`");
  const int nopts = (cond.has_value() ? 1 : 0) + (fcond.has_value() ? 1 : 0) +
                    (pcond.has_value() ? 1 : 0);
  if (nopts > 1) return utilErrorRc("must specify at most one of cond(), fcond(), pcond()", 198);
  if (pcond.has_value()) cond = "((" + *pcond + ") > 0 & (" + *pcond + ") < .)";
  if (!cond.has_value() && !fcond.has_value() && parsed.varlist.empty())
    return utilErrorRc("insufficient information", 198);

  std::vector<char> inSample;
  if (auto err = dataset_shape_detail::ifInMask(session, parsed, &inSample)) return *err;

  const std::string spellVar = optStr(parsed.opts, "spell").value_or(std::string("_spell"));
  const std::string seqVar = optStr(parsed.opts, "seq").value_or(std::string("_seq"));
  const std::string endVar = optStr(parsed.opts, "end").value_or(std::string("_end"));
  const bool replaceVars = hasFlag(parsed.opts, "replace");
  // `replace` drops a variable of that name that already exists (the ado's `capture
  // confirm new variable` / `drop`), so the three are made anew at the end of the data.
  if (replaceVars) {
    std::vector<std::string> doomed;
    Datastore* d0 = session.datastore();
    for (const auto& nm : {spellVar, seqVar, endVar})
      if (d0 != nullptr && d0->has(nm) && nm != *session.timevar &&
          std::find(doomed.begin(), doomed.end(), nm) == doomed.end())
        doomed.push_back(nm);
    if (!doomed.empty()) {
      ojson dropped = runDrop(doomed, session);
      if (const ojson* e = firstError(dropped)) return *e;
    }
  }
  for (const auto& nm : {spellVar, seqVar, endVar}) {
    ojson guard = newVarGuard(session, "tsspell", nm);
    if (!guard.is_null()) return guard;
  }
  if (spellVar == seqVar || spellVar == endVar || seqVar == endVar)
    return utilSyntax("tsspell: spell(), seq() and end() must name three different variables.");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);
  if (byVars.empty() && session.panelvar.has_value() && !session.panelvar->empty())
    byVars.push_back(*session.panelvar);

  // The cond()/fcond() forms need a row-wise truth value of an arbitrary expression.
  // PIET has exactly one expression evaluator and it lives behind `generate`;
  // this materializes the indicator through it and drops it afterwards, so
  // there is no second evaluator to drift.
  const bool condForm = cond.has_value();
  const bool fcondExpr = !condForm && fcond.has_value();
  std::string indicator;
  if (condForm || fcondExpr) {
    if (auto err = materializeIndicator(session, "tsspell", condForm ? *cond : *fcond, &indicator))
      return *err;
  }
  // The indicator column is scaffolding, never data. It has to be gone BEFORE
  // `mutationResult` reads the frame, or the varlist this command reports would
  // name a temporary the user never asked for and cannot see -- so the drop is
  // a callable that the success path fires at exactly that point and the
  // failure paths fire on the way out. Idempotent: whichever runs first wins.
  bool indicatorLive = !indicator.empty();
  ojson indicatorDropError;
  auto dropIndicator = [&]() {
    if (!indicatorLive) return;
    indicatorLive = false;
    ojson dropped = runDrop({indicator}, session);
    if (const ojson* e = firstError(dropped)) indicatorDropError = *e;
  };

  ojson result;
  {
    std::vector<std::string> readCols = {*session.timevar};
    const bool byValue = !condForm && !fcondExpr;
    if (byValue) readCols.push_back(parsed.varlist[0]);
    if (!indicator.empty()) readCols.push_back(indicator);
    for (const auto& b : byVars) readCols.push_back(b);

    ReadScope scope(session, parsed, readCols, "tsspell", false);
    if (scope.failed()) {
      result = scope.error().is_null() ? utilError("tsspell: could not read the data", "data_io")
                                       : scope.error();
    } else {
      Datastore& ds = *scope.get();
      std::string missingName;
      for (const auto& nm : readCols) {
        if (!ds.has(nm)) missingName = nm;
      }
      if (!missingName.empty()) {
        result = utilError("variable " + missingName + " not found");
      } else if (ds.dtype(*session.timevar) == "factor") {
        result = utilError("tsspell: the time variable `" + *session.timevar +
                           "' is a string variable; tsspell needs a numeric time index.");
      } else {
        const int64_t n = ds.nRows();
        const double* tv = ds.col(*session.timevar).f64();
        const double delta = session.delta.value_or(1.0);
        int64_t nGroups = 0;
        const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

        ColView valueView;
        const double* ind = nullptr;
        if (byValue)
          valueView = viewOf(ds, parsed.varlist[0]);
        else
          ind = ds.col(indicator).f64();
        // A missing indicator is non-zero, hence true, in the ado's `cond & touse`.
        auto indTrue = [&](int64_t r) { return std::isnan(ind[r]) || ind[r] != 0.0; };

        std::vector<std::vector<int64_t>> order(
            static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
        for (int64_t i = 0; i < n; ++i)
          order[static_cast<size_t>(gid[static_cast<size_t>(i)])].push_back(i);
        for (auto& g : order) {
          std::stable_sort(g.begin(), g.end(),
                           [&](int64_t a, int64_t b) { return tv[a] < tv[b]; });
        }

        std::vector<double> spell(static_cast<size_t>(n), 0.0);
        std::vector<double> seq(static_cast<size_t>(n), 0.0);
        std::vector<double> endf(static_cast<size_t>(n), 0.0);
        int64_t gaps = 0;  // the ado's r(N): adjacent rows whose time step is not delta

        for (const auto& g : order) {
          const size_t m = g.size();
          for (size_t j = 1; j < m; ++j) {
            const int64_t r = g[j], pr = g[j - 1];
            const double dt = tv[r] - tv[pr];
            if (!std::isnan(tv[r]) && dt != delta && (!byVars.empty() || inSample[static_cast<size_t>(r)]))
              ++gaps;
          }
          if (condForm) {
            double counter = 0.0;
            for (size_t j = 0; j < m; ++j) {
              const int64_t r = g[j];
              const bool c = indTrue(r) && inSample[static_cast<size_t>(r)];
              double sq = c ? 1.0 : 0.0;
              if (c && j > 0) sq = seq[static_cast<size_t>(g[j - 1])] + 1.0;
              seq[static_cast<size_t>(r)] = sq;
              if (sq != 0.0) {
                if (inSample[static_cast<size_t>(r)] && sq == 1.0) counter += 1.0;
                spell[static_cast<size_t>(r)] = counter;
              }
            }
          } else {
            double counter = 0.0;
            for (size_t j = 0; j < m; ++j) {
              const int64_t r = g[j];
              bool begins;
              if (fcondExpr) {
                begins = indTrue(r);  // an explicit fcond() is used as given
              } else {
                begins = j == 0;
                if (!begins) {
                  const int64_t pr = g[j - 1];
                  const double dt = tv[r] - tv[pr];
                  // L.v is the value one step back in time: missing across a gap.
                  if (std::isnan(dt) || dt != delta)
                    begins = !cellMissing(valueView, r);
                  else
                    begins = cellKey(valueView, r) != cellKey(valueView, pr);
                }
              }
              if (inSample[static_cast<size_t>(r)]) {
                if (begins) counter += 1.0;
                spell[static_cast<size_t>(r)] = counter;
              }
            }
            std::map<double, double> within;
            for (size_t j = 0; j < m; ++j) {
              const int64_t r = g[j];
              const double sp = spell[static_cast<size_t>(r)];
              double& pos = within[sp];
              pos += 1.0;
              seq[static_cast<size_t>(r)] = sp > 0.0 ? pos : 0.0;
            }
          }
          for (size_t j = 0; j < m; ++j) {
            const int64_t r = g[j];
            const double sq = seq[static_cast<size_t>(r)];
            endf[static_cast<size_t>(r)] =
                (j + 1 < m ? (sq >= seq[static_cast<size_t>(g[j + 1])] && sq != 0.0) : sq > 0.0)
                    ? 1.0
                    : 0.0;
          }
        }

        auto cellsOf = [&](const std::vector<double>& v) {
          std::vector<std::string> out;
          out.reserve(static_cast<size_t>(n));
          for (int64_t i = 0; i < n; ++i) out.push_back(numCell(v[static_cast<size_t>(i)]));
          return out;
        };

        std::string cur = *session.currentHbin();
        bool failed = false;
        for (const auto& [nm, vals] :
             std::vector<std::pair<std::string, const std::vector<double>*>>{
                 {spellVar, &spell}, {seqVar, &seq}, {endVar, &endf}}) {
          std::string out;
          if (auto err = writeColumn(session, cur, nm, cellsOf(*vals), "f64", true, &out)) {
            result = *err;
            failed = true;
            break;
          }
          cur = out;
        }
        if (!failed) {
          session.load(cur);
          dropIndicator();
          result = mutationResult(session, "gen", {spellVar, seqVar, endVar});
          eval::replaceRClass(session.eval);
          session.eval.rValues.set("N", static_cast<double>(gaps));
          eval::declareRPosted(session.eval, "tsspell", {"N"});
          if (gaps > 0) {
            ojson warn;
            warn["type"] = "display";
            warn["text"] = "warning: data contain gaps; see help on tsspell\n";
            result = ojson::array({warn, result});
          }
        }
      }
    }
  }

  // Every path that did not reach the success branch above still owes the drop.
  // A drop failure is reported only when nothing worse already has.
  dropIndicator();
  if (!indicatorDropError.is_null() && !isErrorBlock(result)) return indicatorDropError;
  return result;
}


}  // namespace piet::handlers
