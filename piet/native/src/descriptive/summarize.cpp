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
#include <functional>
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
#include "descriptive/summarize_prefetch.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

using namespace engine_call_detail;

// ---------------------------------------------------------------------------
// summarize

// The handed-over `eval --summarize` answer (summarize_prefetch.h says why and when).
// thread_local: a session's commands run on one thread, and nothing is ever handed
// from one thread to another.
namespace {
struct SummarizePrefetch {
  bool armed = false;
  std::string path, var;
  ojson stats;
};
SummarizePrefetch& summarizePrefetchSlot() {
  static thread_local SummarizePrefetch slot;
  return slot;
}
}  // namespace

void summarizePrefetchSet(const std::string& path, const std::string& var, ojson stats) {
  SummarizePrefetch& p = summarizePrefetchSlot();
  p.armed = true;
  p.path = path;
  p.var = var;
  p.stats = std::move(stats);
}

std::optional<ojson> summarizePrefetchTake(const std::string& path, const std::string& var) {
  SummarizePrefetch& p = summarizePrefetchSlot();
  if (!p.armed || p.path != path || p.var != var) return std::nullopt;
  p.armed = false;
  return std::move(p.stats);
}

void summarizePrefetchClear() {
  SummarizePrefetch& p = summarizePrefetchSlot();
  p.armed = false;
  p.stats = ojson();
}

namespace {

ojson numpyStats(const std::vector<double>& valid, bool detail) {
  int64_t n = static_cast<int64_t>(valid.size());
  ojson s;
  if (n == 0) {
    s["N"] = 0;
    s["mean"] = nullptr;
    s["sd"] = nullptr;
    s["min"] = nullptr;
    s["max"] = nullptr;
    // BUG-344: count and sum stay well-defined on a degenerate (all-missing) column —
    // Stata posts r(N)=0 and r(sum)=0 there, NOT missing (the same rule cmds_summary.cpp
    // already follows for by-groups). Only mean/sd/min/max read as missing.
    s["sum"] = 0.0;
    s["sum_w"] = 0.0;
    return s;
  }
  // BUG-1013: Stata's accumulation (core/exact_sum.h), the engine's own rule.
  cecon::ExactSum acc;
  for (double v : valid) acc.add(v);
  const double sum = acc.value();
  double mean = sum / static_cast<double>(n);
  double sd = 0.0;
  if (n > 1) {
    double ss = 0.0;
    for (double v : valid) ss += (v - mean) * (v - mean);
    sd = std::sqrt(ss / static_cast<double>(n - 1));
  }
  double mn = *std::min_element(valid.begin(), valid.end());
  double mx = *std::max_element(valid.begin(), valid.end());
  s["N"] = n;
  s["mean"] = mean;
  s["sd"] = sd;
  s["min"] = mn;
  s["max"] = mx;
  // BUG-344: this path already summed the column to get the mean — it just never
  // published the total, so captureRFromBlock's r(sum)/r(sum_w) mapping (which has
  // been waiting for these keys all along) found nothing to map. This fallback is
  // the UNWEIGHTED path by construction (the weighted arm errors rather than falling
  // back here), hence sum_w = N.
  s["sum"] = sum;
  s["sum_w"] = static_cast<double>(n);
  if (detail) {
    std::vector<double> sorted = valid;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> double {
      // numpy.percentile linear-interpolation method (matches _numpy_stats' np.percentile default).
      double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
      int64_t lo = static_cast<int64_t>(std::floor(idx));
      int64_t hi = static_cast<int64_t>(std::ceil(idx));
      if (lo == hi) return sorted[static_cast<size_t>(lo)];
      double frac = idx - static_cast<double>(lo);
      return sorted[static_cast<size_t>(lo)] * (1.0 - frac) + sorted[static_cast<size_t>(hi)] * frac;
    };
    s["p1"] = pct(1);
    s["p25"] = pct(25);
    s["p50"] = pct(50);
    s["p75"] = pct(75);
    s["p99"] = pct(99);
  }
  return s;
}

}  // namespace

