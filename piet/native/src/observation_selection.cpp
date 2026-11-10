// SPDX-License-Identifier: GPL-3.0-or-later
#include "piet/translate.h"
// Copyright (C) 2026 M.K. Kim
// Observation selection.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // BUG-540: stataMissingWithKind — `.a`-`.z` identity in matrix literals
#include "piet/eval/macros.h"
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/program.h"  // BUG-922: program::restorePreservesDeeperThan is defined here
#include "piet/serset.h"  // BUG-1461: serset::storageType, signed into `datasignature`
#include "piet/strmutate.h"  // BUG-532: needsStringCount, the string-condition test under by:
#include "piet/stata_format.h"  // eb-order T-P1: `matrix list ..., format()`
#include "piet/strutil.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"  // cecon::native_exists / to_native_path / env_utf8 (BUG-050/051)
#include "data/missing.h"   // Stata `.` / `.a`-`.z` kinds + stata_cmp (shared with the engine)
#include "core/stata_arith.h"  // `^` with StataBE's bits (frame.cpp's)

#if defined(__APPLE__)
#include <sys/clonefile.h>  // Q1 (gate ③): APFS copy-on-write snapshot for preserve
#endif  // BUG-1469: the vocabularies below sat inside it, undeclared off macOS

#include "command_internal.h"
#include "estimation_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace command_detail {}
using namespace command_detail;
using namespace session_vocab;

ojson runDrop(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep: same mechanism as runSort's fix/sort-validate-
  // lazy fix below — this varlist check only ever needs column NAMES, so
  // answer it from the resident shadow card when it is provably current for
  // the loaded hbin, instead of unconditionally forcing session.datastore()
  // to reopen the file from disk. Falls back to the exact prior behavior
  // (session.datastore() + ds->has()) whenever the card cannot be trusted —
  // see session.shadowVarlistIfCurrent()'s doc comment (session_state.h) for
  // every case that must fall back rather than risk a wrong "found"/"not
  // found" answer.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varlist) {
      if (!cardHasName(*card, v)) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  std::string outPath = nextPath(session.workdir(), "drop");
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  for (const auto& v : varlist) {
    args.push_back("--drop");
    args.push_back(v);
  }
  // MAXPERF Stage 2 (item 3): drop is a single terminal mutation whose --out
  // becomes the session head and is NEVER read back as a file (session.load()
  // records the virtual head; the card renders from the shadow/materialized
  // datastore) — opt it into the resident frame plane (in-RAM column mutation,
  // no per-verb process spawn). No-op on the fallback plane (serve down/§26-No).
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "drop", {}, varlist);
}

ojson runKeep(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep: same mechanism as runDrop just above — both of
  // keep's own name-only lookups (the full-column enumeration that builds
  // toDrop, and the has-check that validates varlist) come from the resident
  // shadow card when it is provably current, instead of unconditionally
  // forcing session.datastore() open. Falls back to the exact prior
  // behavior (session.datastore() + ds->names()/ds->has()) otherwise.
  std::vector<std::string> toDrop;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) {
      if (std::find(varlist.begin(), varlist.end(), info.name) == varlist.end()) {
        toDrop.push_back(info.name);
      }
    }
    for (const auto& v : varlist) {
      if (!cardHasName(*card, v)) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& name : ds->names()) {
      if (std::find(varlist.begin(), varlist.end(), name) == varlist.end()) toDrop.push_back(name);
    }
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  std::string outPath = nextPath(session.workdir(), "keep");
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  for (const auto& v : toDrop) {
    args.push_back("--drop");
    args.push_back(v);
  }
  // MAXPERF Stage 2 (item 3): keep is a column-subset drop — same single-
  // terminal-mutation profile as runDrop; opt into the resident frame plane.
  SessionState::ResidentEligible resident(session);
  try {
    runCobraJson(args);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "keep", {}, toDrop);
}

