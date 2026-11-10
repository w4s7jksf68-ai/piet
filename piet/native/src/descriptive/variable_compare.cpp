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
  if (auto q = refuseQualifiers(parsed, "compare")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 2)
    return rsSyntax("compare: syntax is `compare var1 var2`");
  const std::string& v1 = parsed.varlist[0];
  const std::string& v2 = parsed.varlist[1];

  ReadFrame rf(session, {v1, v2});
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return rsError("compare: could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  for (const auto& v : {v1, v2}) {
    if (!ds->has(v)) return rsError("variable " + v + " not found");
    if (ds->dtype(v) == "factor")
      return rsError("compare: `" + v +
                     "' is a string variable; compare works on numeric variables.");
  }
  const std::vector<double> a = columnAsDoubles(*ds, v1);
  const std::vector<double> b = columnAsDoubles(*ds, v2);
  const size_t n = a.size();

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
  for (size_t i = 0; i < n; ++i) {
    const bool ma = std::isnan(a[i]);
    const bool mb = std::isnan(b[i]);
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
    const double d = a[i] - b[i];
    joint.add(d);
    if (d < 0)
      lt.add(d);
    else if (d > 0)
      gt.add(d);
    else
      eq.bump();
  }

  auto num = [](double v) {
    if (std::isnan(v)) return std::string(".");
    return piet::fmtStataDefaultG(v);
  };
  auto line = [&](const std::string& label, const Bucket& bk, bool withStats) {
    char buf[256];
    if (!withStats || bk.count == 0) {
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
  oss << "                                   ------- difference -------\n";
  oss << "                            count       minimum      average      maximum\n";
  oss << "--------------------------------------------------------------------------\n";
  oss << line(v1 + "<" + v2, lt, true);
  oss << line(v1 + "=" + v2, eq, false);
  oss << line(v1 + ">" + v2, gt, true);
  oss << "                       ----------\n";
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
    total.count = static_cast<int64_t>(n);
    oss << line("   total", total, false);
  }
  return displayBlock(oss.str());
}

}  // namespace piet::handlers