// BS-092 (BETA campaign, Phase G): the pure per-Datastore stats computation
// runSummarize below already did, hoisted into its own function so
// dispatcher.cpp's new `by:`/`bysort: summarize` grouped-dispatch loop can
// invoke the EXACT SAME computation once per by-group's own scratch slice,
// rather than re-deriving/duplicating this logic. Depends on nothing but
// `ds` itself (no SessionState, no ifExpr/inRange — those are already
// resolved into whichever Datastore the caller hands in), so it composes
// cleanly with BS-090's if/in prefilter (runSummarize resolves `ds` first,
// same as before) and BS-092's by-group slicing (dispatcher.cpp resolves
// `ds` to one group's slice) alike.
// P3 weights wave: the actual per-Datastore stats computation, parameterized
// on which cobra weight FLAG to push ("--fweight" vs "--aweight") — shared by
// summarizeCoreStats (below, [fw=] path, unchanged signature/behavior for
// freeze-preservation) and summarizeCoreStatsAweight (the new [aw=] path;
// registry.cpp's summarize row is now AweightFweight). piet/handlers.h is
// out of this batch's edit manifest (owned by a different stint) and its
// summarizeCoreStats/runSummarize declarations are frozen by existing
// callers (dispatcher.cpp's by-group summarize loop, describe's internal
// stats pull) — this helper stays file-local (not header-declared) so those
// two public signatures never change; only their SHARED body moved here.
namespace {

// What the computation below asks of the dataset: which columns exist, their base type, and the
// path the engine is to read. A Datastore answers all of it; so does the resident card of a head
// that is not on disk yet (summarizeResidentHead), which is the point: neither question needs
// the file. `ds` is the open file when there is one. It is null on the card path, where an
// engine refusal is not answered here at all: the caller is told to take the Datastore path.
struct SummarizeCatalog {
  std::function<bool(const std::string&)> has;
  std::function<std::string(const std::string&)> dtype;  // "f64" | "factor"
  std::function<std::vector<std::string>()> names;       // every column, dataset order
  std::string path;
  Datastore* ds = nullptr;
};

// nullopt only when the catalog has no open file and the engine refused the call.
std::optional<ojson> summarizeCore(const SummarizeCatalog& cat,
                                   const std::optional<std::vector<std::string>>& varlist,
                                   bool detail, const std::optional<std::string>& weightVar,
                                   const char* weightFlag) {
  if (varlist.has_value()) {
    for (const auto& n : *varlist) {
      if (!cat.has(n)) {
        return engineErrorBlock("variable " + n + " not found");
      }
    }
  }
  std::vector<std::string> names = varlist.has_value() ? *varlist : cat.names();
  std::vector<std::string> f64Vars, facVars;
  for (const auto& n : names) {
    if (!cat.has(n)) continue;
    (cat.dtype(n) == "f64" ? f64Vars : facVars).push_back(n);
  }

  ojson stats = ojson::object();

  if (!f64Vars.empty()) {
    std::string joined;
    for (size_t i = 0; i < f64Vars.size(); ++i) {
      if (i) joined += ",";
      joined += f64Vars[i];
    }
    std::vector<std::string> args = {"eval", "--data", cat.path, "--summarize", joined};
    if (detail) args.push_back("--detail");
    bool hasWeight = weightVar.has_value() && !weightVar->empty();
    if (hasWeight) {
      args.push_back(weightFlag);
      args.push_back(*weightVar);
    }
    bool usedCobra = false;
    try {
      // egen's own request may already have asked exactly this (summarize_prefetch.h).
      std::optional<ojson> handed;
      if (!detail && !hasWeight && f64Vars.size() == 1) handed = summarizePrefetchTake(cat.path, joined);
      ojson raw;
      if (handed.has_value()) {
        raw = ojson::object();
        raw[joined] = std::move(*handed);
      } else {
        ojson out = runCobraJson(args);
        raw = out.value("summarize", ojson::object());
      }
      for (const auto& v : f64Vars) {
        if (raw.contains(v)) {
          stats[v] = raw[v];
        }
      }
      usedCobra = true;
    } catch (const CobraError& ce) {
      // No open file behind this catalog (summarizeResidentHead): whatever the engine refused,
      // the ordinary path opens the head and asks again, and ITS answer is the one that stands.
      if (cat.ds == nullptr) return std::nullopt;
      // gate-①: numpyStats below is UNWEIGHTED. Falling back to it silently
      // when a weight was requested would answer a different question than
      // the one asked (silent-wrong-number parity violation) — worse than
      // erroring. Only the no-weight path is allowed to use this fallback;
      // with a weight present, propagate the cobra failure instead.
      if (hasWeight) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      usedCobra = false;
    }
    if (!usedCobra) {
      for (const auto& v : f64Vars) {
        const cecon::Column& c = cat.ds->col(v);
        const double* d = c.f64();
        int64_t count = c.nbytes / static_cast<int64_t>(sizeof(double));
        std::vector<double> valid;
        valid.reserve(count);
        for (int64_t i = 0; i < count; ++i) {
          if (!std::isnan(d[i])) valid.push_back(d[i]);
        }
        stats[v] = numpyStats(valid, detail);
      }
    }
  }

  // A string variable has no numeric observation: Stata's summarize answers it
  // with N 0, sum_w 0 and sum 0 and every moment missing (BUG-1013; StataBE 19.5
  // w2as_summary_stats_family sx_sum_str). It used to count the non-empty
  // strings into r(N) (23 there) and post no r(sum_w)/r(sum).
  for (const auto& v : facVars) {
    ojson s;
    s["N"] = 0;
    s["mean"] = nullptr;
    s["sd"] = nullptr;
    s["min"] = nullptr;
    s["max"] = nullptr;
    s["sum"] = 0.0;
    s["sum_w"] = 0.0;
    stats[v] = s;
  }
  // The rows (and r(), which is the LAST variable's) follow the varlist order,
  // strings in their place: `summarize make price` posts price's r(N) 74 and
  // `summarize` on auto foreign's (vs-stata bank); grouping the strings last
  // made every varlist with a string read r(N) 0.
  if (!facVars.empty() && !f64Vars.empty()) {
    ojson ordered = ojson::object();
    for (const auto& n : names)
      if (stats.contains(n) && !ordered.contains(n)) ordered[n] = stats[n];
    stats = std::move(ordered);
  }

  ojson out;
  out["type"] = "summarize";
  out["detail"] = detail;
  out["stats"] = stats;
  return out;
}

// `summarize` of a resident head that is NOT on disk yet, answered without opening it.
//
// runSummarize used to start with session.datastore(), which first writes the whole resident
// frame out, and then read from that Datastore only names, types and a path: the statistics
// themselves come from the engine, which answers them from the frame it already holds. So every
// `summarize` that followed a `generate`/`replace` cost one full write of the dataset plus the
// scratch reclaim that write sets off, and egen pays it on every call because it runs a quiet
// `summarize` to leave what egen.ado leaves in r(). On the AEA package openICPSR-226844
// (39 runs of a 1,000-iteration loop with two egen each) that was 78,002 frame snapshots.
//
// The resident card carries the same three facts (row_evaluation.cpp factorColumns documents
// why its name set, order and "f64"|"factor" vocabulary can be relied on), and a virtual head's
// token is the path the engine expects for the frame it holds. nullopt = take the ordinary
// path: the head is on disk already (opening it writes nothing), the card is not current, the
// card spells a type this code does not know, or the engine call failed and the in-process
// fallback, which reads the file, has to run.
std::optional<ojson> summarizeResidentHead(SessionState& session,
                                           const std::optional<std::vector<std::string>>& varlist,
                                           bool detail, const std::optional<std::string>& weightVar,
                                           const char* weightFlag) {
  if (!session.residentHeadDirty() || !session.currentHbin().has_value()) return std::nullopt;
  const std::vector<VarInfo>* card = session.residentReadCard();
  if (card == nullptr) return std::nullopt;
  for (const auto& v : *card)
    if (v.dtype != "f64" && v.dtype != "factor") return std::nullopt;
  auto find = [card](const std::string& name) -> const VarInfo* {
    for (const auto& v : *card)
      if (v.name == name) return &v;
    return nullptr;
  };
  SummarizeCatalog cat;
  cat.has = [&find](const std::string& n) { return find(n) != nullptr; };
  cat.dtype = [&find](const std::string& n) { return find(n)->dtype; };
  cat.names = [card]() {
    std::vector<std::string> all;
    all.reserve(card->size());
    for (const auto& v : *card) all.push_back(v.name);
    return all;
  };
  cat.path = *session.currentHbin();
  return summarizeCore(cat, varlist, detail, weightVar, weightFlag);
}

}  // namespace