ojson runDropIf(const std::string& ifExpr, SessionState& session,
               const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::optional<std::string> ifOpt = ifExpr.empty() ? std::nullopt : std::optional<std::string>(ifExpr);
  // BS-090 (BETA Phase G): `complement=true` keeps the COMPLEMENT of the
  // combined (if AND in) condition. BUG-533: the if-only complement is
  // `--keepif "(EXPR)==0"`, not the old `"!(EXPR)"`, which kept the rows whose
  // EXPR is missing — Stata drops them (missing is true); see
  // applyIfAndInFilter.
  //
  // fix/verb-overhead-sweep S2: opt the row filter into the resident frame
  // plane — the same single-terminal-mutation profile runDrop/runKeep declare
  // above, and the assertion the guard exists to make holds here exactly:
  // `fr.dataPath` becomes the session head via session.load() and is NEVER read
  // back as a file (mutationResult renders it from the shadow card; the
  // fallback rebuild materializes the head first). This is stated HERE, not
  // inside applyIfAndInFilter, because that helper is shared with ~9 callers
  // that DO read its output as a file — summarize's `Datastore(sumifPath)`,
  // collapse's stateless `--data`, the by-group dispatch's `Datastore groupDs`
  // — and only the call site knows which it is. The `in`-range chain suspends
  // this again internally (inrange_filter.cpp), since its own intermediate
  // temps are read back as files.
  SessionState::ResidentEligible resident(session);
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, *session.currentHbin(), ifOpt, inRange, /*complement=*/true);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!fr.ok) return inRangeRejectError(fr, *inRange, "drop");
  session.load(fr.dataPath);
  return mutationResult(session, "keepif");
}

ojson runKeepIf(const std::string& ifExpr, SessionState& session,
               const std::optional<std::string>& inRange) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  std::optional<std::string> ifOpt = ifExpr.empty() ? std::nullopt : std::optional<std::string>(ifExpr);
  // fix/verb-overhead-sweep S2: same opt-in as runDropIf just above, for the
  // same reason — see its comment for why this belongs at the call site rather
  // than inside the shared applyIfAndInFilter.
  SessionState::ResidentEligible resident(session);
  InRangeFilterResult fr;
  try {
    fr = applyIfAndInFilter(session, *session.currentHbin(), ifOpt, inRange);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!fr.ok) return inRangeRejectError(fr, *inRange, "keep");
  session.load(fr.dataPath);
  return mutationResult(session, "keepif");
}

StringKeyOrder stringKeyOrder(const std::vector<std::string>& levels) {
  StringKeyOrder o;
  std::vector<std::string> distinct = levels;
  distinct.push_back(std::string());  // a -1 code reads as ""
  // std::string's operator< is char_traits<char>::compare: byte order, each
  // byte unsigned — Stata's string order.
  std::sort(distinct.begin(), distinct.end());
  distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
  auto rankOf = [&](const std::string& v) {
    return static_cast<double>(std::lower_bound(distinct.begin(), distinct.end(), v) -
                               distinct.begin());
  };
  o.rank.reserve(levels.size());
  for (const auto& l : levels) {
    if (l.empty()) o.blankLevel = true;
    o.rank.push_back(rankOf(l));
  }
  o.blankRank = rankOf(std::string());
  // The engine keys a factor on its code with -1 first, so the code order is
  // Stata's exactly when the levels are strictly byte-ordered and none is "".
  o.codeOrderIsStata = !o.blankLevel;
  for (size_t i = 1; i < levels.size() && o.codeOrderIsStata; ++i)
    o.codeOrderIsStata = levels[i - 1] < levels[i];
  return o;
}

