// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Stata's e(b) column order and names (factor base levels, o. columns) and the
// eb_stripe annotation of the live estimation block.

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
#include "estimation/statistics_command_support.h"
#include "rtable_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using statcmd_detail::ColumnScope;
using statcmd_detail::numericColumn;
using namespace rtable_detail;

namespace rtable_detail {

double rtNan() { return std::numeric_limits<double>::quiet_NaN(); }

double rowNum(const ojson& row, const char* key) {
  if (!row.contains(key) || !row.at(key).is_number()) return rtNan();
  return row.at(key).get<double>();
}

}  // namespace rtable_detail

namespace {

// Levels each factor variable takes over the rows the fit could have used (every
// model column that is a dataset column non-missing). A superset of the sample at
// worst — which only ever turns a post into a (safe) non-post, never into a wrong
// base, because a superset can only ADD levels without design rows.
bool sampleFactorLevels(SessionState& session, const LastModel& lm,
                        const std::vector<std::string>& fvars,
                        std::map<std::string, std::vector<double>>& out,
                        const std::vector<std::string>& maskVars = {}) {
  std::vector<std::string> cols;
  // Plain column names only: a `L.x`/`i.c`/`a#b` token is not a dataset column.
  auto want = [&cols](const std::string& c) {
    if (c.empty() || c.find_first_of(".#()") != std::string::npos) return;
    if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
  };
  want(lm.y);
  for (const auto& x : lm.x) want(x);
  for (const auto& v : fvars) want(v);
  for (const auto& v : maskVars) want(v);  // an interaction's continuous atoms
  for (const auto& f : lm.fe) want(f);

  std::unique_ptr<Datastore> owned;
  const Datastore* ds = nullptr;
  std::optional<ColumnScope> scope;
  try {
    if (!lm.fitDataPath.empty()) {
      owned = std::make_unique<Datastore>(lm.fitDataPath);
      ds = owned.get();
    } else {
      eval::ParsedCommand none;
      scope.emplace(session, none, cols, "r(table)");
      if (scope->failed()) return false;
      ds = scope->get();
    }
  } catch (const std::exception&) {
    return false;
  }
  if (ds == nullptr) return false;
  const size_t n = ds->nRows();
  std::vector<char> keep(n, 1);
  std::string err;
  std::vector<double> buf;
  for (const auto& c : cols) {
    if (!ds->has(c)) continue;
    if (!numericColumn(*ds, c, &buf, &err)) continue;
    for (size_t i = 0; i < n; ++i)
      if (std::isnan(buf[i])) keep[i] = 0;
  }
  for (const auto& v : fvars) {
    if (!ds->has(v) || !numericColumn(*ds, v, &buf, &err)) return false;
    std::vector<double> lv;
    for (size_t i = 0; i < n; ++i)
      if (keep[i]) lv.push_back(buf[i]);
    std::sort(lv.begin(), lv.end());
    lv.erase(std::unique(lv.begin(), lv.end()), lv.end());
    out[v] = std::move(lv);
  }
  return true;
}

// An omitted continuous regressor is reproducible as Stata's `o.x` column only when
// the omission cannot depend on drop order: the variable is constant within every
// absorbed group (constant overall when nothing is absorbed), so Stata's rmcoll
// drops it whatever else is in the model. A collinear PAIR (z = 2x) is not: which
// member goes depends on the command (reghdfe/areg/xtreg,fe keep x and omit z,
// regress the reverse; StataBE scratch-r150/rtab/r1.do, BUG-1067), and that case
// keeps the named refusal here. The rows are the
// superset sampleFactorLevels uses, which only errs toward refusing.
bool omittedTermIsUnambiguous(SessionState& session, const LastModel& lm, const std::string& term) {
  if (term.empty() || term.find_first_of(".#()") != std::string::npos) return false;
  std::vector<std::string> cols;
  auto want = [&cols](const std::string& c) {
    if (c.empty() || c.find_first_of(".#()") != std::string::npos) return;
    if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
  };
  want(lm.y);
  for (const auto& x : lm.x) want(x);
  for (const auto& f : lm.fe) want(f);
  want(term);
  std::unique_ptr<Datastore> owned;
  const Datastore* ds = nullptr;
  std::optional<ColumnScope> scope;
  try {
    if (!lm.fitDataPath.empty()) {
      owned = std::make_unique<Datastore>(lm.fitDataPath);
      ds = owned.get();
    } else {
      eval::ParsedCommand none;
      scope.emplace(session, none, cols, "r(table)");
      if (scope->failed()) return false;
      ds = scope->get();
    }
  } catch (const std::exception&) {
    return false;
  }
  if (ds == nullptr || !ds->has(term)) return false;
  const size_t n = ds->nRows();
  std::vector<char> keep(n, 1);
  std::string err;
  std::vector<double> buf;
  for (const auto& c : cols) {
    if (!ds->has(c) || !numericColumn(*ds, c, &buf, &err)) continue;
    for (size_t i = 0; i < n; ++i)
      if (std::isnan(buf[i])) keep[i] = 0;
  }
  std::vector<double> xv;
  if (!numericColumn(*ds, term, &xv, &err)) return false;
  std::vector<std::vector<double>> fe;
  for (const auto& f : lm.fe) {
    if (f.find_first_of(".#()") != std::string::npos || !ds->has(f)) return false;
    fe.emplace_back();
    if (!numericColumn(*ds, f, &fe.back(), &err)) return false;
  }
  std::map<std::vector<double>, double> firstByGroup;
  bool any = false;
  for (size_t i = 0; i < n; ++i) {
    if (!keep[i]) continue;
    any = true;
    std::vector<double> key;
    key.reserve(fe.size());
    for (const auto& col : fe) key.push_back(col[i]);
    auto [it, inserted] = firstByGroup.emplace(std::move(key), xv[i]);
    if (!inserted && it->second != xv[i]) return false;
  }
  return any;
}

}  // namespace