ojson summarizeCoreStatsWeighted(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                                  bool detail, const std::optional<std::string>& weightVar,
                                  const char* weightFlag) {
  SummarizeCatalog cat;
  cat.has = [&ds](const std::string& n) { return ds.has(n); };
  cat.dtype = [&ds](const std::string& n) { return ds.dtype(n); };
  cat.names = [&ds]() { return ds.names(); };
  cat.path = ds.path();
  cat.ds = &ds;
  return *summarizeCore(cat, varlist, detail, weightVar, weightFlag);
}

ojson summarizeCoreStats(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                         bool detail, const std::optional<std::string>& weightVar) {
  return summarizeCoreStatsWeighted(ds, varlist, detail, weightVar, "--fweight");
}

// ONESHOT-LASTFLUSH: what `summarize VARLIST if COND` needs on disk is VARLIST plus
// whatever the condition names — never the whole frame. Filtering the head instead
// meant flushing the entire resident frame first (28 of the flushes left after the
// §38 wave, every one of them a caller that only reads a handful of columns), so
// fetch the projection and filter THAT. Returns the path the row filter should run
// against; every refusal returns the head, which is the pre-existing behavior.
//
// `summarize` with no varlist reads ds.names() — every column — so it is excluded
// by the first test rather than by hoping the scanner finds nothing.
std::string summarizeFilterBase(SessionState& session,
                                const std::optional<std::vector<std::string>>& varlist,
                                const std::optional<std::string>& weightVar,
                                const std::optional<std::string>& ifExpr) {
  const std::string head = *session.currentHbin();
  if (!varlist.has_value() || varlist->empty()) return head;
  std::string named;
  for (const auto& v : *varlist) named += v + " ";
  if (weightVar.has_value()) named += *weightVar;
  auto cols = referencedColumns(session, {&named, ifExpr.has_value() ? &*ifExpr : nullptr}, "");
  if (!cols.has_value()) return head;
  std::string projPath;
  std::unique_ptr<Datastore> proj = fetchColumns(session, *cols, session.workdir(), &projPath);
  if (!proj) return head;
  // The scanner only takes names the shadow card confirms, so a varlist entry the
  // card does not know about is simply absent from the projection. Summarizing it
  // would then report "variable not found" for a column that DOES exist — a wrong
  // answer, not a slow one. Prove the projection carries the whole request instead.
  for (const auto& v : *varlist)
    if (!proj->has(v)) return head;
  if (weightVar.has_value() && !weightVar->empty() && !proj->has(*weightVar)) return head;
  return projPath;
}