namespace command_detail {

// A scratch f64 column runByKeepIf adds before its engine calls and drops in
// the last one.
struct ByKeepTempCol {
  std::string name;
  std::vector<double> values;
};

// core/ident.h: a byte >= 0x80 is a name byte, so a Unicode name is one name.
bool byKeepIdentChar(char c) { return cecon::ident::isIdentByte(c); }

// One whole-operand comparison of a string variable with a "..." literal,
// `VAR ==/!= "lit"` or `"lit" ==/!= VAR`.
struct ByKeepLitCompare {
  std::string var;
  bool eq = true;   // `==` (true) or `!=`
  std::string lit;  // the literal's text, quotes stripped
};

// BUG-532: the one string form the engine evaluates right under `by:` is a
// string variable compared with a literal, `VAR ==/!= "lit"` (frame.cpp eqne
// resolves the literal against VAR's dictionary). This scanner finds every
// such comparison that stands as a WHOLE operand -- bounded by the
// expression's start/end, `(`, `)`, `,`, `&`, `|`, with a plain name (no
// subscript, call or TS operator) -- and hands it to `fn`: a returned text
// replaces the comparison, nullopt keeps it as written. Everything else is
// copied, "..." literals whole.
template <typename IsString, typename Fn>
std::string mapStringLiteralCompares(const std::string& cond, IsString isString, Fn fn) {
  auto skipWs = [&](size_t j) {
    while (j < cond.size() && std::isspace(static_cast<unsigned char>(cond[j]))) ++j;
    return j;
  };
  auto boundedLeft = [&](size_t start) {
    size_t j = start;
    while (j > 0 && std::isspace(static_cast<unsigned char>(cond[j - 1]))) --j;
    if (j == 0) return true;
    const char c = cond[j - 1];
    // `&` / `|` but not the tail of `!=`/`==`/`<=`/`>=` (those end in '=').
    return c == '(' || c == ',' || c == '&' || c == '|';
  };
  auto boundedRight = [&](size_t end) {
    const size_t j = skipWs(end);
    if (j >= cond.size()) return true;
    const char c = cond[j];
    return c == ')' || c == ',' || c == '&' || c == '|';
  };
  // `==` or `!=` at j: 0 (none), 1 (==), 2 (!=).
  auto eqOpAt = [&](size_t j) -> int {
    if (j + 1 >= cond.size() || cond[j + 1] != '=') return 0;
    if (cond[j] == '=') return 1;
    if (cond[j] == '!') return 2;
    return 0;
  };
  // A plain name at [nb, ne) that names a string variable.
  auto plainStringName = [&](size_t nb, size_t ne) {
    return ne > nb && !std::isdigit(static_cast<unsigned char>(cond[nb])) &&
           (ne >= cond.size() || (cond[ne] != '[' && cond[ne] != '(' && cond[ne] != '.')) &&
           isString(cond.substr(nb, ne - nb));
  };

  std::string out;
  out.reserve(cond.size() + 16);
  size_t i = 0;
  while (i < cond.size()) {
    const char c = cond[i];
    if (c == '"') {
      const size_t q = cond.find('"', i + 1);
      if (q == std::string::npos) {
        out.append(cond, i, std::string::npos);
        break;
      }
      // `"lit" OP NAME`
      const size_t opAt = skipWs(q + 1);
      if (const int op = eqOpAt(opAt)) {
        const size_t nb = skipWs(opAt + 2);
        size_t ne = nb;
        while (ne < cond.size() && byKeepIdentChar(cond[ne])) ++ne;
        if (plainStringName(nb, ne) && boundedLeft(i) && boundedRight(ne)) {
          const ByKeepLitCompare lc{cond.substr(nb, ne - nb), op == 1,
                                    cond.substr(i + 1, q - i - 1)};
          if (const std::optional<std::string> rep = fn(lc)) {
            out += *rep;
            i = ne;
            continue;
          }
        }
      }
      out.append(cond, i, q + 1 - i);
      i = q + 1;
      continue;
    }
    // `NAME OP "lit"`
    if (cecon::ident::isIdentStartByte(c) &&
        (i == 0 || (!byKeepIdentChar(cond[i - 1]) && cond[i - 1] != '.'))) {
      size_t ne = i;
      while (ne < cond.size() && byKeepIdentChar(cond[ne])) ++ne;
      const size_t opAt = skipWs(ne);
      const int op = eqOpAt(opAt);
      const size_t lit = op != 0 ? skipWs(opAt + 2) : cond.size();
      if (op != 0 && lit < cond.size() && cond[lit] == '"' && plainStringName(i, ne)) {
        const size_t q = cond.find('"', lit + 1);
        if (q != std::string::npos && boundedLeft(i) && boundedRight(q + 1)) {
          const ByKeepLitCompare lc{cond.substr(i, ne - i), op == 1,
                                    cond.substr(lit + 1, q - lit - 1)};
          if (const std::optional<std::string> rep = fn(lc)) {
            out += *rep;
            i = q + 1;
            continue;
          }
        }
      }
      out.append(cond, i, ne - i);
      i = ne;
      continue;
    }
    out += c;
    ++i;
  }
  return out;
}

// BUG-532: the engine resolves `factorvar ==/!= "literal"` against the level
// dictionary and folds a literal that is not a level to a CONSTANT (frame.cpp
// eqne). A blank read from .dta or CSV is code -1 with no "" level, so `s==""`
// was 0 on every row and `s!=""` 1 — the rows Stata calls blank were never
// seen. Every whole-operand `NAME ==/!= ""` (either operand order) naming a
// string variable is therefore answered here: a 0/1 scratch column, 1 where
// the row is blank (code -1, or the level ""), and the comparison respelled
// `(T==1)` / `(T==0)`. Any other `""` left next to a string variable
// (`(s=="")==1` spelled without the parentheses, ...) is refused by name
// rather than handed to the engine's constant fold.
std::optional<ojson> rewriteBlankStringCompares(std::string& cond, Datastore& ds,
                                                 std::vector<ByKeepTempCol>& temps) {
  auto isFactor = [&](const std::string& nm) { return ds.has(nm) && ds.dtype(nm) != "f64"; };
  std::map<std::string, std::string> tempFor;  // variable -> its blank column
  auto blankCol = [&](const std::string& var) -> std::string {
    auto it = tempFor.find(var);
    if (it != tempFor.end()) return it->second;
    std::string name = "__piet_bkblank" + std::to_string(temps.size()) + "__";
    const cecon::Column& c = ds.col(var);
    const int32_t* codes = c.codes();
    std::vector<char> blankLevel(c.levels.size(), 0);
    for (size_t l = 0; l < c.levels.size(); ++l) blankLevel[l] = c.levels[l].empty() ? 1 : 0;
    ByKeepTempCol t;
    t.name = name;
    t.values.resize(static_cast<size_t>(ds.nRows()));
    for (int64_t r = 0; r < ds.nRows(); ++r) {
      const int32_t code = codes[r];
      t.values[static_cast<size_t>(r)] =
          (code < 0 || blankLevel[static_cast<size_t>(code)]) ? 1.0 : 0.0;
    }
    temps.push_back(std::move(t));
    tempFor.emplace(var, name);
    return name;
  };
  const std::string out = mapStringLiteralCompares(
      cond, isFactor, [&](const ByKeepLitCompare& lc) -> std::optional<std::string> {
        if (!lc.lit.empty()) return std::nullopt;
        return "(" + blankCol(lc.var) + (lc.eq ? "==1)" : "==0)");
      });
  // What is left: a blank literal the respelling did not take. Next to a
  // string variable, the engine would fold it to a constant — refuse.
  bool leftover = false;
  for (size_t j = 0; j < out.size() && !leftover;) {
    if (out[j] == '"') {
      const size_t q = out.find('"', j + 1);
      if (q == j + 1) leftover = true;
      j = q == std::string::npos ? out.size() : q + 1;
    } else {
      ++j;
    }
  }
  if (leftover) {
    bool namesString = false;
    for (size_t j = 0; j < out.size() && !namesString;) {
      if (cecon::ident::isIdentStartByte(out[j])) {
        size_t e = j;
        while (e < out.size() && byKeepIdentChar(out[e])) ++e;
        namesString = (j == 0 || out[j - 1] != '.') && isFactor(out.substr(j, e - j));
        j = e;
      } else if (out[j] == '"') {
        const size_t q = out.find('"', j + 1);
        j = q == std::string::npos ? out.size() : q + 1;
      } else {
        ++j;
      }
    }
    if (namesString) {
      return ojson(eval::errorBlock(
          "unsupported_command",
          "a comparison with the blank string \"\" under `by:` is supported only as a whole "
          "operand, `VAR == \"\"` or `VAR != \"\"` (optionally in parentheses, joined by & or |); "
          "rewrite the condition in that form.",
          std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
    }
  }
  cond = out;
  return std::nullopt;
}

// Unique scratch names inside one second (mintScratchPath is second-grained on
// POSIX, and one call here mints up to three files under the same prefix).
std::string byKeepPath(SessionState& session, const char* what) {
  static unsigned long long seq = 0;
  return nextPath(session.workdir(), std::string("bykeep") + what + std::to_string(seq++));
}

// A scratch file one of these helpers made, unlinked once the next step has
// read it (never `head`, the session's own file).
void dropScratchFile(const std::string& p, const std::string& head) {
  if (p.empty() || p == head) return;
  cecon::native_unlink(p);
  forgetScratchMint(p);
}

// Adds each of `temps` to the file `cur`, file to file (`setcol --new`), and
// leaves `cur` naming the last file. Every intermediate file (the CSV payload,
// the previous copy) is unlinked once read, and on every error exit; `head`
// is never touched.
std::optional<ojson> addTempCols(SessionState& session, const std::string& head, std::string& cur,
                                 const std::vector<ByKeepTempCol>& temps) {
  for (const auto& t : temps) {
    // mintScratchPath registers a ".hbin" name; the setcol payload is a CSV,
    // so the name that actually exists is the one registered (BUG-389).
    std::string valuesPath = byKeepPath(session, "vals");
    forgetScratchMint(valuesPath);
    valuesPath = valuesPath.substr(0, valuesPath.size() - 5) + ".csv";  // 5 == strlen(".hbin")
    registerScratchMint(valuesPath);
    {
      std::ofstream vf(cecon::to_native_path(valuesPath), std::ios::binary);
      bool okWrite = static_cast<bool>(vf);
      if (okWrite) {
        char buf[32];
        for (double v : t.values) {
          std::snprintf(buf, sizeof(buf), "%.17g\n", v);
          vf << buf;
        }
        vf.flush();
        okWrite = static_cast<bool>(vf);
      }
      if (!okWrite) {
        vf.close();
        cecon::native_unlink(valuesPath);
        forgetScratchMint(valuesPath);
        dropScratchFile(cur, head);
        return engineErrorBlock("could not write a scratch file under " + session.workdir());
      }
    }
    const std::string next = byKeepPath(session, "col");
    CobraRunResult cr = runCobra({"setcol", "--data", cur, "--var", t.name, "--values", valuesPath,
                                  "--dtype", "f64", "--new", "--out", next});
    cecon::native_unlink(valuesPath);
    forgetScratchMint(valuesPath);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      dropScratchFile(next, head);
      dropScratchFile(cur, head);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    dropScratchFile(cur, head);
    cur = next;
  }
  return std::nullopt;
}

std::string joinCommaKeys(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

}  // namespace

ojson runByKeepIf(SessionState& session, const std::string& ifExpr,
                  const std::vector<std::string>& byVars,
                  const std::vector<std::string>& stataSortKeys, bool drop) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // BUG-532: `by g [(s)]: keep if EXPR` / `drop if EXPR`. EXPR is evaluated
  // per by-group (group-local _n/_N, x[k], sum()) into a scratch mask, the rows
  // are filtered on it and the mask is dropped again — ONE engine call, because
  // cmd_eval applies --bygen/--keepif/--drop in argv order over a single frame
  // load (the same fused shape applyIfAndInFilter's `in` chain uses).
  //
  // Row order: the rows are in by++sort key order when the mask is computed —
  // either the caller checked that, or they are sorted in place here first on
  // `stataSortKeys` (the `bysort` half). --bygen is then given by() ONLY: its
  // stable sort on the by-keys keeps every group's rows in their physical
  // order, so _n is the row's position inside its group in the CURRENT order,
  // as in Stata — also when a key in the parentheses is a string, whose
  // engine code order is not Stata's.
  //
  // Missing is TRUE in Stata's `if`. The engine's keep_if keeps a missing mask,
  // which is right for keep; for drop the complement is written `mask==0`, which
  // is false on a missing mask (missing compares above every number), so the
  // row is dropped. EXPR's own `!`, `&`, `|` are left as written: the engine
  // reads a missing OPERAND of those as true, as Stata does (BUG-620), the
  // same reading every other engine-evaluated `if` in PIET gets.
  static const char* kMask = "__piet_bykeep__";
  std::string cond = ifExpr;
  auto joinComma = [](const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) s += ",";
      s += v[i];
    }
    return s;
  };

