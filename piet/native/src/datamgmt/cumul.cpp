// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/data_values.h"
#include "piet/handlers/variables.h"
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
// cumul
// ===========================================================================

namespace {

// One `cumul` call, once its syntax has been accepted.
struct CumulRequest {
  std::string source;
  std::string target;
  std::vector<std::string> groupVars;
  bool frequencyUnits = false;
  bool tiesShareValue = false;
};

size_t optionCount(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   std::initializer_list<const char*> spellings) {
  size_t count = 0;
  for (const auto& [key, value] : opts)
    for (const char* spelling : spellings)
      if (key == spelling) ++count;
  return count;
}

// The syntax checks, in the order Stata answers them when several faults meet
// (per the black-box oracle in the item's folder). `by()` comes last, so the
// checks that need the data can run in the same pass.
std::optional<ojson> readCumulRequest(const eval::ParsedCommand& parsed, SessionState& session,
                                      CumulRequest* request) {
  const Datastore* data = session.datastore();
  if (data == nullptr) return ojson(eval::noDataLoaded());

  std::vector<std::string> sourceWords = parsed.varlist;
  if (sourceWords.empty()) return rsErrorRc("varlist required", 100);
  if (auto ambiguous = resolveByKeyNames(session, sourceWords)) return *ambiguous;
  if (sourceWords.size() > 1) return rsErrorRc("too many variables specified", 103);
  request->source = sourceWords.front();
  if (!data->has(request->source))
    return rsErrorRc("variable " + request->source + " not found", 111);

  const auto newNameWords = splitWs(optStrAny(parsed.opts, {"generate", "gen"}).value_or(""));
  if (newNameWords.empty()) return rsErrorRc("option generate() required", 198);
  if (auto unknown = eval::rejectUnknownOptions(parsed.opts, kVocabCumul.known,
                                                kVocabCumul.unimplemented))
    return ojson(*unknown);
  if (optionCount(parsed.opts, {"generate", "gen"}) > 1)
    return rsErrorRc("option gen() not allowed", 198);
  if (optionCount(parsed.opts, {"by"}) > 1) return rsErrorRc("option by() not allowed", 198);
  if (newNameWords.size() > 1) return rsErrorRc(newNameWords[1] + " not allowed", 101);
  request->target = newNameWords.front();
  if (auto refused = strutil::newVarNameRefusal(request->target))
    return rsErrorRc(refused->message, refused->rc);
  if (!strutil::isValidStataIdentifier(request->target))
    return rsErrorRc(request->target + " invalid name", 198);
  if (data->has(request->target))
    return rsErrorRc("variable " + request->target + " already defined", 110);

  request->groupVars = splitWs(optStr(parsed.opts, "by").value_or(""));
  if (auto ambiguous = resolveByKeyNames(session, request->groupVars)) return *ambiguous;
  for (const std::string& name : request->groupVars)
    if (!data->has(name))
      return rsErrorRc("variable " + name + " not found\n(error in option by())", 111);

  request->frequencyUnits = hasFlag(parsed.opts, "freq");
  request->tiesShareValue = hasFlag(parsed.opts, "equal");
  return std::nullopt;
}

// One 64-bit word per observation, equal exactly when two observations belong
// together under this by() variable. Each missing code and the empty string are
// groups of their own; 0 and -0 are one value.
std::vector<uint64_t> groupingWords(const Datastore& data, const std::string& name) {
  std::vector<uint64_t> words(static_cast<size_t>(data.nRows()));
  if (data.col(name).dtype == cecon::DType::Factor) {
    std::map<std::string, uint64_t> seen;
    const auto cells = columnAsStrings(data, name);
    for (size_t row = 0; row < cells.size(); ++row)
      words[row] = seen.emplace(cells[row], seen.size()).first->second;
    return words;
  }
  const auto cells = columnAsDoubles(data, name);
  for (size_t row = 0; row < cells.size(); ++row) {
    double cell = cells[row] == 0.0 ? 0.0 : cells[row];
    if (std::isnan(cell)) cell = cecon::stata_missing(cecon::missing_kind(cell));
    std::memcpy(&words[row], &cell, sizeof cell);
  }
  return words;
}

// Fills `ecdf` for one by() group, given as sample rows in ascending value order.
// Both sums are held as a float stores them: the running total is exact double
// arithmetic rounded on its way into storage, and the share is formed from those
// stored numbers (the arithmetic measured on StataBE, the item's oracle section B;
// the textbook function is [R] cumul's empirical distribution).
void fillGroupEcdf(const std::vector<int64_t>& rows, size_t begin, size_t end,
                   const std::vector<double>& value, const std::vector<double>* weight,
                   const CumulRequest& request, std::vector<double>* ecdf) {
  std::vector<float> stored(end - begin);
  double running = 0.0;
  for (size_t k = begin; k < end; ++k) {
    running += weight != nullptr ? (*weight)[static_cast<size_t>(rows[k])] : 1.0;
    stored[k - begin] = static_cast<float>(running);
  }
  const double groupTotal = stored.back();
  for (size_t k = begin; k < end;) {
    size_t tieEnd = k + 1;
    if (request.tiesShareValue)
      while (tieEnd < end && value[static_cast<size_t>(rows[tieEnd])] == value[static_cast<size_t>(rows[k])])
        ++tieEnd;
    const float reached = stored[tieEnd - 1 - begin];
    const double result =
        request.frequencyUnits ? reached : static_cast<float>(reached / groupTotal);
    for (; k < tieEnd; ++k) (*ecdf)[static_cast<size_t>(rows[k])] = result;
  }
}

}  // namespace

