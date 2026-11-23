// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph_bar.cpp -- `graph bar`, `graph hbar`, `graph box` and `graph hbox`.
//
// What a batch run can observe of these graphs is the serset Stata keeps for
// them (read back by `serset use`) and their refusals; this file builds both,
// plus a JSON block listing the bars or boxes. Behaviour per [G-2] graph bar,
// [G-2] graph box (Methods and formulas) and the statistics of [D] collapse,
// as measured against StataBE 19.5 (tests/eval/golden/aer_w2/
// w2ai_graph_family.log).
//
// Vocabulary: a bar graph has up to three roles -- the colour series (bars told
// apart by colour), the category (positions on the axis) and the outer category
// (positions grouping those). A role holds an over() variable, `_variables` (the
// index of the plotted item) or `_spacer` (a constant 1).
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers/graph.h"
#include "piet/loops.h"
#include "piet/serset.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

using OptionList = std::vector<std::pair<std::string, eval::OptValue>>;
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();
constexpr double kMissingKey = std::numeric_limits<double>::infinity();  // a missing code inside a map key

ojson barRefusal(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

// A variable of the bar/dot/box statlist, over() or a pie's varlist, as `set varabbrev on`
// (Stata's default) reads it: the unique prefix of one variable names it (`graph dot (mean)
// pri, over(rep)` on auto is price by rep78), several are r(111) "<word> ambiguous
// abbreviation", none is r(111) "variable <word> not found".
std::optional<ojson> namedVariable(SessionState& session, Datastore& ds, std::string* word) {
  if (ds.has(*word)) return std::nullopt;
  const std::string full = handlers::unabbreviateGraphVar(session, ds, *word);
  if (full != *word) {
    *word = full;
    return std::nullopt;
  }
  size_t hits = 0;
  for (const auto& nm : ds.names()) {
    if (nm.size() > word->size() && nm.compare(0, word->size(), *word) == 0) ++hits;
  }
  const std::string* off = session.eval.globals.find(" __piet_set_varabbrev");
  if (hits > 1 && !(off != nullptr && *off == "off")) return barRefusal(*word + " ambiguous abbreviation", 111);
  return barRefusal("variable " + *word + " not found", 111);
}

std::string lowered(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trimmed(const std::string& s) {
  const size_t first = s.find_first_not_of(" \t");
  if (first == std::string::npos) return std::string();
  return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

std::vector<std::string> words(const std::string& s) {
  std::istringstream in(s);
  std::vector<std::string> out;
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

// True when `typed` abbreviates option `full` with at least `shortest` letters.
bool abbreviates(const std::string& typed, const std::string& full, size_t shortest) {
  return typed.size() >= shortest && typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0;
}

// Missing values as map keys: NaN does not order, so it travels as +inf.
double asKey(double v) { return std::isnan(v) ? kMissingKey : v; }
double fromKey(double k) { return std::isinf(k) ? kMissing : k; }

// Stata's numeric sort order: missing after every number.
int stataCompare(double a, double b) {
  const bool aMiss = std::isnan(a), bMiss = std::isnan(b);
  if (aMiss || bMiss) return aMiss == bMiss ? 0 : (aMiss ? 1 : -1);
  return (a > b) - (a < b);
}

// ===========================================================================
// statistics ([D] collapse)

enum class StatKind {
  Mean, Sd, Sum, RawSum, Count, Max, Min, Iqr, First, FirstNm, Last, LastNm, Percent, Pctile, SeMean,
  SeBinomial, SePoisson
};

struct Statistic {
  StatKind kind = StatKind::Mean;
  int pct = 0;  // for Pctile
};

std::optional<Statistic> statisticNamed(const std::string& typed) {
  const std::string word = lowered(trimmed(typed));
  if (word.empty() || word.find(' ') != std::string::npos) return std::nullopt;
  static const std::map<std::string, StatKind> kWhole = {
      {"mean", StatKind::Mean},   {"sd", StatKind::Sd},         {"sum", StatKind::Sum},
      {"rawsum", StatKind::RawSum}, {"count", StatKind::Count}, {"max", StatKind::Max},
      {"min", StatKind::Min},     {"iqr", StatKind::Iqr},       {"first", StatKind::First},
      {"firstnm", StatKind::FirstNm}, {"last", StatKind::Last}, {"lastnm", StatKind::LastNm},
      {"percent", StatKind::Percent}};
  if (const auto hit = kWhole.find(word); hit != kWhole.end()) return Statistic{hit->second, 0};
  if (word == "median") return Statistic{StatKind::Pctile, 50};
  if (abbreviates(word, "semean", 3)) return Statistic{StatKind::SeMean, 0};
  if (abbreviates(word, "sebinomial", 3)) return Statistic{StatKind::SeBinomial, 0};
  if (abbreviates(word, "sepoisson", 3)) return Statistic{StatKind::SePoisson, 0};
  if (word.size() >= 2 && word[0] == 'p' &&
      std::all_of(word.begin() + 1, word.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
    const int p = std::atoi(word.c_str() + 1);
    if (p >= 1 && p <= 99) return Statistic{StatKind::Pctile, p};
  }
  return std::nullopt;
}

// The statistic as Stata names it in its messages (`p 50` for the median).
std::string statisticTitle(const Statistic& s) {
  switch (s.kind) {
    case StatKind::Mean: return "mean";
    case StatKind::Sd: return "sd";
    case StatKind::Sum: return "sum";
    case StatKind::RawSum: return "rawsum";
    case StatKind::Count: return "count";
    case StatKind::Max: return "max";
    case StatKind::Min: return "min";
    case StatKind::Iqr: return "iqr";
    case StatKind::First: return "first";
    case StatKind::FirstNm: return "firstnm";
    case StatKind::Last: return "last";
    case StatKind::LastNm: return "lastnm";
    case StatKind::Percent: return "percent";
    case StatKind::Pctile: return "p " + std::to_string(s.pct);
    case StatKind::SeMean: return "semean";
    case StatKind::SeBinomial: return "sebinomial";
    case StatKind::SePoisson: return "sepoisson";
  }
  return std::string();
}

bool picksARow(StatKind k) {
  return k == StatKind::First || k == StatKind::FirstNm || k == StatKind::Last || k == StatKind::LastNm;
}

// fweights and pweights count as replicated observations; aweights do not.
bool replicationWeights(const std::string& wtype) {
  return wtype == "fweight" || wtype == "iweight" || wtype == "pweight";
}

// The p-th weighted percentile ([R] summarize, Methods and formulas): sort by
// value, P = W p/100; where the running weight first equals P (and a value
// follows) the midpoint of that value and the next, where it first exceeds P
// that value, else the largest value.
double weightedPercentile(std::vector<std::pair<double, double>> valueWeight, int p) {
  if (valueWeight.empty()) return kMissing;
  std::stable_sort(valueWeight.begin(), valueWeight.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  double total = 0.0;
  for (const auto& vw : valueWeight) total += vw.second;
  const double target = total * static_cast<double>(p) / 100.0;
  double running = 0.0;
  for (size_t i = 0; i < valueWeight.size(); ++i) {
    running += valueWeight[i].second;
    const bool hasNext = i + 1 < valueWeight.size();
    if (running == target && hasNext) return (valueWeight[i].first + valueWeight[i + 1].first) / 2.0;
    if (running > target) return valueWeight[i].first;
  }
  return valueWeight.back().first;
}

// One statistic over one cell: `x` holds the cell's rows in data order
// (missing included), `w` their weights (1 when unweighted).
double cellStatistic(const Statistic& stat, const std::vector<double>& x, const std::vector<double>& w,
                     const std::string& wtype) {
  switch (stat.kind) {
    case StatKind::First: return x.empty() ? kMissing : x.front();
    case StatKind::Last: return x.empty() ? kMissing : x.back();
    default: break;
  }
  std::vector<double> vals, wts;
  for (size_t i = 0; i < x.size(); ++i) {
    if (std::isnan(x[i])) continue;
    vals.push_back(x[i]);
    wts.push_back(w[i]);
  }
  const double nobs = static_cast<double>(vals.size());
  const double wsum = std::accumulate(wts.begin(), wts.end(), 0.0);
  double wxSum = 0.0;
  for (size_t i = 0; i < vals.size(); ++i) wxSum += wts[i] * vals[i];
  const double effectiveN = replicationWeights(wtype) ? wsum : nobs;

  switch (stat.kind) {
    case StatKind::FirstNm: return vals.empty() ? kMissing : vals.front();
    case StatKind::LastNm: return vals.empty() ? kMissing : vals.back();
    case StatKind::Count: return effectiveN;
    case StatKind::Percent: return wsum;  // turned into a share of the total by the caller
    case StatKind::RawSum: return std::accumulate(vals.begin(), vals.end(), 0.0);
    case StatKind::Sum:
      return (wtype == "aweight" && wsum > 0) ? wxSum * nobs / wsum : wxSum;
    default: break;
  }
  if (vals.empty()) return kMissing;
  const double mean = wxSum / wsum;
  switch (stat.kind) {
    case StatKind::Mean: return mean;
    case StatKind::Min: return *std::min_element(vals.begin(), vals.end());
    case StatKind::Max: return *std::max_element(vals.begin(), vals.end());
    case StatKind::Pctile:
    case StatKind::Iqr: {
      std::vector<std::pair<double, double>> vw;
      for (size_t i = 0; i < vals.size(); ++i) vw.emplace_back(vals[i], wts[i]);
      if (stat.kind == StatKind::Pctile) return weightedPercentile(vw, stat.pct);
      return weightedPercentile(vw, 75) - weightedPercentile(vw, 25);
    }
    case StatKind::Sd:
    case StatKind::SeMean: {
      if (effectiveN <= 1) return kMissing;
      double squares = 0.0;
      for (size_t i = 0; i < vals.size(); ++i) squares += wts[i] * (vals[i] - mean) * (vals[i] - mean);
      const double sd = std::sqrt((squares / wsum) * effectiveN / (effectiveN - 1));
      return stat.kind == StatKind::Sd ? sd : sd / std::sqrt(effectiveN);
    }
    case StatKind::SeBinomial:
      return (mean < 0 || mean > 1) ? kMissing : std::sqrt(mean * (1 - mean) / effectiveN);
    case StatKind::SePoisson:
      return mean < 0 ? kMissing : std::sqrt(mean / effectiveN);
    default: return kMissing;
  }
}

// The storage type collapse gives a result.
std::string resultType(const Statistic& stat, const std::string& sourceType, const std::string& wtype) {
  switch (stat.kind) {
    case StatKind::Sum:
    case StatKind::RawSum:
    case StatKind::Percent: return "double";
    case StatKind::Count: return replicationWeights(wtype) ? "double" : "long";
    case StatKind::Max:
    case StatKind::Min:
    case StatKind::First:
    case StatKind::Last:
    case StatKind::FirstNm:
    case StatKind::LastNm: return sourceType;
    default: return (sourceType == "double" || sourceType == "long") ? "double" : "float";
  }
}

// 100 x cell / total of the cells (missing when the total is not positive).
void asPercentOfTotal(std::vector<double>& cells) {
  double total = 0.0;
  for (double c : cells) {
    if (!std::isnan(c)) total += c;
  }
  for (double& c : cells) c = total > 0 ? 100.0 * c / total : kMissing;
}

double storedAs(double v, const std::string& type) { return type == "float" ? serset::toFloat(v) : v; }

// ===========================================================================
// over()

struct OverRequest {
  std::string var;
  std::string sortText;  // the sort() content, "" when none
  bool descending = false;
};

// over(varname [, suboptions]): the variable by its name or a unique prefix of
// one; sort() and descending are the suboptions that change the serset.
std::optional<ojson> readOver(const std::string& text, Datastore& ds, OverRequest* out) {
  const long long comma = eval::findUnquotedComma(text);
  const std::string head = comma >= 0 ? text.substr(0, static_cast<size_t>(comma)) : text;
  const std::vector<std::string> names = words(head);
  if (names.empty()) return barRefusal("varlist required", 100);
  if (names.size() > 1) return barRefusal("too many variables specified", 103);
  out->var = names.front();
  if (!ds.has(out->var)) {
    std::vector<std::string> candidates;
    for (const auto& nm : ds.names()) {
      if (nm.compare(0, out->var.size(), out->var) == 0) candidates.push_back(nm);
    }
    if (candidates.size() != 1) return barRefusal("variable " + out->var + " not found", 111);
    out->var = candidates.front();
  }
  if (comma < 0) return std::nullopt;
  for (const auto& [key, val] : eval::parseOpts(text.substr(static_cast<size_t>(comma) + 1))) {
    const std::string k = lowered(key);
    if (k == "sort") {
      out->sortText = val.isBool ? std::string() : trimmed(val.strValue);
    } else if (abbreviates(k, "descending", 3)) {
      out->descending = true;
    }
  }
  return std::nullopt;
}

// An over() variable coded over the sample: numeric values ranked 1..k (type
// float; with `missing` all missing rows share code k+1), strings ranked in
// byte order 1..k (type long; "" stays a missing code). `labels[c-1]` is code
// c's category label.
struct CategoryCoding {
  std::string type;
  std::vector<double> code;  // per data row; missing outside the sample
  std::vector<std::string> labels;
};

CategoryCoding codeCategories(SessionState& session, Datastore& ds, const std::string& var,
                              const std::vector<char>& inSample, bool keepMissing) {
  CategoryCoding cc;
  const int64_t nRows = ds.nRows();
  cc.code.assign(static_cast<size_t>(nRows), kMissing);
  const cecon::Column& col = ds.col(var);
  if (ds.dtype(var) == "f64") {
    cc.type = "float";
    const double* d = col.f64();
    std::set<double> distinct;
    bool sawMissing = false;
    for (int64_t i = 0; i < nRows; ++i) {
      if (!inSample[static_cast<size_t>(i)]) continue;
      if (std::isnan(d[i])) {
        sawMissing = true;
      } else {
        distinct.insert(d[i]);
      }
    }
    const auto valueLabels = session.valueLabels.find(var);
    std::map<double, int> rank;
    for (double v : distinct) {
      rank[v] = static_cast<int>(rank.size()) + 1;
      std::string label;
      if (valueLabels != session.valueLabels.end() && v == std::floor(v)) {
        const auto hit = valueLabels->second.find(static_cast<int>(v));
        if (hit != valueLabels->second.end()) label = hit->second;
      }
      cc.labels.push_back(label.empty() ? piet::fmtStataDefaultG(v) : label);
    }
    const double missingCode = static_cast<double>(distinct.size() + 1);
    if (keepMissing && sawMissing) cc.labels.push_back(".");
    for (int64_t i = 0; i < nRows; ++i) {
      if (!inSample[static_cast<size_t>(i)]) continue;
      if (!std::isnan(d[i])) {
        cc.code[static_cast<size_t>(i)] = rank[d[i]];
      } else if (keepMissing) {
        cc.code[static_cast<size_t>(i)] = missingCode;
      }
    }
    return cc;
  }
  cc.type = "long";
  const int32_t* codes = col.codes();
  auto textAt = [&](int64_t i) -> const std::string* {
    const int32_t c = codes[i];
    if (c < 0 || static_cast<size_t>(c) >= col.levels.size()) return nullptr;
    return &col.levels[static_cast<size_t>(c)];
  };
  std::set<std::string> distinct;
  for (int64_t i = 0; i < nRows; ++i) {
    const std::string* t = inSample[static_cast<size_t>(i)] ? textAt(i) : nullptr;
    if (t != nullptr && !t->empty()) distinct.insert(*t);
  }
  std::map<std::string, int> rank;
  for (const auto& t : distinct) {
    rank[t] = static_cast<int>(rank.size()) + 1;
    cc.labels.push_back(t);
  }
  for (int64_t i = 0; i < nRows; ++i) {
    const std::string* t = inSample[static_cast<size_t>(i)] ? textAt(i) : nullptr;
    if (t == nullptr) continue;
    const auto hit = rank.find(*t);
    if (hit != rank.end()) cc.code[static_cast<size_t>(i)] = hit->second;
  }
  return cc;
}

// ===========================================================================
// the serset under construction: numeric columns of equal length

class GridTable {
 public:
  serset::Column& add(const std::string& name, const std::string& type) {
    serset::Column c;
    c.name = name;
    c.type = type;
    c.num.assign(static_cast<size_t>(rows_), kMissing);
    cols_.push_back(std::move(c));
    return cols_.back();
  }
  // The column `name`, or null.
  serset::Column* find(const std::string& name) {
    for (auto& c : cols_) {
      if (c.name == name) return &c;
    }
    return nullptr;
  }
  int64_t rows() const { return rows_; }
  // Appends an all-missing row and returns its index.
  size_t newRow() {
    for (auto& c : cols_) c.num.push_back(kMissing);
    return static_cast<size_t>(rows_++);
  }
  std::vector<serset::Column>& columns() { return cols_; }

 private:
  std::vector<serset::Column> cols_;
  int64_t rows_ = 0;
};

// The distinct values of a column in Stata's order (missing last).
std::vector<double> distinctSorted(const std::vector<double>& v) {
  std::vector<double> out(v);
  std::sort(out.begin(), out.end(), [](double a, double b) { return stataCompare(a, b) < 0; });
  out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return stataCompare(a, b) == 0; }),
            out.end());
  return out;
}

// The key of a row over some columns, missing carried as +inf.
std::vector<double> rowKey(const std::vector<serset::Column*>& keyCols, size_t row) {
  std::vector<double> k;
  k.reserve(keyCols.size());
  for (const serset::Column* c : keyCols) k.push_back(asKey(c->num[row]));
  return k;
}

// Adds a row, missing elsewhere, for each combination of the key columns'
// present values that no row has yet (Stata's fillin).
void completeCombinations(GridTable& t, const std::vector<serset::Column*>& keyCols) {
  if (keyCols.size() < 2 || t.rows() == 0) return;
  std::vector<std::vector<double>> levels;
  for (const serset::Column* c : keyCols) levels.push_back(distinctSorted(c->num));
  std::set<std::vector<double>> have;
  for (size_t r = 0; r < static_cast<size_t>(t.rows()); ++r) have.insert(rowKey(keyCols, r));
  std::vector<double> combo(keyCols.size());
  std::function<void(size_t)> walk = [&](size_t depth) {
    if (depth == keyCols.size()) {
      std::vector<double> asKeys;
      for (double v : combo) asKeys.push_back(asKey(v));
      if (have.count(asKeys) != 0) return;
      const size_t r = t.newRow();
      for (size_t i = 0; i < keyCols.size(); ++i) keyCols[i]->num[r] = combo[i];
      return;
    }
    for (double v : levels[depth]) {
      combo[depth] = v;
      walk(depth + 1);
    }
  };
  walk(0);
}

// Reorders every row by the keys (column, ascending?), stable.
void orderRows(GridTable& t, const std::vector<std::pair<const serset::Column*, bool>>& keys) {
  std::vector<size_t> perm(static_cast<size_t>(t.rows()));
  std::iota(perm.begin(), perm.end(), size_t{0});
  std::stable_sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
    for (const auto& [col, ascending] : keys) {
      const int c = stataCompare(col->num[a], col->num[b]);
      if (c != 0) return ascending ? c < 0 : c > 0;
    }
    return false;
  });
  for (auto& c : t.columns()) {
    std::vector<double> reordered(perm.size());
    for (size_t i = 0; i < perm.size(); ++i) reordered[i] = c.num[perm[i]];
    c.num = std::move(reordered);
  }
}

// The type a `replace` gives an integer column once it holds `v`.
std::string widenedFor(const std::string& type, double v) {
  if (std::isnan(v)) return type;
  const bool whole = std::floor(v) == v;
  if (type == "byte") {
    if (!whole) return "float";
    return (v < -127 || v > 100) ? widenedFor("int", v) : type;
  }
  if (type == "int") {
    if (!whole) return "float";
    return (v < -32767 || v > 32740) ? widenedFor("long", v) : type;
  }
  if (type == "long") {
    if (!whole) return "double";
    return (v < -2147483647.0 || v > 2147483620.0) ? "double" : type;
  }
  return type;
}

// The storage of a box graph's `_values` when the y variables differ in type:
// an integer type takes a wider incoming type; float and double keep theirs
// (StataBE 19.5: byte+int int, long+float float, float+double float).
std::string combinedValueType(const std::string& sofar, const std::string& next) {
  static const std::vector<std::string> kWidth = {"byte", "int", "long", "float", "double"};
  auto widthOf = [](const std::string& t) {
    const auto p = std::find(kWidth.begin(), kWidth.end(), t);
    return p == kWidth.end() ? static_cast<long>(kWidth.size()) : static_cast<long>(p - kWidth.begin());
  };
  const bool integerSoFar = sofar == "byte" || sofar == "int" || sofar == "long";
  return (integerSoFar && widthOf(next) > widthOf(sofar)) ? next : sofar;
}

// ===========================================================================
// the command

struct BarOptions {
  bool asyvars = false, ascategory = false, casewise = false, keepMissing = false, percentages = false;
  bool stack = false, horizontal = false, noOutsides = false;
  std::vector<std::string> overTexts;
  bool by = false;
};

BarOptions readBarOptions(const OptionList& opts, const std::string& kind) {
  BarOptions o;
  // A dot chart is a bar graph drawn with dots, horizontal unless `vertical`.
  o.horizontal = kind == "hbar" || kind == "hbox" || kind == "dot";
  bool vertical = false;
  for (const auto& [key, v] : opts) {
    const std::string k = lowered(key);
    const bool on = !v.isBool || v.boolValue;
    if (abbreviates(k, "over", 1)) {
      if (!v.isBool) o.overTexts.push_back(v.strValue);
    } else if (abbreviates(k, "asyvars", 3)) {
      o.asyvars = on;
    } else if (abbreviates(k, "ascategory", 3)) {
      o.ascategory = on;
    } else if (k == "cw") {
      o.casewise = on;
    } else if (abbreviates(k, "missing", 4)) {
      o.keepMissing = on;
    } else if (abbreviates(k, "percentages", 3)) {
      o.percentages = on;
    } else if (k == "stack") {
      o.stack = on;
    } else if (abbreviates(k, "horizontal", 3)) {
      o.horizontal = on;
    } else if (abbreviates(k, "vertical", 4)) {
      vertical = on;
    } else if (abbreviates(k, "nooutsides", 5)) {
      o.noOutsides = on;
    } else if (k == "by") {
      o.by = true;
      break;  // refused before anything else is read
    }
  }
  if (vertical) o.horizontal = false;
  return o;
}

// One plotted item of the statlist: statistic, source variable, result name.
struct PlotItem {
  Statistic stat;
  std::string var;
  std::string result;
};

struct Statlist {
  std::vector<PlotItem> items;
  bool meanci = false;
  std::optional<ojson> err;
};

// `(stat) varlist [(stat) varlist ...]`, with `newname=varname` chunks.
Statlist readStatlist(SessionState& session, Datastore& ds, const std::vector<std::string>& args,
                      bool dropRepeats) {
  Statlist sl;
  Statistic current;
  std::vector<std::string> chunk;
  auto closeChunk = [&]() -> bool {
    if (chunk.empty()) return true;
    std::string text;
    for (const auto& word : chunk) text += (text.empty() ? "" : " ") + word;
    chunk.clear();
    const size_t eq = text.find('=');
    if (eq != std::string::npos) {
      const std::string target = trimmed(text.substr(0, eq));
      std::string source = trimmed(text.substr(eq + 1));
      if (auto bad = namedVariable(session, ds, &source)) {
        sl.err = *bad;
        return false;
      }
      sl.items.push_back({current, source, target});
      return true;
    }
    std::vector<std::string> chunkVars;
    const loops::VarlistExpandResult ex = loops::expandVarlist(text, session);
    if (ex.isError) {
      sl.err = ex.error;
      return false;
    }
    for (std::string v : ex.tokens) {
      if (auto bad = namedVariable(session, ds, &v)) {
        sl.err = *bad;
        return false;
      }
      // In a bar graph a variable repeated in one varlist is plotted once
      // (crgb_edges br_dupvar); a box graph refuses it later (bx_dup).
      if (dropRepeats && std::find(chunkVars.begin(), chunkVars.end(), v) != chunkVars.end()) continue;
      chunkVars.push_back(v);
      sl.items.push_back({current, v, v});
    }
    return true;
  };
  for (const auto& tok : args) {
    const bool statToken = tok.size() >= 2 && tok.front() == '(' && tok.back() == ')';
    if (!statToken) {
      chunk.push_back(tok);
      continue;
    }
    if (!closeChunk()) return sl;
    const std::string inside = trimmed(tok.substr(1, tok.size() - 2));
    if (lowered(inside) == "meanci") {
      sl.meanci = true;
      current = Statistic{};
      continue;
    }
    const std::optional<Statistic> st = statisticNamed(inside);
    if (!st.has_value()) {
      sl.err = barRefusal("(" + inside + ") invalid statistic", 198);
      return sl;
    }
    current = *st;
  }
  closeChunk();
  return sl;
}

std::string fullWeightName(const eval::ParsedCommand& parsed) {
  if (!parsed.weightType.has_value()) return std::string();
  const std::string w = lowered(*parsed.weightType);
  if (w == "fw" || w == "fweight") return "fweight";
  if (w == "aw" || w == "aweight") return "aweight";
  if (w == "pw" || w == "pweight") return "pweight";
  if (w == "iw" || w == "iweight") return "iweight";
  return w;
}

constexpr const char* kVariablesRole = "_variables";
constexpr const char* kSpacerRole = "_spacer";

// The three roles, by name ("" when absent).
struct Roles {
  std::string colourSeries, category, outerCategory;
  bool itemsAreRole() const { return colourSeries == kVariablesRole || colourSeries == kSpacerRole; }
};

// Who plays which role, from the number of plotted items and the over()
// variables in the order typed.
std::optional<ojson> assignRoles(size_t nItems, const std::vector<std::string>& over, bool asyvars,
                                 bool ascategory, Roles* r) {
  const size_t k = over.size();
  auto at = [&](size_t i) { return i < k ? over[i] : std::string(); };
  if (nItems > 1) {
    if (k > 2) return barRefusal("may not specify more than two by variables when a varlist is specified", 198);
    if (ascategory && k < 2) {
      r->colourSeries = kSpacerRole;
      r->category = kVariablesRole;
      r->outerCategory = at(0);
    } else {
      r->colourSeries = kVariablesRole;
      r->category = at(0);
      r->outerCategory = at(1);
    }
    return std::nullopt;
  }
  if (k == 3) {
    r->colourSeries = over[0];
    r->category = over[1];
    r->outerCategory = over[2];
  } else if (asyvars && k > 0) {
    r->colourSeries = over[0];
    r->category = at(1);
  } else {
    r->colourSeries = kVariablesRole;
    r->category = at(0);
    r->outerCategory = at(1);
  }
  return std::nullopt;
}

// Which over() request carries each role's sort() and descending.
struct RoleOptions {
  const OverRequest* colourSeries = nullptr;
  const OverRequest* category = nullptr;
  const OverRequest* outerCategory = nullptr;
};

RoleOptions roleOptions(const std::vector<OverRequest>& overs, bool asyvars, bool ascategory) {
  RoleOptions ro;
  auto at = [&](size_t i) -> const OverRequest* { return i < overs.size() ? &overs[i] : nullptr; };
  if (overs.size() == 3) {
    ro = {at(0), at(1), at(2)};
  } else if (asyvars) {
    ro = {at(0), at(1), nullptr};
  } else if (ascategory && overs.size() == 1) {
    ro = {nullptr, nullptr, at(0)};
  } else {
    ro = {nullptr, at(0), at(1)};
  }
  return ro;
}

// Everything the serset builders share.
struct BarContext {
  SessionState& session;
  Datastore& ds;
  const BarOptions& opt;
  const Roles& roles;
  const std::vector<std::string>& byOrder;  // over() variables, last typed first
  std::map<std::string, CategoryCoding>& codes;
  // Cells: over codes in byOrder (missing as +inf) -> the cell's sample rows.
  const std::map<std::vector<double>, std::vector<int64_t>>& cells;
  const std::vector<double>& weight;  // per data row
  const std::string& wtype;
  bool seriesAscending, categoryAscending, outerAscending;
};

// The label of a role value in the block.
std::string roleLabel(const BarContext& cx, const std::string& role, double code,
                      const std::vector<std::string>& itemLabels) {
  const size_t c = std::isnan(code) ? 0 : static_cast<size_t>(code);
  if (role == kVariablesRole) return (c >= 1 && c <= itemLabels.size()) ? itemLabels[c - 1] : std::string();
  const auto hit = cx.codes.find(role);
  if (hit == cx.codes.end()) return std::string();
  return (c >= 1 && c <= hit->second.labels.size()) ? hit->second.labels[c - 1] : std::string(".");
}

// Writes a cell's over codes into row `r` of the over columns present.
void putCellCodes(GridTable& t, const BarContext& cx, const std::vector<double>& cellKey, size_t r) {
  for (size_t i = 0; i < cx.byOrder.size(); ++i) {
    if (serset::Column* c = t.find(cx.byOrder[i])) c->num[r] = fromKey(cellKey[i]);
  }
}

// One statistic of `var` per cell, in cell order, at its result type.
std::vector<double> perCell(const BarContext& cx, const Statistic& stat, const std::string& var,
                            const std::string& type) {
  const double* src = cx.ds.col(var).f64();
  std::vector<double> out;
  for (const auto& [key, rows] : cx.cells) {
    std::vector<double> x, w;
    for (int64_t r : rows) {
      x.push_back(src[r]);
      w.push_back(cx.weight[static_cast<size_t>(r)]);
    }
    out.push_back(cellStatistic(stat, x, w, cx.wtype));
  }
  if (stat.kind == StatKind::Percent) asPercentOfTotal(out);
  for (double& v : out) v = storedAs(v, type);
  return out;
}

// One plotted series: result name, storage type, value per cell.
struct Series {
  std::string name, type;
  std::vector<double> values;
};

// A sort() request at one level ("var", "grp" or "sup").
struct LevelSort {
  std::string level;
  int category = 0;                      // sort(#), else 0
  std::optional<Statistic> stat;         // sort((stat) varname)
  std::string var;
};

std::optional<ojson> readLevelSort(const std::string& level, const std::string& text, LevelSort* out) {
  out->level = level;
  if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
    out->category = std::atoi(text.c_str());
    return std::nullopt;
  }
  out->stat = Statistic{};
  out->var = text;
  if (text.front() == '(') {
    const size_t close = text.find(')');
    if (close == std::string::npos) return barRefusal("sort() invalid", 198);
    const std::string inside = text.substr(1, close - 1);
    out->stat = statisticNamed(inside);
    if (!out->stat.has_value()) return barRefusal("(" + trimmed(inside) + ") invalid statistic", 198);
    out->var = trimmed(text.substr(close + 1));
  }
  return std::nullopt;
}