  // Scratch columns the engine calls need: a blank indicator per string
  // variable compared with "", and a byte-order rank per string sort key whose
  // dictionary is not in Stata's order.
  std::vector<ByKeepTempCol> temps;
  std::vector<std::string> gsortKeys = stataSortKeys;
  // A string variable, a string literal or a string function in EXPR: the
  // same predicate the global keep/drop use to leave the engine for the
  // row-wise string path (read off the varlist card, no file open).
  const bool stringCond = strmutate::needsStringCount(session, cond);
  bool stringKeyToCheck = false;
  // The Datastore is opened only when a string condition or key is involved.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& info : *card) {
      if (info.dtype == "f64") continue;
      for (const auto& k : byVars) stringKeyToCheck |= info.name == k;
      for (const auto& k : stataSortKeys) stringKeyToCheck |= info.name == k;
    }
  } else {
    stringKeyToCheck = true;
  }
  if (stringCond || stringKeyToCheck) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    if (stringCond) {
      if (cond.find("\"\"") != std::string::npos) {
        if (auto err = rewriteBlankStringCompares(cond, *ds, temps)) return *err;
      }
      // Under `by:` the engine evaluates EXPR, and of the string forms it gets
      // only a whole-operand `VAR ==/!= "lit"` right (a blank one was just
      // respelled). Anything else — inlist(s,...), strpos(s,...),
      // substr(s,1,1)=="a", s==s[_n-1], a string variable read as a number —
      // would die in the engine with a generic error (rc 1) or be answered
      // wrong, so it is refused by name, before any row moves.
      auto isFactor = [&](const std::string& nm) { return ds->has(nm) && ds->dtype(nm) != "f64"; };
      const std::string residual = mapStringLiteralCompares(
          cond, isFactor,
          [](const ByKeepLitCompare&) { return std::optional<std::string>("1"); });
      if (strmutate::needsStringCount(session, residual)) {
        return ojson(eval::errorBlock(
            "unsupported_command",
            "under `by:`, a condition on a string variable is supported only as a whole "
            "operand `VAR == \"text\"` or `VAR != \"text\"` (optionally in parentheses, "
            "joined by & or |); string functions (inlist, strpos, substr, ...) and other "
            "string forms in `by ...: keep|drop if` are not supported yet. Compute the "
            "condition into a numeric variable first, e.g. `generate byte t = " + ifExpr +
                "`, then `by ...: keep if t`.",
            std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
      }
    }
    // A string BY-key whose dictionary holds "" as a level AND has blank (-1)
    // rows: Stata sees one group, the engine's by() two. Refused by name.
    for (const auto& k : byVars) {
      if (!ds->has(k) || ds->dtype(k) == "f64") continue;
      const cecon::Column& c = ds->col(k);
      const StringKeyOrder o = stringKeyOrder(c.levels);
      if (!o.blankLevel) continue;
      const int32_t* codes = c.codes();
      for (int64_t r = 0; r < ds->nRows(); ++r) {
        if (codes[r] < 0) {
          return ojson(eval::errorBlock(
              "unsupported_command",
              "`by " + k + ":` is not supported yet when `" + k +
                  "` holds blank values stored two ways (as missing and as an empty "
                  "level): Stata sees one group, this engine would see two. Group on a "
                  "numeric key instead, e.g. `egen id = group(" + k + "), missing`.",
              std::nullopt, std::nullopt, std::nullopt, "piet_gap"));
        }
      }
    }
    for (const auto& t : temps) {
      if (ds->has(t.name)) {
        return engineErrorBlock("a variable named " + t.name +
                                " exists; it is a name PIET reserves for scratch columns");
      }
    }
  }

  const std::string bygenArg =
      std::string(kMask) + "=(" + cond + "), by(" + joinComma(byVars) + ")";
  const std::string pred = drop ? (std::string(kMask) + "==0") : std::string(kMask);

  if (temps.empty()) {
    if (!stataSortKeys.empty()) {
      ojson sorted = runSort(stataSortKeys, session);
      if (sorted.is_object() && sorted.value("type", "") == "error") return sorted;
    }
    std::string outPath = byKeepPath(session, "");
    // BUG-369: a TS operator in EXPR needs the tsset metadata on the call; a
    // runiform()/runiformint() draw needs the session's seed and stream (the
    // runBysortGen pattern), or `set seed` is ignored and the stream never
    // advances.
    std::vector<std::string> args = translate::injectTsContextForExpr(
        {"eval", "--data", *session.currentHbin(), "--bygen", bygenArg, "--keepif", pred,
         "--drop", kMask, "--out", outPath},
        cond, session);
    args = translate::injectRngContextForExpr(args, cond, session);
    // Single terminal mutation whose --out becomes the head and is never read
    // back as a file — the runKeepIf/runDropIf opt-in, for the same reason.
    SessionState::ResidentEligible resident(session);
    try {
      translate::absorbRngDraws(runCobraJson(args), session);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(outPath);
    return mutationResult(session, "keepif");
  }

  // With scratch columns: file to file (the head was materialized by
  // session.datastore() above), and only the last file becomes the head, so
  // no scratch column can reach the user's frame even when a step fails.
  // Every intermediate file is this call's own and is unlinked as soon as
  // the next step has read it, and on every error exit; the head is never
  // touched.
  const std::string head = *session.currentHbin();
  std::string cur = head;
  auto dropIntermediate = [&](const std::string& p) { dropScratchFile(p, head); };
  auto fail = [&](const std::string& partial, ojson err) {
    dropIntermediate(cur);
    if (!partial.empty()) dropIntermediate(partial);
    return err;
  };
  if (std::optional<ojson> err = addTempCols(session, head, cur, temps)) return *err;
  if (!stataSortKeys.empty()) {
    // With blank-compare columns only, gsortKeys are the Stata keys, and the
    // marker this leaves is exactly them, as Stata's `bysort` sets it
    // (w2e_probes.log bo_sort_prefix_noop: a coarser sort downgrades "g x" to
    // "g"); adding or dropping the scratch columns leaves it standing.
    const std::string next = byKeepPath(session, "sort");
    CobraRunResult cr =
        runCobra({"gsort", "--data", cur, "--vars", joinComma(gsortKeys), "--out", next});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return fail(next, engineErrorBlock(cobraErrorStr(ce), ce.code));
    }
    dropIntermediate(cur);
    cur = next;
  }
  std::vector<std::string> base = {"eval", "--data", cur, "--bygen", bygenArg, "--keepif", pred,
                                   "--drop", kMask};
  for (const auto& t : temps) {
    base.push_back("--drop");
    base.push_back(t.name);
  }
  const std::string outPath = byKeepPath(session, "");
  base.push_back("--out");
  base.push_back(outPath);
  std::vector<std::string> args = translate::injectTsContextForExpr(base, cond, session);
  args = translate::injectRngContextForExpr(args, cond, session);
  try {
    translate::absorbRngDraws(runCobraJson(args), session);
  } catch (const CobraError& ce) {
    return fail(outPath, engineErrorBlock(cobraErrorStr(ce), ce.code));
  }
  session.load(outPath);
  dropIntermediate(cur);
  return mutationResult(session, "keepif");
}