namespace rtable_detail {

// Stata's spelling of an omitted column: `o.x` for a plain variable, and inside a
// factor-variable name an `o` on every atom off its base level: `2o.g`,
// `0b.a#1o.b`, `0b.g#co.x` (StataBE 19.5 `regress y i.a##i.b`, `c.x##i.g`).
std::string omittedColumnName(const std::string& name) {
  if (name.find('.') == std::string::npos) return "o." + name;
  static const std::regex atom(R"((^|#)([0-9]+|c)\.)");
  return std::regex_replace(name, atom, "$1$2o.");
}

}  // namespace rtable_detail

namespace {

// One atom of a factor-variable interaction: `i.g`, `ib2.g`, `ib(first).g`,
// `ib(last).g`, a bare `g` (inside an interaction Stata and the engine read it as
// `i.g`: StataBE `regress y a##g` names `0b.a ... 2.a#1.g`) -- a factor, with
// the rule for its base level -- or `c.x` (continuous). False = a spelling these
// columns do not reproduce: `ibn.`, `ib(freq).`, `i2.`, a time-series operator.
struct FvAtom {
  std::string spelling;  // as typed
  std::string var;
  bool factor = false;
  int baseRule = 0;  // 0 the lowest level, 1 `ib#.`'s level, 2 the highest level, 3 none (`ibn.`)
  double base = 0;
};

bool parseFvAtom(const std::string& s, FvAtom& a) {
  a.spelling = s;
  const size_t dot = s.find('.');
  if (dot == std::string::npos) {
    if (s.empty() || s.find_first_of("#()") != std::string::npos) return false;
    a.var = s;
    a.factor = true;
    return true;
  }
  if (dot == 0 || dot + 1 >= s.size()) return false;
  const std::string op = s.substr(0, dot);
  a.var = s.substr(dot + 1);
  if (a.var.find_first_of(".#()") != std::string::npos) return false;
  if (op == "c") return true;
  a.factor = true;
  if (op == "i" || op == "ib(first)") return true;
  if (op == "ib(last)") {
    a.baseRule = 2;
    return true;
  }
  if (op == "ibn") {
    a.baseRule = 3;
    return true;
  }
  if (op.size() < 3 || op.compare(0, 2, "ib") != 0) return false;
  for (size_t i = 2; i < op.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(op[i]))) return false;
  a.baseRule = 1;
  a.base = std::strtod(op.c_str() + 2, nullptr);
  return true;
}

// `A##B##C` is the factorial: every sub-interaction, in fvexpand's order A, B,
// A#B, C, A#C, B#C, A#B#C (StataBE `regress y i.a##c.x##i.g`); `A#B#C` is the
// one interaction. A term mixing `#` and `##` is refused.
bool expandFvTerm(const std::string& term, std::vector<std::vector<FvAtom>>& subs) {
  const bool factorial = term.find("##") != std::string::npos;
  std::vector<FvAtom> atoms;
  size_t start = 0;
  for (;;) {
    const size_t h = term.find('#', start);
    FvAtom a;
    if (!parseFvAtom(term.substr(start, h == std::string::npos ? std::string::npos : h - start), a))
      return false;
    atoms.push_back(std::move(a));
    if (h == std::string::npos) break;
    if (factorial) {
      if (h + 1 >= term.size() || term[h + 1] != '#') return false;
      start = h + 2;
    } else {
      start = h + 1;
    }
  }
  if (atoms.size() < 2 || atoms.size() > 8) return false;
  if (!factorial) {
    subs.push_back(std::move(atoms));
    return true;
  }
  // A repeated subterm appears once (`c.x##c.x` is `x c.x#c.x`, StataBE).
  std::set<std::vector<std::string>> seen;
  for (unsigned mask = 1; mask < (1u << atoms.size()); ++mask) {
    std::vector<FvAtom> s;
    std::vector<std::string> id;
    for (size_t i = 0; i < atoms.size(); ++i)
      if (mask & (1u << i)) {
        s.push_back(atoms[i]);
        id.push_back(atoms[i].spelling);
      }
    if (seen.insert(id).second) subs.push_back(std::move(s));
  }
  return true;
}

}  // namespace

