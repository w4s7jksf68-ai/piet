// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// `misstable summarize [varlist] [if] [in] [, all showzeros]` -- the missing-value
// table and the r() it leaves.
//
// The rules are [R] misstable and misstable.ado (StataNow 19.5), read for what
// the command computes and stores; the code is our own:
//   * the varlist defaults to every variable; without `all` only the numeric
//     variables with a missing value (`.` or `.a`-`.z`) in the sample are shown,
//     and when there are none the answer is "(variables nonmissing or string)";
//   * per variable: Obs=. (system missing), Obs>. (extended missing), Obs<.
//     (non-missing), the number of distinct non-missing values when it is at most
//     500 (0 when there is none; else `.`, shown ">500"), and the minimum and
//     maximum;
//   * r() holds the LAST variable shown: r(vartype) numeric|string|none and the
//     scalars N_eq_dot, N_gt_dot, N_lt_dot, K_uniq, min, max (all `.` for a
//     string variable, or when nothing is shown).
// `misstable patterns [varlist] [if] [in] [, frequency exok asis]`:
//   * the variables with a missing value among the sample's incomplete
//     observations, ordered by their number of missing values (ascending; `asis`
//     keeps the varlist order), each run of equal counts reordered greedily so
//     that neighbours share missing observations (misstable.ado's fixruns: the
//     first of a run that starts the list is the one sharing most with any other,
//     every later one the one sharing most with its predecessor, ties to the
//     alphabetically first name);
//   * r(N_complete), r(N_incomplete), r(K) (distinct missing-value patterns among
//     the incomplete observations) and r(vars), the ordered variables;
//   * the table lists the complete pattern, then the others by descending
//     frequency (ties by descending missing indicators), as percentages or, with
//     `frequency`, counts.
// `misstable tree [varlist] [if] [in] [, frequency exok asis]`: the same variables
// ordered by DESCENDING count, the nested split of the sample by missing /
// non-missing on each in turn (first 7 variables), as percentages of the sample
// or counts; r(vars) only.
// `generate()`, `bypatterns`, `replace` and `clear` are refused by name;
// `misstable nested` is refused as not implemented.

#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "data/missing.h"  // cecon::missing_kind: `.` versus `.a`-`.z`
#include "engine/scratch_columns.h"
#include "language/command_reply_support.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers/utility.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace {

const piet::eval::OptVocab& kVocabMisstable = piet::eval::registerOptVocab(
    {"misstable"}, {{"all", "showzeros", "nopreserve", "frequency", "exok", "asis"},
                    {"generate", "bypatterns", "replace", "clear"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;
using namespace command_reply_detail;

namespace {

std::string field(double v, const std::string& fmt, size_t width) {
  std::string s = std::isnan(v) ? std::string(".") : eval::applyStataFmt(v, fmt);
  if (s.size() < width) s = std::string(width - s.size(), ' ') + s;
  return s;
}

// The variable name in a 12-wide column: abbrev(name, 12).
std::string abbrev12(const std::string& name) {
  if (name.size() <= 12) return name;
  return name.substr(0, 10) + "~" + name.substr(name.size() - 1);
}

void padTo(std::string& line, size_t col) {  // col is 1-based
  if (line.size() < col - 1) line.append(col - 1 - line.size(), ' ');
}

struct MtRow {
  bool isString = false;
  double nEq = 0, nGt = 0, nLt = 0, kUniq = 0, min = 0, max = 0;
};

std::string fixedField(double v, const std::string& fmt, size_t width) {
  return field(v, fmt, width);
}

struct MtCol {
  std::string name;
  long long count = 0;
  const std::vector<char>* d = nullptr;
};

// misstable.ado's mt_tree: one line per leaf of the missing/non-missing split,
// each column `%10.0f` percent of the sample (`<1` below one, `%` on the first
// line only) or `%10.0fc` count, 11 characters a column.
ojson treeTable(const std::vector<MtCol>& colsIn,
                const std::vector<std::pair<long long, std::vector<char>>>& patterns, double n,
                bool frequency) {
  std::vector<std::string> out;
  std::vector<MtCol> cols = colsIn;
  if (cols.size() > 7) {
    cols.resize(7);
    out.push_back("(only 7 variables shown)");
  }
  const size_t k = cols.size();
  const size_t indent = 2;
  out.push_back("");
  out.push_back(std::string(indent, ' ') + "Nested pattern of missing values");
  {
    std::string h(indent - 1, ' ');
    for (const auto& c : cols) {
      std::string nm = c.name.size() > 10 ? c.name.substr(0, 8) + "~" + c.name.substr(c.name.size() - 1)
                                          : c.name;
      h += std::string(11 - std::min<size_t>(11, nm.size()), ' ') + nm;
    }
    out.push_back(h);
  }
  const std::string rule = std::string(indent, ' ') + std::string(11 * k - 1, '-');
  out.push_back(rule);
  bool firstLine = true;
  std::string line(indent, ' ');
  const auto number = [&](double count) {
    if (frequency) {
      line += field(count, "%10.0fc", 10) + " ";
      return;
    }
    const double per = count / n * 100.0;
    line += per < 1 && per != 0 ? std::string(8, ' ') + "<1" : field(per, "%10.0f", 10);
    line += firstLine ? "%" : " ";
  };
  // `rows` is the subset of patterns (by index) still in this branch.
  std::function<void(size_t, size_t, const std::vector<size_t>&, double)> walk =
      [&](size_t ind, size_t j0, const std::vector<size_t>& rows, double denom) {
        if (j0 >= k) {
          out.push_back(line);
          line.clear();
          firstLine = false;
          return;
        }
        std::vector<size_t> miss, rest;
        double thisCount = 0;
        for (size_t r : rows) {
          if (patterns[r].second[j0]) {
            miss.push_back(r);
            thisCount += static_cast<double>(patterns[r].first);
          } else {
            rest.push_back(r);
          }
        }
        number(thisCount);
        walk(ind + 11, j0 + 1, miss, thisCount);
        line = std::string(ind, ' ');
        number(denom - thisCount);
        walk(ind + 11, j0 + 1, rest, denom - thisCount);
      };
  std::vector<size_t> all(patterns.size());
  for (size_t i = 0; i < all.size(); ++i) all[i] = i;
  walk(indent, 0, all, n);
  out.push_back(rule);
  out.push_back(frequency ? " (number missing listed first)" : " (percent missing listed first)");
  std::string text;
  for (size_t i = 0; i < out.size(); ++i) text += (i ? "\n" : "") + out[i];
  return displayBlock(text);
}

// `misstable patterns`.
ojson misstablePatterns(const eval::ParsedCommand& parsed, SessionState& session,
                        const std::vector<std::string>& words, bool tree) {
  if (hasOpt(parsed.opts, "showzeros") || hasOpt(parsed.opts, "all"))
    return envError(std::string("option ") + (hasOpt(parsed.opts, "all") ? "all" : "showzeros") +
                        " not allowed",
                    198);
  const bool frequency = hasOpt(parsed.opts, "frequency");
  const bool exok = hasOpt(parsed.opts, "exok");
  const bool asis = hasOpt(parsed.opts, "asis");
  const double dot = std::numeric_limits<double>::quiet_NaN();
  (void)dot;

  eval::replaceRClass(session.eval);
  auto post = [&](double nComplete, double nIncomplete, double k, const std::string& vars) {
    if (tree) {  // misstable_tree returns r(vars) only
      if (!vars.empty()) session.eval.rStrings.set("vars", vars);
      eval::declareRPosted(session.eval, "misstable",
                           vars.empty() ? std::vector<std::string>{} : std::vector<std::string>{"vars"});
      return;
    }
    session.eval.rValues.set("N_complete", nComplete);
    session.eval.rValues.set("N_incomplete", nIncomplete);
    session.eval.rValues.set("K", k);
    if (!vars.empty()) session.eval.rStrings.set("vars", vars);
    eval::declareRPosted(session.eval, "misstable",
                         vars.empty() ? std::vector<std::string>{"N_complete", "N_incomplete", "K"}
                                      : std::vector<std::string>{"N_complete", "N_incomplete", "K",
                                                                 "vars"});
  };
  if (!session.hasData()) {
    post(0, 0, 0, "");
    return ojson::array();
  }

  ScratchColumns scratch(session);
  std::string qualifier;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
    qualifier += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
    qualifier += " in " + *parsed.inRange;
  const size_t rows = static_cast<size_t>(session.datastore()->nRows());
  std::vector<char> inSample(rows, 1);
  if (!qualifier.empty()) {
    std::string mark;
    if (auto e = scratch.add("1", qualifier, &mark)) return *e;
    const auto m = scratch.values(mark);
    for (size_t r = 0; r < rows; ++r) inSample[r] = std::isnan(m[r]) ? 0 : 1;
  }
  double n = 0;
  for (char c : inSample) n += c ? 1 : 0;

  std::string spec;
  for (size_t i = 1; i < words.size(); ++i) spec += (spec.empty() ? "" : " ") + words[i];
  std::vector<std::string> vars;
  if (spec.empty()) {
    for (const auto& v : session.datastore()->names())
      if (!isHiddenColumn(v)) vars.push_back(v);
  } else {
    auto vr = loops::expandVarlist(spec, session);
    if (vr.isError) return vr.error;
    if (auto e = loops::checkKnownVarlist(vr.tokens, session)) return *e;
    for (const auto& v : vr.tokens)
      if (std::find(vars.begin(), vars.end(), v) == vars.end()) vars.push_back(v);  // list uniq
  }
  if (vars.empty()) {
    post(n, 0, 0, "");
    return ojson::array();
  }
  Datastore* ds = session.datastore();
  std::vector<std::string> numeric;
  for (const auto& v : vars)
    if (ds->dtype(v) != "factor") numeric.push_back(v);
  if (numeric.empty()) {
    post(n, 0, 0, "");
    return displayBlock("(no numeric variables)");
  }
  if (n == 0) {
    post(n, 0, 0, "");
    return displayBlock("(no observations)");
  }
  // The missing indicator of every numeric variable on every sample row.
  const auto isMissing = [exok](double x) {
    return std::isnan(x) && (!exok || cecon::missing_kind(x) == 0);
  };
  std::vector<std::vector<char>> miss;
  for (const auto& v : numeric) {
    const double* cells = ds->col(v).f64();
    std::vector<char> col(rows, 0);
    for (size_t r = 0; r < rows; ++r) col[r] = inSample[r] && isMissing(cells[r]) ? 1 : 0;
    miss.push_back(std::move(col));
  }
  std::vector<char> incomplete(rows, 0);
  double nIncomplete = 0;
  for (size_t r = 0; r < rows; ++r) {
    for (const auto& col : miss)
      if (col[r]) {
        incomplete[r] = 1;
        break;
      }
    nIncomplete += incomplete[r] ? 1 : 0;
  }
  if (nIncomplete == 0) {
    post(n, 0, 0, "");
    return displayBlock("(no missing values)");
  }

  // reduce_and_sort: keep the variables with a missing value, order them.
  using Col = MtCol;
  std::vector<Col> cols;
  for (size_t j = 0; j < numeric.size(); ++j) {
    long long c = 0;
    for (size_t r = 0; r < rows; ++r) c += miss[j][r];
    if (c > 0) cols.push_back({numeric[j], c, &miss[j]});
  }
  const auto shared = [rows](const Col& a, const Col& b) {
    long long k = 0;
    for (size_t r = 0; r < rows; ++r) k += (*a.d)[r] && (*b.d)[r];
    return k;
  };
  if (!asis) {
    // patterns orders by ascending count, tree by descending (mt_std_parse's
    // `desire` 1 / -1).
    std::stable_sort(cols.begin(), cols.end(), [tree](const Col& a, const Col& b) {
      return tree ? a.count > b.count : a.count < b.count;
    });
    for (size_t i0 = 0; i0 + 1 < cols.size();) {
      size_t i1 = i0;
      while (i1 + 1 < cols.size() && cols[i1 + 1].count == cols[i0].count) ++i1;
      for (size_t at = i0; at < i1; ++at) {
        size_t best = at;
        long long bestRuns = -1;
        for (size_t i = at; i <= i1; ++i) {
          long long runs = -1;
          if (at == 0) {  // the list's first: most shared with any other of the run
            for (size_t j = at; j <= i1; ++j)
              if (j != i) runs = std::max(runs, shared(cols[j], cols[i]));
          } else {  // later: most shared with the one before
            runs = shared(cols[i], cols[at - 1]);
          }
          if (runs > bestRuns || (runs == bestRuns && cols[i].name < cols[best].name)) {
            best = i;
            bestRuns = runs;
          }
        }
        std::swap(cols[at], cols[best]);
      }
      i0 = i1 + 1;
    }
  }

  // The distinct missing-value patterns of the incomplete rows, with counts.
  std::map<std::vector<char>, long long> patterns;
  for (size_t r = 0; r < rows; ++r) {
    if (!incomplete[r]) continue;
    std::vector<char> p;
    for (const auto& c : cols) p.push_back((*c.d)[r]);
    ++patterns[p];
  }
  std::vector<std::pair<long long, std::vector<char>>> listed;
  for (const auto& kv : patterns) listed.emplace_back(kv.second, kv.first);
  std::sort(listed.begin(), listed.end());  // freq, then indicators, ascending
  std::string varsText;
  for (size_t j = 0; j < cols.size(); ++j) varsText += (j ? " " : "") + cols[j].name;
  const double nComplete = n - nIncomplete;
  post(nComplete, nIncomplete, static_cast<double>(listed.size()), varsText);
  if (tree) return treeTable(cols, listed, n, frequency);

  // The table.
  const size_t trun = cols.size();
  const size_t shown = std::min<size_t>(trun, 16);
  const size_t indent = 2;
  size_t tabwid = 13;
  if (shown >= 4) {
    const size_t blocks4 = shown / 4;
    const size_t extra = blocks4 * 4 != shown ? 2 + 3 * (shown - blocks4 * 4) : 0;
    tabwid = 14 * blocks4 - 1 + extra;
  }
  const size_t width = 12 + tabwid;
  const std::string ttl = "Missing-value patterns";
  const size_t ttlpad = indent + (width > ttl.size() ? (width - ttl.size()) / 2 : 0);
  const std::string ind(indent, ' ');
  std::vector<std::string> out;
  out.push_back("");
  out.push_back(std::string(ttlpad, ' ') + ttl);
  out.push_back(std::string(ttlpad, ' ') + "  (1 means complete)");
  out.push_back("");
  out.push_back(ind + "            |   Pattern");
  {
    std::string h = ind + (frequency ? "  Frequency |" : "  Percent   |");
    for (size_t j = 1; j <= shown; ++j) {
      if (j > 1 && (j - 1) % 4 == 0) h += "  ";
      h += fixedField(static_cast<double>(j), "%3.0f", 3);
    }
    out.push_back(h);
  }
  const std::string rule = ind + std::string(12, '-') + "+" + std::string(tabwid, '-');
  out.push_back(rule);
  const auto oneLine = [&](bool first, double count, const std::vector<char>& v) {
    std::string l = ind;
    if (frequency) {
      l += fixedField(count, "%11.0gc", 11) + " |";
    } else {
      const double f = 100.0 * count / n;
      l += f < 1 ? std::string("     <1") : " " + fixedField(f, "%6.0f", 6);
      l += first ? "%" : " ";
      l += "    |";
    }
    for (size_t j = 1; j <= v.size(); ++j) {
      if ((j - 1) % 16 == 0 && j != 1) {
        out.push_back(l);
        l = ind + std::string(12, ' ') + "|";
      } else if (j > 1 && (j - 1) % 4 == 0) {
        l += "  ";
      }
      l += v[j - 1] ? "  0" : "  1";
    }
    out.push_back(l);
    if (v.size() > 16 && !first) out.push_back(ind + std::string(12, ' ') + "|");
  };
  oneLine(true, nComplete, std::vector<char>(trun, 0));
  out.push_back(ind + std::string(12, ' ') + "|");
  for (auto it = listed.rbegin(); it != listed.rend(); ++it)
    oneLine(false, static_cast<double>(it->first), it->second);
  out.push_back(rule);
  out.push_back(frequency ? ind + fixedField(n, "%11.0gc", 11) + " |" : ind + "    100%    |");
  out.push_back("");
  // "Variables are (1) a (2) b ...", wrapped at 78 columns, continuation at 16.
  {
    std::string l = ind + "Variables are";
    for (size_t j = 0; j < trun; ++j) {
      const std::string unit = " (" + std::to_string(j + 1) + ") " + cols[j].name;
      if (l.size() + 1 + unit.size() > 78) {
        out.push_back(l);
        l = std::string(indent + 14, ' ') + unit.substr(1);
      } else {
        l += " " + unit;
      }
    }
    out.push_back(l);
  }
  std::string text;
  for (size_t i = 0; i < out.size(); ++i) text += (i ? "\n" : "") + out[i];
  return displayBlock(text);
}

}  // namespace

ojson runMisstable(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMisstable.known,
                                            kVocabMisstable.unimplemented))
    return ojson(*err);
  std::vector<std::string> words = splitWs(parsed.argText);
  const std::string sub = words.empty() ? std::string() : words.front();
  const auto isAbbrevOf = [&sub](const std::string& full, size_t minLen) {
    return sub.size() >= minLen && sub.size() <= full.size() && full.compare(0, sub.size(), sub) == 0;
  };
  if (sub.empty())
    return envError("misstable subcommand not specified\nvalid misstable subcommands are "
                    "summarize, patterns, tree, and nested",
                    198);
  if (isAbbrevOf("nested", 4))
    return ojson(eval::engineError("`misstable " + sub +
                                       "` is not implemented in this version of PIET (only "
                                       "`misstable summarize`, `patterns` and `tree`).",
                                   "unimplemented"));
  if (isAbbrevOf("patterns", 3)) return misstablePatterns(parsed, session, words, false);
  if (sub == "tree") return misstablePatterns(parsed, session, words, true);
  if (!isAbbrevOf("summarize", 3))
    return envError("misstable " + sub + " unrecognized\nvalid misstable subcommands are "
                    "summarize, patterns, tree, and nested",
                    198);
  for (const char* only : {"frequency", "exok", "asis"})
    if (hasOpt(parsed.opts, only)) return envError(std::string("option ") + only + " not allowed", 198);
  if (!session.hasData()) {
    // No variable at all: misstable.ado's `syntax [varlist]` is empty, so r() is
    // the initial set (vartype "none", every scalar missing) and nothing prints.
    eval::replaceRClass(session.eval);
    session.eval.rStrings.set("vartype", "none");
    for (const char* k : {"N_eq_dot", "N_gt_dot", "N_lt_dot", "K_uniq", "min", "max"})
      session.eval.rValues.set(k, std::numeric_limits<double>::quiet_NaN());
    return ojson::array();
  }

  // The varlist: every variable when none is given.
  std::string spec;
  for (size_t i = 1; i < words.size(); ++i) spec += (spec.empty() ? "" : " ") + words[i];
  std::vector<std::string> vars;
  if (spec.empty()) {
    vars = session.datastore()->names();
    vars.erase(std::remove_if(vars.begin(), vars.end(),
                              [](const std::string& n) { return isHiddenColumn(n); }),
               vars.end());
  } else {
    auto vr = loops::expandVarlist(spec, session);
    if (vr.isError) return vr.error;
    if (auto e = loops::checkKnownVarlist(vr.tokens, session)) return *e;
    vars = vr.tokens;
  }

  ScratchColumns scratch(session);
  std::string qualifier;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
    qualifier += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
    qualifier += " in " + *parsed.inRange;
  const size_t rows = static_cast<size_t>(session.datastore()->nRows());
  std::vector<char> inSample(rows, 1);
  if (!qualifier.empty()) {
    std::string mark;
    if (auto e = scratch.add("1", qualifier, &mark)) return *e;
    const auto m = scratch.values(mark);
    for (size_t r = 0; r < rows; ++r) inSample[r] = std::isnan(m[r]) ? 0 : 1;
  }
  const bool all = hasOpt(parsed.opts, "all");
  const bool showZeros = hasOpt(parsed.opts, "showzeros");
  const double dot = std::numeric_limits<double>::quiet_NaN();

  std::vector<std::pair<std::string, MtRow>> shown;
  {
    Datastore* ds = session.datastore();
    for (const auto& v : vars) {
      MtRow row;
      if (ds->dtype(v) == "factor") {
        if (!all) continue;
        row.isString = true;
        shown.emplace_back(v, row);
        continue;
      }
      const double* cells = ds->col(v).f64();
      std::set<double> distinct;
      double lo = std::numeric_limits<double>::infinity(), hi = -lo;
      for (size_t r = 0; r < rows; ++r) {
        if (!inSample[r]) continue;
        const double x = cells[r];
        if (std::isnan(x)) {
          (cecon::missing_kind(x) == 0 ? row.nEq : row.nGt) += 1;
          continue;
        }
        row.nLt += 1;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
        distinct.insert(x == 0.0 ? 0.0 : x);
      }
      if (!all && row.nEq + row.nGt == 0) continue;
      row.min = row.nLt > 0 ? lo : dot;
      row.max = row.nLt > 0 ? hi : dot;
      // `capture tabulate`'s r(r): 0 when no value is left (StataBE 19.5, truth bank
      // cell d08d1cecefb76b7c: union68 all missing shows 0), `.` past 500.
      row.kUniq = distinct.size() > 500 ? dot : static_cast<double>(distinct.size());
      shown.emplace_back(v, row);
    }
  }

  eval::replaceRClass(session.eval);
  auto post = [&](const std::string& vartype, const MtRow* row) {
    session.eval.rStrings.set("vartype", vartype);
    const bool num = row != nullptr && !row->isString;
    session.eval.rValues.set("N_eq_dot", num ? row->nEq : dot);
    session.eval.rValues.set("N_gt_dot", num ? row->nGt : dot);
    session.eval.rValues.set("N_lt_dot", num ? row->nLt : dot);
    session.eval.rValues.set("K_uniq", num ? row->kUniq : dot);
    session.eval.rValues.set("min", num ? row->min : dot);
    session.eval.rValues.set("max", num ? row->max : dot);
  };
  eval::declareRPosted(session.eval, "misstable",
                       {"vartype", "N_eq_dot", "N_gt_dot", "N_lt_dot", "K_uniq", "min", "max"});
  if (vars.empty()) {
    post("none", nullptr);
    return ojson::array();
  }
  if (shown.empty()) {
    post("none", nullptr);
    return displayBlock("(variables nonmissing or string)");
  }

  std::vector<std::string> lines;
  {
    std::string l1;
    padTo(l1, 64);
    lines.push_back(l1 + "Obs<.");
    std::string l2;
    padTo(l2, 49);
    lines.push_back(l2 + "+" + std::string(30, '-'));
    std::string l3;
    padTo(l3, 16);
    l3 += "|";
    padTo(l3, 49);
    lines.push_back(l3 + "| Unique");
    std::string l4;
    padTo(l4, 7);
    l4 += "Variable |";
    padTo(l4, 22);
    l4 += "Obs=.";
    padTo(l4, 32);
    l4 += "Obs>.";
    padTo(l4, 42);
    l4 += "Obs<.";
    padTo(l4, 49);
    l4 += "| values";
    padTo(l4, 65);
    l4 += "Min";
    padTo(l4, 77);
    lines.push_back(l4 + "Max");
    lines.push_back("  " + std::string(13, '-') + "+" + std::string(32, '-') + "+" +
                    std::string(30, '-'));
  }
  for (const auto& [v, row] : shown) {
    const std::string vname = abbrev12(v);
    std::string line(2 + 12 - std::min<size_t>(12, vname.size()), ' ');
    line += vname + " |";
    if (row.isString) {
      line += " (string variable)";
      padTo(line, 49);
      lines.push_back(line + "|");
      continue;
    }
    line += row.nEq != 0 || showZeros ? field(row.nEq, "%10.0gc", 10) : std::string(10, ' ');
    line += row.nGt != 0 || showZeros ? field(row.nGt, "%10.0gc", 10) : std::string(10, ' ');
    line += field(row.nLt, "%10.0gc", 10);
    line += std::isnan(row.kUniq) ? "  |   >500" : "  |" + field(row.kUniq, "%7.0gc", 7);
    line += "  " + field(row.min, "%9.0g", 9) + "   " + field(row.max, "%9.0g", 9);
    lines.push_back(line);
  }
  lines.push_back("  " + std::string(77, '-'));

  const auto& last = shown.back().second;
  post(last.isString ? "string" : "numeric", &last);
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) text += (i ? "\n" : "") + lines[i];
  return displayBlock(text);
}

}  // namespace piet::handlers
