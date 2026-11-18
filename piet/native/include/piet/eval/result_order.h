// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// result_order.h -- lay r() out in the order Stata lists it.
//
// `return list` and `: r(scalars)` / `: r(macros)` list r() newest posting
// first (results/return_list.cpp, eval/macro_definitions.cpp), which is Stata's
// own rule for an rclass program (`return scalar a`, `b`, `c` list as c b a;
// vs-stata truth bank). A built-in command lists in its own fixed order
// (summarize: N sum_w mean Var sd min max sum), which a handler that posts in
// another order does not reproduce. orderRResults moves the named results so
// that the newest-first listing is `stataOrder`; a name that is not posted is
// skipped, and any other result keeps its place after them.
//
// A MOVE, never a re-post: each entry keeps the generation of its last write.
// r() still holds the previous command's results while a handler runs (they
// are dropped after it, by generation: handlers::replaceStaleRClass), so a
// re-post would make a left-over name -- r(chi2) of a `tabulate c d, chi2`, r(c)
// of a two-way table -- look written by THIS command and keep it alive through
// a `tabulate c d` / `tabulate c` that posts no such result.
#ifndef PIET_NATIVE_EVAL_RESULT_ORDER_H
#define PIET_NATIVE_EVAL_RESULT_ORDER_H

#include <string>
#include <vector>

#include "piet/eval/session.h"

namespace piet::eval {

inline void orderRResults(EvalSession& ev, const std::vector<std::string>& stataOrder) {
  // Last in the store is listed first: move to the end in reverse of the wanted
  // listing. A hidden mark is keyed on the entry's generation, which a move keeps.
  for (auto it = stataOrder.rbegin(); it != stataOrder.rend(); ++it) {
    const std::string& name = *it;
    if (ev.rValues.moveToEnd(name)) continue;
    if (!isReservedStoreKey(name)) ev.rStrings.moveToEnd(name);
  }
}

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_RESULT_ORDER_H
