// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// renpfix -- rename variables by prefix (variable-metadata family; shared helpers in
// variable_metadata_support.h).
//
//   renpfix   a loop over runRename, ONE mutationResult for the renames done.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::stata_cmp / missing_kind -- Stata's `!=`, extended missings
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // handleDisplay (display's default number text), resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt, fnAbbrev
#include "piet/eval/macros.h"   // captureRFromBlock
#include "piet/eval/pyvalue.h"  // pythonFloatRepr / PyValue
#include "piet/eval/session.h"  // replaceRClass / declareRPosted
#include "piet/eval/tokenizer.h"  // scanOneToken / quotedSpanMask
#include "piet/handlers/variables.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

#include "variable_metadata_support.h"

namespace {

// renpfix.ado takes no option at all: anything after the comma is r(198).
const piet::eval::OptVocab& kVocabRenpfix =
    piet::eval::registerOptVocab({"renpfix"}, {{}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace variable_metadata_detail;

namespace {

// A name Stata reserves: `rename a long` is r(198) "long invalid varname" (mx6.log RV|*:
// _b byte _coef _cons double float if in int long _n _N _pi _pred _rc str# strL using
// with; `_skip`, `strl`, `str0` and `str` are legal). Case-sensitive, as measured.
bool isReservedVarName(const std::string& n) {
  static const std::set<std::string> kReserved = {
      "_b",   "byte",   "_coef", "_cons", "double", "float", "if", "in",
      "int",  "long",   "_n",    "_N",    "_pi",    "_pred", "_rc", "strL",
      "using", "with"};
  if (kReserved.count(n) != 0) return true;
  if (n.size() > 3 && n.compare(0, 3, "str") == 0 && n[3] != '0') {
    for (size_t i = 3; i < n.size(); ++i)
      if (!std::isdigit(static_cast<unsigned char>(n[i]))) return false;
    return true;
  }
  return false;
}

}  // namespace

// ===========================================================================
// renpfix
// ===========================================================================
//
// Implements renpfix per its help file; identified against StataBE probes
// (w2ax_t1_official.log renpfix_*, mx2.log RP|*):
//   * `renpfix old [new]` -- two words at most (a third: r(198) "invalid syntax"), quotes
//     allowed (RP|quoted); every variable matching `old*`, in dataset order, is renamed to
//     new + the rest of its name (renpfix_basic), and `new` omitted strips the prefix
//     (renpfix_drop); the prefix match is case-sensitive (RP|case);
//   * no match r(111) "variable old* not found" (renpfix_nomatch);
//   * the renames run IN ORDER and stop at the first that fails, the earlier ones staying
//     done (renpfix_collide `u1 u2 a b other ttx ux`, r(110) "variable ux already
//     defined"); a name that becomes empty is r(100) "varname required" (RP|exact), an
//     illegal one r(198) "<name> invalid name" (RP|badnew, RP|toempty), a reserved one
//     r(198) "<name> invalid varname" (review p7 rp_resv `long`, `_n`);
//   * r(varlist) holds the new names when every rename succeeded.
ojson runRenpfix(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRenpfix.known,
                                            kVocabRenpfix.unimplemented))
    return ojson(*err);
  eval::replaceRClass(session.eval);  // renpfix is r-class; a failure leaves r() empty
  eval::declareRPosted(session.eval, "renpfix", {});
  const std::vector<std::string> words = stataWords(parsed.argText);
  if (words.size() > 2) return rcError("invalid syntax", 198);
  const std::string prefix = words.empty() ? std::string() : words[0];
  const std::string repl = words.size() > 1 ? words[1] : std::string();

  std::vector<std::string> vars;
  if (auto e = unabVarlist(prefix + "*", session, &vars)) return *e;

  std::vector<std::string> fromNames, toNames;
  std::optional<ojson> failure;
  std::vector<std::string> live = allVarNames(session);
  for (const auto& v : vars) {
    const std::string next = repl + v.substr(std::min(prefix.size(), v.size()));
    if (next.empty()) {
      failure = rcError("varname required", 100);
      break;
    }
    if (!strutil::isValidStataIdentifier(next)) {
      failure = rcError(next + " invalid name", 198);
      break;
    }
    // A reserved name stops the loop like any other failed rename (review p7 rp_resv:
    // `renpfix a long` renames abc to longbc, then `long invalid varname`).
    if (isReservedVarName(next)) {
      failure = rcError(next + " invalid varname", 198);
      break;
    }
    if (std::find(live.begin(), live.end(), next) != live.end()) {
      failure = rcError("variable " + next + " already defined", 110);
      break;
    }
    ojson r = runRename(v, next, session);
    if (const ojson* e = firstError(r)) {
      failure = *e;
      break;
    }
    std::replace(live.begin(), live.end(), v, next);
    fromNames.push_back(v);
    toNames.push_back(next);
  }
  ojson out = ojson::array();
  // ONE block for the renames done (renvarlab's BUG-452 shape), then the stop, if any.
  if (!toNames.empty()) out.push_back(mutationResult(session, "rename", toNames, fromNames));
  if (failure.has_value()) {
    out.push_back(*failure);
    return out;
  }
  session.eval.rStrings.set("varlist", joinWords(toNames));
  eval::declareRPosted(session.eval, "renpfix", {"varlist"});
  return out;
}

}  // namespace piet::handlers