namespace rtable_detail {

// Map the design rows onto Stata's e(b) column order. False = cannot reproduce.
bool stataColumns(SessionState& session, const LastModel& lm, const ojson& ct,
                  std::vector<RtCol>& cols) {
  std::vector<bool> used(ct.size(), false);
  int icpt = -1;
  auto rowName = [&ct](size_t i) { return ct[i].value("name", std::string()); };
  for (size_t i = 0; i < ct.size(); ++i) {
    const std::string nm = rowName(i);
    if (nm.find(':') != std::string::npos || ct[i].contains("eq") || ct[i].contains("equation"))
      return false;
    if (nm == "(intercept)" || nm == "_cons") icpt = static_cast<int>(i);
  }
  // A plain term (a variable, a factor's main effect) or an interaction's atoms.
  struct Item {
    std::string term;
    std::vector<FvAtom> atoms;
    bool repeat = false;  // a continuous term already in the varlist
  };
  std::vector<Item> items;
  std::vector<std::string> fvars, cvars;
  // A term's identity: its atoms as `i.v`/`c.v`, sorted (`a#g` is `g#a`).
  const auto termKey = [](const std::vector<FvAtom>& s) {
    std::vector<std::string> id;
    for (const auto& a : s) id.push_back((a.factor ? "i." : "c.") + a.var);
    std::sort(id.begin(), id.end());
    std::string key;
    for (const auto& p : id) key += p + "#";
    return key;
  };
  std::set<std::string> modelKeys;
  // A factor-variable term repeated in the varlist is one term, at its first
  // position (StataBE `i.g i.g` is `0b.g 1.g`; `a##g b##g` has one i.g); a
  // repeated continuous term is not (`x x` is `x o.x`).
  const auto first = [&](const std::vector<FvAtom>& s) {
    const bool anyFactor =
        std::any_of(s.begin(), s.end(), [](const FvAtom& a) { return a.factor; });
    return modelKeys.insert(termKey(s)).second || !anyFactor;
  };
  // A regressor typed with the omit operator (`o.x`): the fit leaves it out and
  // Stata keeps its column, `o.x`, at its place in the varlist (bank 66ec62b7
  // `regress price mpg o.weight`: e(b) `mpg o.weight _cons`, a 3 x 3 e(V) with
  // zeros). factor_vars_of reads `o.` as a factor operator; this is a variable.
  const auto plainOmit = [](const std::string& t) {
    return t.size() > 2 && t.compare(0, 2, "o.") == 0 && t.find_first_of(".#", 2) == std::string::npos;
  };
  for (const auto& term : lm.x) {
    if (plainOmit(term)) {
      items.push_back({term, {}});
      continue;
    }
    if (term.find('#') == std::string::npos) {
      FvAtom fa;
      // `c.x` alone is the column `x`
      const bool cont = term.compare(0, 2, "c.") == 0 && term.find('.', 2) == std::string::npos;
      const std::string col = cont ? term.substr(2) : term;
      const std::vector<std::string> fv = cecon::postest::factor_vars_of({term});
      // (`ibn.v` alone keeps its own path below: the noBase naming, not deduplicated)
      if (term.find('.') != std::string::npos && parseFvAtom(term, fa) && fa.factor &&
          fa.baseRule != 3) {
        if (!first({fa})) continue;
        items.push_back({col, {}});
      } else {
        bool seen = false;
        if (fv.empty()) {
          fa.var = col;
          fa.factor = false;
          seen = modelKeys.count(termKey({fa})) != 0;
          first({fa});
        }
        items.push_back({col, {}, seen});
      }
      for (const auto& v : fv) fvars.push_back(v);
      continue;
    }
    std::vector<std::vector<FvAtom>> subs;
    if (!expandFvTerm(term, subs)) return false;
    for (auto& s : subs) {
      // A factor repeated inside one interaction is that factor once
      // (`i.foreign#i.foreign` is `i.foreign`; a continuous `c.x#c.x` stays x^2).
      for (size_t a = 0; a < s.size(); ++a)
        for (size_t b = a + 1; b < s.size();)
          if (s[a].factor && s[b].factor && s[a].var == s[b].var) s.erase(s.begin() + static_cast<long>(b));
          else ++b;
      if (s.size() == 1 && !s[0].factor) {
        // a factorial's continuous main effect
        const bool seen = modelKeys.count(termKey(s)) != 0;
        first(s);
        cvars.push_back(s[0].var);
        items.push_back({s[0].var, {}, seen});
        continue;
      }
      if (!first(s)) continue;
      for (const auto& a : s) (a.factor ? fvars : cvars).push_back(a.var);
      if (s.size() == 1)
        items.push_back(
            {s[0].spelling.find('.') == std::string::npos ? "i." + s[0].var : s[0].spelling, {}});
      else
        items.push_back({std::string(), std::move(s)});
    }
  }
  std::map<std::string, std::vector<double>> levels;
  if (!fvars.empty() && !sampleFactorLevels(session, lm, fvars, levels, cvars)) return false;

  for (const auto& item : items) {
    if (!item.atoms.empty()) {
      // The interaction's cells in Stata's order (the first factor slowest), each
      // named factor atoms first, then the continuous ones (`1.g#c.x` for
      // `c.x#i.g`); the engine's row is the cell's atoms in any order
      // (interaction_key: it names that cell `x#1.g`).
      std::vector<const FvAtom*> fac, con;
      for (const auto& a : item.atoms) (a.factor ? fac : con).push_back(&a);
      std::vector<const std::vector<double>*> lv;
      std::vector<double> base;
      // `ibn.g` interacted with continuous variables only: every level is a cell and
      // none is a base; Stata marks the first level `bn` (bank 0d53e090 `regress price
      // ibn.foreign#c.mpg, noconstant`: `0bn.foreign#c.mpg 1.foreign#c.mpg`). With
      // another factor in the cell the spelling is not measured: no stripe.
      const bool noBaseCell = fac.size() == 1 && fac[0]->baseRule == 3 && !con.empty();
      for (const FvAtom* a : fac)
        if (a->baseRule == 3 && !noBaseCell) return false;
      for (const FvAtom* a : fac) {
        const auto f = levels.find(a->var);
        if (f == levels.end() || f->second.empty()) return false;
        for (double v : f->second)
          if (v < 0 || v != std::floor(v)) return false;  // not a legal factor level
        if (a->baseRule == 3) {
          lv.push_back(&f->second);
          base.push_back(std::numeric_limits<double>::quiet_NaN());
          continue;
        }
        const double b = a->baseRule == 0 ? f->second.front()
                         : a->baseRule == 2 ? f->second.back()
                                            : a->base;
        if (!std::binary_search(f->second.begin(), f->second.end(), b)) return false;
        lv.push_back(&f->second);
        base.push_back(b);
      }
      std::vector<size_t> pos(fac.size(), 0);
      for (bool more = true; more;) {
        std::string name, engine;
        std::vector<FvAtom> rest;  // the cell's atoms off their base level
        bool anyBase = false;
        for (size_t k = 0; k < fac.size(); ++k) {
          const double v = (*lv[k])[pos[k]];
          std::string at = cecon::postest::level_name(v, fac[k]->var);
          engine += (engine.empty() ? "" : "#") + at;
          if (v == base[k]) {
            at.insert(at.find('.'), "b");
            anyBase = true;
          } else if (noBaseCell && pos[k] == 0) {
            at.insert(at.find('.'), "bn");
            rest.push_back(*fac[k]);
          } else {
            rest.push_back(*fac[k]);
          }
          name += (name.empty() ? "" : "#") + at;
        }
        for (const FvAtom* a : con) {
          engine += (engine.empty() ? "" : "#") + a->var;
          name += (name.empty() ? "" : "#") + ("c." + a->var);
          rest.push_back(*a);
        }
        const std::string key = cecon::postest::interaction_key(engine);
        const ojson* row = nullptr;
        for (size_t i = 0; i < ct.size(); ++i) {
          if (used[i] || cecon::postest::interaction_key(rowName(i)) != key) continue;
          row = &ct[i];
          used[i] = true;
          break;
        }
        // Stata's fvexpand: a cell with an atom at its base level is the
        // interaction's base when nothing else is left (`0b.a#0b.b`), omitted when
        // the term left without those atoms is in the model, wherever it stands
        // (`c.x##i.g` and `c.x#i.g c.x` both omit `0b.g#co.x`; `i.a i.a#i.g` omits
        // `1o.a#0b.g` and keeps `0b.a#1.g`: StataBE 19.5), and estimable
        // otherwise (`i.a#i.b` alone keeps `0b.a#1.b`). The engine must have built
        // the same design: a column it estimated that Stata omits is another
        // parameterization, and the stripe is refused.
        const bool isBase = anyBase && rest.empty();
        const bool isOmitted = anyBase && !rest.empty() && modelKeys.count(termKey(rest)) != 0;
        if ((isBase || isOmitted) && row != nullptr &&
            !(rowNum(*row, "b") == 0.0 &&
              (rowNum(*row, "se") == 0.0 || std::isnan(rowNum(*row, "se")))))
          return false;
        // A cell with no design column is omitted unless it is the base: an `o` on
        // each atom off its base (`0b.a#1o.b`, `0b.g#co.x`, an empty cell `2o.a#1o.b`).
        if (row == nullptr && !isBase) name = omittedColumnName(name);
        cols.push_back({name, row});
        more = false;
        for (size_t k = fac.size(); k-- > 0;) {
          if (++pos[k] < lv[k]->size()) {
            more = true;
            break;
          }
          pos[k] = 0;
        }
      }
      continue;
    }
    const std::string& term = item.term;
    if (plainOmit(term)) {
      cols.push_back({term, nullptr});
      continue;
    }
    const std::vector<std::string> fv = cecon::postest::factor_vars_of({term});
    if (fv.empty()) {
      bool found = false;
      for (size_t i = 0; i < ct.size(); ++i) {
        if (used[i] || rowName(i) != term) continue;
        used[i] = true;
        cols.push_back({term, &ct[i]});
        found = true;
        break;
      }
      // An engine that names the omitted regressor's row as Stata names the
      // column (qreg: `o.w2`, b 0, se 0; bank 2c169e60 `qreg price mpg weight w2`:
      // r(table) `mpg weight o.w2 _cons`).
      for (size_t i = 0; !found && i < ct.size(); ++i) {
        if (used[i] || rowName(i) != "o." + term) continue;
        if (!(rowNum(ct[i], "b") == 0.0 &&
              (rowNum(ct[i], "se") == 0.0 || std::isnan(rowNum(ct[i], "se")))))
          return false;
        used[i] = true;
        cols.push_back({"o." + term, &ct[i]});
        found = true;
      }
      if (!found) {
        // An omitted continuous regressor with no row: Stata's `o.x` column
        // (b 0, the rest missing; StataBE 007 FA3 `o.miss_age_PAP`), posted only
        // when the omission is order-independent (see omittedTermIsUnambiguous)
        // or the term repeats one earlier in the varlist (`x x` is `x o.x`).
        if (!item.repeat && !omittedTermIsUnambiguous(session, lm, term)) return false;
        cols.push_back({"o." + term, nullptr});
      }
      continue;
    }
    const std::string& v = fv.front();
    const bool noBase = term.compare(0, 4, "ibn.") == 0;
    std::map<double, size_t> rowsByLevel;
    std::map<size_t, std::string> selfCell;  // a `L.v#L.v` row: the column `L.v`
    for (size_t i = 0; i < ct.size(); ++i) {
      if (used[i]) continue;
      double lv = 0;
      std::string var;
      if (cecon::postest::parse_level_ref(rowName(i), lv, var) && var == v) {
        rowsByLevel[lv] = i;
        continue;
      }
      // A factor interacted with itself is the factor (Stata's fvexpand: `regress
      // price i.foreign#i.foreign` posts `0b.foreign 1.foreign _cons`, bank
      // 12299eb3); the engine names its cells `L.v#M.v`. The cell L == M is the
      // level's column; L != M cannot occur and must be an empty (zero) row.
      const std::string nm = rowName(i);
      if (nm.find('#') == std::string::npos) continue;
      bool same = true, allV = true;
      double l0 = 0;
      size_t start = 0;
      for (bool firstPart = true;; firstPart = false) {
        const size_t h = nm.find('#', start);
        double l1 = 0;
        std::string v1;
        if (!cecon::postest::parse_level_ref(nm.substr(start, h == std::string::npos ? std::string::npos : h - start), l1, v1) ||
            v1 != v) {
          allV = false;
          break;
        }
        if (firstPart) l0 = l1;
        else if (l1 != l0) same = false;
        if (h == std::string::npos) break;
        start = h + 1;
      }
      if (!allV) continue;
      if (same) {
        rowsByLevel[l0] = i;
        selfCell[i] = cecon::postest::level_name(l0, v);
      } else if (rowNum(ct[i], "b") == 0.0 &&
                 (rowNum(ct[i], "se") == 0.0 || std::isnan(rowNum(ct[i], "se")))) {
        used[i] = true;
      } else {
        return false;
      }
    }
    const auto facRowName = [&](size_t i) {
      const auto a = selfCell.find(i);
      return a != selfCell.end() ? a->second : rowName(i);
    };
    auto it = levels.find(v);
    if (it == levels.end()) return false;
    if (noBase) {
      // `ibn.v`: every level is a column and none is the base; beside the constant
      // the fit omits one of them (the engine's dropped level). Stata names the first
      // level `Lbn.v`, the omitted one `Lo.v`, the rest `L.v` (bank e5e0afb4 `regress
      // price ibn.rep78`: `1bn.rep78 2.rep78 3.rep78 4.rep78 5o.rep78 _cons`; 9075b99d
      // `ibn.foreign`: `0bn.foreign 1o.foreign _cons`).
      // Only the shape measured is named: the LAST level omitted beside the constant
      // (e5e0afb4, 9075b99d), or none (noconstant). An earlier level omitted (`regress
      // price r1 ibn.rep78`, r1 = 1.rep78) has no measured spelling (`1o.` or
      // `1bno.`?), so no stripe is posted and the readers keep their refusal.
      const std::vector<double>& all = it->second;
      for (double lv : all)
        if (lv < 0 || lv != std::floor(lv)) return false;
      const auto isOmitted = [&](double lv) {
        const auto r = rowsByLevel.find(lv);
        if (r == rowsByLevel.end()) return true;
        const ojson& row = ct[r->second];
        return rowNum(row, "b") == 0.0 &&
               (rowNum(row, "se") == 0.0 || std::isnan(rowNum(row, "se")));
      };
      for (size_t i = 0; i + 1 < all.size(); ++i)
        if (isOmitted(all[i])) return false;
      size_t k = 0;
      for (double lv : all) {
        std::string nm = cecon::postest::level_name(lv, v);
        const auto r = rowsByLevel.find(lv);
        const ojson* row = r == rowsByLevel.end() ? nullptr : &ct[r->second];
        if (r != rowsByLevel.end()) used[r->second] = true;
        const bool omitted = isOmitted(lv);
        if (omitted) nm.insert(nm.find('.'), "o");
        else if (k == 0) nm.insert(nm.find('.'), "bn");
        cols.push_back({nm, row});
        ++k;
      }
      continue;
    }
    {
      // `ib#.v` whose base level # has no observation in the sample: Stata keeps
      // the base as an empty column at its place among the levels, and every
      // observed level enters the fit, the one collinear with the constant omitted
      // (bank c3e8b865 `regress price ib7.rep78`: `1.rep78 ... 4.rep78 5o.rep78
      // 7b.rep78 _cons`; 0028355d `ib2.rep78 if rep78!=2`: `1.rep78 2b.rep78 3.rep78
      // 4.rep78 5o.rep78 _cons`).
      static const std::regex reIb(R"(^ib(\d+)\.)");
      std::smatch mb;
      if (std::regex_search(term, mb, reIb)) {
        const double declared = std::strtod(mb.str(1).c_str(), nullptr);
        std::vector<double> all = it->second;
        if (!std::binary_search(all.begin(), all.end(), declared)) {
          for (double lv : all)
            if (lv < 0 || lv != std::floor(lv)) return false;
          all.insert(std::upper_bound(all.begin(), all.end(), declared), declared);
          for (double lv : all) {
            std::string nm = cecon::postest::level_name(lv, v);
            if (lv == declared) {
              nm.insert(nm.find('.'), "b");
              cols.push_back({nm, nullptr});
              continue;
            }
            const auto r = rowsByLevel.find(lv);
            if (r != rowsByLevel.end()) {
              used[r->second] = true;
              cols.push_back({facRowName(r->second), &ct[r->second]});
            } else {
              nm.insert(nm.find('.'), "o");
              cols.push_back({nm, nullptr});
            }
          }
          continue;
        }
      }
    }
    std::vector<double> missing;
    for (double lv : it->second) {
      if (lv < 0 || lv != std::floor(lv)) return false;  // not a legal factor level
      if (!rowsByLevel.count(lv)) missing.push_back(lv);
    }
    if (missing.empty()) {
      // An engine that keeps the base level as a row of its own, named as Stata
      // names the column (qreg: `0b.foreign`, b 0, se 0; bank 02464d3e `qreg
      // price mpg i.foreign`: r(table) `mpg 0b.foreign 1.foreign _cons`): that
      // level is the base. Only one such row, holding zeros, qualifies.
      double baseLv = 0;
      int nBase = 0;
      for (const auto& [lv, i] : rowsByLevel) {
        const std::string nm = rowName(i);
        const size_t dot = nm.find('.');
        if (dot == std::string::npos || dot == 0 || nm[dot - 1] != 'b') continue;
        if (!(rowNum(ct[i], "b") == 0.0 &&
              (rowNum(ct[i], "se") == 0.0 || std::isnan(rowNum(ct[i], "se")))))
          return false;
        baseLv = lv;
        ++nBase;
      }
      if (nBase == 1) {
        used[rowsByLevel[baseLv]] = true;
        rowsByLevel.erase(baseLv);
        missing.push_back(baseLv);
      }
    }
    if (missing.size() != 1 || rowsByLevel.empty()) return false;
    std::vector<double> all = it->second;
    for (const auto& kv : rowsByLevel)
      if (!std::binary_search(all.begin(), all.end(), kv.first)) return false;
    for (double lv : all) {
      if (lv == missing.front()) {
        std::string nm = cecon::postest::level_name(lv, v);
        nm.insert(nm.find('.'), "b");
        cols.push_back({nm, nullptr});
      } else {
        const size_t i = rowsByLevel[lv];
        used[i] = true;
        cols.push_back({facRowName(i), &ct[i]});
      }
    }
  }
  for (size_t i = 0; i < ct.size(); ++i)
    if (!used[i] && static_cast<int>(i) != icpt) return false;  // a row no term explains
  if (icpt >= 0) cols.push_back({"_cons", &ct[static_cast<size_t>(icpt)]});
  return !cols.empty();
}

// eb-order T-E2 (ME08/C7/EB-BASE): record Stata's e(b) stripe on the live
// estimation block, once, at the fit -- the columns stataColumns places for
// r(table) (the same base-level rule: the one sample level without a design
// row), plus the `o.` spelling of a column the binary perfect-prediction screen
// dropped (the engine's reply lists it in `omitted`; Stata posts `x1 o.dz0
// _cons`, w2u_binary_ml_edges.log:215). Every e(b) reader takes it from here
// (eval::ebStripeOf / stataEbColumns / ebStripeCoef): `matrix X = e(b)`,
// `: colnames e(b)`, el()/colsof(), `_b[0b.g]`, e(V), parmest and regsave. A
// fit whose columns stataColumns cannot place (interactions, equations, an
// ambiguous base) records nothing and keeps the readers' named refusals.
void annotateEbStripe(SessionState& session) {
  if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object()) return;
  ojson& blk = *session.lastEstBlock;
  blk.erase("eb_stripe");
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value() || !blk.contains("coeftable") || !blk["coeftable"].is_array() ||
      blk["coeftable"].empty())
    return;
  std::vector<std::string> omitted;
  if ((lmOpt->cobraCmd == "logit" || lmOpt->cobraCmd == "probit") && lmOpt->result.has_value() &&
      lmOpt->result->is_object() && lmOpt->result->contains("omitted") &&
      (*lmOpt->result)["omitted"].is_array()) {
    for (const auto& o : (*lmOpt->result)["omitted"])
      if (o.is_string()) omitted.push_back(o.get<std::string>());
  }
  std::vector<RtCol> cols;
  try {
    if (!stataColumns(session, *lmOpt, blk["coeftable"], cols)) return;
  } catch (const std::exception&) {
    return;
  }
  bool special = false;
  ojson stripe = ojson::array();
  for (const auto& c : cols) {
    ojson e;
    if (c.row == nullptr) {
      e["name"] = c.name;
      e["src"] = "";
      special = true;
    } else {
      const bool zero = rowNum(*c.row, "b") == 0.0 &&
                        (rowNum(*c.row, "se") == 0.0 || std::isnan(rowNum(*c.row, "se")));
      const bool om = std::find(omitted.begin(), omitted.end(), c.name) != omitted.end() || zero;
      std::string name = om ? omittedColumnName(c.name) : c.name;
      e["name"] = name;
      std::string src = c.row->value("name", std::string());
      if (src == "(intercept)") src = "_cons";
      e["src"] = src;
      // a column Stata names otherwise than the engine (`1.g#c.x` for `x#1.g`)
      special = special || om || name != src;
    }
    stripe.push_back(e);
  }
  if (!special) {
    // Stata orders the columns otherwise than the engine (`i.a##c.x##i.g`: the
    // engine puts i.g before i.a#c.x): the readers' default is the engine's
    // order with `_cons` last.
    std::vector<std::string> engine, mine;
    for (const auto& r : blk["coeftable"]) {
      const std::string n = r.value("name", std::string());
      if (n != "(intercept)" && n != "_cons") engine.push_back(n);
    }
    if (engine.size() < blk["coeftable"].size()) engine.push_back("_cons");
    for (const auto& e : stripe) mine.push_back(e.value("src", std::string()));
    special = engine != mine;
  }
  if (special && blk.contains("beta_std")) {
    // e(beta) of a column the fit has no row for (a base level, an empty or an
    // interaction cell the stripe places): its b is 0, so its standardized
    // coefficient is 0 -- unless the column does not vary over e(sample), when
    // Stata's e(beta) is missing (bank 04d54ce9 `regress price i.foreign##i.rep78`:
    // the empty cells 1o.foreign#1b.rep78 and 1o.foreign#2o.rep78 are `.`, the base
    // and 0b.foreign#2o.rep78 0). The column is the product of its atoms over the
    // fit's rows: `L.v` (any b/o/bn marks) is v == L, `c.v`/`co.v` is v.
    const auto rows = est_detail::esampleRowsM4(session, *lmOpt);
    static const std::regex reFactor(R"(^(-?\d+(?:\.\d+)?)(?:bn|bo|ob|b|o|n)?\.([A-Za-z_][A-Za-z0-9_]*)$)");
    static const std::regex reCont(R"(^c?o?\.([A-Za-z_][A-Za-z0-9_]*)$)");
    if (rows.has_value() && !rows->empty()) {
      for (auto& e : stripe) {
        if (!e.value("src", std::string()).empty()) continue;
        const std::string nm = e.value("name", std::string());
        std::vector<std::pair<const double*, double>> atoms;  // (column, level) ; NaN level = value
        bool ok = true;
        size_t start = 0;
        while (ok) {
          const size_t hash = nm.find('#', start);
          const std::string atom = nm.substr(start, hash == std::string::npos ? std::string::npos : hash - start);
          std::smatch mm;
          if (std::regex_match(atom, mm, reFactor)) {
            const double* col = est_detail::f64ColumnM4(session, mm.str(2));
            ok = col != nullptr;
            atoms.emplace_back(col, std::strtod(mm.str(1).c_str(), nullptr));
          } else if (std::regex_match(atom, mm, reCont)) {
            const double* col = est_detail::f64ColumnM4(session, mm.str(1));
            ok = col != nullptr;
            atoms.emplace_back(col, std::numeric_limits<double>::quiet_NaN());
          } else {
            ok = false;
          }
          if (hash == std::string::npos) break;
          start = hash + 1;
        }
        if (!ok || atoms.empty()) continue;
        bool constant = true, first = true;
        double v0 = 0;
        for (const int64_t r : *rows) {
          double v = 1.0;
          for (const auto& [col, lvl] : atoms)
            v *= std::isnan(lvl) ? col[r] : (col[r] == lvl ? 1.0 : 0.0);
          if (first) {
            v0 = v;
            first = false;
          } else if (v != v0) {
            constant = false;
            break;
          }
        }
        if (constant) e["novar"] = true;
      }
    }
  }
  if (special) {
    // The engine's covariance names only the independent columns. Omitted
    // columns are exact zero rows/columns, so retain them on the same fitted
    // matrix before every e(V) consumer applies the stripe.
    if (blk.contains("e_V") && blk["e_V"].is_object()) {
      auto& v = blk["e_V"];
      if (v.contains("names") && v["names"].is_array() &&
          v.contains("matrix") && v["matrix"].is_array()) {
        for (const auto& c : cols) {
          if (!c.row || rowNum(*c.row, "b") != 0.0 || rowNum(*c.row, "se") != 0.0) continue;
          const std::string src = c.row->value("name", std::string());
          if (std::find(v["names"].begin(), v["names"].end(), ojson(src)) != v["names"].end()) continue;
          for (auto& row : v["matrix"]) row.push_back(0.0);
          v["names"].push_back(src);
          v["matrix"].push_back(std::vector<double>(v["names"].size(), 0.0));
        }
      }
    }
    // e(k) of logit/probit is the number of e(b) columns, base and omitted columns
    // included (bank 3d02bbfd `logit foreign i.rep78, asis`: k 6 against the 5
    // estimated; aebefa4c/a5efb737/308eb878: 7 against 6). The reply's `k` counted
    // the engine's columns (binary_estimation.cpp); cloglog's the same (bank 21336c67:
    // 7 against 6).
    if ((lmOpt->cobraCmd == "logit" || lmOpt->cobraCmd == "probit" || lmOpt->cobraCmd == "cloglog") &&
        lmOpt->result.has_value() &&
        lmOpt->result->is_object() && lmOpt->result->contains("k"))
      session.patchLastModelResult("k", static_cast<double>(stripe.size()));
    // nbreg's e(b) carries /lnalpha after the equation's columns (bank d93bc657
    // `nbreg fatal beertax i.year`: k 10 = 8 + base 1982 + /lnalpha); xtpoisson,fe
    // has the equation alone (bank 057cf53a `... i.year, fe`: k 9 with the base).
    if ((lmOpt->cobraCmd == "nbreg" || lmOpt->cobraCmd == "xtpois") && lmOpt->result.has_value() &&
        lmOpt->result->is_object() && lmOpt->result->contains("k"))
      session.patchLastModelResult(
          "k", static_cast<double>(stripe.size() + (lmOpt->cobraCmd == "nbreg" ? 1 : 0)));
    blk["eb_stripe"] = std::move(stripe);
  }
}