// `cumul`, [R] cumul: the empirical distribution function of a variable, as a new
// float variable, within by() groups.
ojson runCumul(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  CumulRequest request;
  if (auto refusal = readCumulRequest(parsed, session, &request)) return *refusal;

  const bool haveIf = parsed.ifExpr.has_value() && !trimOf(*parsed.ifExpr).empty();
  const bool haveIn = parsed.inRange.has_value() && !trimOf(*parsed.inRange).empty();
  const bool inWithBy = haveIn && !request.groupVars.empty();
  std::string qualifier;
  if (haveIf) qualifier += " if " + *parsed.ifExpr;
  if (haveIn && !inWithBy) qualifier += " in " + *parsed.inRange;

  ScratchColumns scratch(session);
  std::string restrictionColumn;
  if (!qualifier.empty())
    if (auto failed = scratch.add("1", qualifier, &restrictionColumn)) return *failed;
  if (inWithBy) return rsErrorRc("in may not be combined with by()", 190);

  std::string weightColumn;
  const bool weighted = parsed.weightVar.has_value() && !parsed.weightVar->empty();
  if (weighted)
    if (auto failed = scratch.add(*parsed.weightVar, "", &weightColumn)) return *failed;

  const auto rowCount = static_cast<size_t>(session.datastore()->nRows());
  std::vector<char> restricted(rowCount, 1);
  if (!restrictionColumn.empty()) {
    const auto marks = scratch.values(restrictionColumn);
    for (size_t row = 0; row < rowCount; ++row) restricted[row] = std::isnan(marks[row]) ? 0 : 1;
  }

  // A weight only has to be valid inside the if/in restriction.
  std::vector<double> weights;
  if (weighted) {
    weights = scratch.values(weightColumn);
    const std::string kind = parsed.weightType.value_or("");
    const bool frequencyWeight = kind == "fw" || kind == "fweight";
    bool negative = false, fractional = false;
    for (size_t row = 0; row < rowCount; ++row) {
      if (!restricted[row] || std::isnan(weights[row])) continue;
      negative = negative || weights[row] < 0.0;
      fractional = fractional || weights[row] != std::floor(weights[row]);
    }
    if (negative) return rsErrorRc("negative weights encountered", 402);
    if (frequencyWeight && fractional)
      return rsErrorRc("may not use noninteger frequency weights", 401);
  }

  const Datastore& data = *session.datastore();
  std::vector<double> sourceValues;  // stays empty for a string variable: no sample
  if (data.col(request.source).dtype != cecon::DType::Factor)
    sourceValues = columnAsDoubles(data, request.source);

  std::vector<int64_t> sample;
  for (size_t row = 0; row < sourceValues.size(); ++row) {
    if (!restricted[row] || std::isnan(sourceValues[row])) continue;
    if (weighted && (std::isnan(weights[row]) || weights[row] == 0.0)) continue;
    sample.push_back(static_cast<int64_t>(row));
  }

  std::vector<std::vector<uint64_t>> groupColumns;
  for (const std::string& name : request.groupVars) groupColumns.push_back(groupingWords(data, name));
  const auto sameGroup = [&](int64_t a, int64_t b) {
    return std::all_of(groupColumns.begin(), groupColumns.end(), [&](const std::vector<uint64_t>& g) {
      return g[static_cast<size_t>(a)] == g[static_cast<size_t>(b)];
    });
  };
  // Stable, so tied observations accumulate in dataset order and PIET is
  // deterministic where Stata's tie order is unspecified.
  std::stable_sort(sample.begin(), sample.end(), [&](int64_t a, int64_t b) {
    for (const auto& g : groupColumns) {
      const uint64_t wordA = g[static_cast<size_t>(a)], wordB = g[static_cast<size_t>(b)];
      if (wordA != wordB) return wordA < wordB;
    }
    return sourceValues[static_cast<size_t>(a)] < sourceValues[static_cast<size_t>(b)];
  });

  std::vector<double> ecdf(rowCount, std::numeric_limits<double>::quiet_NaN());
  for (size_t begin = 0; begin < sample.size();) {
    size_t end = begin + 1;
    while (end < sample.size() && sameGroup(sample[begin], sample[end])) ++end;
    fillGroupEcdf(sample, begin, end, sourceValues, weighted ? &weights : nullptr, request, &ecdf);
    begin = end;
  }

  scratch.dropAll();
  (void)session.datastore();  // the head file the setcol below reads
  std::vector<std::string> cells;
  cells.reserve(rowCount);
  for (double v : ecdf) cells.push_back(numCell(v));
  std::string written;
  if (auto failed = appendColumn(session, *session.currentHbin(), request.target, cells, "f64", &written))
    return *failed;
  session.load(written);
  session.setVarLabel(request.target, "ECDF of " + request.source);
  session.varMeta[request.target].stataType = "float";
  scratch.noteDataChanged();
  return mutationResult(session, "cumul", {request.target});
}

}  // namespace piet::handlers