// ---------------------------------------------------------------------------
// sort

std::optional<ojson> sortInStataOrder(SessionState& session, const std::vector<std::string>& keys) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (keys.empty()) return std::nullopt;
  // Frame::gsort now orders both level strings and missing kinds as Stata
  // does. The shared route also preserves a claim on the actual user keys.
  ojson sorted = runSort(keys, session);
  if (sorted.is_object() && sorted.value("type", "") == "error") return sorted;
  return std::nullopt;
}

ojson runSort(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/sort-validate-lazy: this varlist check only ever needs COLUMN NAMES,
  // never column DATA — so answer it from the resident shadow card
  // (session.shadowVarlistIfCurrent(), a plain O(columns) name scan over
  // already-cached metadata) instead of unconditionally forcing
  // session.datastore() open the current hbin from disk. That eager re-open
  // (Datastore::Datastore -> cecon::Dataset::open, a full mmap + header
  // parse of however many MB the file is) purely to answer "does column X
  // exist" measured ~700ms-1700ms per sort on a 164 MB corpus in phase-split
  // instrumentation of this function — see the fix's own report for the
  // table. shadowVarlistIfCurrent() returns nullptr for every case where the
  // card cannot be trusted (absent, a `use`/`clear`/restore swapped datasets
  // without reseeding it, a post-estimation command resynced the resident
  // worker onto a DIFFERENT file, or a respawned/rolled-back worker) — the
  // fallback below is the exact pre-existing behavior, unchanged, so a
  // stale/foreign card never turns into a wrong "not found" (or wrongly
  // "found") answer; it only ever costs the same as before.
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    for (const auto& v : varlist) {
      bool found = false;
      for (const auto& info : *card) {
        if (info.name == v) {
          found = true;
          break;
        }
      }
      if (!found) return engineErrorBlock("variable " + v + " not found");
    }
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    for (const auto& v : varlist) {
      if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found");
    }
  }
  // BUG-532: Stata's `sort K` on data whose marker already begins with K moves
  // no row and sets the marker to K (w2e_probes.log bo_sort_prefix_noop: "g x"
  // -> `sort g` -> "g"; bo_sort_prefix_then_by). That is the engine's gsort as
  // is (Frame::gsort skips the radix pass there), so this handler neither
  // reads the marker nor opens the head's Datastore: on the resident plane
  // that read materialized the whole dataset on every sort and every bysort
  // line (review of e19291ad).
  // BS-001 fix: this used to shell out to `eval --data ... --sort <vars>`,
  // but cobra's `eval` verb (cmds_data.cpp's cmd_eval) has no `--sort` flag
  // at all — its option-parsing loop only recognizes a fixed set of flags
  // (--xtset/--gen/--egen/.../--keepif/--summarize) and silently ignores any
  // OTHER flag/value pair without error (no catch-all reject branch), so
  // `--sort <vars>` was a pure no-op: cobra ran, wrote the SAME row order
  // back out, and PIET reported success. Real row reordering lives in
  // cobra's dedicated `gsort` verb (cmds_data.cpp's cmd_gsort / Frame::gsort,
  // std::stable_sort — see runGsort in handlers_data_verbs.cpp, the already-
  // correct sibling this now mirrors). Stata's plain `sort` is that same
  // stable multi-key sort with every key ascending (no [+-] prefix), so the
  // varlist is forwarded unprefixed. `gsort` writes its output file directly
  // and prints a plain-text status line (JSON only on error), so this must
  // use runCobra (not runCobraJson, which requires a JSON stdout payload).
  std::string outPath = nextPath(session.workdir(), "sort");
  std::string joined;
  for (size_t i = 0; i < varlist.size(); ++i) {
    if (i) joined += ",";
    joined += varlist[i];
  }
  std::vector<std::string> args = {"gsort", "--data", *session.currentHbin(), "--vars", joined,
                                    "--out", outPath};
  // MAXPERF Stage 2 (item 3): a pure row-reorder — single terminal mutation,
  // --out never read back as a file. Opt into the resident frame plane.
  SessionState::ResidentEligible resident(session);
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  return mutationResult(session, "sort");
}