// P3 weights wave: the [aw=] sibling of summarizeCoreStats above — same
// computation, --aweight instead of --fweight. NOT declared in
// piet/handlers.h (out of this batch's edit manifest); forward-declared
// locally where dispatcher.cpp's summarize arm needs it.
ojson summarizeCoreStatsAweight(Datastore& ds, const std::optional<std::vector<std::string>>& varlist,
                                bool detail, const std::optional<std::string>& weightVar) {
  return summarizeCoreStatsWeighted(ds, varlist, detail, weightVar, "--aweight");
}

ojson runSummarize(const std::optional<std::vector<std::string>>& varlist, bool detail,
                   SessionState& session, const std::optional<std::string>& ifExpr,
                   const std::optional<std::string>& weightVar,
                   const std::optional<std::string>& inRange) {
  // `summarize ... if COND [in RANGE]`: apply the row filter FIRST (BS-090:
  // `if`/`in` composed via AND, piet::handlers::applyIfAndInFilter) into a
  // transient hbin, then summarize the filtered data with the ordinary path
  // below. This keeps the numeric (cobra --summarize) and factor/fallback
  // paths seeing the SAME restricted sample, and reuses the validated
  // keepif rather than re-deriving a row mask natively.
  std::string sumifPath;
  struct FileGuard {
    const std::string& p;
    ~FileGuard() { if (!p.empty()) cecon::native_unlink(p); }
  } guard{sumifPath};
  std::unique_ptr<Datastore> tempDs;  // destroyed before `guard` unlinks the file
  Datastore* ds = nullptr;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (haveIf || haveIn) {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, summarizeFilterBase(session, varlist, weightVar, ifExpr),
                              ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "summarize");
    sumifPath = fr.dataPath;
    tempDs = std::make_unique<Datastore>(sumifPath);
    ds = tempDs.get();
  } else {
    if (auto resident = summarizeResidentHead(session, varlist, detail, weightVar, "--fweight"))
      return *resident;
    ds = session.datastore();
  }
  if (ds == nullptr) return eval::noDataLoaded();

  // ONESHOT-LASTFLUSH: when `ds` is the filtered temp, the stats call below reads a
  // file that is NOT the session head — exactly the case StatelessRead exists for.
  // Routed to the resident plane it frame-uses the temp, which evicts the real head
  // and snapshots it on the way out: the flush the projection above just avoided,
  // moved one call later and renamed "resident-resync". Measured that way first —
  // 28 flushes in, 28 flushes out, no win at all until this guard was added.
  std::optional<SessionState::StatelessRead> statsStateless;
  if (tempDs) statsStateless.emplace(session);
  return summarizeCoreStats(*ds, varlist, detail, weightVar);
}

