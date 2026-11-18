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

#include "datamgmt/dataset_shape_support.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/handlers/variables.h"
#include "piet/strutil.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
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

enum class RangeStat {
  Mean, Sum, Count, Min, Max, Sd, Variance, Skewness, Kurtosis, Median, Missing, Obs, First, Last,
  FirstNm, LastNm
};

struct RangeSpec {
  RangeStat stat = RangeStat::Mean;
  std::string statName;
  std::string src;
  std::string target;
};

bool parseRangeStatName(const std::string& s, RangeStat* out) {
  static const std::map<std::string, RangeStat> kStats = {
      {"mean", RangeStat::Mean},         {"sum", RangeStat::Sum},
      {"count", RangeStat::Count},       {"min", RangeStat::Min},
      {"max", RangeStat::Max},           {"sd", RangeStat::Sd},
      {"variance", RangeStat::Variance}, {"skewness", RangeStat::Skewness},
      {"kurtosis", RangeStat::Kurtosis}, {"median", RangeStat::Median},
      {"missing", RangeStat::Missing},   {"obs", RangeStat::Obs},
      {"first", RangeStat::First},       {"last", RangeStat::Last},
      {"firstnm", RangeStat::FirstNm},   {"lastnm", RangeStat::LastNm}};
  auto it = kStats.find(s);
  if (it == kStats.end()) return false;
  *out = it->second;
  return true;
}

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// One statistic over the values of a window, in key order (a missing value is NaN).
double windowStat(RangeStat st, const std::vector<double>& vals) {
  std::vector<double> nm;
  nm.reserve(vals.size());
  for (double v : vals)
    if (!std::isnan(v)) nm.push_back(v);
  const double n = static_cast<double>(nm.size());
  auto moments = [&](double* mean, double* m2, double* m3, double* m4) {
    double s = 0.0;
    for (double v : nm) s += v;
    *mean = s / n;
    *m2 = *m3 = *m4 = 0.0;
    for (double v : nm) {
      const double d = v - *mean;
      *m2 += d * d;
      *m3 += d * d * d;
      *m4 += d * d * d * d;
    }
    *m2 /= n;
    *m3 /= n;
    *m4 /= n;
  };
  switch (st) {
    case RangeStat::Obs: return static_cast<double>(vals.size());
    case RangeStat::Missing: return static_cast<double>(vals.size() - nm.size());
    case RangeStat::Count: return n;
    case RangeStat::First: return vals.empty() ? kNaN : vals.front();
    case RangeStat::Last: return vals.empty() ? kNaN : vals.back();
    case RangeStat::FirstNm: return nm.empty() ? kNaN : nm.front();
    case RangeStat::LastNm: return nm.empty() ? kNaN : nm.back();
    case RangeStat::Sum: {
      if (nm.empty()) return kNaN;
      double s = 0.0;
      for (double v : nm) s += v;
      return s;
    }
    case RangeStat::Mean: {
      if (nm.empty()) return kNaN;
      double s = 0.0;
      for (double v : nm) s += v;
      return s / n;
    }
    case RangeStat::Min: return nm.empty() ? kNaN : *std::min_element(nm.begin(), nm.end());
    case RangeStat::Max: return nm.empty() ? kNaN : *std::max_element(nm.begin(), nm.end());
    case RangeStat::Median: {
      if (nm.empty()) return kNaN;
      std::sort(nm.begin(), nm.end());
      const size_t h = nm.size() / 2;
      return nm.size() % 2 ? nm[h] : (nm[h - 1] + nm[h]) / 2.0;
    }
    case RangeStat::Variance:
    case RangeStat::Sd: {
      if (nm.size() < 2) return kNaN;
      double mean, m2, m3, m4;
      moments(&mean, &m2, &m3, &m4);
      const double var = m2 * n / (n - 1.0);
      return st == RangeStat::Sd ? std::sqrt(var) : var;
    }
    case RangeStat::Skewness:
    case RangeStat::Kurtosis: {
      if (nm.size() < 2) return kNaN;
      double mean, m2, m3, m4;
      moments(&mean, &m2, &m3, &m4);
      if (m2 <= 0.0) return kNaN;
      return st == RangeStat::Skewness ? m3 / std::pow(m2, 1.5) : m4 / (m2 * m2);
    }
  }
  return kNaN;
}

}  // namespace

