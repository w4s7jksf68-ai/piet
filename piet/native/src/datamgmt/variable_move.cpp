// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `move varname1 varname2`: put one variable in another's position. Forwards to
// `runOrder`; the measurement below is why it is not `order, before()` alone.
//
// Measured against StataNow 19.5 BE with the packages installed on this machine.
// The plan for this bundle got it wrong, and the measurement is what caught it:
//
//   * The plan said `move v1 v2` is `order v1, before(v2)`. It is that in ONE
//     direction only. `move` puts v1 in v2's POSITION, so with `a b c`,
//     `move c a` gives `c a b` (before) but `move a c` gives `b c a` (after).
//     A before()-always reuse answers `b a c` for the second -- a silently
//     different column order, which is exactly the kind of wrong answer that
//     survives every gate because nothing crashes.
#include "piet/handlers/variables.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "variable_rename_move_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

using namespace variable_rename_move_detail;

// ===========================================================================
// move
// ===========================================================================
//
//     move varname1 varname2
//
// Puts varname1 in varname2's POSITION. Direction matters and the plan's
// `order v1, before(v2)` is only half of it -- see the file header for the
// measured table. Implemented as the two order() forms rather than as a private
// permutation so the Frame-level column move, the resident card invalidation
// and the mutation result all stay in one place.
ojson runMove(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, {}, {})) return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // Measured rcs: one name is r(102) `too few variables specified`, three is
  // r(103) `too many variables specified`.
  if (parsed.varlist.size() < 2) return k1ErrorRc("too few variables specified", 102);
  if (parsed.varlist.size() > 2) return k1ErrorRc("too many variables specified", 103);

  const std::string& from = parsed.varlist[0];
  const std::string& to = parsed.varlist[1];

  std::vector<std::string> names;
  ojson err;
  if (!columnNames(session, &names, &err)) return err;

  const auto itFrom = std::find(names.begin(), names.end(), from);
  const auto itTo = std::find(names.begin(), names.end(), to);
  // r(111) `variable zz not found`, measured.
  if (itFrom == names.end()) return k1ErrorRc("variable " + from + " not found", 111);
  if (itTo == names.end()) return k1ErrorRc("variable " + to + " not found", 111);
  // `move a a` is rc 0 and changes nothing, measured.
  if (itFrom == itTo) return silent();

  // v1 is BEFORE v2 -> v1 lands where v2 is, so it ends up AFTER it once v2 has
  // shifted left. v1 is AFTER v2 -> v1 lands where v2 is and v2 shifts right.
  const bool fromIsEarlier = itFrom < itTo;
  std::vector<std::pair<std::string, eval::OptValue>> opts;
  opts.emplace_back(fromIsEarlier ? "after" : "before", eval::OptValue::ofString(to));
  return runOrder({from}, opts, session);
}
}  // namespace piet::handlers