// P3 weights wave: the [aw=] sibling of runSummarize above — identical if/in
// pre-materialization, routes to summarizeCoreStatsAweight instead. NOT
// declared in piet/handlers.h (out of this batch's edit manifest); forward-
// declared locally in dispatcher.cpp, which picks this vs. runSummarize off
// parsed.weightType (aw/aweight vs fw/fweight — registry.cpp's summarize row
// is AweightFweight, so those are the only two spellings that reach here).
ojson runSummarizeAweight(const std::optional<std::vector<std::string>>& varlist, bool detail,
                          SessionState& session, const std::optional<std::string>& ifExpr,
                          const std::optional<std::string>& weightVar,
                          const std::optional<std::string>& inRange, const char* weightFlag) {
  std::string sumifPath;
  struct FileGuard {
    const std::string& p;
    ~FileGuard() { if (!p.empty()) cecon::native_unlink(p); }
  } guard{sumifPath};
  std::unique_ptr<Datastore> tempDs;  // destroyed before `guard` unlinks the file
  Datastore* ds = nullptr;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (haveIf || haveIn) {
    if (!session.currentHbin().has_value()) return eval::noDataLoaded();
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, summarizeFilterBase(session, varlist, weightVar, ifExpr),
                              ifExpr, inRange);
    } catch (const CobraError& ce) {
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, *inRange, "summarize");
    sumifPath = fr.dataPath;
    tempDs = std::make_unique<Datastore>(sumifPath);
    ds = tempDs.get();
  } else {
    if (auto resident = summarizeResidentHead(session, varlist, detail, weightVar, weightFlag))
      return *resident;
    ds = session.datastore();
  }
  if (ds == nullptr) return eval::noDataLoaded();

  // ONESHOT-LASTFLUSH: when `ds` is the filtered temp, the stats call below reads a
  // file that is NOT the session head — exactly the case StatelessRead exists for.
  // Routed to the resident plane it frame-uses the temp, which evicts the real head
  // and snapshots it on the way out: the flush the projection above just avoided,
  // moved one call later and renamed "resident-resync". Measured that way first —
  // 28 flushes in, 28 flushes out, no win at all until this guard was added.
  std::optional<SessionState::StatelessRead> statsStateless;
  if (tempDs) statsStateless.emplace(session);
  // `--aweight`, or `--iweight` for [iw=] (QF-SUM-IW): the same engine path.
  return summarizeCoreStatsWeighted(*ds, varlist, detail, weightVar, weightFlag);
}

}  // namespace piet::handlers