std::string sortColumnName(const std::string& level) { return "_" + level + "_sort"; }

// graph bar / hbar: the collapsed values reshaped per role, the fill rows, the
// sort() columns and the row order.
std::optional<ojson> buildBarSerset(BarContext& cx, const std::vector<PlotItem>& items, bool barePercent,
                                    const RoleOptions& ro, GridTable& t, ojson& drawn) {
  const Roles& roles = cx.roles;
  std::vector<Series> series;
  std::vector<std::string> itemLabels;
  if (barePercent) {
    Series s{"percent", "double", {}};
    for (const auto& [key, rows] : cx.cells) {
      std::vector<double> ones(rows.size(), 1.0), w;
      for (int64_t r : rows) w.push_back(cx.weight[static_cast<size_t>(r)]);
      s.values.push_back(cellStatistic(Statistic{StatKind::Percent, 0}, ones, w, cx.wtype));
    }
    asPercentOfTotal(s.values);
    series.push_back(std::move(s));
    itemLabels.push_back("percent");
  } else {
    cx.session.syncEvalVarLabels();
    for (const PlotItem& it : items) {
      const std::string type = resultType(it.stat, serset::storageType(cx.session, cx.ds, it.var), cx.wtype);
      series.push_back({it.result, type, perCell(cx, it.stat, it.var, type)});
      const std::string label = cx.session.eval.varLabels.get(it.var, std::string());
      itemLabels.push_back(label.empty() ? it.var : label);
    }
  }

  // sort() requests, colour-series level first.
  std::vector<LevelSort> byVariable, byHeight;
  for (const auto& [level, req] : {std::make_pair("var", ro.colourSeries), std::make_pair("grp", ro.category),
                                   std::make_pair("sup", ro.outerCategory)}) {
    if (req == nullptr || req->sortText.empty()) continue;
    LevelSort ls;
    if (auto bad = readLevelSort(level, req->sortText, &ls)) return bad;
    if (!ls.stat.has_value()) {
      if (ls.category > 0) byHeight.push_back(ls);
      continue;  // sort(0) adds no column (crgb_edges br_sort0)
    }
    // An unknown or string sort variable adds no column either, as in StataBE
    // (crgb_edges br_sortnovar, br_sortstr).
    if (cx.ds.has(ls.var) && cx.ds.dtype(ls.var) == "f64") byVariable.push_back(ls);
  }

  // Columns: the value column(s), the role columns, the sort inputs.
  const bool longForm = roles.itemsAreRole();
  if (longForm) {
    t.add("_values", "double");
    if (roles.colourSeries == kSpacerRole) t.add(kSpacerRole, "float");
    t.add(kVariablesRole, "long");
  } else {
    for (const Series& s : series) t.add(s.name, s.type);
  }
  for (const std::string* role : {&roles.colourSeries, &roles.category, &roles.outerCategory}) {
    if (!role->empty() && t.find(*role) == nullptr) t.add(*role, cx.codes.at(*role).type);
  }
  // Per-cell inputs of sort(varname): the colour-series level's is its final
  // column; the other levels' are averaged later.
  std::vector<std::vector<double>> sortInputs;
  std::vector<serset::Column*> sortInputCols;
  for (const LevelSort& ls : byVariable) {
    const std::string type = resultType(*ls.stat, serset::storageType(cx.session, cx.ds, ls.var), cx.wtype);
    sortInputs.push_back(perCell(cx, *ls.stat, ls.var, type));
    t.add(ls.level == "var" ? sortColumnName("var") : "#" + ls.level + "-input", type);
  }
  for (const LevelSort& ls : byVariable) {
    sortInputCols.push_back(t.find(ls.level == "var" ? sortColumnName("var") : "#" + ls.level + "-input"));
  }

  // Rows: one per (item, cell) in long form, else one per cell.
  auto fillCell = [&](size_t cellIndex, const std::vector<double>& key, size_t r) {
    putCellCodes(t, cx, key, r);
    for (size_t s = 0; s < sortInputs.size(); ++s) sortInputCols[s]->num[r] = sortInputs[s][cellIndex];
  };
  if (longForm) {
    for (size_t item = 0; item < series.size(); ++item) {
      size_t cellIndex = 0;
      for (const auto& [key, rows] : cx.cells) {
        const size_t r = t.newRow();
        t.find("_values")->num[r] = series[item].values[cellIndex];
        t.find(kVariablesRole)->num[r] = static_cast<double>(item + 1);
        fillCell(cellIndex++, key, r);
      }
    }
  } else {
    size_t cellIndex = 0;
    for (const auto& [key, rows] : cx.cells) {
      const size_t r = t.newRow();
      for (const Series& s : series) t.find(s.name)->num[r] = s.values[cellIndex];
      fillCell(cellIndex++, key, r);
    }
  }

  std::vector<serset::Column*> roleCols;
  for (const std::string* role : {&roles.outerCategory, &roles.category, &roles.colourSeries}) {
    if (!role->empty()) roleCols.push_back(t.find(*role));
  }
  completeCombinations(t, roleCols);
  if (serset::Column* spacer = t.find(kSpacerRole)) std::fill(spacer->num.begin(), spacer->num.end(), 1.0);

  auto levelKeyCols = [&](const std::string& level) {
    std::vector<serset::Column*> cols;
    if (!roles.outerCategory.empty()) cols.push_back(t.find(roles.outerCategory));
    if (level != "sup" && !roles.category.empty()) cols.push_back(t.find(roles.category));
    if (level == "var") cols.push_back(t.find(roles.colourSeries));
    return cols;
  };
  const size_t nRows = static_cast<size_t>(t.rows());

  // sort(varname) above the colour-series level: the mean of the per-row inputs
  // over the rows of the same (outer category[, category]), as float.
  std::vector<std::string> sortCols;
  for (const LevelSort& ls : byVariable) {
    if (ls.level == "var") {
      sortCols.push_back(sortColumnName("var"));
      continue;
    }
    const serset::Column* input = t.find("#" + ls.level + "-input");
    const std::vector<serset::Column*> keyCols = levelKeyCols(ls.level);
    std::map<std::vector<double>, std::pair<double, int>> sumCount;
    for (size_t r = 0; r < nRows; ++r) {
      if (std::isnan(input->num[r])) continue;
      auto& acc = sumCount[rowKey(keyCols, r)];
      acc.first += input->num[r];
      acc.second += 1;
    }
    std::vector<double> means(nRows, kMissing);
    for (size_t r = 0; r < nRows; ++r) {
      const auto hit = sumCount.find(rowKey(keyCols, r));
      if (hit != sumCount.end() && hit->second.second > 0) {
        means[r] = serset::toFloat(hit->second.first / hit->second.second);
      }
    }
    t.add(sortColumnName(ls.level), "float").num = std::move(means);
    sortCols.push_back(sortColumnName(ls.level));
  }

  // sort(#): the height of category # summed over the rows of the level's key.
  const std::string valueName = longForm ? std::string("_values") : series.front().name;
  for (const LevelSort& ls : byHeight) {
    const std::vector<double>& value = t.find(valueName)->num;
    std::vector<double> height(nRows, kMissing);
    if (ls.level == "var") {
      height = value;
    } else {
      const serset::Column* numbered = t.find(cx.opt.ascategory ? std::string(kVariablesRole) : roles.colourSeries);
      if (numbered == nullptr) numbered = t.find(roles.colourSeries);
      const std::vector<double> levels = distinctSorted(numbered->num);
      for (size_t r = 0; r < nRows; ++r) {
        const double g = numbered->num[r];
        if (std::isnan(g)) continue;
        const auto pos = std::lower_bound(levels.begin(), levels.end(), g,
                                          [](double a, double b) { return stataCompare(a, b) < 0; });
        const double number = static_cast<double>(pos - levels.begin() + 1);
        height[r] = number == ls.category ? value[r] : (std::isnan(value[r]) ? kMissing : 0.0);
      }
    }
    if (std::all_of(height.begin(), height.end(), [](double h) { return h == 0.0; })) {
      return barRefusal("sort() invalid, category " + std::to_string(ls.category) + " not found", 198);
    }
    const std::vector<serset::Column*> keyCols = levelKeyCols(ls.level);
    if (ls.level == "grp" && cx.opt.percentages) {
      std::map<std::vector<double>, double> groupTotal;
      for (size_t r = 0; r < nRows; ++r) {
        if (!std::isnan(value[r])) groupTotal[rowKey(keyCols, r)] += value[r];
      }
      for (size_t r = 0; r < nRows; ++r) height[r] = height[r] / groupTotal[rowKey(keyCols, r)];
    }
    std::map<std::vector<double>, double> summed;
    for (size_t r = 0; r < nRows; ++r) summed[rowKey(keyCols, r)] += std::isnan(height[r]) ? 0.0 : height[r];
    std::vector<double> col(nRows);
    for (size_t r = 0; r < nRows; ++r) col[r] = serset::toFloat(summed[rowKey(keyCols, r)]);
    if (serset::Column* existing = t.find(sortColumnName(ls.level))) {
      existing->type = "float";
      existing->num = std::move(col);
    } else {
      t.add(sortColumnName(ls.level), "float").num = std::move(col);
      sortCols.push_back(sortColumnName(ls.level));
    }
  }

  // Keep the value column(s), the roles and the sort columns, in that order.
  std::vector<std::string> keep;
  if (longForm) {
    keep.push_back("_values");
  } else {
    for (const Series& s : series) keep.push_back(s.name);
  }
  for (const std::string* role : {&roles.colourSeries, &roles.category, &roles.outerCategory}) {
    if (!role->empty()) keep.push_back(*role);
  }
  keep.insert(keep.end(), sortCols.begin(), sortCols.end());
  std::vector<serset::Column> kept;
  std::set<std::string> taken;
  for (const auto& name : keep) {
    if (!taken.insert(name).second) continue;
    if (serset::Column* c = t.find(name)) kept.push_back(*c);
  }
  t.columns() = std::move(kept);

  // Row order: outer category, category, colour series -- each by its sort column first.
  std::vector<std::pair<const serset::Column*, bool>> keys;
  auto addLevel = [&](const std::string& role, const std::string& level, bool ascending) {
    if (role.empty()) return;
    if (const serset::Column* s = t.find(sortColumnName(level))) keys.emplace_back(s, ascending);
    keys.emplace_back(t.find(role), ascending);
  };
  addLevel(roles.outerCategory, "sup", cx.outerAscending);
  addLevel(roles.category, "grp", cx.categoryAscending);
  addLevel(roles.colourSeries, "var", cx.seriesAscending);
  orderRows(t, keys);

  const serset::Column* valueCol = t.find(valueName);
  for (size_t r = 0; r < static_cast<size_t>(t.rows()); ++r) {
    ojson bar;
    const double v = valueCol->num[r];
    bar["value"] = std::isnan(v) ? ojson(nullptr) : ojson(v);
    const std::array<std::pair<const std::string*, const char*>, 3> named = {
        {{&roles.colourSeries, "series"}, {&roles.category, "category"}, {&roles.outerCategory, "supercategory"}}};
    for (const auto& [role, key] : named) {
      if (role->empty()) continue;
      const serset::Column* c = t.find(*role);
      if (c != nullptr) bar[key] = roleLabel(cx, *role, c->num[r], itemLabels);
    }
    drawn.push_back(bar);
  }
  return std::nullopt;
}

