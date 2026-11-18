// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/observations.h"
#include "piet/handlers/variables.h"  // resolveByKeyNames: splitsample's varlist
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
#include "dataset_shape_support.h"
#include "dataset_shape_options.h"

namespace piet::handlers {

using namespace dataset_shape_detail;

// ===========================================================================
// sample
// ===========================================================================

ojson runSample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSample.known,
                                            kVocabSample.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("sample: syntax is `sample # [, count]` (# is a percent unless `count` is "
                    "given)");
  double amount = 0.0;
  if (!parseStrictDouble(parsed.varlist[0], &amount))
    return rsErrorRc("sample: `" + parsed.varlist[0] + "' found where number expected", 7);
  const bool byCount = hasFlag(parsed.opts, "count");
  const std::optional<std::string> byOpt = optStr(parsed.opts, "by");
  if (byOpt.has_value() && parsed.inRange.has_value() && !parsed.inRange->empty())
    return rsErrorRc("in may not be combined with by()", 190);

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("sample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  // sample.ado's argument checks, in its order. A percent of 100, or a count that
  // reaches the number of observations in memory, leaves the data as it is and
  // posts nothing (the ado exits before it counts anything).
  if (byCount) {
    if (amount != std::floor(amount)) return rsErrorRc("sample: integer expected", 7);
    if (amount >= static_cast<double>(n)) return ojson::array();
    if (amount < 0) return rsErrorRc("sample: # must be 0 or positive with the count option", 198);
  } else {
    if (amount == 100.0) return ojson::array();
    if (amount < 0.0 || amount > 100.0)
      return rsErrorRc("sample: # must be between 0 and 100 and is interpreted as a percent unless "
                       "option count is also specified",
                       198);
  }

  std::vector<std::string> byVars;
  if (byOpt.has_value()) {
    byVars = splitWs(*byOpt);
    if (auto err = resolveByKeyNames(session, byVars)) return *err;
  }
  std::vector<char> inSample;
  if (auto err = ifInMask(session, parsed, &inSample)) return *err;
  ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  int64_t nElig = 0;
  for (char t : inSample) nElig += t ? 1 : 0;
  // The ado's `quietly count if tokeep == 0` is the last count it runs: r(N) is the
  // number of observations the if/in qualifier leaves to sample from.
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", static_cast<double>(nElig));
  eval::declareRPosted(session.eval, "sample", {"N"});
  if (nElig == 0) return ojson::array();

  // The groups of by(): each distinct combination of the by variables (a missing
  // value is its own level), counted over the observations the qualifier selects.
  std::vector<int64_t> group(static_cast<size_t>(n), 0);
  if (!byVars.empty()) {
    std::map<std::vector<std::string>, int64_t> ids;
    std::vector<std::vector<std::string>> cols;
    for (const auto& v : byVars) {
      if (!ds->has(v)) return rsErrorRc("variable " + v + " not found", 111);
      cols.push_back(columnAsStrings(*ds, v));
    }
    for (int64_t i = 0; i < n; ++i) {
      std::vector<std::string> key;
      for (const auto& c : cols) key.push_back(c[static_cast<size_t>(i)]);
      auto it = ids.emplace(std::move(key), static_cast<int64_t>(ids.size())).first;
      group[static_cast<size_t>(i)] = it->second;
    }
  }

  // A key per ROW, then keep the smallest N of each group. Equivalent to drawing
  // without replacement, and reproducible without any ordering assumption: the row
  // index breaks a key tie so the answer never depends on sort stability.
  const uint64_t seed = drawSeed(session, std::nullopt);
  std::map<int64_t, std::vector<std::pair<uint32_t, int64_t>>> byGroup;
  for (int64_t j = 0; j < n; ++j)
    if (inSample[static_cast<size_t>(j)])
      byGroup[group[static_cast<size_t>(j)]].emplace_back(rowKey32(seed, j), j);
  std::vector<char> keep(static_cast<size_t>(n), 1);  // rows outside if/in stay
  int64_t kept = n - nElig;
  for (auto& [g, keys] : byGroup) {
    (void)g;
    std::sort(keys.begin(), keys.end());
    // Percent: round-half-up of the group's share, int(N * # / 100 + .5); count: # a group.
    int64_t k = byCount ? static_cast<int64_t>(amount)
                        : static_cast<int64_t>(std::floor(static_cast<double>(keys.size()) *
                                                              amount / 100.0 + 0.5));
    if (k > static_cast<int64_t>(keys.size())) k = static_cast<int64_t>(keys.size());
    for (size_t j = 0; j < keys.size(); ++j) {
      const bool take = static_cast<int64_t>(j) < k;
      keep[static_cast<size_t>(keys[j].second)] = take ? 1 : 0;
    }
    kept += k;
  }

  const std::string keepVar = "__piet_sample_keep";
  if (ds->has(keepVar))
    return rsError("sample: the scratch column " + keepVar +
                   " already exists in this dataset; rename it and retry.");
  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) cells.push_back(keep[static_cast<size_t>(j)] ? "1" : "0");

