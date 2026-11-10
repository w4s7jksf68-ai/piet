// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/handlers/utility.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // std::memcpy -- the ±0-safe key bit-normalization
#include <ctime>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/session_state.h"

#include "utility_support.h"
#include "utility_options.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace utility_detail;

// ===========================================================================
// rangestat
// ===========================================================================
//
// `rangestat (stat) var [(stat) var ...], interval(key lo hi) [by(varlist)]`.
//
// THE WINDOW IS CLOSED AND CONTAINS THE ROW ITSELF: for a row whose key is k,
// every observation of the same by-group whose key lies in [k+lo, k+hi] enters
// the statistic, including that row. `.` for a bound means unbounded on that
// side. `sd` divides by n-1. `count` counts NON-MISSING values. A window with
// no observations at all gives missing on every statistic -- never 0, not even
// for count; a window that HAS observations but none with a non-missing value
// gives count 0 and missing for the rest, which is the same distinction Stata's
// own count/mean pair makes.
//
// RESULTS GO BACK IN ORIGINAL ROW ORDER. The computation sorts by key within
// group because a two-pointer window needs sorted keys; every value is then
// written at the row index it came from. Sorting to compute and forgetting to
// restore is a silent wrong answer that looks perfectly plausible, so the
// sorted array below never holds anything but an index INTO the original rows.
namespace {

enum class RangeStat { Mean, Sum, Count, Min, Max, Sd };

struct RangeSpec {
  RangeStat stat = RangeStat::Mean;
  std::string statName;
  std::string src;
  std::string target;
};

bool parseRangeStatName(const std::string& s, RangeStat* out) {
  if (s == "mean") *out = RangeStat::Mean;
  else if (s == "sum") *out = RangeStat::Sum;
  else if (s == "count") *out = RangeStat::Count;
  else if (s == "min") *out = RangeStat::Min;
  else if (s == "max") *out = RangeStat::Max;
  else if (s == "sd") *out = RangeStat::Sd;
  else return false;
  return true;
}

}  // namespace

