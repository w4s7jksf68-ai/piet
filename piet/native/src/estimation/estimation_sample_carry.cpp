// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/eval/tokenizer.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
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

#include "estimation_internal.h"  // e(sample) T2: the esample helpers' declarations
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

// ---------------------------------------------------------------------------
// e(sample) T2, the data verbs that build new observations out of old ones. The
// mask is a column, so it follows rows through every verb that only reorders,
// drops or appends them; these helpers serve the verbs whose output rows are
// COPIES or GROUPS of input rows. The rule each follows is StataBE 19.5's,
// archived by the supervisor's stata_capture job (2026-10-13, 22 PREC, 0
// PRECFAIL) at internal capture
// w2_esample_copies.log from internal script w2_esample_copies.do
// (records cited by name):
//   expand / cross: the original observation keeps its marker, every created
//     copy reads 0 (X1 `old=23|created=0`, X3b `k1=23|k2=0|k3=0`, and
//     w2_esample.log:1076 E6 `0|0|28|67|`). fillin needs nothing: its created
//     rows carry a missing cell, which reads 0 (X5 `created=0`).
//   collapse / contract with by-groups, data in by-group order: a result row
//     carries the marker of its group's FIRST observation in the current order
//     (K1, K2, K4, K6, K7 first_diff=0; K8 = w2_esample.log:1193 E10 `3`).
//     collapse without by() reads 0 (K5 `|0|1|` with the first row in).
//   merge m:1 / 1:1: no master row is copied; using-only rows carry a missing
//     cell, which reads 0 (M2 `23|62|using_only=0|old_m0_diff=0`). xpose and
//     stack replace the data and read 0 everywhere (X6 `0|4`, X7/X7b `0|122`).
//   merge 1:m and joinby: one copy of each master row keeps its marker, the
//     others read 0 (M1 `0|9|0|9|22|matched=9|not_first_of_run=0|`, X4
//     `0|19|72|m0=38|m0_diff=19|`); PIET keeps the first copy
//     (esampleKeepFirstCopy below). Which joinby copy Stata marks follows its
//     sort tie order (X4b `not_first_of_run=8`); the count does not.
//   bsample, and collapse/contract of data NOT in by-group order with a group
//     whose rows disagree: Stata's answer depends on its own random draw or
//     tie order (X2, K3 first_diff=2) -> EsampleLostBy (datastore.h).
// The archived capture is the golden aer_w2 case w2_esample_copies
// (tests/eval/golden/aer_w2/w2_esample_copies.log, landed d6613eff).

// The working frame's mask cells, or nullopt when it carries none.
std::optional<std::vector<double>> esampleHeadMask(SessionState& session) {
  if (session.esampleColumnInstalls == 0 || !session.currentHbin().has_value()) return std::nullopt;
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (ds == nullptr || !ds->has(kEsampleColumn)) return std::nullopt;
  const cecon::Column& c = ds->col(kEsampleColumn);
  if (c.dtype != cecon::DType::F64) return std::nullopt;
  const double* v = c.f64();
  return std::vector<double>(v, v + ds->nRows());
}

// Writes `cells` (one per row of the working frame) as its mask column and
// adopts the result as the head. Part of the verb's own data change, so no block.
// False when the engine refused; the caller then names the failure.
bool esampleRewriteHeadMask(SessionState& session, const std::vector<double>& cells) {
  if (!session.currentHbin().has_value()) return false;
  const std::string hb = mintScratchPath(session.workdir(), "esample_cells");
  forgetScratchMint(hb);
  const std::string valuesPath = hb.substr(0, hb.size() - 5) + ".txt";  // 5 == strlen(".hbin")
  registerScratchMint(valuesPath);
  struct Unlink {
    const std::string& p;
    ~Unlink() { cecon::native_unlink(p); }
  } unlinkValues{valuesPath};
  {
    std::string body;
    body.reserve(cells.size() * 3);
    char buf[32];
    for (double v : cells) {
      if (std::isnan(v)) {
        body += ".\n";
        continue;
      }
      std::snprintf(buf, sizeof buf, "%.17g\n", v);
      body += buf;
    }
    std::ofstream o(cecon::to_native_path(valuesPath), std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << body;
    o.flush();
    if (!o) return false;
  }
  session.ensureMaterializedHead("esample-rewrite");
  const std::string out = nextPath(session.workdir(), "esample_rewrite");
  CobraRunResult r;
  {
    SessionState::ResidentEligible resident(session);
    r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", kEsampleColumn, "--values",
                  valuesPath, "--dtype", "f64", "--new", "--out", out});
  }
  if (!r.ok) return false;
  if (!session.residentFrameLive() && !cecon::native_exists(out)) return false;  // BUG-124
  const std::string before = *session.currentHbin();
  session.load(out);
  session.carryCardAcrossHiddenInstall(before, out);
  return true;
}