namespace {

// One `#`-atom of a coefficient-table column name, as the table note reads it.
struct NoteAtom {
  bool factor = false;  // `<level><flags>.var`
  double level = 0;
  std::string var;
  bool base = false;     // a `b` flag that is not `bn`
  bool omitted = false;  // an `o` flag (`2o.g`, `1bno.g`, `co.x`, `o.x`, `oL.x`)
  bool tsop = false;     // a time-series operator on a factor (`1L.g`): no sample check
};

bool parseNoteAtom(const std::string& s, NoteAtom& a) {
  const size_t dot = s.find('.');
  if (dot == std::string::npos) return true;  // a plain variable, `_cons`
  const std::string op = s.substr(0, dot);
  a.var = s.substr(dot + 1);
  size_t i = 0;
  while (i < op.size() && std::isdigit(static_cast<unsigned char>(op[i]))) ++i;
  const std::string flags = op.substr(i);
  if (i > 0) {
    a.factor = true;
    a.level = std::strtod(op.substr(0, i).c_str(), nullptr);
    for (size_t j = 0; j < flags.size(); ++j) {
      const char c = flags[j];
      if (c == 'b' && !(j + 1 < flags.size() && flags[j + 1] == 'n')) a.base = true;
      else if (c == 'o') a.omitted = true;
      else if (std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)))
        a.tsop = true;
    }
    if (a.var.find('.') != std::string::npos) a.tsop = true;
    return true;
  }
  a.omitted = flags.find('o') != std::string::npos;
  return true;
}