// The five statistics of one box: median, p25, p75 and the adjacent values,
// with the outside values in data order ([G-2] graph box, Methods and
// formulas: fences at p25 - 1.5 IQR and p75 + 1.5 IQR).
struct BoxSummary {
  std::array<double, 5> stat{};  // median, p25, p75, lower adjacent, upper adjacent
  std::vector<double> outside;
};

BoxSummary summarizeBox(const std::vector<double>& x, const std::vector<double>& w, bool weighted) {
  std::vector<std::pair<double, double>> vw;
  for (size_t i = 0; i < x.size(); ++i) {
    if (!std::isnan(x[i])) vw.emplace_back(x[i], weighted ? w[i] : 1.0);
  }
  const double p25 = weightedPercentile(vw, 25);
  const double p50 = weightedPercentile(vw, 50);
  const double p75 = weightedPercentile(vw, 75);
  const double lowFence = 2.5 * p25 - 1.5 * p75;
  const double highFence = 2.5 * p75 - 1.5 * p25;
  double lowAdj = kMissing, highAdj = kMissing;
  BoxSummary b;
  // A zero IQR leaves no adjacent values: anything off the fence is outside.
  const bool collapsed = (std::isnan(lowFence) && std::isnan(highFence)) || lowFence == highFence;
  for (double v : x) {
    if (std::isnan(v)) continue;
    if (collapsed) {
      if (!(v == lowFence && v == highFence)) b.outside.push_back(v);
      continue;
    }
    if (v >= lowFence && v < p25) lowAdj = std::isnan(lowAdj) ? v : std::min(lowAdj, v);
    if (v <= highFence && v > p75) highAdj = std::isnan(highAdj) ? v : std::max(highAdj, v);
    if (!(lowFence <= v && v <= highFence)) b.outside.push_back(v);
  }
  b.stat = {p50, p25, p75, lowAdj, highAdj};
  return b;
}