ojson runRangestat(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRangestat.known,
                                            kVocabRangestat.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // The registry row allows if/in, and rangestat WRITES columns -- see
  // carryforward for why a filtered read cannot be mapped back onto the rows.
  {
    ojson q = refuseQualifiers(
        parsed, "`rangestat` writes columns as long as the dataset, and a restricted sample would also "
        "change which observations enter each window. Restrict first (`preserve` + `keep if "
        "...`), then run rangestat.");
    if (!q.is_null()) return q;
  }

  // The stat specs are read off the varlist: the tokenizer splits the pre-comma
  // text on whitespace, so `(mean) y (sum) z` arrives as four tokens and
  // `(mean)y` as one. Rejoining and scanning is the same grammar either way.
  const std::string head = joinWith(parsed.varlist, " ");
  std::vector<RangeSpec> specs;
  {
    size_t p = 0;
    while (p < head.size()) {
      while (p < head.size() && std::isspace(static_cast<unsigned char>(head[p]))) ++p;
      if (p >= head.size()) break;
      if (head[p] != '(')
        return utilSyntax("rangestat: expected `(statname)' at `" + head.substr(p) +
                          "' -- syntax is `rangestat (stat) var [(stat) var ...], interval(key lo "
                          "hi) [by(varlist)]`");
      const size_t close = head.find(')', p);
      if (close == std::string::npos)
        return utilSyntax("rangestat: unclosed `(' in `" + head + "'");
      const std::string statName = lowerOf(trimOf(head.substr(p + 1, close - p - 1)));
      p = close + 1;
      while (p < head.size() && std::isspace(static_cast<unsigned char>(head[p]))) ++p;
      size_t q = p;
      while (q < head.size() && !std::isspace(static_cast<unsigned char>(head[q])) && head[q] != '(')
        ++q;
      if (q == p)
        return utilSyntax("rangestat: `(" + statName + ")' names no variable.");
      const std::string term = head.substr(p, q - p);
      p = q;

      RangeSpec spec;
      if (!parseRangeStatName(statName, &spec.stat)) {
        return ojson(eval::unsupportedOption(
            "(" + statName + ")",
            "`rangestat` in this build computes mean, sum, count, min, max and sd. `" + statName +
                "` is a real rangestat statistic this build does not implement; answering it with "
                "one of the six would be a different number under the requested name."));
      }
      spec.statName = statName;
      const size_t eq = term.find('=');
      if (eq == std::string::npos) {
        spec.src = term;
        spec.target = term + "_" + statName;
      } else {
        spec.target = trimOf(term.substr(0, eq));
        spec.src = trimOf(term.substr(eq + 1));
      }
      if (spec.src.empty() || spec.target.empty())
        return utilSyntax("rangestat: could not read `" + term + "' as [newname=]varname");
      specs.push_back(spec);
    }
  }
  if (specs.empty())
    return utilSyntax(
        "rangestat: syntax is `rangestat (stat) var [(stat) var ...], interval(key lo hi) "
        "[by(varlist)]`");

  const std::optional<std::string> intervalOpt = optStr(parsed.opts, "interval");
  if (!intervalOpt.has_value())
    return utilSyntax("rangestat: interval(key lo hi) is required -- without it there is no "
                      "window.");
  const std::vector<std::string> iv = splitWs(*intervalOpt);
  if (iv.size() != 3)
    return utilSyntax("rangestat: interval() takes exactly three terms -- a key variable, a low "
                      "offset and a high offset (`.` for unbounded), got `" + *intervalOpt + "'");
  const std::string keyVar = iv[0];
  double lo = -std::numeric_limits<double>::infinity();
  double hi = std::numeric_limits<double>::infinity();
  if (iv[1] != ".") {
    if (!parseStrictDouble(iv[1], &lo))
      return utilSyntax("rangestat: interval()'s low offset `" + iv[1] +
                        "' is not a number (use `.` for unbounded).");
  }
  if (iv[2] != ".") {
    if (!parseStrictDouble(iv[2], &hi))
      return utilSyntax("rangestat: interval()'s high offset `" + iv[2] +
                        "' is not a number (use `.` for unbounded).");
  }
  if (lo > hi)
    return utilSyntax("rangestat: interval()'s low offset (" + iv[1] +
                      ") is above its high offset (" + iv[2] + "); the window would be empty.");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);

  std::vector<std::string> readCols = {keyVar};
  for (const auto& s : specs) readCols.push_back(s.src);
  for (const auto& b : byVars) readCols.push_back(b);
  ReadScope scope(session, parsed, readCols, "rangestat", false);
  if (scope.failed())
    return scope.error().is_null() ? utilError("rangestat: could not read the data", "data_io")
                                   : scope.error();
  Datastore& ds = *scope.get();
  for (const auto& nm : readCols) {
    if (!ds.has(nm)) return utilError("variable " + nm + " not found");
  }
  if (ds.dtype(keyVar) == "factor")
    return utilError("rangestat: the interval() key `" + keyVar +
                     "' is a string variable; the window is an arithmetic range and needs a "
                     "numeric key.");
  for (const auto& s : specs) {
    if (ds.dtype(s.src) == "factor")
      return utilError("rangestat: `" + s.src +
                       "' is a string variable; " + s.statName + " needs a numeric variable.");
  }
  std::set<std::string> targetsSeen;
  for (const auto& s : specs) {
    ojson guard = newVarGuard(session, "rangestat", s.target);
    if (!guard.is_null()) return guard;
    if (!targetsSeen.insert(s.target).second)
      return utilError("rangestat: two results would both be called `" + s.target +
                           "'; name one of them with `newname=var`.",
                       "bad_input");
  }

  const int64_t n = ds.nRows();
  const double* key = ds.col(keyVar).f64();
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);

  // Row indices per group, sorted by key ascending. Rows with a MISSING key
  // have no window at all and never enter one, so they are left out here and
  // their results stay missing.
  std::vector<std::vector<int64_t>> order(static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(key[i])) continue;
    order[static_cast<size_t>(gid[static_cast<size_t>(i)])].push_back(i);
  }
  for (auto& g : order) {
    std::stable_sort(g.begin(), g.end(),
                     [&](int64_t a, int64_t b) { return key[a] < key[b]; });
  }

  const bool wantExtrema =
      std::any_of(specs.begin(), specs.end(), [](const RangeSpec& s) {
        return s.stat == RangeStat::Min || s.stat == RangeStat::Max;
      });

  std::vector<std::vector<double>> results(
      specs.size(),
      std::vector<double>(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN()));

  for (const auto& g : order) {
    if (g.empty()) continue;
    const size_t m = g.size();
    // Per statistic source: prefix count/sum/sumsq over the sorted order, so a
    // window's totals are one subtraction rather than a walk. Only NON-MISSING
    // values contribute -- which is exactly what makes `count` the count of
    // non-missing.
    std::vector<std::vector<double>> pSum(specs.size(), std::vector<double>(m + 1, 0.0));
    std::vector<std::vector<double>> pSumSq(specs.size(), std::vector<double>(m + 1, 0.0));
    std::vector<std::vector<int64_t>> pCnt(specs.size(), std::vector<int64_t>(m + 1, 0));
    std::vector<const double*> srcPtr;
    srcPtr.reserve(specs.size());
    for (const auto& s : specs) srcPtr.push_back(ds.col(s.src).f64());
    for (size_t s = 0; s < specs.size(); ++s) {
      for (size_t j = 0; j < m; ++j) {
        const double v = srcPtr[s][g[j]];
        const bool ok = !std::isnan(v);
        pSum[s][j + 1] = pSum[s][j] + (ok ? v : 0.0);
        pSumSq[s][j + 1] = pSumSq[s][j] + (ok ? v * v : 0.0);
        pCnt[s][j + 1] = pCnt[s][j] + (ok ? 1 : 0);
      }
    }
    // The multiset window for min/max, driven by the SAME two pointers. Both
    // are monotone as the centre row's key increases, so each element is
    // inserted once and erased once.
    std::vector<std::multiset<double>> win(wantExtrema ? specs.size() : 0);
    size_t winL = 0, winR = 0;

    size_t L = 0, R = 0;  // [L, R) is the window, as indices into g
    for (size_t c = 0; c < m; ++c) {
      const double k = key[g[c]];
      const double lowBound = k + lo;   // -inf when lo is `.`
      const double highBound = k + hi;  // +inf when hi is `.`
      while (L < m && key[g[L]] < lowBound) ++L;
      while (R < m && key[g[R]] <= highBound) ++R;
      if (R < L) R = L;  // an empty window still has to be well formed
      if (wantExtrema) {
        while (winR < R) {
          for (size_t s = 0; s < specs.size(); ++s) {
            const double v = srcPtr[s][g[winR]];
            if (!std::isnan(v)) win[s].insert(v);
          }
          ++winR;
        }
        while (winL < L) {
          for (size_t s = 0; s < specs.size(); ++s) {
            const double v = srcPtr[s][g[winL]];
            if (!std::isnan(v)) {
              auto it = win[s].find(v);
              if (it != win[s].end()) win[s].erase(it);
            }
          }
          ++winL;
        }
      }
      const int64_t obsInWindow = static_cast<int64_t>(R) - static_cast<int64_t>(L);
      if (obsInWindow <= 0) continue;  // no observations at all -> every result stays missing
      for (size_t s = 0; s < specs.size(); ++s) {
        const int64_t cnt = pCnt[s][R] - pCnt[s][L];
        const double sum = pSum[s][R] - pSum[s][L];
        double& out = results[s][static_cast<size_t>(g[c])];
        switch (specs[s].stat) {
          case RangeStat::Count:
            out = static_cast<double>(cnt);
            break;
          case RangeStat::Sum:
            if (cnt > 0) out = sum;
            break;
          case RangeStat::Mean:
            if (cnt > 0) out = sum / static_cast<double>(cnt);
            break;
          case RangeStat::Sd: {
            if (cnt >= 2) {
              const double ss = (pSumSq[s][R] - pSumSq[s][L]) - sum * sum / static_cast<double>(cnt);
              out = std::sqrt(std::max(ss, 0.0) / static_cast<double>(cnt - 1));
            }
            break;
          }
          case RangeStat::Min:
            if (!win[s].empty()) out = *win[s].begin();
            break;
          case RangeStat::Max:
            if (!win[s].empty()) out = *win[s].rbegin();
            break;
        }
      }
    }
  }

  std::string cur = *session.currentHbin();
  std::vector<std::string> newVars;
  for (size_t s = 0; s < specs.size(); ++s) {
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) cells.push_back(numCell(results[s][static_cast<size_t>(i)]));
    std::string out;
    if (auto err = writeColumn(session, cur, specs[s].target, cells, "f64", true, &out)) return *err;
    cur = out;
    newVars.push_back(specs[s].target);
  }
  session.load(cur);
  return mutationResult(session, "gen", newVars);
}


}  // namespace piet::handlers