// The fit's sample when the engine did not record e(sample): the rows on which
// every model variable is non-missing -- accepted only when there are exactly
// e(N) of them and the fit ran without weights or an if/in restriction, so that
// they ARE the sample. nullopt otherwise (the caller then posts no note that
// needs the sample).
template <class ColumnFn>
std::optional<std::vector<int64_t>> completeCaseRows(SessionState& session, const LastModel& lm,
                                                     ColumnFn& column) {
  if (!lm.fitWeight.empty() || !lm.fitKeepif.empty() || !lm.fitDataPath.empty())
    return std::nullopt;
  if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object()) return std::nullopt;
  const ojson& blk = *session.lastEstBlock;
  if (!blk.contains("n_obs") || !blk["n_obs"].is_number()) return std::nullopt;
  const double nObs = blk["n_obs"].get<double>();
  std::vector<std::string> vars;
  bool ok = true;
  const auto add = [&](const std::string& v) {
    if (v.empty()) return;
    for (const char c : v)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
        ok = false;
        return;
      }
    if (std::find(vars.begin(), vars.end(), v) == vars.end()) vars.push_back(v);
  };
  add(lm.y);
  for (const auto& term : lm.x) {
    size_t start = 0;
    for (;;) {
      const size_t h = term.find('#', start);
      std::string atom = term.substr(start, h == std::string::npos ? std::string::npos : h - start);
      const size_t dot = atom.rfind('.');
      add(dot == std::string::npos ? atom : atom.substr(dot + 1));
      if (h == std::string::npos) break;
      start = h + 1;
      while (start < term.size() && term[start] == '#') ++start;
    }
  }
  for (const auto& f : lm.fe) add(f);
  for (const auto& c : lm.cluster) add(c);
  if (!ok || vars.empty()) return std::nullopt;
  std::vector<const std::vector<double>*> cols;
  for (const auto& v : vars) {
    const std::vector<double>* c = column(v);
    if (c == nullptr) return std::nullopt;
    cols.push_back(c);
  }
  std::vector<int64_t> rows;
  const size_t n = cols.front()->size();
  for (size_t i = 0; i < n; ++i) {
    bool keep = true;
    for (const auto* c : cols)
      if (i >= c->size() || std::isnan((*c)[i]) || (*c)[i] >= eval::kStataMissingSentinel) {
        keep = false;
        break;
      }
    if (keep) rows.push_back(static_cast<int64_t>(i));
  }
  if (static_cast<double>(rows.size()) != nObs) return std::nullopt;
  return rows;
}

}  // namespace

