// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/descriptive.h"
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
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation/estimation_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "engine/scratch_columns.h"          // cumul's if/in restriction and weight columns
#include "datamgmt/dataset_shape_support.h"
#include "datamgmt/dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

// ===========================================================================
// compare
// ===========================================================================

ojson runCompare(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCompare.known,
                                            kVocabCompare.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  // compare's syntax is `varname varname`, read as a varlist: the names expand
  // first (a missing one is r(111)), then one variable is r(102) "too few
  // variables specified" and three or more r(103) "too many variables
  // specified" (vs-stata bank: `compare price`, `compare price mpg weight`,
  // `compare t* mpg`).
  std::vector<std::string> cmpVars = parsed.varlist;
  if (!cmpVars.empty()) {
    std::string spec;
    for (const auto& t : parsed.varlist) spec += (spec.empty() ? "" : " ") + t;
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    cmpVars = ex.tokens;
    if (auto bad = loops::checkKnownVarlist(cmpVars, session)) return *bad;
  }
  if (cmpVars.size() == 1)
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, "r(102)"));
  if (cmpVars.size() > 2)
    return ojson(eval::errorBlock("syntax_error", "too many variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, "r(103)"));
  if (cmpVars.size() != 2)
    return rsSyntax("compare: syntax is `compare var1 var2`");
  const std::string v1 = cmpVars[0];
  const std::string v2 = cmpVars[1];

  // compare's sample is `mark touse `if' `in'`; the count of its Total line is the
  // r(N) it leaves (vs-stata bank: `compare price mpg in 1/20` r(N) 20, `if
  // foreign` 22, `if price > 1e6` 0).
  ReadFrame rf(session, {v1, v2});
  Datastore* ds = rf.get();
  std::unique_ptr<Datastore> filtered;
  const bool hasIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
  const bool hasIn = parsed.inRange.has_value() && !parsed.inRange->empty();
  if (hasIf || hasIn) {
    Datastore* full = session.datastore();
    if (full == nullptr) return ojson(eval::noDataLoaded());
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, full->path(), parsed.ifExpr, parsed.inRange);
    } catch (const CobraError& ce) {
      return ojson(eval::engineError(cobraErrorMessage(ce), ce.code));
    }
    if (!fr.ok) return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "compare");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }
  if (ds == nullptr)
    return rsError("compare: could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  for (const auto& v : {v1, v2}) {
    if (!ds->has(v)) return rsError("variable " + v + " not found");
  }
  // A string variable is read as text (missing = ""), a numeric one as numbers.
  const bool s1 = ds->dtype(v1) == "factor";
  const bool s2 = ds->dtype(v2) == "factor";
  auto textOf = [&](const std::string& v) {
    std::vector<std::string> out;
    const cecon::Column& c = ds->col(v);
    const int64_t rows = ds->nRows();
    out.reserve(static_cast<size_t>(rows));
    for (int64_t r = 0; r < rows; ++r) {
      const int32_t code = c.codes()[r];
      out.push_back(code >= 0 && static_cast<size_t>(code) < c.levels.size()
                        ? c.levels[static_cast<size_t>(code)]
                        : std::string());
    }
    return out;
  };
  std::vector<double> a, b;
  std::vector<std::string> ta, tb;
  if (s1) ta = textOf(v1); else a = columnAsDoubles(*ds, v1);
  if (s2) tb = textOf(v2); else b = columnAsDoubles(*ds, v2);
  const size_t n = static_cast<size_t>(ds->nRows());

  struct Bucket {
    int64_t count = 0;
    double lo = std::numeric_limits<double>::quiet_NaN();
    double hi = std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0;
    void add(double d) {
      if (count == 0 || d < lo) lo = d;
      if (count == 0 || d > hi) hi = d;
      sum += d;
      ++count;
    }
    void bump() { ++count; }
    double avg() const {
      return count ? sum / static_cast<double>(count) : std::numeric_limits<double>::quiet_NaN();
    }
  };
  Bucket lt, eq, gt, joint;
  int64_t missA = 0, missB = 0, missBoth = 0;
  const bool sameKind = s1 == s2;
  for (size_t i = 0; i < n; ++i) {
    const bool ma = s1 ? ta[i].empty() : std::isnan(a[i]);
    const bool mb = s2 ? tb[i].empty() : std::isnan(b[i]);
    if (ma && mb) {
      ++missBoth;
      continue;
    }
    // "var1 missing only" means var1 is missing and var2 is not — the counter
    // is named for the variable that IS missing, not for the one that survives.
    if (ma) {
      ++missA;
      continue;
    }
    if (mb) {
      ++missB;
      continue;
    }
    // The jointly defined count is all there is for two kinds of variable; two
    // strings are ordered as text, two numbers also by their difference.
    if (!sameKind) {
      joint.bump();
    } else if (s1) {
      joint.bump();
      const int cmp = ta[i].compare(tb[i]);
      if (cmp < 0) lt.bump();
      else if (cmp > 0) gt.bump();
      else eq.bump();
    } else {
      const double d = a[i] - b[i];
      joint.add(d);
      if (d < 0)
        lt.add(d);
      else if (d > 0)
        gt.add(d);
      else
        eq.bump();
    }
  }
  const bool textOnly = s1 || s2;
  const int64_t totalCount = joint.count + missA + missB + missBoth;
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", static_cast<double>(totalCount));
  eval::declareRPosted(session.eval, "compare", {"N"});

  auto num = [](double v) {
    if (std::isnan(v)) return std::string(".");
    return piet::fmtStataDefaultG(v);
  };
  auto line = [&](const std::string& label, const Bucket& bk, bool withStats) {
    char buf[256];
    if (!withStats || textOnly || bk.count == 0) {
      std::snprintf(buf, sizeof(buf), "%-22s %10lld\n", label.c_str(),
                    static_cast<long long>(bk.count));
      return std::string(buf);
    }
    std::snprintf(buf, sizeof(buf), "%-22s %10lld  %12s %12s %12s\n", label.c_str(),
                  static_cast<long long>(bk.count), num(bk.lo).c_str(), num(bk.avg()).c_str(),
                  num(bk.hi).c_str());
    return std::string(buf);
  };

  std::ostringstream oss;
  if (textOnly) {
    oss << "                            count\n";
    oss << "---------------------------------\n";
  } else {
    oss << "                                   ------- difference -------\n";
    oss << "                            count       minimum      average      maximum\n";
    oss << "--------------------------------------------------------------------------\n";
  }
  if (sameKind) {
    oss << line(v1 + "<" + v2, lt, true);
    oss << line(v1 + "=" + v2, eq, false);
    oss << line(v1 + ">" + v2, gt, true);
    oss << "                       ----------\n";
  }
  oss << line("   jointly defined", joint, true);
  if (missA > 0) {
    Bucket b1;
    b1.count = missA;
    oss << line("   " + v1 + " missing only", b1, false);
  }
  if (missB > 0) {
    Bucket b2;
    b2.count = missB;
    oss << line("   " + v2 + " missing only", b2, false);
  }
  if (missBoth > 0) {
    Bucket b3;
    b3.count = missBoth;
    oss << line("   jointly missing", b3, false);
  }
  oss << "                       ----------\n";
  {
    Bucket total;
    total.count = totalCount;
    oss << line("   total", total, false);
  }
  return displayBlock(oss.str());
}

}  // namespace piet::handlers