  std::string flagged;
  if (auto err = appendColumn(session, *session.currentHbin(), keepVar, cells, "f64", &flagged))
    return *err;
  const std::string out = nextPath(session.workdir(), "sample");
  if (auto err = runVerb({"eval", "--data", flagged, "--keepif", std::string(keepVar) + "==1",
                          "--drop", keepVar, "--out", out}))
    return *err;
  session.load(out);
  if (auto bad = assertRowCount(session, "sample", kept)) return *bad;
  return mutationResult(session, "sample");
}

// ===========================================================================
// splitsample
// ===========================================================================

// base/s/splitsample.ado (StataBE 19.5), the path without rround/balance/
// cluster/split()/values() (those options are refused by the vocabulary):
//  - the sample is the observations with no missing value in the varlist (string
//    variables are left out of the varlist unless strok); none is r(2000), and
//    nsplit() above the sample size r(2001);
//  - rseed() is `set seed`; r(rngstate) is c(rngstate) after it;
//  - Split: r1 = runiform() then r2 = runiform() over the sample rows in row
//    order, the rows ranked by (r1, r2); rank p goes to sample s, the cut points
//    being round(s * N / nsplit) (Mata round);
//  - the result is a byte variable at the end of the data, after
//    `cap drop <generate>` -- which, as Stata reads a varlist, also drops the
//    one existing variable the name abbreviates (vs-stata bank: on auto,
//    generate(g) replaces gear_ratio, k stays 12);
//  - r(N), r(n_samples), r(rngstate).
ojson runSplitsample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplitsample.known,
                                            kVocabSplitsample.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // generate(newvar [, replace]) is required.
  const std::optional<std::string> genOpt = optStrAny(parsed.opts, {"generate", "gen"});
  if (!genOpt.has_value()) return rsErrorRc("option generate() required", 198);
  std::string gen = strutil::trim(*genOpt);
  bool replace = false;
  if (const size_t comma = gen.find(','); comma != std::string::npos) {
    const std::string tail = strutil::trim(gen.substr(comma + 1));
    gen = strutil::trim(gen.substr(0, comma));
    if (lowerOf(tail) == "replace") replace = true;
    else return rsErrorRc("option " + tail + " not allowed", 198);
  }
  if (gen.empty()) return rsErrorRc(replace ? "option replace must be specified after a newvarname"
                                            : "option generate() required",
                                    198);
  // BUG-1120 review: generate() is read as a newvarlist, so a storage type is
  // "byte invalid varname" r(198) and if/in/using/with open a qualifier the option
  // does not take, "if not allowed" r(101) (StataBE 19.5).
  if (auto r = strutil::newVarOptionRefusal(gen)) return rsErrorRc(r->message, r->rc);
  if (!replace) {
    ojson guard = newVarGuard(session, "splitsample", gen);
    if (!guard.is_null()) return guard;
  }
  long long nsplit = 2;
  if (auto v = optStr(parsed.opts, "nsplit")) {
    if (!parseStrictInt(strutil::trim(*v), &nsplit) || nsplit < 1)
      return rsErrorRc("nsplit() invalid -- invalid numlist", 125);
  }

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("splitsample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  // The sample: marksample over the numeric variables of the varlist.
  std::vector<std::string> vl = parsed.varlist;
  if (auto err = resolveByKeyNames(session, vl)) return *err;
  std::vector<char> touse;
  if (auto err = ifInMask(session, parsed, &touse)) return *err;
  ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  for (const auto& v : vl) {
    if (!ds->has(v)) return rsErrorRc("variable " + v + " not found", 111);
    if (ds->dtype(v) != "f64") continue;  // a string variable leaves the varlist (no strok)
    const double* d = ds->col(v).f64();
    for (int64_t i = 0; i < n; ++i)
      if (std::isnan(d[i])) touse[static_cast<size_t>(i)] = 0;
  }
  int64_t nUse = 0;
  for (char t : touse) nUse += t ? 1 : 0;
  if (nUse == 0) return rsErrorRc("no observations", 2000);
  if (nsplit > nUse)
    return rsErrorRc("too many subsamples requested\nnsplit() must be less than or equal to the "
                     "number of observations. nsplit() is " + std::to_string(nsplit) +
                         " and the number of observations is " + std::to_string(nUse) + ".",
                     2001);

  if (auto rs = optStr(parsed.opts, "rseed")) {
    const ojson set = dispatch("set seed " + strutil::trim(*rs), session);
    if (set.is_array())
      for (const auto& b : set)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
  }
  const std::string rngstate = session.seed.stream().state_string();

  // Split: two uniforms per sample row, the rows ranked by them.
  std::vector<int64_t> rows;
  for (int64_t i = 0; i < n; ++i)
    if (touse[static_cast<size_t>(i)]) rows.push_back(i);
  std::vector<double> r1(rows.size()), r2(rows.size());
  cecon::Mt64& g = session.seed.stream();
  for (size_t k = 0; k < rows.size(); ++k) r1[k] = g.runiform();
  for (size_t k = 0; k < rows.size(); ++k) r2[k] = g.runiform();
  std::vector<size_t> ord(rows.size());
  std::iota(ord.begin(), ord.end(), size_t{0});
  std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
    if (r1[a] != r1[b]) return r1[a] < r1[b];
    return r2[a] < r2[b];
  });
  std::vector<int64_t> cut(static_cast<size_t>(nsplit) + 1, 0);
  const double per = static_cast<double>(nUse) / static_cast<double>(nsplit);
  for (long long s = 1; s <= nsplit; ++s)
    cut[static_cast<size_t>(s)] = static_cast<int64_t>(std::floor(static_cast<double>(s) * per + 0.5));
  std::vector<int64_t> grp(static_cast<size_t>(n), 0);
  int64_t cur = 1;
  for (size_t p = 0; p < ord.size(); ++p) {
    if (p > 0 && static_cast<int64_t>(p + 1) > cut[static_cast<size_t>(cur)]) ++cur;
    grp[static_cast<size_t>(rows[ord[p]])] = cur;
  }
  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j)
    cells.push_back(touse[static_cast<size_t>(j)] ? std::to_string(grp[static_cast<size_t>(j)])
                                                  : std::string());

  // `cap drop <generate>`: the variable of that name, else the one it abbreviates.
  {
    const std::vector<std::string> names = ds->names();
    std::string target;
    if (std::find(names.begin(), names.end(), gen) != names.end()) {
      target = gen;
    } else {
      std::vector<std::string> hits;
      for (const auto& v : names)
        if (v.size() > gen.size() && v.compare(0, gen.size(), gen) == 0) hits.push_back(v);
      if (hits.size() == 1) target = hits.front();
    }
    if (!target.empty()) {
      ojson dropped = runDrop({target}, session);
      if (dropped.is_object() && dropped.value("type", std::string()) == "error") return dropped;
    }
  }

  std::string out;
  if (auto err = appendColumn(session, *session.currentHbin(), gen, cells, "f64", &out)) return *err;
  session.load(out);
  session.varMeta.erase(gen);
  storage::recordCreateType(session, gen, "byte", "byte");

  eval::replaceRClass(session.eval);
  session.eval.rValues.set("N", static_cast<double>(nUse));
  session.eval.rValues.set("n_samples", static_cast<double>(nsplit));
  session.eval.rStrings.set("rngstate", rngstate);
  eval::declareRPosted(session.eval, "splitsample", {"N", "n_samples", "rngstate"});

  ojson blocks = ojson::array();
  if (hasFlag(parsed.opts, "show")) {
    std::vector<int64_t> counts(static_cast<size_t>(nsplit) + 1, 0);
    for (int64_t j = 0; j < n; ++j)
      if (touse[static_cast<size_t>(j)]) counts[static_cast<size_t>(grp[static_cast<size_t>(j)])] += 1;
    std::ostringstream oss;
    oss << gen << "  |      Freq.\n";
    oss << "------------+------------\n";
    for (long long g2 = 1; g2 <= nsplit; ++g2) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%11lld | %10lld\n", g2,
                    static_cast<long long>(counts[static_cast<size_t>(g2)]));
      oss << buf;
    }
    blocks.push_back(displayBlock(oss.str()));
  }
  blocks.push_back(mutationResult(session, "splitsample", {gen}));
  return blocks;
}

}  // namespace piet::handlers
