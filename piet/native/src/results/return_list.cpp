// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/results.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // stataExtendedMissingKind (BUG-1511); stataMissingToken for a `.a` cell (BUG-1399)
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// `return list`

// Both `return list` sections pad the NAME to 22 and then write the value, and
// that width is not this file's invention: it is scalar_results.cpp's
// runEreturnList padLabel, and the two commands print the SAME table from
// different stores. A second width here would let `ereturn list` and `return
// list` drift into disagreeing about their own column.
std::string padLabel22(const std::string& s) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= 22 ? s : std::string(22 - n, ' ') + s;
}

// runEreturnList's fmtScalar, plus the missing case. Its own scalars come from
// a cobra result that OMITS an absent key, so it never had to answer this; r()
// genuinely carries missing -- program.cpp's handleReturnStmt stages the literal
// "nan" whenever the `return scalar` expression did not evaluate, and Stata
// prints that as ".".
//
// The other missing is Stata's own double, 2^1023 and up (kStataMissingSentinel):
// summarize posts one observation's r(sd) and r(Var) that way, and centile a
// missing r(c_#), r(lb_#), r(ub_#). It is listed by its kind, `.` or `.a`..`.z`
// (StataBE 19.5, BUG-1511: `r(sd) =  .` where this printed 8.98846567431158e+307).
std::string fmtRScalar(double v) {
  if (std::isnan(v)) return ".";
  if (const auto kind = eval::stataExtendedMissingKind(v))
    return *kind == 0 ? std::string(".") : std::string(".") + static_cast<char>('a' + *kind - 1);
  std::ostringstream oss;
  oss.precision(16);
  oss << v;
  return oss.str();
}

// eval.rStrings doubles as the s-class store: `sreturn local` writes there under
// program.cpp's / eval/macros.cpp's kSClassKeyPrefix ("\x01s:"), because P4's
// "no new session stores" constraint left rStrings as the only persistent string
// store. `return list` prints r() ONLY, so those rows must be filtered here or
// `sreturn local x` shows up as an r() macro literally named "\x01s:x".
//
// The test is on the RESERVED LEAD BYTE rather than the exact three-byte prefix:
// \x01 cannot begin a real Stata name (strutil.h's isValidStataIdentifier
// rejects it), so every reserved-key convention this store may grow is excluded
// by construction instead of only today's single one.
constexpr char kReservedKeyLeadByte = '\x01';

}  // namespace

