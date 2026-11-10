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

ojson runReturnList(SessionState& session) {
  std::ostringstream out;

  // MOST-RECENTLY-SET FIRST, which is why both loops run in reverse: the
  // OrderedDict/OrderedFloatDict contract is insertion order with
  // assignment-keeps-position, so the tail of items() is the newest key and
  // forward iteration would print Stata's list upside down.
  out << "\nscalars:\n";
  const auto& scalars = session.eval.rValues.items();
  for (auto it = scalars.rbegin(); it != scalars.rend(); ++it) {
    out << padLabel22("r(" + it->first + ")") << " =  " << fmtRScalar(it->second) << "\n";
  }

  out << "\nmacros:\n";
  const auto& macros = session.eval.rStrings.items();
  for (auto it = macros.rbegin(); it != macros.rend(); ++it) {
    if (!it->first.empty() && it->first[0] == kReservedKeyLeadByte) continue;
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
  if (!session.eval.rMatrices.empty()) {
    out << "\nmatrices:\n";
    const auto& rmats = session.eval.rMatrices;
    for (auto it = rmats.rbegin(); it != rmats.rend(); ++it) {
      const size_t nr = it->second.data.size();
      const size_t nc = nr > 0 ? it->second.data[0].size() : 0;
      out << padLabel22("r(" + it->first + ")") << " :  " << nr << " x " << nc << "\n";
    }
  }

  // Headers with no rows when a store is empty: that is the honest report that
  // r() has no scalars, and it is what real Stata prints. Suppressing the
  // header would make "nothing set" indistinguishable from "this build does not
  // track that half".
  // "display": output, not the command echo, so `noisily` shows it and `quietly`
  // hides it (BUG-1505, as runEreturnList).
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

}  // namespace piet::handlers