ojson numberOrNull(double v) { return std::isnan(v) ? ojson(nullptr) : ojson(v); }

// graph box / hbox: five statistic rows and the outside values per box, the
// fill rows, and the row order.
std::optional<ojson> buildBoxSerset(BarContext& cx, const std::vector<PlotItem>& items, GridTable& t,
                                    ojson& drawn) {
  const Roles& roles = cx.roles;
  std::vector<std::string> yvars;
  for (const PlotItem& it : items) yvars.push_back(it.var);

  std::string valueType;
  for (const auto& v : yvars) {
    const std::string st = serset::storageType(cx.session, cx.ds, v);
    valueType = valueType.empty() ? st : combinedValueType(valueType, st);
  }
  const bool several = yvars.size() > 1;
  const std::string indexType = several ? (yvars.size() <= 100 ? "byte" : "int") : "float";

  // One box per (cell, y variable), ordered by the over codes then the y index.
  struct Box {
    std::vector<double> key;
    size_t item = 0;
    std::vector<double> x, w;
  };
  std::map<std::pair<std::vector<double>, size_t>, Box> boxes;
  for (size_t item = 0; item < yvars.size(); ++item) {
    const double* src = cx.ds.col(yvars[item]).f64();
    for (const auto& [key, rows] : cx.cells) {
      for (int64_t r : rows) {
        Box& b = boxes[{key, item}];
        b.key = key;
        b.item = item;
        b.x.push_back(src[r]);
        b.w.push_back(cx.weight[static_cast<size_t>(r)]);
      }
    }
  }

  // Columns: `_values`, the roles (colour series, category, outer category),
  // `_box_sort`. Under ascategory the colour series is `_spacer` (byte 1) and
  // `_variables` the category (crgb_edges bx_ascat).
  t.add("_values", valueType);
  for (const std::string* role : {&roles.colourSeries, &roles.category, &roles.outerCategory}) {
    if (role->empty()) continue;
    if (*role == kVariablesRole) {
      t.add(kVariablesRole, indexType);
    } else if (*role == kSpacerRole) {
      t.add(kSpacerRole, "byte");
    } else {
      t.add(*role, cx.codes.at(*role).type);
    }
  }
  t.add("_box_sort", "float");

  auto placeRow = [&](const Box& b, double value, double boxSort) {
    const size_t r = t.newRow();
    t.find("_values")->num[r] = value;
    putCellCodes(t, cx, b.key, r);
    if (serset::Column* idx = t.find(kVariablesRole)) idx->num[r] = static_cast<double>(b.item + 1);
    if (serset::Column* spacer = t.find(kSpacerRole)) spacer->num[r] = 1.0;
    t.find("_box_sort")->num[r] = boxSort;
  };
  const bool weighted = !cx.wtype.empty();
  std::vector<std::pair<const Box*, BoxSummary>> summaries;
  for (const auto& [id, b] : boxes) summaries.emplace_back(&b, summarizeBox(b.x, b.w, weighted));
  // Outside values first, then the statistic rows (the order `replace`
  // widening sees them in).
  if (!cx.opt.noOutsides) {
    for (const auto& [b, s] : summaries) {
      for (double v : s.outside) placeRow(*b, v, 9999);
    }
  }
  for (const auto& [b, s] : summaries) {
    for (size_t j = 0; j < s.stat.size(); ++j) placeRow(*b, s.stat[j], static_cast<double>(j + 1));
  }

  serset::Column* values = t.find("_values");
  for (double v : values->num) values->type = widenedFor(values->type, v);
  if (values->type == "float") {
    for (double& v : values->num) v = serset::toFloat(v);
  }

  for (const auto& [b, s] : summaries) {
    ojson box;
    box["median"] = numberOrNull(s.stat[0]);
    box["p25"] = numberOrNull(s.stat[1]);
    box["p75"] = numberOrNull(s.stat[2]);
    box["adjl"] = numberOrNull(s.stat[3]);
    box["adjh"] = numberOrNull(s.stat[4]);
    box["outside"] = s.outside;
    box["series"] = several ? yvars[b->item] : yvars.front();
    const auto groupPos = std::find(cx.byOrder.begin(), cx.byOrder.end(), roles.category);
    if (!roles.category.empty() && groupPos != cx.byOrder.end()) {
      const auto i = static_cast<size_t>(groupPos - cx.byOrder.begin());
      box["category"] = roleLabel(cx, roles.category, fromKey(b->key[i]), {});
    }
    drawn.push_back(box);
  }

  std::vector<serset::Column*> fillKeys;
  for (const std::string* role : {&roles.outerCategory, &roles.category, &roles.colourSeries}) {
    if (!role->empty()) fillKeys.push_back(t.find(*role));
  }
  fillKeys.push_back(t.find("_box_sort"));
  completeCombinations(t, fillKeys);

  std::vector<std::pair<const serset::Column*, bool>> keys;
  if (!roles.outerCategory.empty()) keys.emplace_back(t.find(roles.outerCategory), cx.outerAscending);
  if (!roles.category.empty()) keys.emplace_back(t.find(roles.category), cx.categoryAscending);
  keys.emplace_back(t.find(roles.colourSeries), cx.seriesAscending);
  keys.emplace_back(t.find("_box_sort"), true);
  keys.emplace_back(t.find("_values"), true);
  orderRows(t, keys);
  return std::nullopt;
}

