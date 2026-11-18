// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_combine.h"
#include "piet/handlers/variables.h"
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "estimation/bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

// ===========================================================================
// stack
// ===========================================================================

ojson runStack(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabStack.known,
                                            kVocabStack.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("stack: syntax is `stack varlist, into(newvars) [clear wide group(#)]`");
  // The varlist may carry wildcards and ranges; the stack runs over their expansion.
  std::vector<std::string> varlist = parsed.varlist;
  if (auto err = resolveByKeyNames(session, varlist)) return *err;
  std::vector<char> inSample;
  if (auto err = ifInMask(session, parsed, &inSample)) return *err;
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("stack: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  // The varlist's names are looked up before its length is counted.
  for (const auto& v : varlist)
    if (!ds->has(v)) return rsError("variable " + v + " not found");
  // stack.ado: `varlist(min=2)` -- one variable is r(102) "too few variables
  // specified" (vs-stata bank 6ffa835d02765c98 `stack price, into(x) clear`);
  // into() and group() together are r(198) (ee30d36ac8ca5199), and so is
  // neither of them (`error 198`).
  if (varlist.size() < 2)
    return ojson(eval::errorBlock("engine_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, "r(102)"));
  {
    const bool hasInto = optStr(parsed.opts, "into").has_value();
    const bool hasGroup = optStr(parsed.opts, "group").has_value();
    if (hasInto && hasGroup)
      return ojson(eval::errorBlock("syntax_error",
                                    "options group() and into() may not be specified together",
                                    std::nullopt, std::nullopt, std::nullopt, "r(198)"));
    if (!hasInto && !hasGroup)
      return ojson(eval::errorBlock("syntax_error", "invalid syntax", std::nullopt, std::nullopt,
                                    std::nullopt, "r(198)"));
  }
  const bool wide = hasFlag(parsed.opts, "wide");

  const size_t nv = varlist.size();
  // [D] stack: into(newvars) names one block's variables, so without group()
  // the varlist is cut into blocks of as many variables as into() names
  // (`stack x1 y x2 y, into(a b) clear` stacks (x1,y) over (x2,y);
  // w2_esample_copies.do:281, StataBE `0|0|122|`, .log:702). Only with
  // neither option is every variable its own block.
  std::optional<std::string> intoOpt = optStr(parsed.opts, "into");
  long long groups = static_cast<long long>(nv);
  if (auto g = optStr(parsed.opts, "group")) {
    if (!parseStrictInt(*g, &groups) || groups < 1)
      return rsSyntax("stack: group() requires a positive whole number, got `" + *g + "'");
  } else if (intoOpt.has_value()) {
    const size_t k = splitWs(*intoOpt).size();
    if (k == 0 || nv % k != 0)
      return rsSyntax("stack: " + std::to_string(nv) + " variable(s) do not divide into blocks of " +
                      std::to_string(k) + ", the number of into() names");
    groups = static_cast<long long>(nv / k);
  }
  if (static_cast<size_t>(groups) > nv || nv % static_cast<size_t>(groups) != 0)
    return rsSyntax("stack: " + std::to_string(nv) + " variable(s) do not divide into " +
                    std::to_string(groups) + " equal group(s)");
  const size_t per = nv / static_cast<size_t>(groups);

  std::vector<std::string> intoNames;
  if (intoOpt.has_value()) {
    intoNames = splitWs(*intoOpt);
  } else {
    // group(#) without into(): the new variables take the first group's names (the ado's
    // Makeinto), also under `wide`, where the first group's variables are then the
    // results themselves and only the later groups' variables are added.
    intoNames.assign(varlist.begin(), varlist.begin() + static_cast<long>(per));
  }
  if (intoNames.size() != per)
    return rsSyntax("stack: into() names " + std::to_string(intoNames.size()) +
                    " variable(s) but each group has " + std::to_string(per));
  for (const auto& nm : intoNames)
    if (!strutil::isValidStataIdentifier(nm))
      return rsSyntax("stack: invalid variable name `" + nm + "' — " +
                      strutil::stataIdentifierRuleText());

  const int64_t nAll = ds->nRows();
  std::vector<int64_t> useRows;
  for (int64_t i = 0; i < nAll; ++i)
    if (inSample[static_cast<size_t>(i)]) useRows.push_back(i);
  const int64_t n = static_cast<int64_t>(useRows.size());
  const std::vector<std::string> before = ds->names();

  // Column texts, read once. A stacked column is a factor if ANY of the source
  // columns feeding that position is one: mixing a string into a numeric column
  // has to widen, never truncate.
  std::map<std::string, std::vector<std::string>> textOf;
  for (const auto& nm : varlist)
    if (!textOf.count(nm)) textOf[nm] = columnAsStrings(*ds, nm);

  CsvTable t;
  t.names.push_back("_stack");
  t.isFactor.push_back(0);
  for (size_t c = 0; c < per; ++c) {
    bool anyFactor = false;
    for (long long g = 0; g < groups; ++g)
      if (ds->dtype(varlist[static_cast<size_t>(g) * per + c]) == "factor") anyFactor = true;
    t.names.push_back(intoNames[c]);
    t.isFactor.push_back(anyFactor ? 1 : 0);
  }
  // stack.ado's `wide`: each varlist variable not already a result variable
  // (`capture confirm new variable`) is generated from its own block's into()
  // column, on that block's rows only (missing on the others). The other
  // variables of the dataset are gone, as without `wide` (vs-stata bank: `stack
  // price mpg weight length, into(a b) wide clear` -> _stack a b price mpg weight
  // length, k 7).
  struct WideCol {
    std::string name;
    long long group;
    size_t pos;
  };
  std::vector<WideCol> wideCols;
  if (wide) {
    for (long long g = 0; g < groups; ++g) {
      for (size_t c = 0; c < per; ++c) {
        const std::string& nm = varlist[static_cast<size_t>(g) * per + c];
        if (std::find(t.names.begin(), t.names.end(), nm) != t.names.end()) continue;
        t.names.push_back(nm);
        t.isFactor.push_back(t.isFactor[1 + c]);
        wideCols.push_back(WideCol{nm, g, c});
      }
    }
  }
  t.rows.reserve(static_cast<size_t>(n) * static_cast<size_t>(groups));
  for (long long g = 0; g < groups; ++g) {
    for (int64_t ui = 0; ui < n; ++ui) {
      const size_t i = static_cast<size_t>(useRows[static_cast<size_t>(ui)]);
      std::vector<std::string> row;
      row.reserve(t.names.size());
      row.push_back(std::to_string(g + 1));
      for (size_t c = 0; c < per; ++c)
        row.push_back(textOf[varlist[static_cast<size_t>(g) * per + c]][i]);
      for (const auto& w : wideCols)
        row.push_back(w.group == g ? row[1 + w.pos] : std::string());
      t.rows.push_back(std::move(row));
    }
  }

  std::map<std::string, std::string> colType;
  {
    // Storage types, as the ado leaves them: _stack is an int; each result column takes
    // the widest type among the columns stacked into it (append promotes), and a `wide`
    // column the type of its result column. A display format follows the type.
    auto rank = [](const std::string& ty) {
      if (ty == "byte") return 1;
      if (ty == "int") return 2;
      if (ty == "long") return 3;
      if (ty == "float") return 4;
      return 5;
    };
    auto typeOf = [&](const std::string& nm) {
      auto mit = session.varMeta.find(nm);
      if (mit != session.varMeta.end() && mit->second.stataType.has_value())
        return *mit->second.stataType;
      return std::string(ds->dtype(nm) == "factor" ? "str1" : "float");
    };
    auto widest = [&](const std::string& a, const std::string& b) {
      if (a.compare(0, 3, "str") == 0 || b.compare(0, 3, "str") == 0) {
        // append keeps strL over any str#
        if (a == "strL" || b == "strL") return std::string("strL");
        const long wa = a.compare(0, 3, "str") == 0 ? std::atol(a.c_str() + 3) : 0;
        const long wb = b.compare(0, 3, "str") == 0 ? std::atol(b.c_str() + 3) : 0;
        return "str" + std::to_string(std::max(wa, wb));
      }
      if ((a == "long" && b == "float") || (a == "float" && b == "long")) return std::string("double");
      return rank(a) >= rank(b) ? a : b;
    };

    colType["_stack"] = "int";
    for (size_t c = 0; c < per; ++c) {
      std::string ty = typeOf(varlist[c]);
      for (long long g = 1; g < groups; ++g)
        ty = widest(ty, typeOf(varlist[static_cast<size_t>(g) * per + c]));
      colType[intoNames[c]] = ty;
    }
    for (const auto& w : wideCols) colType[w.name] = colType[intoNames[w.pos]];
  }
  std::string outPath;
  if (auto err = writeAndConvert(session, t, "stack", &outPath)) return *err;
  session.load(outPath);
  if (auto bad = assertRowCount(session, "stack", n * groups)) return *bad;
  // BUG-1147: `clear` only silences stack's warning; without it Stata warns, waits
  // for a key (a no-op in a do-file), and stacks all the same, whatever the data in
  // memory: `stack x1 x2 y z1, into(a b)` is rc 0, _N 4 (StataBE 19.5, stack.ado
  // never reads c(changed)). The warning comes after the varlist checks, so a
  // refused stack does not print it, and `quietly` drops it.
  {
    for (const auto& nm : t.names) {
      session.implicitFloatVars.erase(nm);
      VarMeta& vm = session.varMeta[nm];
      vm = VarMeta();
      vm.stataType = colType[nm];
      if (colType[nm].compare(0, 3, "str") != 0) vm.format = storage::stataDefaultFormat(colType[nm]);
    }
  }
  if (hasFlag(parsed.opts, "clear")) return mutationResult(session, "stack", t.names, before);
  ojson out = ojson::array();
  out.push_back(displayBlock("Warning: data in memory will be lost.\n"
                             "         Press any key to continue, Ctrl-Break to abort."));
  out.push_back(mutationResult(session, "stack", t.names, before));
  return out;
}

}  // namespace piet::handlers
