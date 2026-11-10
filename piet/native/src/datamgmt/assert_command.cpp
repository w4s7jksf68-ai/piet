// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/descriptive.h"
#include "piet/eval/exprfns.h"  // B3(c): exprHasRunningSum

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "estimation/estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/counters.h"  // PP-L1 use-plane split counters (runUse)
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // BUG-471 T2: --store-type
#include "piet/strmutate.h"      // BUG-1326: assert's string-condition count
#include "core/exact_sum.h"  // cecon::ExactSum: summarize's accumulation (BUG-1013)
#include "estimators/posthest.h"  // fitted design atoms for named absorbed effects
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)
#include "io/columnar.h"  // cecon::hbin_level_unescape: .dta label text (BUG-1372)
#include "piet/strutil.h"
#include "piet/translate.h"
#include "engine/engine_call_internal.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

using namespace engine_call_detail;

// `assert EXP [if COND] [in RANGE]` — see handlers.h for why this delegates to
// runCount instead of evaluating the expression itself.
ojson runAssert(const std::string& expr, SessionState& session,
                const std::optional<std::string>& ifExpr,
                const std::optional<std::string>& inRange) {
  if (!session.hasData()) return eval::noDataLoaded();
  // A contradiction is a row where EXP is false, i.e. 0. Wrapping in `( ... )==0`
  // — rather than splicing a negation operator into the text — keeps the user's
  // own operator precedence intact, so `assert a == 1 | b == 2` negates the WHOLE
  // claim and not just its last term. A number's `==0` is its `!`, missing
  // included; a string claim (`assert g`, g a string) is a string compared with a
  // number, which the row-wise string evaluator (runCount's BUG-032 detour) calls
  // r(109) type mismatch, as StataBE 19.5 does (BUG-412 review).
  std::string negated = "(" + expr + ")==0";
  if (ifExpr.has_value() && !ifExpr->empty()) negated += " & (" + *ifExpr + ")";

  // `assert` is not r-class -- Stata leaves every r() value in place. The count
  // below runs on an empty r() store (so a stale r(N) can never pose as the
  // contradiction count) and the caller's r() scalars and macros are put back
  // afterwards on every exit path -- an error block, and an exception out of
  // runCount (fetchColumns, a Datastore open) alike: the scope guard below puts
  // them back. Restoring the saved COPIES also restores the stores' generations
  // (eval/session.h), so a posted r(table) survives `assert` as it does in Stata
  // (w2f_residuals.log:1752 `e_rtable_index_after_assert`). This used to
  // reset r(N) to the dataset row count instead: after `summarize x1 if x1>5`
  // (r(N) 32) an `assert x1>=0` turned r(N) into 60 at rc 0, and the changed
  // r() also retired a posted r(table) (its stamp no longer matched).
  // Oracle: w2f_residuals.log:1741 `PREC|e_assert_then_rn|0|32|1|` (r(N) of
  // `summarize x1 if x1>5` is still 32 after `assert x1 >= 0`).
  struct RRestore {
    eval::EvalSession& ev;
    const eval::OrderedFloatDict values;
    const eval::OrderedDict strings;
    ~RRestore() {
      ev.rValues = values;
      ev.rStrings = strings;
    }
  };
  ojson counted;
  std::optional<double> badCount;
  // BUG-1326 / BUG-1419 / BUG-464: a claim the engine's f64 evaluator cannot read -- a
  // concatenation (`assert scalar(s1) + "x" == "-3x"`), a
  // string variable inside a function (`assert !missing(s)`), a string
  // function, bofd("cal", d) -- is counted by the row evaluator `count if`
  // detours to (dispatcher count arm, BUG-032). assert went to runCount alone,
  // so the same expression `count if` answered was rc 1 here ("string/factor
  // variable used where a number is required", "function bofd() is not
  // supported"). A claim that row evaluator cannot read either -- it has no
  // subscript, so `assert x[_n-1] < x if s == "a"` -- is counted by runCount
  // below, as it always was (StataBE rc 0; the row evaluator alone was rc 1).
  // A failure the row evaluator names Stata's code for is the answer, not a
  // decline: a string used as a truth value (`assert s`, `assert upper(s)`,
  // `assert s & 1`) is r(109) type mismatch, as in StataBE.
  if (strmutate::needsStringCount(session, negated)) {
    strmutate::CountResult cr = strmutate::countRowsIf(session, negated, inRange);
    if (cr.ok) {
      badCount = static_cast<double>(cr.n);
    } else if (cr.inRangeError) {  // Stata's r(198) sentence, as runCount words it
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = cr.errMsg;
      return ojson(inRangeRejectError(fr, *inRange, "assert"));
    } else if (!cr.errReason.empty()) {
      return ojson(eval::errorBlock("engine_error", cr.errMsg, std::nullopt, std::nullopt,
                                    std::nullopt, cr.errReason));
    }
  }
  if (!badCount.has_value()) {
    const RRestore restore{session.eval, session.eval.rValues, session.eval.rStrings};
    session.eval.rValues.clear();
    counted = handlers::runCount(negated, session, inRange);
    // Read before the guard puts the caller's r() back.
    if (const double* bad = session.eval.rValues.find("N")) badCount = *bad;
  }
  // runCount surfaces a bad expression as an error block; pass it through
  // verbatim rather than reporting a contradiction count we never computed.
  if (counted.is_object() && counted.value("type", std::string()) == "error") return counted;

  if (!badCount.has_value()) {
    return engineErrorBlock("assert: could not evaluate `" + expr + "`", "engine_error");
  }
  const int64_t contradictions = static_cast<int64_t>(*badCount);
  if (contradictions == 0) return ojson::array();  // Stata prints nothing on success

  Datastore* ds = session.datastore();
  const int64_t total = (ds != nullptr) ? ds->nRows() : 0;
  return eval::errorBlock(
      "assertion_false",
      std::to_string(contradictions) + " contradiction" + (contradictions == 1 ? "" : "s") +
          " in " + std::to_string(total) + " observation" + (total == 1 ? "" : "s") +
          "\nassertion is false",
      std::nullopt, std::nullopt, std::nullopt, "r(9)");
}

}  // namespace piet::handlers