// title()/ytitle(): the first such option's text before an unquoted comma,
// trimmed, without one pair of enclosing double quotes; null when absent.
ojson titleText(const OptionList& opts, const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (lowered(k) != key || v.isBool) continue;
    std::string s = trimmed(v.strValue);
    const long long comma = eval::findUnquotedComma(s);
    if (comma >= 0) s = trimmed(s.substr(0, static_cast<size_t>(comma)));
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return ojson(s);
  }
  return ojson(nullptr);
}

}  // namespace

BarGraphResult runGraphBarBox(SessionState& session, Datastore& ds, const std::string& kind,
                              const std::vector<std::string>& args, const eval::ParsedCommand& parsed) {
  BarGraphResult res;
  auto refuse = [&res](ojson block) {
    res.block = std::move(block);
    res.sets.clear();
    res.ok = false;
    return res;
  };
  const bool isBox = kind == "box" || kind == "hbox";

  // Options are read again from the command text: the dispatcher's canonical
  // option names belong to other commands (`over` would become `overlay`).
  OptionList opts;
  if (const long long comma = eval::findUnquotedComma(parsed.raw); comma >= 0) {
    opts = eval::parseOpts(parsed.raw.substr(static_cast<size_t>(comma) + 1));
  }
  // `graph bar y, over(x), title(T)`: the option list ends at the first top-level comma's
  // options; a SECOND top-level comma is a syntax error that names the next word (bank:
  // `graph bar (mean) price, over(foreign), ylabel(0(5000)15000)` r(198) "invalid 'ylabel'").
  if (const long long c1 = eval::findUnquotedComma(parsed.raw); c1 >= 0) {
    int depth = 0;
    char quote = 0;
    for (size_t i = static_cast<size_t>(c1) + 1; i < parsed.raw.size(); ++i) {
      const char ch = parsed.raw[i];
      if (quote != 0) {
        if (ch == quote) quote = 0;
        continue;
      }
      if (ch == '"') quote = ch;
      else if (ch == '(') ++depth;
      else if (ch == ')' && depth > 0) --depth;
      else if (ch == ',' && depth == 0) {
        size_t a = i + 1;
        while (a < parsed.raw.size() && std::isspace(static_cast<unsigned char>(parsed.raw[a]))) ++a;
        size_t b = a;
        while (b < parsed.raw.size() &&
               (std::isalnum(static_cast<unsigned char>(parsed.raw[b])) || parsed.raw[b] == '_'))
          ++b;
        return refuse(barRefusal("invalid '" + parsed.raw.substr(a, b - a) + "'", 198));
      }
    }
  }
  // The options of the twoway bar PLOT that `graph bar` hands to bar() instead are not its own
  // (bank: `graph bar (mean) price, over(foreign) base(0)` r(198) "option base() not allowed");
  // `as` and `c` are no abbreviation of aspectratio / caption here.
  for (const auto& [key, v] : opts) {
    const std::string k = lowered(key);
    if (k == "as" || k == "c" || k == "barwidth" || k == "base" || k == "bcolor" || k == "fcolor" ||
        k == "lcolor") {
      return refuse(barRefusal("option " + k + (v.isBool ? "" : "()") + " not allowed", 198));
    }
  }
  // Every option of the bar / dot / box graph: the bar-graph object's own syntax (statistic
  // layout, over() groups, bar / dot / box looks), the axis, title, legend, region, added-text
  // and std graph options it hands on, and by(). A word that abbreviates none of them is
  // r(198) "option <word> not allowed" (bank: `graph bar (mean) price, over(foreign)
  // zznotanoption`, `graph dot (mean) price, over(rep78) zznotanoption`). The spellings
  // StataBE refuses although they begin an option (`n`, `as` ...) are the measured
  // abbreviation rows the dispatcher applies before this point.
  {
    static const char* kBarVocab[] = {
        // the statistic layout and groups
        "over", "nofill", "missing", "allcategories", "ascategory", "assecondcategory", "asyvars",
        "percentages", "stack", "cw", "nolabel", "nooutsides", "curorder", "yvaroptions",
        "showyvars", "groupyvars", "varsascategories", "shadevar", "cilevel", "ciline", "by",
        "boxplot", "dotchart",
        // looks
        "horizontal", "vertical", "exclude0", "yreverse", "bargap", "boxgap", "outergap",
        "linegap", "linetype", "ndots", "intensity", "lintensity", "alsize", "capsize", "medtype",
        "medline", "medmarker", "nocwhiskers", "cwhiskers", "rwidth", "lowextension",
        "highextension", "noextendlines", "extendlines", "yalternate", "xalternate", "style",
        "gstyle", "pcycle", "aspectratio", "xoverhangs", "yoverhangs", "bar", "box", "marker",
        "dots", "lines", "rectangles", "blabel", "outside",
        // axes, added lines and text
        "yscale", "ylabel", "ytick", "ymlabel", "ymtick", "ytitle", "yline", "text", "legend",
        // titles
        "title", "subtitle", "note", "caption", "t1title", "t2title", "b1title", "b2title",
        "l1title", "l2title", "r1title", "r2title",
        // regions and std options
        "graphregion", "plotregion", "bgcolor", "scale", "scheme", "copyscheme", "refscheme",
        "xsize", "ysize", "fxsize", "fysize", "name", "saving", "nodraw", "play", "clegend",
        "plegend"};
    for (const auto& [key, v] : opts) {
      const std::string k = lowered(key);
      bool known = false;
      for (const char* full : kBarVocab) {
        if (abbreviates(k, full, 1)) {
          known = true;
          break;
        }
      }
      if (!known) return refuse(barRefusal("option " + k + (v.isBool ? "" : "()") + " not allowed", 198));
    }
  }
  // yvaroptions() takes the over() suboptions; its label(...) holds the categorical axis' label OPTIONS
  // (labsize(), angle(), nolabels ...), never a label text: a quoted text inside it is read as an option name
  // and refused (bank: `graph bar (mean) price, over(foreign) yvaroptions(label("Avg"))` r(198)
  // `option "Avg  not allowed`); label(labsize(small)) is a valid spelling and stays accepted.
  for (const auto& [key, v] : opts) {
    const std::string k = lowered(key);
    if (v.isBool || !(k == "yvaroptions" || (k.size() >= 4 && std::string("yvaroptions").compare(0, k.size(), k) == 0)))
      continue;
    const std::string& t = v.strValue;
    const std::string tl = lowered(t);
    for (size_t at = tl.find("label("); at != std::string::npos; at = tl.find("label(", at + 1)) {
      if (!(at == 0 || !(std::isalnum(static_cast<unsigned char>(tl[at - 1])) || tl[at - 1] == '_'))) continue;
      // the text between label( and its closing parenthesis (nested groups and quotes skipped)
      size_t i = at + 6;
      int depth = 1;
      char quote = 0;
      size_t close = i;
      for (; close < t.size(); ++close) {
        const char c = t[close];
        if (quote != 0) {
          if (c == quote) quote = 0;
        } else if (c == '"') {
          quote = c;
        } else if (c == '(') {
          ++depth;
        } else if (c == ')' && --depth == 0) {
          break;
        }
      }
      // every word of it must be an option name, with its own (...) group when it has one
      while (i < close) {
        const char c = t[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
          ++i;
          continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
          while (i < close && (std::isalnum(static_cast<unsigned char>(t[i])) || t[i] == '_')) ++i;
          if (i < close && t[i] == '(') {
            int d = 0;
            char q = 0;
            for (; i < close; ++i) {
              if (q != 0) {
                if (t[i] == q) q = 0;
              } else if (t[i] == '"') {
                q = t[i];
              } else if (t[i] == '(') {
                ++d;
              } else if (t[i] == ')' && --d == 0) {
                ++i;
                break;
              }
            }
          }
          continue;
        }
        // a quoted text, a number or a rule is not an option name
        size_t e = i;
        if (c == '"') {
          for (++e; e < close && t[e] != '"'; ++e) {
          }
          return refuse(barRefusal("option " + t.substr(i, e - i) + "  not allowed", 198));
        }
        while (e < close && !std::isspace(static_cast<unsigned char>(t[e]))) ++e;
        return refuse(barRefusal("option " + t.substr(i, e - i) + " not allowed", 198));
      }
    }
  }
  const BarOptions opt = readBarOptions(opts, kind);
  if (opt.by) {
    return refuse(ojson(eval::unsupportedOption(
        "by()", "graph " + kind + ", by() draws one graph per by-group; this version of PIET does not "
                "build the by-graph (use over() or one graph per group)")));
  }
  if (opt.overTexts.size() > 3) return refuse(barRefusal("may not specify more than 3 over() options", 198));
  std::vector<OverRequest> overs;
  for (const std::string& text : opt.overTexts) {
    if (trimmed(text).empty()) continue;  // an empty over() is ignored (crgb_edges br_overempty)
    overs.emplace_back();
    if (auto bad = readOver(text, ds, &overs.back())) return refuse(*bad);
  }

  if (isBox && args.empty()) return refuse(barRefusal("varlist or statlist required", 198));
  // A box takes no statistic: `(mean)` reads as a variable name (crgb_edges bx_stat).
  if (isBox) {
    for (const std::string& tok : args) {
      if (tok.size() >= 2 && tok.front() == '(' && tok.back() == ')') {
        return refuse(barRefusal("variable " + trimmed(tok.substr(1, tok.size() - 2)) + " not found", 111));
      }
    }
  }
  const Statlist sl = readStatlist(session, ds, args, /*dropRepeats=*/!isBox);
  if (sl.err) return refuse(*sl.err);
  const std::string wtype = fullWeightName(parsed);
  if (!wtype.empty() && wtype != "aweight" && wtype != "fweight" && wtype != "pweight") {
    return refuse(barRefusal(wtype + "s not allowed", 101));
  }
  if (sl.meanci) {
    if (wtype == "pweight") return refuse(barRefusal("pweights not allowed with statistic meanci", 198));
    return refuse(ojson(eval::unsupportedOption(
        "(meanci)", "graph " + kind + " (meanci) draws confidence intervals from a second serset "
                    "variable that this version of PIET does not build")));
  }
  // No y variable: a bar graph shows the percent of observations per cell.
  if (isBox && sl.items.empty()) return refuse(barRefusal("varlist or statlist required", 198));
  const bool barePercent = sl.items.empty();

  std::vector<std::string> overVars;
  for (const auto& o : overs) overVars.push_back(o.var);
  Roles roles;
  if (auto bad = assignRoles(barePercent ? 1 : sl.items.size(), overVars, opt.asyvars, opt.ascategory, &roles)) {
    return refuse(*bad);
  }
  const RoleOptions ro = roleOptions(overs, opt.asyvars, opt.ascategory);
  // Directions: ascending unless descending; a horizontal graph runs the other
  // way at the category levels, and at the colour-series level unless stacked.
  bool seriesAsc = !(ro.colourSeries != nullptr && ro.colourSeries->descending);
  bool categoryAsc = !(ro.category != nullptr && ro.category->descending);
  bool outerAsc = !(ro.outerCategory != nullptr && ro.outerCategory->descending);
  if (opt.horizontal) {
    if (!opt.stack) seriesAsc = !seriesAsc;
    categoryAsc = !categoryAsc;
    outerAsc = !outerAsc;
  }

  // The estimation sample.
  const int64_t nRows = ds.nRows();
  std::vector<double> weight(static_cast<size_t>(nRows), 1.0);
  std::vector<char> inSample(static_cast<size_t>(nRows), 1);
  if (!wtype.empty()) {
    const std::string wvar = trimmed(parsed.weightVar.value_or(std::string()));
    if (!ds.has(wvar)) return refuse(barRefusal("variable " + wvar + " not found", 111));
    if (ds.dtype(wvar) != "f64") return refuse(barRefusal("type mismatch", 109));
    const double* wcol = ds.col(wvar).f64();
    for (int64_t i = 0; i < nRows; ++i) {
      const double wi = wcol[i];
      weight[static_cast<size_t>(i)] = wi;
      if (std::isnan(wi) || wi == 0) {
        inSample[static_cast<size_t>(i)] = 0;
      } else if (wi < 0) {
        return refuse(barRefusal("negative weights encountered", 402));
      } else if (wtype == "fweight" && std::floor(wi) != wi) {
        return refuse(barRefusal("may not use noninteger frequency weights", 401));
      }
    }
  }
  std::vector<std::string> byOrder(overVars.rbegin(), overVars.rend());
  if (!opt.keepMissing) {
    for (const auto& v : byOrder) {
      const cecon::Column& col = ds.col(v);
      const bool numeric = ds.dtype(v) == "f64";
      for (int64_t i = 0; i < nRows; ++i) {
        bool missing = false;
        if (numeric) {
          missing = std::isnan(col.f64()[i]);
        } else {
          const int32_t c = col.codes()[i];
          missing = c < 0 || static_cast<size_t>(c) >= col.levels.size() || col.levels[static_cast<size_t>(c)].empty();
        }
        if (missing) inSample[static_cast<size_t>(i)] = 0;
      }
    }
  }
  if (opt.casewise && !barePercent) {
    for (const PlotItem& it : sl.items) {
      if (ds.dtype(it.var) != "f64") continue;
      const double* d = ds.col(it.var).f64();
      for (int64_t i = 0; i < nRows; ++i) {
        if (std::isnan(d[i])) inSample[static_cast<size_t>(i)] = 0;
      }
    }
  }
  std::map<std::string, CategoryCoding> codes;
  for (const auto& v : byOrder) codes[v] = codeCategories(session, ds, v, inSample, opt.keepMissing);
  if (std::none_of(inSample.begin(), inSample.end(), [](char c) { return c != 0; })) {
    return refuse(barRefusal("no observations", 2000));
  }

  if (isBox) {
    std::set<std::string> seen;
    for (const PlotItem& it : sl.items) {
      if (!seen.insert(it.var).second) {
        return refuse(barRefusal("variables must be unique, the following are repeated:\n" + it.var, 198));
      }
    }
    for (const PlotItem& it : sl.items) {
      if (ds.dtype(it.var) != "f64") {
        return refuse(barRefusal("string variables not allowed in varlist;\n" + it.var + " is a string variable", 109));
      }
    }
  }
  for (const PlotItem& it : sl.items) {
    if (ds.dtype(it.var) != "f64") {
      if (picksARow(it.stat.kind)) return refuse(barRefusal("type mismatch", 109));
      return refuse(barRefusal(
          "string variable " + it.var + " not allowed with statistic " + statisticTitle(it.stat), 109));
    }
    const StatKind k = it.stat.kind;
    const bool standardError = k == StatKind::SeBinomial || k == StatKind::SePoisson;
    if (wtype == "pweight" && (k == StatKind::Sd || k == StatKind::SeMean || standardError)) {
      return refuse(barRefusal(statisticTitle(it.stat) + " not allowed with pweights", 135));
    }
    if (wtype == "aweight" && standardError) {
      return refuse(barRefusal(statisticTitle(it.stat) + " not allowed with aweights", 135));
    }
  }

  // Cells: the over-code combinations present, keyed last-typed over() first.
  std::map<std::vector<double>, std::vector<int64_t>> cells;
  for (int64_t i = 0; i < nRows; ++i) {
    if (!inSample[static_cast<size_t>(i)]) continue;
    std::vector<double> key;
    for (const auto& v : byOrder) key.push_back(asKey(codes[v].code[static_cast<size_t>(i)]));
    cells[key].push_back(i);
  }

  BarContext cx{session, ds, opt, roles, byOrder, codes, cells, weight, wtype, seriesAsc, categoryAsc, outerAsc};
  GridTable table;
  ojson drawn = ojson::array();
  const std::optional<ojson> bad =
      isBox ? buildBoxSerset(cx, sl.items, table, drawn) : buildBarSerset(cx, sl.items, barePercent, ro, table, drawn);
  if (bad) return refuse(*bad);

  serset::Serset ss;
  ss.n = table.rows();
  ss.cols = std::move(table.columns());

  ojson block;
  block["type"] = "graph";
  block["kind"] = "bar";  // graph describe reports family `bar` for bar and box graphs
  block["chart"] = kind;
  block["horizontal"] = opt.horizontal;
  block["stack"] = opt.stack;
  block["percentages"] = opt.percentages;
  block["title"] = titleText(opts, "title");
  block["ytitle"] = titleText(opts, "ytitle");
  block[isBox ? "boxes" : "bars"] = drawn;
  block["suppressed"] = session.graphicsOff;
  res.block = block;
  res.sets.push_back(std::move(ss));
  res.ok = true;
  return res;
}