// BS-092 (BETA campaign, Phase G): a NON-MUTATING sibling of runSort/
// runGsort above — sorts `basePath` by `byVars` (ascending, stable, same
// `gsort` verb and shape as runSort) into a FRESH scratch .hbin instead of
// calling session.load() on the result. dispatcher.cpp's `by:`/`bysort:
// summarize` grouped-dispatch loop needs the PHYSICALLY contiguous
// same-group row blocks a stable sort produces (so each group can be sliced
// out via piet::handlers::applyIfAndInFilter's plain row-range mechanism),
// but summarize is a READ-ONLY command — it must never reorder the live
// session dataset a user has loaded, unlike a real `sort`/`bysort`'s own
// (documented) side effect on gen/replace's target. Throws CobraError on
// failure (same convention as applyIfAndInFilter), not an ojson error
// block, so callers can wrap it in their own try/catch around the whole
// per-group loop instead of checking a sentinel after every call.
std::string sortForGrouping(SessionState& session, const std::string& basePath,
                            const std::vector<std::string>& byVars) {
  std::string joined;
  for (size_t i = 0; i < byVars.size(); ++i) {
    if (i) joined += ",";
    joined += byVars[i];
  }
  std::string outPath = nextPath(session.workdir(), "bygroup_sort");
  std::vector<std::string> args = {"gsort", "--data", basePath, "--vars", joined, "--out", outPath};
  CobraRunResult cr = runCobra(args);
  if (!cr.ok) throw errorFromRaw(cr);
  return outPath;
}

// ---------------------------------------------------------------------------
// count [if COND]


}  // namespace piet::handlers