bool esampleMarkHeadLost(SessionState& session, EsampleLostBy by) {
  const std::optional<std::vector<double>> cells = esampleHeadMask(session);
  if (!cells.has_value()) return true;
  return esampleRewriteHeadMask(
      session, std::vector<double>(cells->size(), -static_cast<double>(static_cast<int>(by))));
}

namespace {
// One by-group key cell: a number by its bits (-0 folded into 0, every missing
// one key), a factor by its level text (the result's dictionary is rebuilt).
std::string esampleKeyCell(const cecon::Column& c, int64_t i) {
  if (c.dtype == cecon::DType::Factor) {
    const int32_t code = c.codes()[i];
    return code < 0 || static_cast<size_t>(code) >= c.levels.size()
               ? std::string("s")
               : "s" + std::string(c.levels[static_cast<size_t>(code)]);
  }
  double v = c.f64()[i];
  if (std::isnan(v)) return "m";
  if (v == 0.0) v = 0.0;
  char buf[24];
  std::snprintf(buf, sizeof buf, "n%a", v);
  return buf;
}
// Stata's sort order of two cells of one by-variable: numbers ascending, then
// missing; strings by bytes ("" first). <0, 0, >0.
int esampleKeyCompare(const cecon::Column& c, int64_t i, int64_t j) {
  if (c.dtype == cecon::DType::Factor) {
    auto text = [&](int64_t r) -> std::string {
      const int32_t code = c.codes()[r];
      return code < 0 || static_cast<size_t>(code) >= c.levels.size()
                 ? std::string()
                 : std::string(c.levels[static_cast<size_t>(code)]);
    };
    return text(i).compare(text(j));
  }
  const double a = c.f64()[i], b = c.f64()[j];
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) - std::isnan(b);
  return a < b ? -1 : (a > b ? 1 : 0);
}
}  // namespace

EsampleGroupMarks esampleGroupMarksBefore(SessionState& session,
                                          const std::vector<std::string>& byVars,
                                          bool restricted) {
  EsampleGroupMarks g;
  const std::optional<std::vector<double>> cells = esampleHeadMask(session);
  if (!cells.has_value() || byVars.empty()) return g;  // nothing to carry (no by(): reads 0)
  g.active = true;
  Datastore* ds = session.datastore();
  std::vector<const cecon::Column*> cols;
  for (const auto& v : byVars) {
    if (!ds->has(v)) {
      g.lost = true;  // a by-list this check cannot read (abbreviation, wildcard)
      return g;
    }
    cols.push_back(&ds->col(v));
  }
  const int64_t n = ds->nRows();
  bool ordered = true;
  for (int64_t i = 1; i < n && ordered; ++i) {
    for (const cecon::Column* c : cols) {
      const int d = esampleKeyCompare(*c, i - 1, i);
      if (d < 0) break;
      if (d > 0) {
        ordered = false;
        break;
      }
    }
  }
  std::map<std::string, bool> mixed;
  for (int64_t i = 0; i < n; ++i) {
    std::string key;
    for (const cecon::Column* c : cols) {
      key += esampleKeyCell(*c, i);
      key += '\x1f';
    }
    const double v = (*cells)[static_cast<size_t>(i)];
    auto it = g.byKey.find(key);
    if (it == g.byKey.end()) {
      g.byKey.emplace(key, v);  // the group's first row in the current order
      mixed.emplace(key, false);
    } else if (!(it->second == v || (std::isnan(it->second) && std::isnan(v)))) {
      mixed[key] = true;
    }
  }
  // A group whose rows all agree has one answer under any order and any subset;
  // a mixed one is answered by its first row only when that row is first for
  // Stata too (data in by-group order) and every row of it entered the verb.
  if (!ordered || restricted) {
    for (const auto& [k, m] : mixed) {
      (void)k;
      if (m) {
        g.lost = true;
        break;
      }
    }
  }
  return g;
}