// ===========================================================================
// graph pie

namespace {

// `%w.p[fge][c]` -> the number as Stata's string(x, fmt) prints it (leading blanks dropped).
std::string pieFormat(double v, const std::string& fmtText) {
  fmt::StataFmtSpec sp;
  std::string f = trimmed(fmtText);
  bool ok = f.size() >= 3 && f[0] == '%';
  size_t i = 1;
  int w = 0, p = 0;
  while (ok && i < f.size() && std::isdigit(static_cast<unsigned char>(f[i]))) w = w * 10 + (f[i++] - '0');
  if (ok && i < f.size() && f[i] == '.') {
    ++i;
    while (i < f.size() && std::isdigit(static_cast<unsigned char>(f[i]))) p = p * 10 + (f[i++] - '0');
  } else {
    ok = false;
  }
  char conv = ok && i < f.size() ? f[i++] : 0;
  const bool group = ok && i < f.size() && f[i] == 'c';
  if (group) ++i;
  if (!ok || w == 0 || i != f.size() || (conv != 'f' && conv != 'g' && conv != 'e')) {
    w = 6;
    p = 0;
    conv = 'g';
  }
  sp.hasWidth = true;
  sp.width = w;
  sp.prec = p;
  sp.conv = conv;
  sp.groupThousands = group;
  std::string out = fmt::renderValue(v, sp).value_or(fmtStataDefaultG(v));
  return trimmed(out);
}

// The text of a parenthesised option list `a(...) b c(...)`: name -> argument ("" bare).
OptionList pieSubopts(const std::string& text) { return eval::parseOpts(text); }

std::vector<std::string> splitWs(const std::string& s) {
  std::istringstream is(s);
  std::vector<std::string> out;
  std::string t;
  while (is >> t) out.push_back(t);
  return out;
}

// A whole-text number, or nullopt.
std::optional<double> pieNumber(const std::string& raw) {
  const std::string t = trimmed(raw);
  if (t.empty()) return std::nullopt;
  char* end = nullptr;
  const double d = std::strtod(t.c_str(), &end);
  if (end == nullptr || *end != '\0') return std::nullopt;
  return d;
}

}  // namespace