// r(label#) beside an estimation's r(table): Stata's coefficient table stores, as
// hidden r() macros, the note it prints in place of the statistics of column #
// (vs-stata truth bank, every estimation with a factor variable or an omitted
// column: `regress price mpg i.rep78 c.weight##c.weight` stores r(label2)
// `(base)`; `logit foreign mpg i.rep78` -- levels 1 and 2 leave the sample by
// perfect prediction -- stores label2/label3 `(empty)` and label6 `(omitted)`).
// The note of a column is decided in this order:
//   1. a factor-variable column flagged base or omitted whose cell has no
//      observation in e(sample) is `(empty)`;
//   2. otherwise any atom at its base level (`0b.a`, `1o.a#0b.b`, `0b.g#co.x`)
//      makes a column whose coefficient is 0 `(base)` (an estimated
//      `0b.foreign#c.mpg` has no note), and any omitted atom (`2o.g`, `o.x`,
//      `co.x`) `(omitted)`;
//   3. a column whose standard error is missing is `(omitted)` when its
//      coefficient is 0 and has no note yet (`2.race`, a dropped `mpg`), and loses
//      an `(omitted)` note when its coefficient is not 0.
// An omitted column of an ancillary `/name` equation reads `(constrained)`.
// Only these notes are posted; the ones that come from information this table
// does not carry (constraints on an estimated column, margins' error codes) are
// not, and a column whose emptiness cannot be read from the data posts nothing.
std::vector<std::optional<std::string>> coefficientNotes(SessionState& session,
                                                        const std::vector<std::string>& names,
                                                        const std::vector<double>& bv,
                                                        const std::vector<double>& sev,
                                                        const std::vector<bool>& eformv) {
  const size_t k = names.size();
  std::vector<std::optional<std::string>> notes(k);
  if (bv.size() != k || sev.size() != k || eformv.size() != k) return notes;
  bool rowsTried = false;
  std::optional<std::vector<int64_t>> rows;
  std::map<std::string, std::vector<double>> cache;
  std::map<std::string, bool> cacheOk;
  const auto column = [&](const std::string& v) -> const std::vector<double>* {
    const auto it = cacheOk.find(v);
    if (it != cacheOk.end()) return it->second ? &cache[v] : nullptr;
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    std::string err;
    std::vector<double> buf;
    const bool ok = ds != nullptr && ds->has(v) && numericColumn(*ds, v, &buf, &err);
    cacheOk[v] = ok;
    if (!ok) return nullptr;
    cache[v] = std::move(buf);
    return &cache[v];
  };
  for (size_t i = 0; i < k; ++i) {
    std::string full = names[i];
    const size_t colon = full.rfind(':');
    const std::string eq = colon == std::string::npos ? std::string() : full.substr(0, colon);
    const std::string name = colon == std::string::npos ? full : full.substr(colon + 1);
    if (name.find('@') != std::string::npos) continue;  // a margins-style name: unknown
    std::vector<NoteAtom> atoms;
    size_t start = 0;
    for (;;) {
      const size_t h = name.find('#', start);
      NoteAtom a;
      parseNoteAtom(name.substr(start, h == std::string::npos ? std::string::npos : h - start), a);
      atoms.push_back(std::move(a));
      if (h == std::string::npos) break;
      start = h + 1;
    }
    bool anyBase = false, anyOmit = false, anyFactor = false, anyTs = false;
    for (const auto& a : atoms) {
      anyBase = anyBase || a.base;
      anyOmit = anyOmit || a.omitted;
      anyFactor = anyFactor || a.factor;
      anyTs = anyTs || a.tsop;
    }
    const double b = bv[i];
    const double se = sev[i];
    const bool eform = eformv[i];
    const bool bZero = eform ? b == 1.0 : b == 0.0;
    const bool seMissing = std::isnan(se);
    std::string label;
    if ((anyBase || anyOmit) && anyFactor && bZero && seMissing) {
      if (anyTs) continue;
      if (!rowsTried) {
        rowsTried = true;
        const auto& lm = session.lastModel();
        if (lm.has_value()) rows = est_detail::esampleRowsM4(session, *lm);
        if (lm.has_value() && !rows.has_value()) rows = completeCaseRows(session, *lm, column);
      }
      if (!rows.has_value()) continue;
      std::vector<std::pair<const std::vector<double>*, double>> cell;
      bool ok = true;
      for (const auto& a : atoms) {
        if (!a.factor) continue;
        const std::vector<double>* c = column(a.var);
        if (c == nullptr) {
          ok = false;
          break;
        }
        cell.emplace_back(c, a.level);
      }
      if (!ok) continue;
      bool seen = false;
      for (const int64_t r : *rows) {
        bool all = true;
        for (const auto& [c, lv] : cell)
          if (static_cast<size_t>(r) >= c->size() || (*c)[static_cast<size_t>(r)] != lv) {
            all = false;
            break;
          }
        if (all) {
          seen = true;
          break;
        }
      }
      if (!seen) label = "(empty)";
    }
    // A `b` flag makes a base note only on a column held at 0: an interaction
    // with a base level of one factor and a continuous variable is estimated
    // (vs-stata bank f9a2c12a `regress price i.foreign#c.mpg i.rep78`:
    // `0b.foreign#c.mpg` b -329.86 is shown as a coefficient and carries no
    // r(label1); estat vce keeps its row and column).
    if (label.empty()) {
      if (anyBase && bZero) label = "(base)";
      else if (anyOmit) label = "(omitted)";
    }
    if (seMissing) {
      if (bZero) {
        if (label.empty()) label = "(omitted)";
      } else if (label == "(omitted)") {
        label.clear();
      }
    }
    if (label == "(omitted)" && !eq.empty() && eq[0] == '/') label = "(constrained)";
    notes[i] = label;
  }
  return notes;
}

void postCoefficientLabels(SessionState& session, const eval::EvalMatrix& tbl, bool baseOnly) {
  if (tbl.data.size() < 9) return;
  const size_t k = tbl.colnames.size();
  if (tbl.data[0].size() != k || tbl.data[1].size() != k || tbl.data[8].size() != k) return;
  std::vector<bool> eform;
  for (const double e : tbl.data[8]) eform.push_back(e == 1.0);
  const auto notes = coefficientNotes(session, tbl.colnames, tbl.data[0], tbl.data[1], eform);
  for (size_t i = 0; i < k; ++i) {
    if (!notes[i].has_value() || notes[i]->empty()) continue;
    if (baseOnly && *notes[i] != "(base)") continue;
    const std::string key = "label" + std::to_string(i + 1);
    session.eval.rStrings.set(key, *notes[i]);
    session.eval.markRMacroHidden(key);
  }
}

}  // namespace rtable_detail

}  // namespace piet::handlers