ojson runReturnList(SessionState& session, bool all) {
  std::ostringstream out;

  // MOST-RECENTLY-SET FIRST, which is why both loops run in reverse: the
  // OrderedDict/OrderedFloatDict contract is insertion order with
  // assignment-keeps-position, so the tail of items() is the newest key and
  // forward iteration would print Stata's list upside down.
  //
  // A section is printed only when it has a row, and an empty r() prints
  // nothing at all (StataBE 19.5, the vs-stata truth bank: `return list` after
  // reshape shows `scalars:` and no `macros:` header; after a display-only
  // cell it shows no line). The headers used to print unconditionally.
  // A hidden scalar (describe's r(N_max), r(k_max), r(widthmax)) is not
  // listed, as in Stata (truth bank: `return list` after reshape shows
  // changed width k N while `: r(scalars)` also names the three limits).
  const auto& scalars = session.eval.rValues.items();
  bool scalarHeader = false;
  for (auto it = scalars.rbegin(); it != scalars.rend(); ++it) {
    if (session.eval.isRHidden(it->first)) continue;
    if (!scalarHeader) out << "\nscalars:\n";
    scalarHeader = true;
    out << padLabel22("r(" + it->first + ")") << " =  " << fmtRScalar(it->second) << "\n";
  }

  const auto& macros = session.eval.rStrings.items();
  bool macroHeader = false;
  for (auto it = macros.rbegin(); it != macros.rend(); ++it) {
    if (!it->first.empty() && it->first[0] == kReservedKeyLeadByte) continue;
    if (session.eval.isRMacroHidden(it->first)) continue;
    if (!macroHeader) out << "\nmacros:\n";
    macroHeader = true;
    out << padLabel22("r(" + it->first + ")") << " : \"" << it->second << "\"\n";
  }

  // r-class matrices (sktest's r(table)). CONDITIONAL, unlike the two headers
  // above: those are unconditional by an older decision, and every frozen
  // `return list` fixture in the tree was captured without a matrices: section.
  // Emitting one unconditionally would change the bytes of every existing
  // return-list pin at once -- a whole-tree re-capture to say "no matrices",
  // which is exactly the re-pin that hides real drift. So the section appears
  // only when a command actually published an r-matrix, and its absence keeps
  // meaning what it has always meant.
  const auto hiddenMatrix = [&session](const std::pair<std::string, eval::EvalMatrix>& m) {
    for (const auto& [n, g] : session.eval.rHiddenMatrices)
      if (n == m.first && g == m.second.gen) return true;
    return false;
  };
  bool anyShown = false;
  for (const auto& m : session.eval.rMatrices) anyShown = anyShown || !hiddenMatrix(m);
  if (anyShown) {
    out << "\nmatrices:\n";
    const auto& rmats = session.eval.rMatrices;
    for (auto it = rmats.rbegin(); it != rmats.rend(); ++it) {
      if (hiddenMatrix(*it)) continue;
      const size_t nr = it->second.data.size();
      const size_t nc = nr > 0 ? it->second.data[0].size() : 0;
      out << padLabel22("r(" + it->first + ")") << " :  " << nr << " x " << nc << "\n";
    }
  }

  // `return list, all` adds the hidden results after a warning line, in the
  // same sections (StataBE 19.5, vs-stata bank b6400ae8).
  if (all) {
    std::ostringstream hid;
    bool hs = false, hm = false, hx = false;
    for (auto it = scalars.rbegin(); it != scalars.rend(); ++it) {
      if (!session.eval.isRHidden(it->first)) continue;
      if (!hs) hid << "\nscalars:\n";
      hs = true;
      hid << padLabel22("r(" + it->first + ")") << " =  " << fmtRScalar(it->second) << "\n";
    }
    for (auto it = macros.rbegin(); it != macros.rend(); ++it) {
      if (!it->first.empty() && it->first[0] == kReservedKeyLeadByte) continue;
      if (!session.eval.isRMacroHidden(it->first)) continue;
      if (!hm) hid << "\nmacros:\n";
      hm = true;
      hid << padLabel22("r(" + it->first + ")") << " : \"" << it->second << "\"\n";
    }
    const auto& rmats = session.eval.rMatrices;
    for (auto it = rmats.rbegin(); it != rmats.rend(); ++it) {
      if (!hiddenMatrix(*it)) continue;
      if (!hx) hid << "\nmatrices:\n";
      hx = true;
      const size_t nr = it->second.data.size();
      const size_t nc = nr > 0 ? it->second.data[0].size() : 0;
      hid << padLabel22("r(" + it->first + ")") << " :  " << nr << " x " << nc << "\n";
    }
    if (hs || hm || hx)
      out << "\nHidden; names and contents could change in future versions of Stata\n" << hid.str();
  }

  if (out.str().empty()) return ojson::array();  // nothing in r(): no output
  // "display": output, not the command echo, so `noisily` shows it and `quietly`
  // hides it (BUG-1505, as runEreturnList).
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

// s() lives in rStrings under program.cpp's / macros.cpp's kSClassKeyPrefix.
namespace {
const std::string& sClassPrefix() {
  static const std::string kPrefix = "\x01s:";
  return kPrefix;
}
}  // namespace

ojson runSreturnList(SessionState& session) {
  std::ostringstream out;
  const auto& macros = session.eval.rStrings.items();
  for (auto it = macros.rbegin(); it != macros.rend(); ++it) {
    if (it->first.compare(0, sClassPrefix().size(), sClassPrefix()) != 0) continue;
    if (out.tellp() == 0) out << "\nmacros:\n";
    out << padLabel22("s(" + it->first.substr(sClassPrefix().size()) + ")") << " : \""
        << it->second << "\"\n";
  }
  if (out.str().empty()) return ojson::array();
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

void clearSreturn(SessionState& session) {
  std::vector<std::string> keys;
  for (const auto& kv : session.eval.rStrings.items())
    if (kv.first.compare(0, sClassPrefix().size(), sClassPrefix()) == 0) keys.push_back(kv.first);
  for (const auto& k : keys) session.eval.rStrings.erase(k);
}

}  // namespace piet::handlers