ojson runRangestat(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRangestat.known,
                                            kVocabRangestat.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // The if/in sample: its rows are the only ones that enter a window or receive a result.
  std::vector<char> inSample;
  if (auto err = dataset_shape_detail::ifInMask(session, parsed, &inSample)) return *err;

  // The stat specs follow rangestat.ado's reading: `(stat)` sets the statistic for every
  // variable up to the next `(stat)` (mean when none comes first), and `new = old` names a
  // result. The tokenizer splits the pre-comma text on whitespace; rejoining it and
  // scanning is the same grammar however the user spaced it.
  const std::string head = joinWith(parsed.varlist, " ");
  std::vector<RangeSpec> specs;
  {
    std::string statName = "mean";
    size_t p = 0;
    while (p < head.size()) {
      while (p < head.size() && std::isspace(static_cast<unsigned char>(head[p]))) ++p;
      if (p >= head.size()) break;
      if (head[p] == '(') {
        const size_t close = head.find(')', p);
        if (close == std::string::npos)
          return utilSyntax("rangestat: unclosed `(' in `" + head + "'");
        statName = lowerOf(trimOf(head.substr(p + 1, close - p - 1)));
        p = close + 1;
        continue;
      }
      size_t q = p;
      while (q < head.size() && !std::isspace(static_cast<unsigned char>(head[q])) && head[q] != '(')
        ++q;
      std::string term = head.substr(p, q - p);
      p = q;
      // `new = old` written with blanks around the equal sign
      auto skipWs = [&]() {
        while (p < head.size() && std::isspace(static_cast<unsigned char>(head[p]))) ++p;
      };
      if (term.back() == '=' || (term.find('=') == std::string::npos && [&] {
            skipWs();
            return p < head.size() && head[p] == '=';
          }())) {
        if (term.back() != '=') {
          ++p;  // the `=`
          term += "=";
        }
        skipWs();
        size_t q2 = p;
        while (q2 < head.size() && !std::isspace(static_cast<unsigned char>(head[q2])) &&
               head[q2] != '(')
          ++q2;
        term += head.substr(p, q2 - p);
        p = q2;
      }
      RangeSpec spec;
      if (!parseRangeStatName(statName, &spec.stat)) {
        // Any other word is looked up as a Mata function of that name.
        if (statName != "cov" && statName != "corr" && statName != "reg")
          return utilErrorRc(statName + "() not found", 3499);
        return ojson(eval::unsupportedOption(
            "(" + statName + ")",
            "`rangestat` in this build computes mean, sum, count, min, max, sd, variance, "
            "skewness, kurtosis, median, missing, obs, first, last, firstnm and lastnm. `" +
                statName + "` is a real rangestat statistic this build does not implement; "
                "answering it with another would be a different number under the requested "
                "name."));
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
  {
    // variable names may be abbreviated
    std::vector<std::string> names;
    for (const auto& s : specs) names.push_back(s.src);
    if (auto err = resolveByKeyNames(session, names)) return *err;
    for (size_t i = 0; i < specs.size(); ++i) {
      if (specs[i].target == specs[i].src + "_" + specs[i].statName)
        specs[i].target = names[i] + "_" + specs[i].statName;
      specs[i].src = names[i];
    }
  }

  const std::optional<std::string> intervalOpt = optStr(parsed.opts, "interval");
  if (!intervalOpt.has_value())
    return utilSyntax("rangestat: interval(key lo hi) is required -- without it there is no "
                      "window.");
  std::string ivText = *intervalOpt;
  std::replace(ivText.begin(), ivText.end(), ',', ' ');
  const std::vector<std::string> iv = splitWs(ivText);
  if (iv.size() > 3) return utilErrorRc("extra argument in interval() option: " + iv[3], 198);
  if (iv.size() != 3)
    return utilSyntax("rangestat: interval() takes exactly three terms -- a key variable, a low "
                      "bound and a high bound (`.` for unbounded), got `" + *intervalOpt + "'");
  const std::string keyVar = iv[0];
  // A bound is a number (an offset from the key), `.` (unbounded) or a numeric variable
  // (the bound itself, row by row).
  auto bound = [&](const std::string& t, double* off, std::string* var) -> std::optional<ojson> {
    Datastore* d = session.datastore();
    if (d != nullptr && d->has(t) && d->dtype(t) != "factor") {
      *var = t;
      return std::nullopt;
    }
    if (t == ".") {
      *off = std::numeric_limits<double>::quiet_NaN();
      return std::nullopt;
    }
    if (!parseStrictDouble(t, off))
      return utilErrorRc("was expecting a numeric variable, a number, or a system missing value "
                         "for the interval bound: " + t, 198);
    return std::nullopt;
  };
  double loOff = 0.0, hiOff = 0.0;
  std::string loVar, hiVar;
  if (auto e = bound(iv[1], &loOff, &loVar)) return *e;
  if (auto e = bound(iv[2], &hiOff, &hiVar)) return *e;
  if (loVar.empty() && hiVar.empty() && !std::isnan(loOff) && !std::isnan(hiOff) && loOff > hiOff)
    return utilSyntax("rangestat: interval()'s low offset (" + iv[1] +
                      ") is above its high offset (" + iv[2] + "); the window would be empty.");

  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) byVars = splitWs(*b);
  const bool excludeSelf = hasFlag(parsed.opts, "excludeself");
  const bool casewise = hasFlag(parsed.opts, "casewise");

  std::vector<std::string> readCols = {keyVar};
  if (!loVar.empty()) readCols.push_back(loVar);
  if (!hiVar.empty()) readCols.push_back(hiVar);
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
      return utilErrorRc("'" + s.src + "' found where numeric variable expected", 7);
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
  const double* loCol = loVar.empty() ? nullptr : ds.col(loVar).f64();
  const double* hiCol = hiVar.empty() ? nullptr : ds.col(hiVar).f64();
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(ds, byVars, &nGroups);
  std::vector<const double*> srcPtr;
  for (const auto& s : specs) srcPtr.push_back(ds.col(s.src).f64());

  // The sample is the if/in rows with a key (marksample, markout key); an empty sample is
  // r(2000). With casewise, a row missing any of the variables leaves every window.
  std::vector<char> usable(static_cast<size_t>(n), 0);
  int64_t nUse = 0;
  for (int64_t i = 0; i < n; ++i) {
    bool ok = inSample[static_cast<size_t>(i)] != 0 && !std::isnan(key[i]);
    if (ok) {
      ++nUse;
      if (casewise)
        for (const double* sp : srcPtr)
          if (std::isnan(sp[i])) ok = false;
    }
    usable[static_cast<size_t>(i)] = ok ? 1 : 0;
  }
  if (nUse == 0) return utilErrorRc("no observations", 2000);

  // Row indices per group, sorted by key ascending (ties keep the data order). Rows outside
  // the sample have no window and never enter one, so their results stay missing.
  std::vector<std::vector<int64_t>> order(static_cast<size_t>(std::max<int64_t>(nGroups, 1)));
  for (int64_t i = 0; i < n; ++i) {
    if (!usable[static_cast<size_t>(i)]) continue;
    order[static_cast<size_t>(gid[static_cast<size_t>(i)])].push_back(i);
  }
  for (auto& g : order) {
    std::stable_sort(g.begin(), g.end(),
                     [&](int64_t a, int64_t b) { return key[a] < key[b]; });
  }

  std::vector<std::vector<double>> results(specs.size(), std::vector<double>(static_cast<size_t>(n), kNaN));
  const double inf = std::numeric_limits<double>::infinity();
  for (const auto& g : order) {
    if (g.empty()) continue;
    const size_t m = g.size();
    std::vector<double> gk(m);
    for (size_t j = 0; j < m; ++j) gk[j] = key[g[j]];
    // Prefix totals over the non-missing values, for the statistics a subtraction can give.
    std::vector<std::vector<double>> pSum(specs.size(), std::vector<double>(m + 1, 0.0));
    std::vector<std::vector<double>> pSumSq(specs.size(), std::vector<double>(m + 1, 0.0));
    std::vector<std::vector<int64_t>> pCnt(specs.size(), std::vector<int64_t>(m + 1, 0));
    for (size_t sI = 0; sI < specs.size(); ++sI) {
      for (size_t j = 0; j < m; ++j) {
        const double v = srcPtr[sI][g[j]];
        const bool ok = !std::isnan(v);
        pSum[sI][j + 1] = pSum[sI][j] + (ok ? v : 0.0);
        pSumSq[sI][j + 1] = pSumSq[sI][j] + (ok ? v * v : 0.0);
        pCnt[sI][j + 1] = pCnt[sI][j] + (ok ? 1 : 0);
      }
    }
    for (size_t c = 0; c < m; ++c) {
      const int64_t row = g[c];
      const double k = key[row];
      double lowB = loCol ? loCol[row] : std::isnan(loOff) ? -inf : k + loOff;
      double highB = hiCol ? hiCol[row] : std::isnan(hiOff) ? inf : k + hiOff;
      if (std::isnan(lowB)) lowB = -inf;
      if (std::isnan(highB)) highB = inf;
      const size_t L = static_cast<size_t>(std::lower_bound(gk.begin(), gk.end(), lowB) - gk.begin());
      size_t R = static_cast<size_t>(std::upper_bound(gk.begin(), gk.end(), highB) - gk.begin());
      if (R < L) R = L;
      const int64_t obsInWindow = static_cast<int64_t>(R) - static_cast<int64_t>(L);
      if (obsInWindow <= 0) continue;  // no observations at all -> every result stays missing
      for (size_t sI = 0; sI < specs.size(); ++sI) {
        double& out = results[sI][static_cast<size_t>(row)];
        const RangeStat st = specs[sI].stat;
        const bool fast = !excludeSelf && (st == RangeStat::Mean || st == RangeStat::Sum ||
                                           st == RangeStat::Count || st == RangeStat::Sd);
        if (fast) {
          const int64_t cnt = pCnt[sI][R] - pCnt[sI][L];
          const double sum = pSum[sI][R] - pSum[sI][L];
          if (st == RangeStat::Count) out = static_cast<double>(cnt);
          else if (st == RangeStat::Sum) { if (cnt > 0) out = sum; }
          else if (st == RangeStat::Mean) { if (cnt > 0) out = sum / static_cast<double>(cnt); }
          else if (cnt >= 2) {
            const double ss = (pSumSq[sI][R] - pSumSq[sI][L]) - sum * sum / static_cast<double>(cnt);
            out = std::sqrt(std::max(ss, 0.0) / static_cast<double>(cnt - 1));
          }
          continue;
        }
        std::vector<double> vals;
        vals.reserve(static_cast<size_t>(obsInWindow));
        for (size_t j = L; j < R; ++j) {
          if (excludeSelf && j == c) continue;
          vals.push_back(srcPtr[sI][g[j]]);
        }
        if (vals.empty()) continue;
        out = windowStat(st, vals);
      }
    }
  }

  std::string cur = *session.currentHbin();
  std::vector<std::string> newVars;
  for (size_t sI = 0; sI < specs.size(); ++sI) {
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) cells.push_back(numCell(results[sI][static_cast<size_t>(i)]));
    std::string out;
    if (auto err = writeColumn(session, cur, specs[sI].target, cells, "f64", true, &out)) return *err;
    cur = out;
    newVars.push_back(specs[sI].target);
  }
  session.load(cur);
  for (size_t sI = 0; sI < specs.size(); ++sI) {
    // generated as doubles, labelled "<stat> of <var>"
    session.implicitFloatVars.erase(specs[sI].target);
    VarMeta& vm = session.varMeta[specs[sI].target];
    vm.stataType = "double";
    vm.format = "%10.0g";
    vm.label = specs[sI].statName + " of " + specs[sI].src;
  }
  std::string names;
  for (const auto& v : newVars) names += (names.empty() ? "" : " ") + v;
  ojson mut = mutationResult(session, "gen", newVars);
  ojson blocks = ojson::array();
  blocks.push_back(mut);
  if (hasFlag(parsed.opts, "describe")) {
    ojson desc = dispatch("describe " + names, session);
    if (desc.is_array()) for (const auto& b : desc) blocks.push_back(b);
    else if (desc.is_object()) blocks.push_back(desc);
  }
  if (auto lc = optStr(parsed.opts, "local")) session.eval.locals.set(strutil::trim(*lc), names);
  // rangestat.ado returns the names it generated and the version of its own computation
  // routine.
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("newvars", names);
  session.eval.rStrings.set("rangestat_version", "1.1.1");
  eval::declareRPosted(session.eval, "rangestat", {"newvars", "rangestat_version"});
  return blocks;
}


}  // namespace piet::handlers