PieGraphResult runGraphPie(SessionState& session, Datastore& ds, const std::vector<std::string>& typedArgs,
                           const eval::ParsedCommand& parsed, bool ifOrInGiven) {
  PieGraphResult res;
  std::vector<std::string> args = typedArgs;  // abbreviated names are expanded below
  auto refuse = [&res](ojson block) {
    res.block = std::move(block);
    res.ok = false;
    return res;
  };
  OptionList opts;
  if (const long long comma = eval::findUnquotedComma(parsed.raw); comma >= 0) {
    opts = eval::parseOpts(parsed.raw.substr(static_cast<size_t>(comma) + 1));
  }
  // The pie graph's own options (over() groups, wedge order, wedge looks and labels) and
  // the title, legend, region and std graph options it hands on; a word that abbreviates
  // none of them is r(198).
  static const char* kPieVocab[] = {
      "cw", "over", "missing", "sort", "descending", "allcategories", "noclockwise", "clockwise",
      "angle0", "pie", "plabel", "ptext", "intensity", "line", "pcycle", "legend", "style",
      "title", "subtitle", "note", "caption", "t1title", "t2title", "b1title", "b2title",
      "l1title", "l2title", "r1title", "r2title", "graphregion", "plotregion", "bgcolor", "scale",
      "scheme", "copyscheme", "refscheme", "xsize", "ysize", "fxsize", "fysize", "name",
      "saving", "nodraw", "play", "aspectratio", "by", "text", "clegend", "plegend"};
  std::vector<std::string> overVars;
  bool cw = false, keepMissing = false, sortTotal = false, descending = false, allCats = false;
  bool clockwise = true;
  double angle0 = 90.0;
  std::vector<std::string> plabelTexts, pieTexts;
  std::vector<std::string> notApplied;
  for (const auto& [key, v] : opts) {
    const std::string k = lowered(key);
    bool known = false;
    for (const char* full : kPieVocab) {
      if (abbreviates(k, full, 1)) {
        known = true;
        break;
      }
    }
    if (!known) return refuse(barRefusal("option " + k + (v.isBool ? "" : "()") + " not allowed", 198));
    if (k == "by") {
      return refuse(ojson(eval::unsupportedOption(
          "by()", "graph pie, by() draws one pie per by-group; this version of PIET does not build the "
                  "by-graph")));
    }
    if (k == "over") {
      for (const std::string& w : splitWs(v.strValue)) overVars.push_back(w);
    } else if (k == "cw") {
      cw = true;
    } else if (abbreviates(k, "missing", 4)) {
      keepMissing = true;
    } else if (k == "sort") {
      if (!v.isBool) {
        return refuse(ojson(eval::unsupportedOption(
            "sort()", "graph pie, sort(varname) orders the wedges by a variable's value on the last "
                      "observation of each group; this version of PIET does not reproduce it")));
      }
      sortTotal = true;
    } else if (abbreviates(k, "descending", 3)) {
      descending = true;
    } else if (abbreviates(k, "allcategories", 4)) {
      allCats = true;
    } else if (abbreviates(k, "noclockwise", 4)) {
      clockwise = false;
    } else if (k == "clockwise") {
      clockwise = true;
    } else if (abbreviates(k, "angle0", 3)) {
      const auto d = pieNumber(trimmed(v.strValue));
      if (!d) return refuse(barRefusal("option angle0() incorrectly specified", 198));
      angle0 = *d;
    } else if (abbreviates(k, "plabel", 2)) {
      plabelTexts.push_back(v.strValue);
    } else if (abbreviates(k, "pie", 1)) {
      pieTexts.push_back(v.strValue);
    } else if (abbreviates(k, "title", 1) || k == "nodraw" || k == "name" || k == "saving" ||
               abbreviates(k, "scheme", 3) || abbreviates(k, "xsize", 4) || abbreviates(k, "ysize", 4)) {
      // drawn (title), or handled by the dispatcher (name, saving, nodraw, scheme, size)
    } else {
      notApplied.push_back(k);
    }
  }
  if (args.size() > 1 && !overVars.empty()) {
    return refuse(barRefusal("option over() invalid with more than one variable", 198));
  }
  if (args.empty() && overVars.empty()) return refuse(barRefusal("graph specification required", 198));
  if (overVars.size() > 1) {
    return refuse(ojson(eval::unsupportedOption(
        "over()", "graph pie with more than one over() variable draws a wedge per combination; this "
                  "version of PIET draws a pie over one variable")));
  }
  if (allCats && ifOrInGiven) {
    return refuse(ojson(eval::unsupportedOption(
        "allcategories", "graph pie, allcategories with if or in keeps the categories outside the "
                         "sample; this version of PIET does not reproduce it")));
  }
  for (std::string& v : args) {
    if (auto bad = namedVariable(session, ds, &v)) return refuse(*bad);
    if (ds.dtype(v) != "f64") {
      return refuse(barRefusal("string variables not allowed in varlist;\n" + v + " is a string variable", 109));
    }
  }
  for (std::string& v : overVars) {
    if (auto bad = namedVariable(session, ds, &v)) return refuse(*bad);
  }

  // The sample and the weights (pweights are iweights here, as Stata reads them).
  const int64_t nRows = ds.nRows();
  std::string wtype = fullWeightName(parsed);
  if (!wtype.empty() && wtype != "aweight" && wtype != "fweight" && wtype != "pweight") {
    return refuse(barRefusal(wtype + "s not allowed", 101));
  }
  std::vector<double> weight(static_cast<size_t>(nRows), 1.0);
  std::vector<char> touse(static_cast<size_t>(nRows), 1);
  if (!wtype.empty()) {
    const std::string wvar = trimmed(parsed.weightVar.value_or(std::string()));
    if (!ds.has(wvar)) return refuse(barRefusal("variable " + wvar + " not found", 111));
    if (ds.dtype(wvar) != "f64") return refuse(barRefusal("type mismatch", 109));
    const double* wcol = ds.col(wvar).f64();
    for (int64_t i = 0; i < nRows; ++i) {
      const double wi = wcol[i];
      weight[static_cast<size_t>(i)] = wi;
      if (std::isnan(wi) || wi == 0) {
        touse[static_cast<size_t>(i)] = 0;
      } else if (wi < 0) {
        return refuse(barRefusal("negative weights encountered", 402));
      } else if (wtype == "fweight" && std::floor(wi) != wi) {
        return refuse(barRefusal("may not use noninteger frequency weights", 401));
      }
    }
  }

  struct Wedge {
    double total = 0;
    std::string label;
  };
  std::vector<Wedge> wedges;
  if (!overVars.empty()) {
    const std::string& over = overVars.front();
    const double* y = args.empty() ? nullptr : ds.col(args.front()).f64();
    for (int64_t i = 0; i < nRows; ++i) {
      if (y != nullptr && std::isnan(y[i])) touse[static_cast<size_t>(i)] = 0;
    }
    std::vector<char> overSample(static_cast<size_t>(nRows), 1);
    if (!keepMissing) {
      const cecon::Column& col = ds.col(over);
      const bool numeric = ds.dtype(over) == "f64";
      for (int64_t i = 0; i < nRows; ++i) {
        bool missing;
        if (numeric) {
          missing = std::isnan(col.f64()[i]);
        } else {
          const int32_t c = col.codes()[i];
          missing = c < 0 || static_cast<size_t>(c) >= col.levels.size() || col.levels[static_cast<size_t>(c)].empty();
        }
        if (missing) {
          touse[static_cast<size_t>(i)] = 0;
          overSample[static_cast<size_t>(i)] = 0;
        }
      }
    }
    // aweights are scaled to mean 1 over the sample; the others count as given
    if (wtype == "aweight") {
      double sw = 0;
      int64_t nw = 0;
      for (int64_t i = 0; i < nRows; ++i) {
        if (!touse[static_cast<size_t>(i)]) continue;
        sw += weight[static_cast<size_t>(i)];
        ++nw;
      }
      const double mean = nw > 0 ? sw / static_cast<double>(nw) : 1.0;
      for (auto& w : weight) w /= mean;
    }
    const std::vector<char>& codeSample = allCats ? overSample : touse;
    const CategoryCoding cc = codeCategories(session, ds, over, codeSample, keepMissing);
    std::vector<double> totals(cc.labels.size(), 0.0);
    std::vector<char> present(cc.labels.size(), allCats ? 1 : 0);
    for (int64_t i = 0; i < nRows; ++i) {
      if (!touse[static_cast<size_t>(i)]) continue;
      const double code = cc.code[static_cast<size_t>(i)];
      if (std::isnan(code)) continue;
      const size_t g = static_cast<size_t>(code) - 1;
      totals[g] += (y != nullptr ? y[i] : 1.0) * weight[static_cast<size_t>(i)];
      present[g] = 1;
    }
    for (size_t g = 0; g < cc.labels.size(); ++g) {
      if (present[g]) wedges.push_back({totals[g], cc.labels[g]});
    }
  } else {
    if (cw) {
      for (const std::string& v : args) {
        const double* d = ds.col(v).f64();
        for (int64_t i = 0; i < nRows; ++i) {
          if (std::isnan(d[i])) touse[static_cast<size_t>(i)] = 0;
        }
      }
    }
    for (const std::string& v : args) {
      const double* d = ds.col(v).f64();
      double sum = 0, sumW = 0;
      int64_t n = 0;
      for (int64_t i = 0; i < nRows; ++i) {
        if (!touse[static_cast<size_t>(i)] || std::isnan(d[i])) continue;
        const double w = weight[static_cast<size_t>(i)];
        sum += w * d[i];
        sumW += w;
        ++n;
      }
      // summarize's r(sum) is the weighted sum; under aweights the wedge is r(sum)*r(N)/r(sum_w)
      double total = sum;
      if (wtype == "aweight") total = sumW > 0 ? sum * static_cast<double>(n) / sumW : 0.0;
      std::string label = session.eval.varLabels.get(v, std::string());
      if (label.empty()) label = v;
      wedges.push_back({total, label});
    }
  }
  for (const Wedge& w : wedges) {
    if (w.total < 0) return refuse(barRefusal("sum for a pie wedge is negative", 459));
  }
  double grand = 0;
  for (const Wedge& w : wedges) grand += w.total;
  if (sortTotal) {
    std::stable_sort(wedges.begin(), wedges.end(), [descending](const Wedge& a, const Wedge& b) {
      return descending ? a.total > b.total : a.total < b.total;
    });
  }
  const bool positive = grand > 0 && std::isfinite(grand);
  if (!positive) res.warning = "sum of pie wedges is not positive";

  // plabel(): `_all|# sum|percent|name|none|"text" [, format() ...]`
  std::vector<std::string> wedgeLabelText(wedges.size());
  for (const std::string& text : plabelTexts) {
    std::string lhs = text, rhs;
    if (const long long c = eval::findUnquotedComma(text); c >= 0) {
      lhs = text.substr(0, static_cast<size_t>(c));
      rhs = text.substr(static_cast<size_t>(c) + 1);
    }
    std::string format = "%6.0g";
    for (const auto& [sk, sv] : pieSubopts(rhs)) {
      if (abbreviates(lowered(sk), "format", 3) && !sv.isBool) format = trimmed(sv.strValue);
    }
    lhs = trimmed(lhs);
    const size_t sp = lhs.find_first_of(" \t");
    const std::string who = sp == std::string::npos ? lhs : lhs.substr(0, sp);
    const std::string what = sp == std::string::npos ? std::string() : trimmed(lhs.substr(sp));
    size_t first = 0, last = wedges.size();
    if (who != "_all") {
      const auto d = pieNumber(who);
      if (!d || std::floor(*d) != *d) return refuse(barRefusal("invalid pie # in plabel(), " + who, 198));
      if (*d < 1 || *d > static_cast<double>(wedges.size())) continue;  // "larger than number of pie wedges, ignored"
      first = static_cast<size_t>(*d) - 1;
      last = first + 1;
    }
    const std::string wl = lowered(what);
    const bool quoted = what.size() >= 2 && what.front() == '"' && what.back() == '"';
    const bool isSum = abbreviates(wl, "sum", 3), isPct = abbreviates(wl, "percent", 3),
               isName = abbreviates(wl, "name", 2), isNone = abbreviates(wl, "none", 1);
    if (!quoted && !isSum && !isPct && !isName && !isNone) {
      return refuse(barRefusal("not a plabel() type , " + what, 198));
    }
    for (size_t j = first; j < last; ++j) {
      if (wedges[j].total == 0) continue;  // an empty wedge carries no label
      if (quoted) wedgeLabelText[j] = what.substr(1, what.size() - 2);
      else if (isSum) wedgeLabelText[j] = pieFormat(wedges[j].total, format);
      else if (isPct) wedgeLabelText[j] = positive ? pieFormat(100.0 * wedges[j].total / grand, format) + "%" : std::string();
      else if (isName) wedgeLabelText[j] = wedges[j].label;
      else wedgeLabelText[j].clear();
    }
  }
  // pie(): `_all|numlist [, explode[()] color() ...]`
  std::vector<char> explode(wedges.size(), 0);
  for (const std::string& text : pieTexts) {
    std::string lhs = text, rhs;
    if (const long long c = eval::findUnquotedComma(text); c >= 0) {
      lhs = text.substr(0, static_cast<size_t>(c));
      rhs = text.substr(static_cast<size_t>(c) + 1);
    }
    std::vector<size_t> which;
    lhs = trimmed(lhs);
    if (lhs == "_all") {
      for (size_t j = 0; j < wedges.size(); ++j) which.push_back(j);
    } else {
      for (const std::string& tok : splitWs(lhs)) {
        const auto d = pieNumber(tok);
        if (!d || std::floor(*d) != *d || *d < 1) {
          return refuse(barRefusal("invalid wedge numbers in pie(), " + lhs, 198));
        }
        if (*d <= static_cast<double>(wedges.size())) which.push_back(static_cast<size_t>(*d) - 1);
      }
    }
    for (const auto& [sk, sv] : pieSubopts(rhs)) {
      const std::string k = lowered(sk);
      if (abbreviates(k, "explode", 7)) {
        for (size_t j : which) explode[j] = 1;
      } else {
        notApplied.push_back("pie(" + k + "())");
      }
    }
  }

  ojson slices = ojson::array();
  for (size_t j = 0; j < wedges.size(); ++j) {
    ojson sl;
    sl["label"] = wedges[j].label;
    sl["value"] = wedges[j].total;
    sl["angle"] = positive ? 360.0 * wedges[j].total / grand : 0.0;
    if (!wedgeLabelText[j].empty()) sl["plabel"] = wedgeLabelText[j];
    if (explode[j]) sl["explode"] = true;
    slices.push_back(sl);
  }
  ojson block;
  block["type"] = "graph";
  block["kind"] = "pie";
  block["title"] = titleText(opts, "title");
  block["clockwise"] = clockwise;
  block["angle0"] = angle0;
  block["slices"] = slices;
  block["suppressed"] = session.graphicsOff;
  if (!notApplied.empty()) {
    std::string list;
    for (const auto& n : notApplied) list += (list.empty() ? "" : ", ") + n;
    block["note"] = "(pie: " + list + " accepted and not applied — this build draws the plot without it.)";
  }
  res.block = block;
  res.ok = true;
  return res;
}

}  // namespace piet::handlers