bool esampleGroupMarksAfter(SessionState& session, const EsampleGroupMarks& g,
                            const std::vector<std::string>& byVars, EsampleLostBy by) {
  if (!g.active) return true;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return false;
  const int64_t n = ds->nRows();
  std::vector<double> out(static_cast<size_t>(n), 0.0);
  if (g.lost) {
    std::fill(out.begin(), out.end(), -static_cast<double>(static_cast<int>(by)));
  } else {
    std::vector<const cecon::Column*> cols;
    for (const auto& v : byVars) {
      if (!ds->has(v)) return false;
      cols.push_back(&ds->col(v));
    }
    for (int64_t i = 0; i < n; ++i) {
      std::string key;
      for (const cecon::Column* c : cols) {
        key += esampleKeyCell(*c, i);
        key += '\x1f';
      }
      const auto it = g.byKey.find(key);
      if (it != g.byKey.end()) out[static_cast<size_t>(i)] = it->second;
    }
  }
  return esampleRewriteHeadMask(session, out);
}

// FU-496 / FU-602 review: merge 1:m and joinby copy a master row once per
// matching using row. StataBE keeps the marker on ONE copy of each master row
// and reads 0 on the others, so `count if e(sample)` stays e(N)
// (w2_esample_copies.log:596 M1 `0|9|0|9|22|matched=9|not_first_of_run=0|`;
// :470 X4 `0|19|72|m0=38|m0_diff=19|`: 38 copies of the 19 marked rows, 19
// marked). merge 1:m marks the first copy in merge's output order (M1
// `not_first_of_run=0`). joinby expands the master and re-sorts the copies
// with a plain `sort`, so which copy keeps it follows
// Stata's tie order (X4b `k0=8|k1=11|not_first_of_run=8`): any one copy is a
// realization of that order, as a tied row's position after `sort` is, and
// PIET keeps the first, like merge. A copy's cell becomes 0 only when it was
// a marked 1; 0, missing (using-only rows) and a withdrawn marker's negative
// code are kept. The copies of one master row are found by key:
//  * blockLen empty (merge 1:m): the master is unique on the key, so every
//    master-origin row (non-missing cell) of a key is a copy of one row.
//  * blockLen given (joinby): the engine emits each master row's matches as one
//    consecutive run in using order (src/data/frame2.cpp Frame::joinby), of
//    blockLen[key] rows; a run's first row is the master row's first copy.
// False when the frame's key columns cannot be read; the caller then withdraws
// the marker by name.
bool esampleKeepFirstCopy(SessionState& session, const std::vector<std::string>& keys,
                          const std::map<std::string, int64_t>* blockLen) {
  std::optional<std::vector<double>> cells = esampleHeadMask(session);
  if (!cells.has_value()) return true;
  Datastore* ds = session.datastore();
  if (ds == nullptr || keys.empty()) return false;
  std::vector<const cecon::Column*> cols;
  for (const auto& k : keys) {
    if (!ds->has(k)) return false;
    cols.push_back(&ds->col(k));
  }
  const int64_t n = ds->nRows();
  auto keyOf = [&](int64_t i) {
    std::string key;
    for (const cecon::Column* c : cols) {
      key += esampleKeyCell(*c, i);
      key += '\x1f';
    }
    return key;
  };
  bool changed = false;
  auto demote = [&](int64_t i) {
    double& v = (*cells)[static_cast<size_t>(i)];
    if (v > 0) {
      v = 0.0;
      changed = true;
    }
  };
  if (blockLen == nullptr) {
    std::set<std::string> seen;
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan((*cells)[static_cast<size_t>(i)])) continue;
      if (!seen.insert(keyOf(i)).second) demote(i);
    }
  } else {
    for (int64_t i = 0; i < n;) {
      const auto it = blockLen->find(keyOf(i));
      if (it == blockLen->end() || it->second < 1 || i + it->second > n) return false;
      for (int64_t j = i + 1; j < i + it->second; ++j) demote(j);
      i += it->second;
    }
  }
  return !changed || esampleRewriteHeadMask(session, *cells);
}

// joinby's run lengths: the using rows per key, keyed as esampleKeepFirstCopy
// keys the result's rows.
std::map<std::string, int64_t> esampleUsingRunLengths(const Datastore& usingDs,
                                                      const std::vector<std::string>& keys) {
  std::map<std::string, int64_t> out;
  std::vector<const cecon::Column*> cols;
  for (const auto& k : keys) {
    if (!usingDs.has(k)) return {};
    cols.push_back(&usingDs.col(k));
  }
  for (int64_t i = 0; i < usingDs.nRows(); ++i) {
    std::string key;
    for (const cecon::Column* c : cols) {
      key += esampleKeyCell(*c, i);
      key += '\x1f';
    }
    ++out[key];
  }
  return out;
}

}  // namespace piet::handlers
