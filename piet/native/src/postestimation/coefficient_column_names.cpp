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
  int baseRule = 0;  // 0 the lowest level, 1 `ib#.`'s level, 2 the highest level
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
  for (const auto& term : lm.x) {
    if (term.find('#') == std::string::npos) {
      FvAtom fa;
      // `c.x` alone is the column `x`
      const bool cont = term.compare(0, 2, "c.") == 0 && term.find('.', 2) == std::string::npos;
      const std::string col = cont ? term.substr(2) : term;
      const std::vector<std::string> fv = cecon::postest::factor_vars_of({term});
      if (term.find('.') != std::string::npos && parseFvAtom(term, fa) && fa.factor) {
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
      for (const FvAtom* a : fac) {
        const auto f = levels.find(a->var);
        if (f == levels.end() || f->second.empty()) return false;
        for (double v : f->second)
          if (v < 0 || v != std::floor(v)) return false;  // not a legal factor level
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
    if (term.compare(0, 4, "ibn.") == 0) return false;
    std::map<double, size_t> rowsByLevel;
    for (size_t i = 0; i < ct.size(); ++i) {
      if (used[i]) continue;
      double lv = 0;
      std::string var;
      if (cecon::postest::parse_level_ref(rowName(i), lv, var) && var == v) rowsByLevel[lv] = i;
    }
    auto it = levels.find(v);
    if (it == levels.end()) return false;
    std::vector<double> missing;
    for (double lv : it->second) {
      if (lv < 0 || lv != std::floor(lv)) return false;  // not a legal factor level
      if (!rowsByLevel.count(lv)) missing.push_back(lv);
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
        cols.push_back({rowName(i), &ct[i]});
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
    blk["eb_stripe"] = std::move(stripe);
  }
}

}  // namespace rtable_detail

}  // namespace piet::handlers
