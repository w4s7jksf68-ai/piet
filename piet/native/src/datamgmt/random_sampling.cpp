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
#include "piet/loops.h"  // expandNumlist: splitsample's split() and values() are numlists
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

// base/s/splitsample.ado (StataBE 19.5), the deterministic-rounding path with split(),
// values(), cluster(), strok and show/percent (rround and balance() are refused by the
// vocabulary):
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
  // nsplit(), split(), values() as splitsample's syntax reads them: split() is a numlist of
  // positive proportions and sets nsplit to its length, values() non-negative ascending
  // integers, one per sample.
  // Both are numlists: blank- or comma-separated numbers and the `a/b`, `a(s)b`
  // ranges, each element the %15.0g text the numlist expander gives (1/3 is 1 2 3).
  auto numlistOf = [&](const std::string& text, std::vector<double>* out) -> std::optional<ojson> {
    std::string spec = text;
    for (char& c : spec)
      if (c == ',') c = ' ';
    std::vector<std::string> elements;
    if (!loops::expandNumlist(spec, &elements)) return rsErrorRc("invalid numlist", 121);
    for (const auto& w : elements) {
      double v = 0;
      if (!parseStrictDouble(w, &v)) return rsErrorRc("invalid numlist", 121);
      out->push_back(v);
    }
    return std::nullopt;
  };
  long long nsplit = 2;
  const std::optional<std::string> nsplitOpt = optStr(parsed.opts, "nsplit");
  if (nsplitOpt.has_value()) {
    if (!parseStrictInt(strutil::trim(*nsplitOpt), &nsplit) || nsplit < 1)
      return rsErrorRc("nsplit() invalid -- invalid numlist", 125);
  }
  std::vector<double> split;
  if (auto sp = optStr(parsed.opts, "split")) {
    if (auto e = numlistOf(*sp, &split)) return *e;
    for (double v : split)
      if (!(v > 0))
        return rsErrorRc("split() invalid -- invalid numlist has elements outside of allowed range", 125);
    if (nsplitOpt.has_value() && static_cast<long long>(split.size()) != nsplit)
      return rsErrorRc("conflict between option split() and option nsplit()", 198);
    nsplit = static_cast<long long>(split.size());
  }
  std::vector<double> values;
  if (auto vo = optStrAny(parsed.opts, {"values", "value"})) {
    if (auto e = numlistOf(*vo, &values)) return *e;
    for (size_t i = 0; i < values.size(); ++i) {
      if (values[i] < 0)
        return rsErrorRc("values() invalid -- invalid numlist has elements outside of allowed range", 125);
      if (values[i] != std::floor(values[i]))
        return rsErrorRc("values() invalid -- invalid numlist has noninteger elements", 126);
      if (i > 0 && values[i] < values[i - 1])
        return rsErrorRc("values() invalid -- invalid numlist has elements out of order", 124);
    }
    if (static_cast<long long>(values.size()) != nsplit)
      return rsErrorRc("option values() must be a numlist with " + std::to_string(nsplit) + " values", 198);
  }
  const bool show = hasFlag(parsed.opts, "show");
  if (hasFlag(parsed.opts, "percent") && !show)
    return rsErrorRc("percent must be specified with option show", 198);
  const bool strok = hasFlag(parsed.opts, "strok");
  std::optional<std::string> cluster = optStrAny(parsed.opts, {"cluster", "cl", "clu", "clus", "clust", "cluste"});
  if (cluster.has_value()) {
    std::vector<std::string> one = {strutil::trim(*cluster)};
    if (auto err = resolveByKeyNames(session, one)) return *err;
    cluster = one.front();
  }

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("splitsample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  // The sample: marksample over the varlist (string variables leave it unless strok, which
  // marks out their empty strings), then the cluster variable (strings always allowed).
  std::vector<std::string> vl = parsed.varlist;
  if (auto err = resolveByKeyNames(session, vl)) return *err;
  std::vector<char> touse;
  if (auto err = ifInMask(session, parsed, &touse)) return *err;
  ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  auto markout = [&](const std::string& v) {
    if (ds->dtype(v) == "f64") {
      const double* d = ds->col(v).f64();
      for (int64_t i = 0; i < n; ++i)
        if (std::isnan(d[i])) touse[static_cast<size_t>(i)] = 0;
    } else {
      const auto& col = ds->col(v);
      const auto& levels = ds->varinfo(v).levels;
      for (int64_t i = 0; i < n; ++i) {
        const int32_t code = col.codes()[i];
        if (code < 0 || static_cast<size_t>(code) >= levels.size() || levels[static_cast<size_t>(code)].empty())
          touse[static_cast<size_t>(i)] = 0;
      }
    }
  };
  for (const auto& v : vl) {
    if (!ds->has(v)) return rsErrorRc("variable " + v + " not found", 111);
    if (ds->dtype(v) != "f64" && !strok) continue;  // a string variable leaves the varlist
    markout(v);
  }
  if (cluster.has_value()) {
    if (!ds->has(*cluster)) return rsErrorRc("variable " + *cluster + " not found", 111);
    markout(*cluster);
  }
  int64_t nUse = 0;
  for (char t : touse) nUse += t ? 1 : 0;
  if (nUse == 0) return rsErrorRc("no observations", 2000);
  // the cluster of each sample row: an id in ascending cluster order (numbers by value, text
  // by bytes), as `bysort touse cluster` orders them
  std::vector<int64_t> clusterId(static_cast<size_t>(n), -1);
  int64_t nUnits = nUse;
  if (cluster.has_value()) {
    std::map<double, int64_t> numIds;
    std::map<std::string, int64_t> strIds;
    const bool isNum = ds->dtype(*cluster) == "f64";
    const double* d = isNum ? ds->col(*cluster).f64() : nullptr;
    const auto& levels = ds->varinfo(*cluster).levels;
    for (int64_t i = 0; i < n; ++i) {
      if (!touse[static_cast<size_t>(i)]) continue;
      if (isNum) numIds.emplace(d[i], 0);
      else strIds.emplace(levels[static_cast<size_t>(ds->col(*cluster).codes()[i])], 0);
    }
    int64_t k = 0;
    for (auto& kv : numIds) kv.second = k++;
    for (auto& kv : strIds) kv.second = k++;
    for (int64_t i = 0; i < n; ++i) {
      if (!touse[static_cast<size_t>(i)]) continue;
      clusterId[static_cast<size_t>(i)] =
          isNum ? numIds[d[i]] : strIds[levels[static_cast<size_t>(ds->col(*cluster).codes()[i])]];
    }
    nUnits = k;
  }
  if (nsplit > nUnits) {
    const std::string units = cluster.has_value() ? "clusters" : "observations";
    return rsErrorRc("too many subsamples requested\nnsplit() must be less than or equal to the "
                     "number of " + units + ". nsplit() is " + std::to_string(nsplit) +
                         " and the number of " + units + " is " + std::to_string(nUnits) + ".",
                     2001);
  }

  if (auto rs = optStr(parsed.opts, "rseed")) {
    const ojson set = dispatch("set seed " + strutil::trim(*rs), session);
    if (set.is_array())
      for (const auto& b : set)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
  }
  const std::string rngstate = session.seed.stream().state_string();

  // Cut points: round(M * units / M[last]) with M the running sum of split() (1..nsplit
  // without it); a sample whose cut equals the one before it is a gap, noted and dropped,
  // and values() (1..nsplit by default) loses the same entries.
  std::vector<double> M;
  {
    double run = 0;
    for (long long s = 1; s <= nsplit; ++s) {
      run += split.empty() ? 1.0 : split[static_cast<size_t>(s - 1)];
      M.push_back(split.empty() ? static_cast<double>(s) : run);
    }
  }
  const double scale = static_cast<double>(nUnits) / M.back();
  std::vector<int64_t> cutAll;
  for (double m : M) cutAll.push_back(static_cast<int64_t>(std::floor(m * scale + 0.5)));
  if (values.empty())
    for (long long s = 1; s <= nsplit; ++s) values.push_back(static_cast<double>(s));
  std::vector<int64_t> cut = {0};
  std::vector<double> V;
  std::vector<double> gaps;
  for (size_t s = 0; s < cutAll.size(); ++s) {
    const int64_t prev = s == 0 ? 0 : cutAll[s - 1];
    if (cutAll.size() > 1 && cutAll[s] - prev == 0) {
      gaps.push_back(values[s]);
      continue;
    }
    cut.push_back(cutAll[s]);
    V.push_back(values[s]);
  }
  const long long nSamples = static_cast<long long>(V.size());

  // Split: two uniforms per unit (row, or cluster in cluster order), units ranked by them.
  std::vector<int64_t> rows;
  for (int64_t i = 0; i < n; ++i)
    if (touse[static_cast<size_t>(i)]) rows.push_back(i);
  const size_t nU = static_cast<size_t>(nUnits);
  std::vector<double> r1(nU), r2(nU);
  cecon::Mt64& g = session.seed.stream();
  for (size_t k = 0; k < nU; ++k) r1[k] = g.runiform();
  for (size_t k = 0; k < nU; ++k) r2[k] = g.runiform();
  std::vector<size_t> ord(nU);
  std::iota(ord.begin(), ord.end(), size_t{0});
  std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
    if (r1[a] != r1[b]) return r1[a] < r1[b];
    return r2[a] < r2[b];
  });
  std::vector<int64_t> unitGroup(nU, 0);
  {
    size_t cur = 1;
    for (size_t p = 0; p < ord.size(); ++p) {
      while (cur + 1 < cut.size() && static_cast<int64_t>(p + 1) > cut[cur]) ++cur;
      unitGroup[ord[p]] = static_cast<int64_t>(cur);
    }
  }
  std::vector<int64_t> grp(static_cast<size_t>(n), 0);
  for (size_t k = 0; k < rows.size(); ++k) {
    const size_t unit = cluster.has_value() ? static_cast<size_t>(clusterId[static_cast<size_t>(rows[k])]) : k;
    grp[static_cast<size_t>(rows[k])] = unitGroup[unit];
  }
  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j)
    cells.push_back(touse[static_cast<size_t>(j)]
                        ? numCell(V[static_cast<size_t>(grp[static_cast<size_t>(j)] - 1)])
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
  // `gen byte` then `replace = values()`: the narrowest integer type holding the values
  {
    const double vmax = *std::max_element(V.begin(), V.end());
    const std::string t = vmax <= 100 ? "byte" : vmax <= 32740 ? "int" : "long";
    storage::recordCreateType(session, gen, t, t);
  }

  auto setR = [&]() {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(nUse));
    session.eval.rValues.set("n_samples", static_cast<double>(nSamples));
    session.eval.rStrings.set("rngstate", rngstate);
    if (cluster.has_value()) {
      session.eval.rValues.set("N_clust", static_cast<double>(nUnits));
      session.eval.rStrings.set("clustvar", *cluster);
      eval::declareRPosted(session.eval, "splitsample",
                           {"N", "n_samples", "rngstate", "N_clust", "clustvar"});
    } else {
      eval::declareRPosted(session.eval, "splitsample", {"N", "n_samples", "rngstate"});
    }
  };
  setR();

  ojson blocks = ojson::array();
  for (size_t gI = 0; gI < gaps.size(); ++gI) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", gaps[gI]);
    blocks.push_back(displayBlock(std::string(gI == 0 ? "note: " : "      ") + "sample " + buf +
                                  " has no observations because specified split proportion for "
                                  "that sample is too small."));
  }
  blocks.push_back(mutationResult(session, "splitsample", {gen}));
  if (show) {
    // `tab <generate>`; with cluster(), one observation per cluster and the note under it
    std::string cmdTab = "tabulate " + gen;
    std::string firstVar;
    // The display re-reads cluster(): a cluster variable that `cap drop <generate>` took
    // with it (generate(g) abbreviating grade) is r(111) there, after the split is stored
    // (vs-stata bank bad48a790e7427f9).
    if (cluster.has_value()) {
      Datastore* now = session.datastore();
      if (now == nullptr || !now->has(*cluster)) {
        blocks.push_back(ojson(eval::errorBlock("engine_error",
                                                "variable " + *cluster + " not found\n(error in option cluster())",
                                                std::nullopt, std::nullopt, std::nullopt, "r(111)")));
        return blocks;
      }
    }
    if (cluster.has_value()) {
      firstVar = "__splitsample_first";
      std::vector<std::string> fc(static_cast<size_t>(n), "0");
      std::set<int64_t> seen;
      for (int64_t j = 0; j < n; ++j)
        if (touse[static_cast<size_t>(j)] && seen.insert(clusterId[static_cast<size_t>(j)]).second)
          fc[static_cast<size_t>(j)] = "1";
      std::string out2;
      if (auto err = appendColumn(session, *session.currentHbin(), firstVar, fc, "f64", &out2)) return *err;
      session.load(out2);
      cmdTab += " if " + firstVar;
    }
    ojson tb = dispatch(cmdTab, session);
    for (const auto& b : tb)
      if (b.is_object() && b.value("type", std::string()) != "echo") blocks.push_back(b);
    if (!firstVar.empty()) {
      ojson dropped = runDrop({firstVar}, session);
      if (dropped.is_object() && dropped.value("type", std::string()) == "error") return dropped;
      blocks.push_back(displayBlock("\nTotal is number of clusters."));
    }
    setR();
  }
  return blocks;
}

}  // namespace piet::handlers
